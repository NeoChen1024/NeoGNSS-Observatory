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
    // RINEX satellite number within the gnssId's system: svId for GPS,
    // Galileo and BeiDou (nonzero) and QZSS (1-10); SBAS PRN 120-158 as
    // S20-58. GLONASS (slot/frequency identifiers) and NavIC have none here.
    std::optional<uint16_t> satellite() const;
};

struct NavigationSubframe {
    SignalKey signal;
    uint8_t chn = 0;
    std::vector<uint32_t> words;
    // No timestamp: RXM-SFRBX does not provide one. Applications may attach
    // reception-context timestamps with explicitly recorded provenance.
};
// RXM-SFRBX version 2 with 1..16 data words; other frames have none.
std::optional<NavigationSubframe>
parse_subframe(const cppgnss::FrameView &frame);
} // namespace UBX
