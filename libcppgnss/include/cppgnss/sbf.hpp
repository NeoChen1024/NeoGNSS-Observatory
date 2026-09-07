// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppgnss/sbas.hpp>
#include <cppgnss/stream.hpp>
#include <map>
#include <string>
#include <variant>

namespace cppgnss::SBF {
struct WideInteger {
    std::vector<uint8_t> little_endian;
    bool is_signed;
};
using Value = std::variant<uint64_t, int64_t, double, std::vector<uint8_t>, WideInteger>;
using Fields = std::map<std::string, Value>;
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
const std::vector<Schema> &schemas(); // Generated from every pinned SBF_BLOCKS entry.
enum class Status { decoded, unknown_block, unsupported_schema, invalid_payload };
struct Block {
    uint16_t id;
    uint8_t revision;
    std::string name;
    Status status;
    Fields fields;
    std::vector<uint8_t> payload, trailing;
    std::string error;
    std::optional<uint64_t> offset; // Set by a stream adapter, absent for payload-only decoding.
};
// CRC/framing validation belongs to StreamDecoder. Preserve the raw payload,
// revision and undecoded tail even when schema decoding fails.
Block decode(uint16_t id, uint8_t revision, std::span<const uint8_t> payload);
struct SbasFrame {
    uint32_t tow_ms;
    uint16_t week, prn;
    uint8_t svid, signal_index, frequency_number, receiver_channel, viterbi_count;
    bool receiver_crc_passed;
    cppgnss::SBAS::Result decoded;
};
// GEORawL1 only. GEORawL5 is deliberately not interpreted as an L1 message.
std::optional<SbasFrame> extract_sbas_l1(const Block &);
} // namespace cppgnss::SBF
