// SPDX-License-Identifier: GPL-3.0-only
#include <cppgnss/sbf_measurement_gen.hpp>
#include <cppgnss/ubx_rxm_gen.hpp>
#include <map>
#include <neognss_obs/measurements.hpp>
#include <stdexcept>

namespace neognss_obs {
namespace {
double freq(const std::string &sys, const std::string &sig) {
    switch (sig[0]) {
    case '1':
        return 1575.42e6;
    case '2':
        return sys == "C" ? 1561.098e6 : 1227.60e6;
    case '5':
        return 1176.45e6;
    case '6':
        return sys == "C" ? 1268.52e6 : 1278.75e6;
    case '7':
        return 1207.14e6;
    case '8':
        return 1191.795e6;
    }
    return 0;
}
// RINEX identities, not merged pilot/data observables. See
// receiver-profiles.md.
const std::map<int, std::string> ubx_codes{
    {0, "G1C"},   {3, "G2L"},   {4, "G2S"},   {6, "G5I"},   {7, "G5Q"},
    {100, "S1C"}, {200, "E1C"}, {201, "E1B"}, {203, "E5I"}, {204, "E5Q"},
    {205, "E7I"}, {206, "E7Q"}, {300, "C2I"}, {301, "C2I"}, {302, "C7I"},
    {303, "C7I"}, {304, "C6I"}, {305, "C1P"}, {306, "C1D"}, {307, "C5P"},
    {308, "C5D"}, {310, "C6I"}, {500, "J1C"}, {501, "J1Z"}, {504, "J2S"},
    {505, "J2L"}, {508, "J5I"}, {509, "J5Q"}};
const std::map<int, std::string> sbf_codes{
    {0, "G1C"},  {1, "G1W"},  {2, "G2W"},  {3, "G2L"},  {4, "G5Q"},
    {5, "G1L"},  {6, "J1C"},  {7, "J2L"},  {13, "C1P"}, {14, "C5P"},
    {17, "E1C"}, {19, "E6C"}, {20, "E5Q"}, {21, "E7Q"}, {22, "E8Q"},
    {24, "S1C"}, {25, "S5I"}, {26, "J5Q"}, {27, "J6L"}, {28, "C2I"},
    {29, "C7I"}, {30, "C6I"}, {32, "J1L"}, {33, "J1Z"}, {34, "C7D"}};
bool identify(Measurement &m, const std::map<int, std::string> &codes, int sig,
              int sv, bool ubx) {
    auto it = codes.find(sig);
    if (it == codes.end())
        return false;
    m.system = it->second.substr(0, 1);
    m.signal = it->second.substr(1);
    if (ubx) {
        m.satellite = m.system == "S" ? sv - 100 : sv;
        return sv > 0 && (m.system != "S" || (sv >= 120 && sv <= 158));
    }
    int prn = 0;
    if (m.system == "G" && sv >= 1 && sv <= 37)
        prn = sv;
    if (m.system == "E" && sv >= 71 && sv <= 106)
        prn = sv - 70;
    if (m.system == "C" && sv >= 141 && sv <= 180)
        prn = sv - 140;
    if (m.system == "C" && sv >= 223 && sv <= 245)
        prn = sv - 182;
    if (m.system == "J" && sv >= 181 && sv <= 190)
        prn = sv - 180;
    if (m.system == "S" && sv >= 120 && sv <= 140)
        prn = sv - 100;
    if (m.system == "S" && sv >= 198 && sv <= 215)
        prn = sv - 157;
    m.satellite = prn;
    if (!prn) {
        m.signal.clear();
        m.system.clear();
    }
    return prn > 0;
}
void domain(Measurement &m) {
    if (!std::isfinite(m.code) || m.code < 0)
        m.code = NAN;
    if (!std::isfinite(m.phase))
        m.phase = NAN;
    if (!std::isfinite(m.doppler))
        m.doppler = NAN;
}
} // namespace
std::optional<Measurements> decode_measurements(const cppgnss::FrameView &f) {
    Measurements e;
    if (f.protocol == cppgnss::Protocol::ubx) {
        if (f.id != uint16_t(cppgnss::UbxMessageId::RXM_RAWX))
            return {};
        auto parsed = cppgnss::parse<UBX::ubx_rxm_rawx>(f);
        if (!parsed)
            throw std::runtime_error(parsed.error().detail);
        const auto &raw = parsed.value();
        if (raw.version != 1)
            throw std::runtime_error("RAWX requires version 1");
        e.week = raw.week;
        e.tow_seconds = raw.rcvTow;
        e.adjustment_reported = bool(raw.recStat_bit & 2);
        e.rows.reserve(raw.meas_grp.size());
        for (const auto &v : raw.meas_grp) {
            Measurement m;
            if (!identify(m, ubx_codes, v.gnssId * 100 + v.sigId, v.svId,
                          true)) {
                if (v.gnssId == 6 || v.gnssId == 7)
                    ++e.excluded;
                else
                    ++e.unsupported;
                continue;
            }
            m.code = v.prMes;
            m.phase = v.cpMes;
            m.doppler = v.doMes;
            m.cn0 = v.cno;
            m.code_status = (v.trkStat_bit & 1) ? 0 : 1;
            m.phase_status = (v.trkStat_bit & 2) ? 0 : 1;
            m.half_ambiguity = !(v.trkStat_bit & 4);
            m.half_subtracted = bool(v.trkStat_bit & 8);
            m.lock_ms = v.locktime;
            m.lock_lower_bound = *m.lock_ms == 64500;
            m.code_sigma = std::ldexp(.01f, v.prStdev_bit & 15);
            if ((v.cpStdev_bit & 15) != 15)
                m.phase_sigma = .004f * (v.cpStdev_bit & 15);
            m.doppler_sigma = std::ldexp(.002f, v.doStdev_bit & 15);
            domain(m);
            e.rows.push_back(std::move(m));
        }
        return e;
    }
    if (f.id != uint16_t(cppgnss::SbfMessageId::MEAS_EPOCH))
        return {};
    auto parsed = cppgnss::parse<cppgnss::SBF::MeasEpoch>(f);
    if (!parsed)
        throw std::runtime_error(parsed.error().detail);
    const auto &raw = parsed.value();
    if (raw.CommonFlags.Scrambling)
        throw std::runtime_error("Scrambled MeasEpoch");
    e.week = raw.WNc;
    e.tow_ms = raw.TOW;
    e.tow_seconds = *e.tow_ms * .001;
    if (f.revision >= 1)
        e.cumulative_adjustment_ms_mod256 = raw.CumClkJumps;
    auto make = [&](const auto &v, int sv, bool type1) {
        Measurement m;
        int sig = v.Type.SigIdxLo;
        if (sig == 31)
            sig = 32 + v.ObsInfo.SigIdxHi;
        m.antenna = v.Type.AntennaID;
        m.native_signal = sig;
        if constexpr (requires { v.RxChannel; })
            m.receiver_channel = v.RxChannel;
        if (!identify(m, sbf_codes, sig, sv, false)) {
            if ((sig >= 8 && sig <= 12) || sig == 15 || sig == 36 || sig == 37)
                ++e.excluded;
            else
                ++e.unsupported;
            return m;
        }
        if (m.system == "E" && m.signal == "6C" && raw.CommonFlags.E6BUsed)
            m.signal = "6B";
        if (v.CN0 != 255)
            m.cn0 = v.CN0 * .25 + ((sig == 1 || sig == 2) ? 0 : 10);
        m.half_ambiguity = v.ObsInfo.HalfCycleAmbiguity;
        m.code_smoothing_applied = bool(v.ObsInfo.PRSmoothed);
        auto lock = v.LockTime;
        if (lock != (type1 ? 65535u : 255u))
            m.lock_ms = lock * 1000;
        m.lock_lower_bound = lock == (type1 ? 65534u : 254u);
        return m;
    };
    for (const auto &v : raw.MeasEpochChannelType1) {
        auto m = make(v, v.SVID, true);
        double pr = (v.Misc.CodeMSB * 4294967296.0 + v.CodeLSB) * .001;
        double d = v.Doppler == INT32_MIN ? NAN : v.Doppler * .0001;
        double f1 = m.signal.empty() ? 0 : freq(m.system, m.signal);
        m.code = pr == 0 ? NAN : pr;
        m.doppler = d;
        if (f1 && m.lock_ms && !(v.CarrierMSB == -128 && v.CarrierLSB == 0))
            m.phase = m.code * f1 / 299792458.0 +
                      (v.CarrierMSB * 65536 + int64_t(v.CarrierLSB)) * .001;
        if (!m.signal.empty()) {
            domain(m);
            e.rows.push_back(m);
        }
        for (const auto &w : v.MeasEpochChannelType2) {
            auto a = make(w, v.SVID, false);
            a.receiver_channel = m.receiver_channel;
            int cm = w.OffsetsMSB.CodeOffsetMSB;
            if (cm >= 4)
                cm -= 8;
            int dm = w.OffsetsMSB.DopplerOffsetMSB;
            if (dm >= 16)
                dm -= 32;
            if (pr != 0 && !(cm == -4 && w.CodeOffsetLSB == 0))
                a.code = pr + (cm * 65536 + int64_t(w.CodeOffsetLSB)) * .001;
            double f2 = a.signal.empty() ? 0 : freq(a.system, a.signal);
            if (f2 && a.lock_ms && !(w.CarrierMSB == -128 && w.CarrierLSB == 0))
                a.phase = a.code * f2 / 299792458.0 +
                          (w.CarrierMSB * 65536 + int64_t(w.CarrierLSB)) * .001;
            if (f1 && f2 && !(dm == -16 && w.DopplerOffsetLSB == 0))
                a.doppler = d * f2 / f1 +
                            (dm * 65536 + int64_t(w.DopplerOffsetLSB)) * .0001;
            if (!a.signal.empty()) {
                domain(a);
                e.rows.push_back(std::move(a));
            }
        }
    }
    return e;
}
std::optional<Measurements>
decode_measurement_extras(const cppgnss::FrameView &f) {
    if (f.protocol != cppgnss::Protocol::sbf ||
        f.id != uint16_t(cppgnss::SbfMessageId::MEAS_EXTRA))
        return {};
    auto parsed = cppgnss::parse<cppgnss::SBF::MeasExtra>(f);
    if (!parsed)
        throw std::runtime_error(parsed.error().detail);
    const auto &raw = parsed.value();
    Measurements e;
    e.week = raw.WNc;
    e.tow_ms = raw.TOW;
    e.tow_seconds = *e.tow_ms * .001;
    float factor = raw.DopplerVarFactor;
    e.rows.reserve(raw.group.size());
    for (const auto &v : raw.group) {
        Measurement m;
        m.has_extra = true;
        m.receiver_channel = v.RxChannel;
        m.antenna = v.Type.AntennaID;
        int sig = v.Type.SigIdxLo;
        if (sig == 31) {
            if (!v.revision3)
                throw std::runtime_error(
                    "Extended MeasExtra signal before revision 3");
            sig = 32 + v.revision3->Misc.SigIdxHi;
        }
        m.native_signal = sig;
        if ((sig >= 8 && sig <= 12) || sig == 15 || sig == 36 || sig == 37) {
            ++e.excluded;
            continue;
        }
        if (!sbf_codes.contains(sig)) {
            ++e.unsupported;
            continue;
        }
        m.code_multipath_m = v.MPCorrection * .001;
        m.code_smoothing_m = v.SmoothingCorr * .001;
        auto cv = v.CodeVar, pv = v.CarrierVar, lock = v.LockTime;
        if (cv != 65535) {
            m.code_sigma = std::sqrt(cv * 1e-4);
            m.code_sigma_lower_bound = cv == 65534;
        }
        if (pv != 65535) {
            m.phase_sigma = std::sqrt(pv * 1e-6);
            m.phase_sigma_lower_bound = pv == 65534;
        }
        if (std::isfinite(factor) && factor >= 0) {
            m.doppler_variance_factor = factor;
            if (pv != 65535) {
                m.doppler_sigma = std::sqrt(pv * 1e-6 * double(factor));
                m.doppler_sigma_lower_bound = factor > 0 && pv == 65534;
            }
        }
        if (lock != 65535) {
            m.lock_ms = lock * 1000;
            m.lock_lower_bound = lock == 65534;
        }
        if (v.revision1) {
            m.continuity_counter = v.revision1->CumLossCont;
            m.phase_multipath_cycles = v.revision1->CarMPCorr / 512.0;
        }
        if (v.revision3)
            m.cn0_increment = v.revision3->Misc.CN0HighRes / 32.0;
        e.rows.push_back(m);
    }
    return e;
}
} // namespace neognss_obs
