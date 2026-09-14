// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppgnss/stream.hpp>
#include <optional>
#include <string>
#include <vector>

namespace cppgnss {
struct RawBitsCheck {
    std::string origin, kind, scope, result, evidence, source_field;
};
struct RawBits {
    // RINEX G/E/C/J/S satellite identity; family/format names are independent.
    std::string system, family, format, unit = "message",
                                        content = "navigation_bits";
    uint16_t satellite = 0;
    std::vector<std::string> signals;
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
RawBitsResult decode_raw_bits(const FrameView &frame);
} // namespace cppgnss
