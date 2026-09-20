// SPDX-License-Identifier: GPL-3.0-only
#include <cmath>
#include <cppgnss/observations.hpp>
#include <cppgnss/sbf_measurement_gen.hpp>
#include <cppgnss/ubx_rxm_gen.hpp>
#include <format>
#include <stdexcept>

namespace cppgnss {
namespace {
constexpr double c = 299792458.0;
constexpr int64_t week_ns = 604800000000000LL;
void identify(Observation &o, std::string code) {
    o.signal = std::move(code);
    o.frequency_hz = o.signal[0] == '1' ? 1575.42e6 : 1227.60e6;
}
int64_t timestamp(uint64_t week, double tow) {
    if (week == 0 || week >= 65535 || !std::isfinite(tow) || tow < 0 ||
        tow >= 604800)
        throw std::runtime_error("Invalid observation week/TOW");
    return int64_t(week) * week_ns + std::llround(tow * 1e9);
}
int signed_bits(int value, int bits) {
    return value >= (1 << (bits - 1)) ? value - (1 << bits) : value;
}
} // namespace
std::optional<ObservationEpoch> decode_gps_observations(const FrameView &f) {
    ObservationEpoch epoch;
    if (f.protocol == Protocol::ubx) {
        if (f.id != 0x0215)
            return {};
        auto parsed = cppgnss::parse<UBX::ubx_rxm_rawx>(f);
        if (!parsed)
            throw std::runtime_error("Invalid RAWX payload");
        const auto &raw = parsed.value();
        epoch.gpst_ns = timestamp(raw.week, raw.rcvTow);
        epoch.clock_reset = raw.recStat_bit & 2;
        for (const auto &m : raw.meas_grp) {
            std::string code;
            if (m.gnssId == 0) {
                if (m.sigId == 0)
                    code = "1C";
                else if (m.sigId == 3)
                    code = "2L";
                else if (m.sigId == 4)
                    code = "2S";
            }
            if (code.empty()) {
                ++epoch.unselected_signals;
                continue;
            }
            Observation o;
            o.prn = m.svId;
            identify(o, code);
            o.pseudorange_m = m.prMes;
            o.phase_cycles = m.cpMes;
            o.doppler_hz = m.doMes;
            o.cn0_dbhz = m.cno;
            o.code_valid =
                (m.trkStat_bit & 1) && std::isfinite(m.prMes) && m.prMes > 0;
            o.phase_valid = (m.trkStat_bit & 2) && std::isfinite(m.cpMes);
            o.doppler_valid = std::isfinite(m.doMes);
            o.cn0_valid = true;
            o.half_cycle = !(m.trkStat_bit & 4);
            o.sub_half_cycle = m.trkStat_bit & 8;
            o.lock_seconds = m.locktime * .001;
            o.lock_valid = true;
            epoch.signals.push_back(o);
        }
        return epoch;
    }
    // Receivers can emit MeasEpoch and Meas3 for the same epoch. Select only
    // MeasEpoch, avoiding duplicate observations; the reader reports the
    // choice.
    if (f.id != 4027)
        return {};
    auto block = cppgnss::parse<cppgnss::SBF::MeasEpoch>(f);
    if (!block)
        throw std::runtime_error("Invalid SBF MeasEpoch: " +
                                 block.error().detail);
    if (block.value().CommonFlags.Scrambling)
        throw std::runtime_error("Scrambled SBF observations are unsupported");
    epoch.gpst_ns = timestamp(block.value().WNc, block.value().TOW * .001);
    // Code smoothing is retained as receiver behavior, not undone here.
    auto make = [&](const auto &m, int prn) {
        Observation o;
        o.prn = prn;
        o.antenna = m.Type.AntennaID;
        int sig = m.Type.SigIdxLo;
        if (sig == 31)
            sig = 32 + m.ObsInfo.SigIdxHi;
        if (prn >= 1 && prn <= 37) {
            if (sig == 0)
                identify(o, "1C");
            if (sig == 1)
                identify(o, "1W");
            if (sig == 2)
                identify(o, "2W");
            if (sig == 3)
                identify(o, "2L");
            if (sig == 5)
                identify(o, "1L");
        }
        o.cn0_valid = m.CN0 != 255;
        o.cn0_dbhz = .25 * m.CN0 + ((sig == 1 || sig == 2) ? 0 : 10);
        o.half_cycle = m.ObsInfo.HalfCycleAmbiguity;
        return o;
    };
    for (const auto &m : block.value().MeasEpochChannelType1) {
        auto o = make(m, m.SVID);
        double p = (m.Misc.CodeMSB * 4294967296.0 + m.CodeLSB) * .001;
        double d = m.Doppler * .0001;
        o.pseudorange_m = p;
        o.code_valid = p > 0;
        o.doppler_hz = d;
        o.doppler_valid = m.Doppler != INT32_MIN;
        o.lock_seconds = m.LockTime;
        o.lock_valid = m.LockTime != 65535;
        o.phase_valid = o.code_valid && o.lock_valid &&
                        !(m.CarrierMSB == -128 && m.CarrierLSB == 0);
        o.phase_cycles = p * o.frequency_hz / c +
                         (m.CarrierMSB * 65536 + m.CarrierLSB) * .001;
        if (!o.signal.empty())
            epoch.signals.push_back(o);
        else
            ++epoch.unselected_signals;
        for (const auto &n : m.MeasEpochChannelType2) {
            auto a = make(n, o.prn);
            int msb = signed_bits(n.OffsetsMSB.CodeOffsetMSB, 3);
            a.code_valid = o.code_valid && !(msb == -4 && n.CodeOffsetLSB == 0);
            a.pseudorange_m = p + (msb * 65536 + n.CodeOffsetLSB) * .001;
            a.lock_seconds = n.LockTime;
            a.lock_valid = n.LockTime != 255;
            a.phase_valid =
                a.code_valid && !(n.CarrierMSB == -128 && n.CarrierLSB == 0);
            a.phase_cycles = a.pseudorange_m * a.frequency_hz / c +
                             (n.CarrierMSB * 65536 + n.CarrierLSB) * .001;
            msb = signed_bits(n.OffsetsMSB.DopplerOffsetMSB, 5);
            a.doppler_valid = o.doppler_valid && o.frequency_hz > 0 &&
                              !(msb == -16 && n.DopplerOffsetLSB == 0);
            if (a.doppler_valid)
                a.doppler_hz = d * a.frequency_hz / o.frequency_hz +
                               (msb * 65536 + n.DopplerOffsetLSB) * .0001;
            if (!a.signal.empty())
                epoch.signals.push_back(a);
            else
                ++epoch.unselected_signals;
        }
    }
    return epoch;
}
} // namespace cppgnss
