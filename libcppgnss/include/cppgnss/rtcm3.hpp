// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppgnss/parse.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace cppgnss::RTCM3 {
struct Cell {
    // One-based IDs from the MSM masks. These are not normalized PRNs.
    uint8_t satellite_id = 0;
    uint8_t signal_id = 0;
    // Two-character RINEX signal code; empty for unassigned or tentative IDs.
    std::string_view observation_code;
    std::optional<double> pseudorange_m;
    std::optional<double> phase_range_m;
    std::optional<double> phase_range_rate_m_s;
    std::optional<double> cn0_dbhz;
    uint16_t lock_indicator = 0;
    uint8_t lock_bits = 0;
    bool half_cycle_ambiguity = false;
    std::optional<uint8_t> extended_satellite_info;
};
struct MsmHeader {
    uint16_t message_id = 0;
    uint16_t station_id = 0;
    char system = '\0'; // G, R, E, S, J, C or I.
    // Integer milliseconds of the source week: BDT for BeiDou, GPST-aligned
    // system time for G/E/S/J. For GLONASS this remains the packed 3-bit day /
    // 27-bit time of day. A full week is not present in an MSM frame.
    uint32_t epoch_ms = 0;
    bool multiple_message = false;
    uint8_t issue_of_data_station = 0;
    uint8_t clock_steering = 0;
    uint8_t external_clock = 0;
    bool divergence_free_smoothing = false;
    uint8_t smoothing_interval = 0;
};
struct ObservationMessage : MsmHeader {
    std::vector<Cell> cells;
};

// Includes unsupported legacy observation and MSM1-3/GLONASS/NavIC messages,
// allowing callers to distinguish skipped observations from other RTCM data.
bool is_observation_message(uint16_t message_id);

// Common MSM1-7 framing metadata, including excluded constellations. Validates
// the complete header and masks, but does not decode or validate satellite or
// signal data. Useful for tracking message-sequence boundaries when skipping
// unsupported observations; it does not make them scientifically supported.
ParseResult<MsmHeader> parse_msm_header(const FrameView &frame);

// Stateless MSM4-7 decoding for GPS, Galileo, SBAS, QZSS and BeiDou. All cells
// are retained, including signals without a reliable RINEX identity. Invalid
// observable sentinels become null; an absent rate is never invented as zero.
// Protocol/framing CRC validation is supplied by StreamDecoder. No ephemeris,
// full-week reconstruction, cycle-slip inference or carrier conversion occurs.
ParseResult<ObservationMessage> parse_observation(const FrameView &frame);
} // namespace cppgnss::RTCM3
