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
