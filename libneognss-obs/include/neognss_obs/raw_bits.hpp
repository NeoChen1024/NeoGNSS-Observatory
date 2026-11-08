// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppgnss/stream.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace neognss_obs {
// PPP-B2b ICD 1.0 table 5-1 assigns exactly these ten ranging codes.
// Assignment is a necessary condition, not proof of service or data validity.
inline constexpr bool bds_ppp_b2b_code_assigned(int64_t prn) {
    return (prn >= 1 && prn <= 5) || (prn >= 59 && prn <= 63);
}
// Canonical layout of one message family (docs/commonnex/raw-bits-formats.md):
// the RINEX system of its broadcasters, body format, meaningful bit length and
// record unit. The producer assigns and every RawBits consumer checks it.
struct RawBitsLayout {
    std::string_view family, system, format;
    uint32_t bit_length;
    std::string_view unit = "message";
    constexpr std::string_view content() const {
        return bit_length >= 576 ? "binary_symbols" : "navigation_bits";
    }
};
inline constexpr RawBitsLayout raw_bits_layouts[] = {
    {"GPS_LNAV", "G", "LNAV_300_V1", 300, "subframe"},
    {"QZS_LNAV", "J", "LNAV_300_V1", 300, "subframe"},
    {"GPS_CNAV", "G", "CNAV_300_V1", 300},
    {"QZS_CNAV", "J", "CNAV_300_V1", 300},
    {"GAL_INAV", "E", "INAV_228_V1", 228, "page"},
    {"GAL_FNAV", "E", "FNAV_238_V1", 238, "page"},
    {"GAL_CNAV", "E", "CNAV_PAGE_486_V1", 486, "page"},
    {"BDS_D1", "C", "D1D2_300_V1", 300, "subframe"},
    {"BDS_D2", "C", "D1D2_300_V1", 300, "subframe"},
    {"BDS_D1D2_UNCLASSIFIED", "C", "D1D2_300_V1", 300, "subframe"},
    {"BDS_BCNAV1", "C", "BCNAV1_1800_V1", 1800},
    {"BDS_BCNAV2", "C", "BCNAV2_576_V1", 576},
    {"BDS_BCNAV3", "C", "B2B_984_V1", 984},
    {"BDS_PPP_B2B", "C", "B2B_984_V1", 984},
    {"BDS_B2B_UNCLASSIFIED", "C", "B2B_984_V1", 984},
    {"GPS_CNAV2", "G", "CNAV2_1800_V1", 1800},
    {"QZS_CNAV2", "J", "CNAV2_1800_V1", 1800},
    {"QZS_L6_UNCLASSIFIED", "J", "L6_2000_V1", 2000},
    {"SBAS_L1", "S", "SBAS_L1_250_V1", 250},
    {"SBAS_L5", "S", "SBAS_L5_250_V1", 250},
    {"QZS_L1S", "J", "QZS_L1S_250_V1", 250},
    {"QZS_L5S", "J", "QZS_L5S_250_V1", 250},
};
// The registered layout of FAMILY, or null for an unknown family.
constexpr const RawBitsLayout *raw_bits_layout(std::string_view family) {
    for (const auto &layout : raw_bits_layouts)
        if (layout.family == family)
            return &layout;
    return nullptr;
}
// Enumerated names are views of string literals with static storage; the
// decoder never builds them at run time.
struct RawBitsCheck {
    std::string_view origin, kind, scope, result, evidence, source_field;
};
struct RawBits {
    // RINEX G/E/C/J/S satellite identity; family/format names are independent.
    std::string_view system, family, format, unit = "message",
                                             content = "navigation_bits";
    uint16_t satellite = 0;
    std::vector<std::string_view> signals;
    uint32_t bit_length = 0;
    std::vector<uint8_t> body;
    std::vector<RawBitsCheck> checks;
    std::optional<uint16_t> receiver_channel;
    std::optional<uint8_t> viterbi_count, rs_corrected_symbols;
    std::optional<int64_t> gpst_ms; // SBF SIS context; UBX has no own time.
};
enum class RawBitsStatus {
    unrelated,
    excluded,
    unsupported,
    malformed,
    decoded
};
struct RawBitsResult {
    RawBitsStatus status = RawBitsStatus::unrelated;
    std::optional<RawBits> record;
};
// Decode validated wire frames without temporal association or terminal output.
// Failed navigation checks remain records. No error correction is applied.
RawBitsResult decode_raw_bits(const cppgnss::FrameView &frame);
} // namespace neognss_obs
