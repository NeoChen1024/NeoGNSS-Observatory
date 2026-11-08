// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cmath>
#include <cppgnss/stream.hpp>
#include <neognss_obs/gpst.hpp>
#include <optional>
#include <string>

namespace neognss_obs {
// Carrier frequency of a RINEX system and band digit; 0 for an unknown band.
constexpr double carrier_hz(char system, char band) {
    switch (band) {
    case '1':
        return 1575.42e6;
    case '2':
        return system == 'C' ? 1561.098e6 : 1227.60e6;
    case '5':
        return 1176.45e6;
    case '6':
        return system == 'C' ? 1268.52e6 : 1278.75e6;
    case '7':
        return 1207.14e6;
    case '8':
        return 1191.795e6;
    }
    return 0;
}
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
    std::optional<bool> code_valid, phase_valid;
    bool half_ambiguity = false;
    std::optional<bool> half_subtracted;
    std::optional<uint32_t> lock_ms;
    bool lock_lower_bound = false;
    bool has_extra = false;
    std::optional<bool> code_smoothing_applied;
    double code_multipath_m = NAN, code_smoothing_m = NAN,
           phase_multipath_cycles = NAN, cn0_increment = 0;
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
// Exact GPST picoseconds of a decoded measurement epoch: integer TOW ms, or the
// binary64 RAWX rcvTow rounded half to even. Week 0 (a receiver without time),
// 65535 (do not use) or TOW outside the week have no GPST: nullopt.
std::optional<GpstTicks> measurement_gpst(const Measurements &);
// measurement_gpst for callers that require a time; throws std::runtime_error.
GpstTicks measurement_time(const Measurements &);
// MeasExtra rows carry epoch-local channel/signal keys, not satellite identity.
std::optional<Measurements>
decode_measurement_extras(const cppgnss::FrameView &);
} // namespace neognss_obs
