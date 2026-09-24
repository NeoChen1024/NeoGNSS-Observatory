// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace cppgnss {
enum class Protocol { ubx, sbf, rtcm3 };
struct FrameView {
    Protocol protocol;
    uint64_t offset;
    uint16_t id; // RTCM3 zero-payload link fillers have ID 0.
    uint8_t revision;
    std::span<const uint8_t> wire, payload;
};
// Frame spans are borrowed until the callback returns. Chunk/file boundaries
// have no framing significance. The caller chooses when to finish a stream.
// If the callback throws, that exception propagates and the decoder is
// poisoned. Further feed()/finish() calls fail; construct a new decoder to
// restart.
class StreamDecoder {
  public:
    explicit StreamDecoder(Protocol protocol) : protocol_(protocol) {}
    void feed(std::span<const uint8_t>,
              const std::function<void(const FrameView &)> &);
    void finish() const;
    uint64_t pending_offset() const { return offset_; }
    size_t pending_bytes() const { return pending_.size(); }
    uint64_t bytes = 0, frames = 0, invalid = 0, noise = 0;
    // Valid frames of other protocols are skipped atomically, not scanned
    // as noise. Applications decide how to present these diagnostics.
    uint64_t skipped_protocol_frames = 0, skipped_protocol_bytes = 0;

  private:
    Protocol protocol_;
    std::vector<uint8_t> pending_;
    uint64_t offset_ = 0;
    bool failed_ = false;
};
uint16_t sbf_crc(std::span<const uint8_t>);
// CRC-24Q, covering the three-byte RTCM3 header and its payload.
uint32_t rtcm3_crc(std::span<const uint8_t>);
} // namespace cppgnss
