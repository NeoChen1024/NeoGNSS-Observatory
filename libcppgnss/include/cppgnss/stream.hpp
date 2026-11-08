// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace cppgnss {
enum class Protocol { ubx, sbf, rtcm3, nmea };
struct UbxHeader {
    uint16_t id;
};
struct SbfHeader {
    uint16_t id;
    uint8_t revision;
};
struct Rtcm3Header {
    uint16_t id;
};
struct NmeaHeader {
    // Borrowed original address, excluding '$'/'!'. Proprietary addresses
    // retain their complete spelling; subtype selection belongs to the parser.
    std::string_view address;
    char delimiter = '$';
    std::string_view talker() const {
        return address.substr(0, address.starts_with('P') ? 1 : 2);
    }
    std::string_view sentence() const {
        return address.substr(std::min(
            address.size(), address.starts_with('P') ? size_t{1} : size_t{2}));
    }
};
using FrameHeader = std::variant<UbxHeader, SbfHeader, Rtcm3Header, NmeaHeader>;
struct FrameView {
    FrameHeader header;
    uint64_t offset;
    std::span<const uint8_t> wire, payload;
    Protocol protocol() const { return static_cast<Protocol>(header.index()); }
    // Numeric IDs exist only for binary protocols. No synthetic NMEA ID.
    uint16_t id() const;
    uint8_t revision() const { return std::get<SbfHeader>(header).revision; }
};
// Framing decision for the bytes at the start of a buffer: one sync/length/
// checksum rule for every reader. `length` bytes are consumed: a whole frame,
// one noise byte (no sync), or an invalid candidate (one byte, or a whole
// RTCM3 frame of reserved length 7). An incomplete candidate needs more bytes
// and consumes none; whether more bytes can arrive is the caller's decision.
struct FrameMatch {
    enum Status : uint8_t { frame, noise, invalid, incomplete } status;
    size_t length = 0;
    FrameView view{}; // Valid only for `frame`; spans borrow the input.
};
FrameMatch match_frame(std::span<const uint8_t> bytes, uint64_t offset);
// Frame spans are borrowed until the callback returns. Chunk/file boundaries
// have no framing significance. The caller chooses when to finish a stream.
// If the callback throws, that exception propagates and the decoder is
// poisoned. Further feed()/finish() calls fail; construct a new decoder to
// restart.
class StreamDecoder {
  public:
    explicit StreamDecoder(Protocol protocol) : StreamDecoder({protocol}) {}
    explicit StreamDecoder(std::initializer_list<Protocol> protocols);
    void feed(std::span<const uint8_t>,
              const std::function<void(const FrameView &)> &);
    void finish() const;
    uint64_t pending_offset() const { return offset_; }
    size_t pending_bytes() const { return pending_.size(); }
    uint64_t bytes = 0, frames = 0, invalid = 0, noise = 0;
    // Valid frames of the other protocol are skipped atomically, not scanned
    // as noise. Applications decide how to present these diagnostics.
    uint64_t skipped_protocol_frames = 0, skipped_protocol_bytes = 0;

  private:
    uint8_t protocols_ = 0;
    std::vector<uint8_t> pending_;
    uint64_t offset_ = 0;
    bool failed_ = false;
};
uint16_t sbf_crc(std::span<const uint8_t>);
// CRC-24Q (RTCM3, SBAS and GNSS navigation messages; zero initial value) over
// MSB-first bits [begin, end) of `bytes`. A message followed by its own parity
// yields zero.
uint32_t crc24q(std::span<const uint8_t> bytes, size_t begin, size_t end);
} // namespace cppgnss
