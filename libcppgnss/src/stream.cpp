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
uint32_t crc24q(std::span<const uint8_t> bytes, size_t begin, size_t end) {
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
    if (begin > end || end > bytes.size() * 8)
        throw std::out_of_range("CRC-24Q range outside buffer");
    const unsigned shift = begin % 8;
    uint32_t crc = 0;
    size_t i = begin;
    for (; i + 8 <= end; i += 8) {
        const size_t k = i / 8;
        const uint8_t b =
            shift ? uint8_t(bytes[k] << shift | bytes[k + 1] >> (8 - shift))
                  : bytes[k];
        crc = ((crc << 8) & 0xffffff) ^ table[(crc >> 16) ^ b];
    }
    for (; i < end; ++i) {
        const bool top = ((crc >> 23) ^ (bytes[i / 8] >> (7 - i % 8))) & 1;
        crc = (crc << 1) & 0xffffff;
        if (top)
            crc ^= 0x864cfb;
    }
    return crc;
}
uint16_t sbf_crc(std::span<const uint8_t> bytes) {
    // CRC-16/CCITT-FALSE with slicing-by-8: table[k][x] is the CRC of byte x
    // followed by k zero bytes, so eight input bytes combine with one XOR each.
    static constexpr auto table = [] {
        std::array<std::array<uint16_t, 256>, 8> values{};
        for (unsigned i = 0; i < 256; ++i) {
            uint16_t c = i << 8;
            for (int bit = 0; bit < 8; ++bit)
                c = (c << 1) ^ ((c & 0x8000) ? 0x1021 : 0);
            values[0][i] = c;
        }
        for (unsigned k = 1; k < 8; ++k)
            for (unsigned i = 0; i < 256; ++i)
                values[k][i] =
                    (values[k - 1][i] << 8) ^ values[0][values[k - 1][i] >> 8];
        return values;
    }();
    uint16_t crc = 0;
    const uint8_t *p = bytes.data();
    size_t n = bytes.size();
    for (; n >= 8; n -= 8, p += 8)
        crc = table[7][p[0] ^ (crc >> 8)] ^ table[6][p[1] ^ (crc & 0xff)] ^
              table[5][p[2]] ^ table[4][p[3]] ^ table[3][p[4]] ^
              table[2][p[5]] ^ table[1][p[6]] ^ table[0][p[7]];
    for (; n; --n, ++p)
        crc = (crc << 8) ^ table[0][(crc >> 8) ^ *p];
    return crc;
}
FrameMatch match_frame(std::span<const uint8_t> bytes, uint64_t offset) {
    using enum FrameMatch::Status;
    const auto *p = bytes.data();
    const size_t available = bytes.size();
    if (available < 2) {
        const bool sync =
            !available || *p == 0xb5 || *p == '$' || *p == '!' || *p == 0xd3;
        return {sync ? incomplete : noise, sync ? 0u : 1u};
    }
    const bool ubx = p[0] == 0xb5 && p[1] == 0x62;
    const bool sbf = p[0] == 0x24 && p[1] == 0x40;
    const bool rtcm = p[0] == 0xd3;
    if (!sbf && (p[0] == '$' || p[0] == '!')) {
        // Bounded ASCII line; stop before another sync byte rather than
        // swallowing a binary frame after a damaged sentence.
        size_t n = 1;
        while (n < available && n < 4096 && p[n] >= 32 && p[n] <= 126 &&
               p[n] != '$' && p[n] != '!')
            ++n;
        if (n == available && n < 4096)
            return {incomplete};
        if (n < 4096 && p[n] == '\r' && n + 1 == available)
            return {incomplete};
        bool crlf = n + 1 < available && p[n] == '\r' && p[n + 1] == '\n';
        size_t comma = 1;
        while (comma < n && p[comma] != ',' && p[comma] != '*')
            ++comma;
        bool address = comma > 1;
        for (size_t j = 1; j < comma; ++j)
            address &=
                (p[j] >= 'A' && p[j] <= 'Z') || (p[j] >= '0' && p[j] <= '9');
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
        if (!crlf || !address || n < 4 || p[n - 3] != '*' || comma > n - 3 ||
            hex(p[n - 2]) < 0 || hex(p[n - 1]) < 0 ||
            checksum != (hex(p[n - 2]) * 16 + hex(p[n - 1])))
            return {invalid, 1};
        const size_t start = comma < n - 3 ? comma + 1 : comma;
        const auto wire = bytes.first(n + 2);
        return {
            frame,
            n + 2,
            {NmeaHeader{std::string_view(reinterpret_cast<const char *>(p + 1),
                                         comma - 1),
                        char(p[0])},
             offset, wire, wire.subspan(start, n - 3 - start)}};
    }
    if (!ubx && !sbf && !rtcm)
        return {noise, 1};
    if (available < (rtcm ? 3u : 8u))
        return {incomplete};
    const size_t length = rtcm  ? 6u + 256u * (p[1] & 3) + p[2]
                          : ubx ? 8u + p[4] + 256u * p[5]
                                : p[6] + 256u * p[7];
    if (length < (rtcm ? 6u : 8u) || (sbf && length % 4))
        return {invalid, 1};
    if (available < length)
        return {incomplete};
    const auto wire = bytes.first(length);
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
        valid = crc24q(wire, 0, 8 * length) == 0;
    if (!valid)
        return {invalid, 1};
    if (rtcm && length == 7)
        return {invalid, length};
    FrameHeader header =
        rtcm  ? FrameHeader{Rtcm3Header{
                    uint16_t(length == 6 ? 0 : (p[3] << 4) | (p[4] >> 4))}}
        : ubx ? FrameHeader{UbxHeader{uint16_t((p[2] << 8) | p[3])}}
              : FrameHeader{SbfHeader{uint16_t((p[4] + 256u * p[5]) & 0x1fff),
                                      uint8_t(p[5] >> 5)}};
    return {frame,
            length,
            {header, offset, wire,
             wire.subspan(rtcm  ? 3
                          : ubx ? 6
                                : 8,
                          length - (rtcm ? 6 : 8))}};
}
void StreamDecoder::feed(std::span<const uint8_t> data,
                         const std::function<void(const FrameView &)> &emit) {
    if (failed_)
        throw std::logic_error(
            "StreamDecoder callback failed; construct a new decoder");
    bytes += data.size();
    pending_.insert(pending_.end(), data.begin(), data.end());
    size_t pos = 0;
    while (pos < pending_.size()) {
        const auto m =
            match_frame(std::span(pending_).subspan(pos), offset_ + pos);
        if (m.status == FrameMatch::incomplete)
            break;
        pos += m.length;
        if (m.status == FrameMatch::noise) {
            ++noise;
            continue;
        }
        if (m.status == FrameMatch::invalid) {
            ++invalid;
            continue;
        }
        if (!(protocols_ & (1u << static_cast<unsigned>(m.view.protocol())))) {
            ++skipped_protocol_frames;
            skipped_protocol_bytes += m.length;
            continue;
        }
        ++frames;
        try {
            emit(m.view);
        } catch (...) {
            failed_ = true;
            throw;
        }
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
