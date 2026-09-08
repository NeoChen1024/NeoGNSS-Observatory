// SPDX-License-Identifier: GPL-3.0-only
#include <cmath>
#include <cppgnss/observations.hpp>
#include <cppgnss/sbf.hpp>
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
    UBX::ubx_rxm_rawx raw;
    if (!raw.parse(UBX::ubx_frame(f.wire.subspan(2))))
      throw std::runtime_error("Invalid RAWX payload");
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
  // MeasEpoch, avoiding duplicate observations; the reader reports the choice.
  if (f.id != 4027)
    return {};
  auto block = SBF::decode(f.id, f.revision, f.payload);
  if (block.status != SBF::Status::decoded)
    throw std::runtime_error("Invalid SBF MeasEpoch: " + block.error);
  auto v = [&](const std::string &name) -> int64_t {
    const auto &x = block.fields.at(name);
    if (auto u = std::get_if<uint64_t>(&x))
      return *u;
    return std::get<int64_t>(x);
  };
  if (v("Scrambling"))
    throw std::runtime_error("Scrambled SBF observations are unsupported");
  epoch.gpst_ns = timestamp(v("WNc"), v("TOW") * .001);
  // Code smoothing is retained as receiver behavior, not undone here.
  auto make = [&](const std::string &s, int prn) {
    Observation o;
    o.prn = prn;
    o.antenna = v("AntennaID" + s);
    int sig = v("SigIdxLo" + s);
    if (sig == 31)
      sig = 32 + v("SigIdxHi" + s);
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
    o.cn0_valid = v("CN0" + s) != 255;
    o.cn0_dbhz = .25 * v("CN0" + s) + ((sig == 1 || sig == 2) ? 0 : 10);
    o.half_cycle = v("HalfCycleAmbiguity" + s);
    return o;
  };
  for (int i = 1; i <= v("N1"); ++i) {
    auto s = std::format("_{:02}", i);
    auto o = make(s, v("SVID" + s));
    double p = (v("CodeMSB" + s) * 4294967296.0 + v("CodeLSB" + s)) * .001;
    double d = v("Doppler" + s) * .0001;
    o.pseudorange_m = p;
    o.code_valid = p > 0;
    o.doppler_hz = d;
    o.doppler_valid = v("Doppler" + s) != INT32_MIN;
    o.lock_seconds = v("LockTime" + s);
    o.lock_valid = v("LockTime" + s) != 65535;
    o.phase_valid = o.code_valid && o.lock_valid &&
                    !(v("CarrierMSB" + s) == -128 && v("CarrierLSB" + s) == 0);
    o.phase_cycles = p * o.frequency_hz / c +
                     (v("CarrierMSB" + s) * 65536 + v("CarrierLSB" + s)) * .001;
    if (!o.signal.empty())
      epoch.signals.push_back(o);
    else
      ++epoch.unselected_signals;
    for (int j = 1; j <= v("N2" + s); ++j) {
      auto t = s + std::format("_{:02}", j);
      auto a = make(t, o.prn);
      int msb = signed_bits(v("CodeOffsetMSB" + t), 3);
      a.code_valid =
          o.code_valid && !(msb == -4 && v("CodeOffsetLSB" + t) == 0);
      a.pseudorange_m = p + (msb * 65536 + v("CodeOffsetLSB" + t)) * .001;
      a.lock_seconds = v("LockTime" + t);
      a.lock_valid = v("LockTime" + t) != 255;
      a.phase_valid = a.code_valid && !(v("CarrierMSB" + t) == -128 &&
                                        v("CarrierLSB" + t) == 0);
      a.phase_cycles =
          a.pseudorange_m * a.frequency_hz / c +
          (v("CarrierMSB" + t) * 65536 + v("CarrierLSB" + t)) * .001;
      msb = signed_bits(v("DopplerOffsetMSB" + t), 5);
      a.doppler_valid = o.doppler_valid && o.frequency_hz > 0 &&
                        !(msb == -16 && v("DopplerOffsetLSB" + t) == 0);
      if (a.doppler_valid)
        a.doppler_hz = d * a.frequency_hz / o.frequency_hz +
                       (msb * 65536 + v("DopplerOffsetLSB" + t)) * .0001;
      if (!a.signal.empty())
        epoch.signals.push_back(a);
      else
        ++epoch.unselected_signals;
    }
  }
  return epoch;
}
} // namespace cppgnss
