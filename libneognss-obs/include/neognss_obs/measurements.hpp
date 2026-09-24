// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cmath>
#include <cppgnss/rtcm3.hpp>
#include <cppgnss/stream.hpp>
#include <optional>
#include <string>

namespace neognss_obs {
// cppgnss::Protocol quantities retain their native time representation at this
// boundary.
struct Measurement {
    std::string system, signal;
    uint16_t satellite = 0;
    uint8_t antenna = 0;
    uint8_t receiver_channel = 0, native_signal = 0;
    double code = NAN, phase = NAN, doppler = NAN, cn0 = NAN;
    float code_sigma = NAN, phase_sigma = NAN, doppler_sigma = NAN;
    std::optional<bool> code_sigma_lower_bound, phase_sigma_lower_bound,
        doppler_sigma_lower_bound;
    int code_status = 2, phase_status = 2; // valid, invalid, unknown
    bool half_ambiguity = false;
    std::optional<bool> half_subtracted;
    std::optional<uint32_t> lock_ms;
    std::optional<uint32_t> lock_upper_ms;
    bool lock_lower_bound = false;
    bool has_extra = false;
    std::optional<bool> code_smoothing_applied;
    double code_multipath_m = NAN, code_smoothing_m = NAN,
           phase_multipath_cycles = NAN, cn0_increment = 0;
    float doppler_variance_factor = NAN;
    std::optional<uint8_t> continuity_counter;
};
struct Measurements {
    uint16_t week = 0;
    double tow_seconds = 0;
    std::optional<uint32_t> tow_ms;
    std::optional<bool> adjustment_reported;
    std::optional<uint8_t> cumulative_adjustment_ms_mod256;
    std::vector<Measurement> rows;
    uint64_t unsupported = 0;
    uint64_t excluded = 0;
};
// Generated typed decoding followed by physical normalization: RAWX v1 and
// MeasEpoch revisions 0/1. Completion policy belongs to the caller.
std::optional<Measurements> decode_measurements(const cppgnss::FrameView &);
// MeasExtra rows carry epoch-local channel/signal keys, not satellite identity.
std::optional<Measurements>
decode_measurement_extras(const cppgnss::FrameView &);
// RTCM MSM quantities normalized to RINEX signal units at an explicitly
// resolved absolute GPST millisecond timestamp. No receiver telemetry fields.
Measurements
normalize_rtcm_observations(const cppgnss::RTCM3::ObservationMessage &,
                            int64_t gpst_ms);
} // namespace neognss_obs
