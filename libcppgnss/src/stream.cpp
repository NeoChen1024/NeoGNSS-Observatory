// SPDX-License-Identifier: GPL-3.0-only
#include <array>
#include <cppgnss/stream.hpp>
#include <stdexcept>

namespace cppgnss {
uint16_t FrameView::id() const {
    return std::visit(
        [](const auto &h) -> uint16_t {
            if constexpr (requires { h.id; })
                return h.id;
            else
                throw std::logic_error("NMEA has no numeric message ID");
        },
        header);
}
StreamDecoder::StreamDecoder(std::initializer_list<Protocol> protocols) {
    for (auto protocol : protocols) {
        auto n = static_cast<unsigned>(protocol);
        if (n > 3)
            throw std::invalid_argument("Unknown protocol");
        protocols_ |= 1u << n;
    }
    if (!protocols_)
        throw std::invalid_argument("Empty protocol selection");
}
uint32_t rtcm3_crc(std::span<const uint8_t> bytes) {
    static constexpr auto table = [] {
        std::array<uint32_t, 256> result{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 16;
            for (int j = 0; j < 8; ++j)
                c = (c << 1) ^ ((c & 0x800000) ? 0x1864cfb : 0);
            result[i] = c & 0xffffff;
        }
        return result;
    }();
    uint32_t crc = 0;
    for (auto b : bytes)
        crc = ((crc << 8) & 0xffffff) ^ table[(crc >> 16) ^ b];
    return crc;
}
uint16_t sbf_crc(std::span<const uint8_t> bytes) {
    static constexpr auto table = [] {
        std::array<uint16_t, 256> values{};
        for (unsigned i = 0; i < values.size(); ++i) {
            uint16_t c = i << 8;
            for (int bit = 0; bit < 8; ++bit)
                c = (c << 1) ^ ((c & 0x8000) ? 0x1021 : 0);
            values[i] = c;
        }
        return values;
    }();
    uint16_t crc = 0;
    for (auto b : bytes)
        crc = (crc << 8) ^ table[(crc >> 8) ^ b];
    return crc;
}
void StreamDecoder::feed(std::span<const uint8_t> data,
                         const std::function<void(const FrameView &)> &emit) {
    if (failed_)
        throw std::logic_error(
            "StreamDecoder callback failed; construct a new decoder");
    bytes += data.size();
    pending_.insert(pending_.end(), data.begin(), data.end());
    size_t pos = 0;
    auto emit_frame = [&](const FrameView &frame) {
        if (!(protocols_ & (1u << static_cast<unsigned>(frame.protocol())))) {
            ++skipped_protocol_frames;
            skipped_protocol_bytes += frame.wire.size();
            return;
        }
        ++frames;
        try {
            emit(frame);
        } catch (...) {
            failed_ = true;
            throw;
        }
    };
    while (pending_.size() > pos) {
        const auto *p = pending_.data() + pos;
        if (pending_.size() - pos == 1) {
            if (*p == 0xb5 || *p == '$' || *p == '!' || *p == 0xd3)
                break;
            ++pos;
            ++noise;
            continue;
        }
        const bool ubx = p[0] == 0xb5 && p[1] == 0x62;
        const bool sbf = p[0] == 0x24 && p[1] == 0x40;
        const bool rtcm = p[0] == 0xd3;
        if (!sbf && (p[0] == '$' || p[0] == '!')) {
            // Bounded ASCII line; stop before another sync byte rather than
            // swallowing a binary frame after a damaged sentence.
            size_t n = 1;
            while (n < pending_.size() - pos && n < 4096 && p[n] >= 32 &&
                   p[n] <= 126 && p[n] != '$' && p[n] != '!')
                ++n;
            if (n == pending_.size() - pos && n < 4096)
                break;
            if (n < 4096 && p[n] == '\r' && n + 1 == pending_.size() - pos)
                break;
            bool crlf = n + 1 < pending_.size() - pos && p[n] == '\r' &&
                        p[n + 1] == '\n';
            size_t comma = 1;
            while (comma < n && p[comma] != ',' && p[comma] != '*')
                ++comma;
            bool address = comma > 1;
            for (size_t j = 1; j < comma; ++j)
                address &= (p[j] >= 'A' && p[j] <= 'Z') ||
                           (p[j] >= '0' && p[j] <= '9');
            auto hex = [](uint8_t c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                return -1;
            };
            uint8_t checksum = 0;
            if (n >= 4)
                for (size_t j = 1; j < n - 3; ++j)
                    checksum ^= p[j];
            if (!crlf || !address || n < 4 || p[n - 3] != '*' ||
                comma > n - 3 || hex(p[n - 2]) < 0 || hex(p[n - 1]) < 0 ||
                checksum != (hex(p[n - 2]) * 16 + hex(p[n - 1]))) {
                ++invalid;
                ++pos;
                continue;
            }
            const size_t start = comma < n - 3 ? comma + 1 : comma;
            std::span<const uint8_t> wire(p, n + 2);
            emit_frame(
                {NmeaHeader{
                     std::string_view(reinterpret_cast<const char *>(p + 1),
                                      comma - 1),
                     char(p[0])},
                 offset_ + pos, wire, wire.subspan(start, n - 3 - start)});
            pos += n + 2;
            continue;
        }
        if (!ubx && !sbf && !rtcm) {
            ++pos;
            ++noise;
            continue;
        }
        if (pending_.size() - pos < (rtcm ? 3u : 8u))
            break;
        const size_t length = rtcm  ? 6u + 256u * (p[1] & 3) + p[2]
                              : ubx ? 8u + p[4] + 256u * p[5]
                                    : p[6] + 256u * p[7];
        if (length < (rtcm ? 6u : 8u) || (sbf && length % 4)) {
            ++pos;
            ++invalid;
            continue;
        }
        if (pending_.size() - pos < length)
            break;
        std::span<const uint8_t> wire(p, length);
        bool valid;
        if (ubx) {
            uint8_t a = 0, b = 0;
            for (auto x : wire.subspan(2, length - 4)) {
                a += x;
                b += a;
            }
            valid = a == p[length - 2] && b == p[length - 1];
        } else if (sbf)
            valid = sbf_crc(wire.subspan(4)) == p[2] + 256u * p[3];
        else
            valid = rtcm3_crc(wire.first(length - 3)) ==
                    ((uint32_t(p[length - 3]) << 16) |
                     (uint32_t(p[length - 2]) << 8) | p[length - 1]);
        if (!valid) {
            ++invalid;
            ++pos;
            continue;
        }
        if (rtcm && length == 7) {
            ++invalid;
            pos += length;
            continue;
        }
        FrameHeader header =
            rtcm ? FrameHeader{Rtcm3Header{
                       uint16_t(length == 6 ? 0 : (p[3] << 4) | (p[4] >> 4))}}
            : ubx
                ? FrameHeader{UbxHeader{uint16_t((p[2] << 8) | p[3])}}
                : FrameHeader{SbfHeader{uint16_t((p[4] + 256u * p[5]) & 0x1fff),
                                        uint8_t(p[5] >> 5)}};
        emit_frame({header, offset_ + pos, wire,
                    wire.subspan(rtcm  ? 3
                                 : ubx ? 6
                                       : 8,
                                 length - (rtcm ? 6 : 8))});
        pos += length;
    }
    pending_.erase(pending_.begin(), pending_.begin() + pos);
    offset_ += pos;
}
void StreamDecoder::finish() const {
    if (failed_)
        throw std::logic_error(
            "StreamDecoder callback failed; construct a new decoder");
    if (!pending_.empty())
        throw std::runtime_error("Truncated protocol stream at byte " +
                                 std::to_string(offset_));
}
} // namespace cppgnss
