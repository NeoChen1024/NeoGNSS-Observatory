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
// RINEX satellite identity of an SBF SVID (SBF Reference Guide numbering):
// G1-37, E1-36 (71-106), S20-40 (120-140) and S41-58 (198-215), C1-40
// (141-180) and C41-63 (223-245), J1-10 (181-190). GLONASS, NavIC and L-band
// SVIDs have no identity here.
struct Satellite {
    char system;
    uint16_t number;
};
constexpr std::optional<Satellite> satellite(unsigned svid) {
    if (svid >= 1 && svid <= 37)
        return Satellite{'G', uint16_t(svid)};
    if (svid >= 71 && svid <= 106)
        return Satellite{'E', uint16_t(svid - 70)};
    if (svid >= 120 && svid <= 140)
        return Satellite{'S', uint16_t(svid - 100)};
    if (svid >= 198 && svid <= 215)
        return Satellite{'S', uint16_t(svid - 157)};
    if (svid >= 141 && svid <= 180)
        return Satellite{'C', uint16_t(svid - 140)};
    if (svid >= 223 && svid <= 245)
        return Satellite{'C', uint16_t(svid - 182)};
    if (svid >= 181 && svid <= 190)
        return Satellite{'J', uint16_t(svid - 180)};
    return std::nullopt;
}
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
