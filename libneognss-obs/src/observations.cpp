// SPDX-License-Identifier: GPL-3.0-only
#include <cmath>
#include <neognss_obs/measurements.hpp>
#include <neognss_obs/observations.hpp>
#include <stdexcept>

namespace neognss_obs {
std::optional<ObservationEpoch>
decode_gps_observations(const cppgnss::FrameView &frame) {
    auto decoded = decode_measurements(frame);
    if (!decoded)
        return {};
    const auto &raw = *decoded;
    ObservationEpoch epoch;
    // The numerical engine keeps a nanosecond interface over the exact time.
    epoch.gpst_ns = nearest_ns(measurement_time(raw));
    epoch.clock_reset = raw.adjustment_reported.value_or(false);
    epoch.unselected_signals = raw.unsupported + raw.excluded;
    for (const auto &m : raw.rows) {
        if (m.system != "G" ||
            (m.signal != "1C" && m.signal != "1W" && m.signal != "1L" &&
             m.signal != "2W" && m.signal != "2L" && m.signal != "2S")) {
            ++epoch.unselected_signals;
            continue;
        }
        Observation o;
        o.prn = m.satellite;
        o.antenna = m.antenna;
        o.signal = m.signal;
        o.frequency_hz = m.signal[0] == '1' ? 1575.42e6 : 1227.60e6;
        o.pseudorange_m = m.code;
        o.phase_cycles = m.phase;
        o.doppler_hz = m.doppler;
        o.cn0_dbhz = m.cn0;
        o.code_valid =
            m.code_valid.value_or(true) && std::isfinite(m.code) && m.code > 0;
        o.phase_valid = m.phase_valid.value_or(true) && std::isfinite(m.phase);
        o.doppler_valid = std::isfinite(m.doppler);
        o.cn0_valid = std::isfinite(m.cn0);
        o.half_cycle = m.half_ambiguity;
        o.sub_half_cycle = m.half_subtracted.value_or(false);
        o.lock_valid = m.lock_ms.has_value();
        o.lock_seconds = m.lock_ms.value_or(0) * .001;
        epoch.signals.push_back(std::move(o));
    }
    return epoch;
}
} // namespace neognss_obs
