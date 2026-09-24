// SPDX-License-Identifier: GPL-3.0-only
#include <array>
#include <cppgnss/stream.hpp>
#include <stdexcept>
#include <string>

namespace cppgnss {
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
uint32_t rtcm3_crc(std::span<const uint8_t> bytes) {
    static constexpr auto table = [] {
        std::array<uint32_t, 256> values{};
        for (unsigned i = 0; i < values.size(); ++i) {
            uint32_t c = i << 16;
            for (int bit = 0; bit < 8; ++bit)
                c = (c << 1) ^ ((c & 0x800000) ? 0x1864cfb : 0);
            values[i] = c & 0xffffff;
        }
        return values;
    }();
    uint32_t crc = 0;
    for (auto b : bytes)
        crc = ((crc << 8) & 0xffffff) ^ table[(crc >> 16) ^ b];
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
    while (pos < pending_.size()) {
        const auto *p = pending_.data() + pos;
        if (pending_.size() - pos == 1) {
            if (p[0] == 0xb5 || p[0] == 0x24 || p[0] == 0xd3)
                break;
            ++pos;
            ++noise;
            continue;
        }
        const bool ubx = p[0] == 0xb5 && p[1] == 0x62;
        const bool sbf = p[0] == 0x24 && p[1] == 0x40;
        const bool rtcm = p[0] == 0xd3;
        if (!ubx && !sbf && !rtcm) {
            ++pos;
            ++noise;
            continue;
        }
        if (pending_.size() - pos < (rtcm ? 3u : 8u))
            break;
        // RTCM3 receivers ignore the six reserved header bits. A payload
        // length of zero is a legal link-filler message.
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
                    (uint32_t(p[length - 3]) << 16 |
                     uint32_t(p[length - 2]) << 8 | p[length - 1]);
        if (!valid) {
            ++invalid;
            ++pos;
            continue;
        }
        if (rtcm && length == 7) {
            // A nonempty RTCM message requires its twelve-bit message ID.
            // Even a malformed one-byte payload is skipped atomically once
            // its CRC has established the frame boundary.
            ++invalid;
            pos += length;
            continue;
        }
        const auto detected = rtcm  ? Protocol::rtcm3
                              : ubx ? Protocol::ubx
                                    : Protocol::sbf;
        if (detected != protocol_) {
            ++skipped_protocol_frames;
            skipped_protocol_bytes += length;
            pos += length;
            continue;
        }
        ++frames;
        const uint16_t id = rtcm ? (length == 6 ? 0 : (p[3] << 4) | (p[4] >> 4))
                            : ubx ? (p[2] << 8) | p[3]
                                  : (p[4] + 256u * p[5]) & 0x1fff;
        try {
            emit({detected, offset_ + pos, id, uint8_t(sbf ? p[5] >> 5 : 0),
                  wire,
                  wire.subspan(rtcm  ? 3
                               : ubx ? 6
                                     : 8,
                               length - (rtcm ? 6 : 8))});
        } catch (...) {
            failed_ = true;
            throw;
        }
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
