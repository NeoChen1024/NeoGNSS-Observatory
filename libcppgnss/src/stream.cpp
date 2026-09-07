// SPDX-License-Identifier: GPL-3.0-only
#include <cppgnss/stream.hpp>
#include <stdexcept>

namespace cppgnss {
uint16_t sbf_crc(std::span<const uint8_t> bytes) {
    uint16_t crc = 0;
    for (auto b : bytes) {
        crc ^= uint16_t(b) << 8;
        for (int i = 0; i < 8; ++i)
            crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
    }
    return crc;
}
void StreamDecoder::feed(std::span<const uint8_t> data, const std::function<void(const FrameView &)> &emit) {
    bytes += data.size();
    pending_.insert(pending_.end(), data.begin(), data.end());
    size_t pos = 0;
    while (pending_.size() - pos >= 2) {
        const auto *p = pending_.data() + pos;
        const bool ubx = p[0] == 0xb5 && p[1] == 0x62;
        const bool sbf = p[0] == 0x24 && p[1] == 0x40;
        if (!ubx && !sbf) {
            ++pos;
            ++noise;
            continue;
        }
        if (pending_.size() - pos < 8)
            break;
        const size_t length = ubx ? 8u + p[4] + 256u * p[5] : p[6] + 256u * p[7];
        if (length < 8 || (!ubx && length % 4)) {
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
        } else
            valid = sbf_crc(wire.subspan(4)) == p[2] + 256u * p[3];
        if (!valid) {
            ++invalid;
            ++pos;
            continue;
        }
        const auto detected = ubx ? Protocol::ubx : Protocol::sbf;
        if (detected != protocol_) {
            ++skipped_protocol_frames;
            skipped_protocol_bytes += length;
            pos += length;
            continue;
        }
        ++frames;
        const uint16_t id = ubx ? (p[2] << 8) | p[3] : (p[4] + 256u * p[5]) & 0x1fff;
        emit({detected, offset_ + pos, id, uint8_t(ubx ? 0 : p[5] >> 5), wire,
              wire.subspan(ubx ? 6 : 8, length - 8)});
        pos += length;
    }
    pending_.erase(pending_.begin(), pending_.begin() + pos);
    offset_ += pos;
}
void StreamDecoder::finish() const {
    if (!pending_.empty())
        throw std::runtime_error("Truncated protocol stream at byte " + std::to_string(offset_));
}
} // namespace cppgnss
