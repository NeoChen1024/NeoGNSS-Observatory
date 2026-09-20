// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppgnss/sbas.hpp>
#include <cppgnss/stream.hpp>
#include <cstdio>
#include <string>

namespace cppgnss::SBF {
struct WideInteger {
    std::vector<uint8_t> little_endian;
    bool is_signed;
};
enum class Kind { scalar, bits, repeat, optional, padding };
struct Field {
    std::string name;
    Kind kind;
    char type = 'U';
    size_t width = 0;
    double scale = 1;
    std::string reference;
    size_t count = 0;
    std::vector<int64_t> condition;
    std::vector<Field> children;
};
struct Schema {
    uint16_t id;
    std::string name;
    std::vector<Field> fields;
};
const std::vector<Schema> &
schemas(); // Generated from every pinned SBF_BLOCKS entry.
enum class Status {
    decoded,
    unknown_block,
    unsupported_schema,
    invalid_payload
};
struct BlockInfo {
    uint16_t id;
    uint8_t revision;
    std::string_view name;
    Status status;
    size_t consumed = 0;
    std::string error;
    // Native block header time, not necessarily a receiver-navigation anchor.
    std::optional<uint32_t> tow_ms;
    std::optional<uint16_t> week;
};
// Validate the generated layout and inspect header time without a dynamic map.
// CRC/framing validation belongs to StreamDecoder; payload ownership stays with
// caller.
BlockInfo inspect(uint16_t id, uint8_t revision,
                  std::span<const uint8_t> payload);
struct SbasFrame {
    uint32_t tow_ms;
    uint16_t week, prn;
    uint8_t svid, signal_index, frequency_number, receiver_channel,
        viterbi_count;
    bool receiver_crc_passed;
    cppgnss::SBAS::Result decoded;
};
// GEORawL1 only. GEORawL5 is deliberately not interpreted as an L1 message.
std::optional<SbasFrame> extract_sbas_l1(const FrameView &);
} // namespace cppgnss::SBF
