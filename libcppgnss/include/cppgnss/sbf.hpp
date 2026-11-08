// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppgnss/stream.hpp>
#include <cstdio>
#include <optional>
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
// Block-header TOW (ms) and WNc as GPST milliseconds. Do-not-use values
// (TOW 4294967295, WNc 65535) and TOW outside the week have no time.
constexpr std::optional<int64_t> gpst_ms(uint32_t tow_ms, uint16_t week) {
    if (tow_ms >= 604800000 || week == 65535)
        return std::nullopt;
    return int64_t(week) * 604800000 + tow_ms;
}
// gpst_ms of the TOW/WNc header fields that begin every SBF block payload.
inline std::optional<int64_t> header_gpst_ms(std::span<const uint8_t> payload) {
    if (payload.size() < 6)
        return std::nullopt;
    return gpst_ms(uint32_t(payload[0]) | uint32_t(payload[1]) << 8 |
                       uint32_t(payload[2]) << 16 | uint32_t(payload[3]) << 24,
                   uint16_t(payload[4] | payload[5] << 8));
}
} // namespace cppgnss::SBF
