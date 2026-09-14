// SPDX-License-Identifier: GPL-3.0-only
#include <bit>
#include <cppgnss/measurements.hpp>
#include <map>
#include <stdexcept>

namespace cppgnss {
namespace {
uint64_t u(std::span<const uint8_t> p, size_t o, size_t n) {
    if (o + n > p.size())
        throw std::runtime_error("Truncated measurement payload");
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i)
        v |= uint64_t(p[o + i]) << (8 * i);
    return v;
}
int64_t s(std::span<const uint8_t> p, size_t o, size_t n) {
    auto v = u(p, o, n);
    return (v & (uint64_t(1) << (n * 8 - 1)))
               ? int64_t(v) - int64_t(uint64_t(1) << (n * 8))
               : int64_t(v);
}
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
std::optional<Measurements> decode_measurements(const FrameView &f) {
    auto p = f.payload;
    Measurements e;
    if (f.protocol == Protocol::ubx) {
        if (f.id != 0x0215)
            return {};
        if (p.size() < 16 || p[13] != 1 || p.size() != 16 + 32u * p[11])
            throw std::runtime_error(
                "Unsupported or malformed RAWX (requires version 1)");
        e.week = u(p, 8, 2);
        e.tow_seconds = std::bit_cast<double>(u(p, 0, 8));
        for (size_t o = 16; o < p.size(); o += 32) {
            Measurement m;
            if (!identify(m, ubx_codes, p[o + 20] * 100 + p[o + 22], p[o + 21],
                          true)) {
                if (p[o + 20] == 6 || p[o + 20] == 7)
                    ++e.excluded;
                else
                    ++e.unsupported;
                continue;
            }
            m.code = std::bit_cast<double>(u(p, o, 8));
            m.phase = std::bit_cast<double>(u(p, o + 8, 8));
            m.doppler = std::bit_cast<float>(uint32_t(u(p, o + 16, 4)));
            m.cn0 = p[o + 26];
            m.code_status = (p[o + 30] & 1) ? 0 : 1;
            m.phase_status = (p[o + 30] & 2) ? 0 : 1;
            m.half_ambiguity = !(p[o + 30] & 4);
            m.half_subtracted = bool(p[o + 30] & 8);
            m.lock_ms = u(p, o + 24, 2);
            m.lock_lower_bound = *m.lock_ms == 64500;
            m.code_sigma = std::ldexp(.01f, p[o + 27] & 15);
            if ((p[o + 28] & 15) != 15)
                m.phase_sigma = .004f * (p[o + 28] & 15);
            m.doppler_sigma = std::ldexp(.002f, p[o + 29] & 15);
            domain(m);
            e.rows.push_back(std::move(m));
        }
        return e;
    }
    if (f.id != 4027)
        return {};
    if (p.size() < 12 || f.revision > 1 || (p[6] && p[7] < 20) || (p[9] & 128))
        throw std::runtime_error("Unsupported/malformed/scrambled MeasEpoch");
    e.week = u(p, 4, 2);
    e.tow_ms = u(p, 0, 4);
    e.tow_seconds = *e.tow_ms * .001;
    size_t o = 12;
    auto make = [&](size_t q, int sv, bool type1) {
        Measurement m;
        size_t type = q + (type1 ? 1 : 0), info = q + (type1 ? 18 : 5);
        int sig = u(p, type, 1) & 31;
        if (sig == 31)
            sig = 32 + (u(p, info, 1) >> 3);
        m.antenna = u(p, type, 1) >> 5;
        m.native_signal = sig;
        m.receiver_channel = type1 ? u(p, q, 1) : 0;
        if (!identify(m, sbf_codes, sig, sv, false)) {
            if ((sig >= 8 && sig <= 12) || sig == 15 || sig == 36 || sig == 37)
                ++e.excluded;
            else
                ++e.unsupported;
            return m;
        }
        if (type1 && m.system == "E" && m.signal == "6C" && (p[9] & 64))
            m.signal = "6B";
        auto cn = u(p, q + (type1 ? 15 : 2), 1);
        if (cn != 255)
            m.cn0 = cn * .25 + ((sig == 1 || sig == 2) ? 0 : 10);
        m.half_ambiguity = u(p, info, 1) & 4;
        auto lock = u(p, q + (type1 ? 16 : 1), type1 ? 2 : 1);
        if (lock != (type1 ? 65535u : 255u))
            m.lock_ms = lock * 1000;
        m.lock_lower_bound = lock == (type1 ? 65534u : 254u);
        return m;
    };
    for (unsigned i = 0; i < p[6]; ++i) {
        if (o + 20 > p.size())
            throw std::runtime_error("Truncated MeasEpoch Type1");
        auto n = u(p, o + 19, 1);
        if (n && p[8] < 12)
            throw std::runtime_error("Invalid MeasEpoch Type2 length");
        int sv = u(p, o + 2, 1);
        auto m = make(o, sv, true);
        double pr =
            ((u(p, o + 3, 1) & 15) * 4294967296.0 + u(p, o + 4, 4)) * .001;
        double d = s(p, o + 8, 4) == INT32_MIN ? NAN : s(p, o + 8, 4) * .0001;
        double f1 = m.signal.empty() ? 0 : freq(m.system, m.signal);
        m.code = pr == 0 ? NAN : pr;
        m.doppler = d;
        if (f1 && m.lock_ms &&
            !(s(p, o + 14, 1) == -128 && u(p, o + 12, 2) == 0))
            m.phase =
                m.code * f1 / 299792458.0 +
                (s(p, o + 14, 1) * 65536 + int64_t(u(p, o + 12, 2))) * .001;
        if (!m.signal.empty()) {
            domain(m);
            e.rows.push_back(m);
        }
        o += p[7];
        for (unsigned j = 0; j < n; ++j, o += p[8]) {
            if (o + 12 > p.size())
                throw std::runtime_error("Truncated MeasEpoch Type2");
            auto a = make(o, sv, false);
            a.receiver_channel = m.receiver_channel;
            int cm = u(p, o + 3, 1) & 7;
            if (cm >= 4)
                cm -= 8;
            int dm = u(p, o + 3, 1) >> 3;
            if (dm >= 16)
                dm -= 32;
            if (pr != 0 && !(cm == -4 && u(p, o + 6, 2) == 0))
                a.code = pr + (cm * 65536 + int64_t(u(p, o + 6, 2))) * .001;
            double f2 = a.signal.empty() ? 0 : freq(a.system, a.signal);
            if (f2 && a.lock_ms &&
                !(s(p, o + 4, 1) == -128 && u(p, o + 8, 2) == 0))
                a.phase =
                    a.code * f2 / 299792458.0 +
                    (s(p, o + 4, 1) * 65536 + int64_t(u(p, o + 8, 2))) * .001;
            if (f1 && f2 && !(dm == -16 && u(p, o + 10, 2) == 0))
                a.doppler = d * f2 / f1 +
                            (dm * 65536 + int64_t(u(p, o + 10, 2))) * .0001;
            if (!a.signal.empty()) {
                domain(a);
                e.rows.push_back(std::move(a));
            }
        }
    }
    if (o > p.size())
        throw std::runtime_error("MeasEpoch sub-block length exceeds payload");
    return e;
}
std::optional<Measurements> decode_measurement_extras(const FrameView &f) {
    if (f.protocol != Protocol::sbf || f.id != 4000)
        return {};
    auto p = f.payload;
    if (p.size() < 12 || f.revision > 3)
        throw std::runtime_error("Unsupported/malformed MeasExtra revision");
    size_t length = p[7], minimum = f.revision == 0   ? 12
                                    : f.revision == 1 ? 14
                                    : f.revision == 2 ? 15
                                                      : 16;
    if (length < minimum)
        throw std::runtime_error("Short MeasExtra sub-block");
    size_t capacity = (p.size() - 12) / length;
    if (capacity < p[6])
        throw std::runtime_error("Invalid MeasExtra count");
    size_t count = p[6] + 256 * ((capacity - p[6]) / 256);
    if (p.size() - 12 - count * length > 3)
        throw std::runtime_error("MeasExtra count/length mismatch");
    Measurements e;
    e.week = u(p, 4, 2);
    e.tow_ms = u(p, 0, 4);
    e.tow_seconds = *e.tow_ms * .001;
    float factor = std::bit_cast<float>(uint32_t(u(p, 8, 4)));
    for (size_t i = 0, o = 12; i < count; ++i, o += length) {
        Measurement m;
        m.has_extra = true;
        m.receiver_channel = p[o];
        m.antenna = p[o + 1] >> 5;
        int sig = p[o + 1] & 31;
        if (sig == 31) {
            if (f.revision < 3)
                throw std::runtime_error(
                    "Extended MeasExtra signal before revision 3");
            sig = 32 + (p[o + 15] >> 3);
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
        m.code_multipath_m = s(p, o + 2, 2) * .001;
        m.code_smoothing_m = s(p, o + 4, 2) * .001;
        auto cv = u(p, o + 6, 2), pv = u(p, o + 8, 2), lock = u(p, o + 10, 2);
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
        if (f.revision >= 1) {
            m.continuity_counter = p[o + 12];
            m.phase_multipath_cycles = s(p, o + 13, 1) / 512.0;
        }
        if (f.revision >= 3)
            m.cn0_increment = (p[o + 15] & 7) / 32.0;
        e.rows.push_back(m);
    }
    return e;
}
} // namespace cppgnss
