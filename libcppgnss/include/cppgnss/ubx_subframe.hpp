// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <compare>
#include <cppgnss/stream.hpp>
#include <cppgnss/ubx_def.hpp>
#include <optional>
#include <vector>

namespace UBX {
// These are wire identifiers, not NMEA/RINEX satellite numbers. Unknown GNSS
// and signal values remain distinct rather than being mapped into GPS.
struct SignalKey {
    uint8_t gnssId = 0, svId = 0, sigId = 0, freqId = 0;
    auto operator<=>(const SignalKey &) const = default;
    // GLONASS uses slot/frequency identifiers, so has no PRN conversion here.
    std::optional<uint16_t> prn() const;
};

struct NavigationSubframe {
    SignalKey signal;
    uint8_t version = 0, chn = 0, raw_freqId = 0, reserved0 = 0;
    std::vector<uint32_t> words;
    // No timestamp: RXM-SFRBX does not provide one. Applications may attach
    // reception-context timestamps with explicitly recorded provenance.
};
enum class SubframeStatus {
    decoded,
    not_sfrbx,
    invalid_frame,
    invalid_length,
    unsupported_version
};
struct SubframeResult {
    SubframeStatus status;
    std::optional<NavigationSubframe> subframe;
};
SubframeResult parse_subframe(const cppgnss::FrameView &frame);
} // namespace UBX
