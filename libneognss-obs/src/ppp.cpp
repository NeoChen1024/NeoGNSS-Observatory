// SPDX-License-Identifier: GPL-3.0-only
#include "rtklib.h"
#include <algorithm>
#include <limits>
#include <mutex>
#include <neognss_obs/ppp.hpp>
#include <set>

// RTKLIB retains process-global caches (e.g. astronomy); serialize native
// calls.
namespace {
std::mutex rtklib_mutex;
}
extern "C" int showmsg(const char *, ...) { return 0; }
extern "C" void settspan(gtime_t, gtime_t) {}
extern "C" void settime(gtime_t) {}

namespace neognss_obs {
ObservationReader::ObservationReader(const std::string &p)
    : reader_(p == "ubx" ? cppgnss::Protocol::ubx : cppgnss::Protocol::sbf) {
  if (p != "ubx" && p != "sbf")
    throw std::invalid_argument("Expected UBX or SBF");
}
ObservationBatch ObservationReader::feed(std::span<const uint8_t> bytes) {
  ObservationBatch batch;
  reader_.feed(bytes, [&](const cppgnss::FrameView &f) {
    if (f.protocol == cppgnss::Protocol::sbf && f.id >= 4109 && f.id <= 4113)
      ++meas3_;
    auto e = cppgnss::decode_gps_observations(f);
    if (e) {
      ++epochs_;
      unselected_ += e->unselected_signals;
      batch.epochs.push_back(std::move(*e));
    }
  });
  return batch;
}
void ObservationReader::finish() {
  reader_.finish();
  if (!epochs_ && meas3_)
    throw std::runtime_error("Meas3-only SBF is unsupported; enable MeasEpoch");
}
Json ObservationReader::summary() const {
  return {{"source_bytes", reader_.bytes},
          {"observation_epochs", epochs_},
          {"unselected_signals", unselected_},
          {"ignored_meas3_blocks", meas3_},
          {"skipped_protocol_frames", reader_.skipped_protocol_frames},
          {"skipped_protocol_bytes", reader_.skipped_protocol_bytes}};
}
namespace {
gtime_t gpstime(int64_t ns) {
  constexpr int64_t week = 604800000000000LL;
  return gpst2time(int(ns / week), double(ns % week) * 1e-9);
}
double sigma(double v) { return v >= 0 ? std::sqrt(v) : NAN; }
} // namespace
struct PppFloat::State {
  struct Bias {
    int64_t start, end;
    double meters;
  };
  std::map<std::pair<int, std::string>, std::vector<Bias>> biases;
  struct Track {
    int64_t time;
    double lock;
    bool lock_valid, half, sub_half;
  };
  std::map<std::pair<int, std::string>, Track> tracks;
  std::unique_ptr<nav_t> nav = std::make_unique<nav_t>();
  std::unique_ptr<rtk_t> rtk = std::make_unique<rtk_t>();
  Json settings, metadata;
  bool initialized = false, ready = false;
  int64_t previous = -1, solved_time = -1, step_ns, gap_ns;
  uint64_t attempted = 0, solved = 0, skipped = 0;
  std::string code1, code2;
  explicit State(const Json &s) : settings(s) {
    code1 = s.at("signal1");
    code2 = s.at("signal2");
    if (code1.size() != 2 || code2.size() != 2 || code1[0] != '1' ||
        code2[0] != '2' || !obs2code(code1.c_str()) || !obs2code(code2.c_str()))
      throw std::invalid_argument(
          "PPP Float currently requires exact GPS L1/L2 signal codes");
    double step = s.at("interval"), gap = s.at("gap_timeout");
    if (!std::isfinite(step) || step <= 0 || !std::isfinite(gap) || gap <= 0)
      throw std::invalid_argument("Invalid PPP interval/gap timeout");
    step_ns = std::llround(step * 1e9);
    gap_ns = std::llround(gap * 1e9);
    if (step_ns < 1 || gap_ns < 1)
      throw std::invalid_argument("PPP interval below nanosecond resolution");
    prcopt_t opt = prcopt_default;
    opt.mode = PMODE_PPP_STATIC;
    opt.nf = 2;
    opt.navsys = SYS_GPS;
    opt.modear = 0;
    opt.ionoopt = IONOOPT_IFLC;
    opt.tropopt = TROPOPT_EST;
    opt.sateph = EPHOPT_PREC;
    opt.dynamics = 0;
    opt.tidecorr = 1;
    opt.elmin = s.at("elevation_deg").get<double>() * D2R;
    if (!std::isfinite(opt.elmin) || opt.elmin < 0 || opt.elmin >= PI / 2)
      throw std::invalid_argument("Invalid elevation mask");
    opt.posopt[0] = 1;
    opt.posopt[1] = 1;
    opt.posopt[2] = 1;
    opt.posopt[3] = 1;
    opt.prn[5] = 0; // Truly static position, not a position random walk.
    for (int i = 0; i < 3; ++i) {
      opt.ru[i] = s.at("position_ecef_m").at(i);
      opt.antdel[0][i] = s.at("arp_enu_m").at(i);
      if (!std::isfinite(opt.antdel[0][i]))
        throw std::invalid_argument("Invalid antenna ARP displacement");
    }
    if (!std::isfinite(norm(opt.ru, 3)) || norm(opt.ru, 3) < 6e6 ||
        norm(opt.ru, 3) > 7e6)
      throw std::invalid_argument("Invalid station marker ECEF position");
    rtkinit(rtk.get(), &opt);
    initialized = true;
    for (int i = 0; i < 3; ++i)
      rtk->sol.rr[i] = opt.ru[i];
  }
  ~State() {
    if (initialized)
      rtkfree(rtk.get());
    freenav(nav.get(), 0x7f);
    free(nav->erp.data);
  }
  double bias(int sat, const std::string &code, int64_t time) {
    auto it = biases.find({sat, code});
    if (it != biases.end())
      for (const auto &b : it->second)
        if (b.start <= time && time < b.end)
          return b.meters;
    throw std::runtime_error("Missing applicable GPS code OSB for G" +
                             std::to_string(sat) + " C" + code);
  }
};
PppFloat::PppFloat(const Json &s) {
  std::lock_guard guard(rtklib_mutex);
  state_ = std::make_unique<State>(s);
}
PppFloat::~PppFloat() {
  std::lock_guard guard(rtklib_mutex);
  state_.reset();
}
void PppFloat::products(const Json &p) {
  std::lock_guard guard(rtklib_mutex);
  auto &s = *state_;
  s.ready = false;
  freenav(s.nav.get(), 0x7f);
  free(s.nav->erp.data);
  *s.nav = nav_t{};
  for (const auto &file : p.at("sp3"))
    readsp3(file.get<std::string>().c_str(), s.nav.get(), 0);
  for (const auto &file : p.at("clk"))
    if (!readrnxc(file.get<std::string>().c_str(), s.nav.get()))
      throw std::runtime_error("Cannot read precise clocks");
  obs_t unused{};
  sta_t station{};
  for (const auto &file : p.at("nav"))
    if (readrnx(file.get<std::string>().c_str(), 1, "", &unused, s.nav.get(),
                &station) <= 0)
      throw std::runtime_error("Cannot read broadcast navigation");
  freeobs(&unused);
  uniqnav(s.nav.get());
  for (const auto &file : p.at("erp"))
    if (!readerp(file.get<std::string>().c_str(), &s.nav->erp))
      throw std::runtime_error("Cannot read ERP");
  if (s.nav->ne < 11 || s.nav->nc < 2)
    throw std::runtime_error("Insufficient precise orbit/clock records");
  const auto at = gpstime(p.at("time_ns").get<int64_t>());
  if (!readsap(p.at("satellite_antex").get<std::string>().c_str(), at,
               s.nav.get()))
    throw std::runtime_error("Cannot read satellite ANTEX");
  pcvs_t antennas{};
  if (!readpcv(p.at("receiver_antex").get<std::string>().c_str(), &antennas))
    throw std::runtime_error("Cannot read receiver ANTEX");
  const std::string antenna = s.settings.at("antenna");
  bool found = false;
  for (int i = 0; i < antennas.n; ++i) {
    auto &a = antennas.pcv[i];
    auto compact = [](std::string v) {
      std::erase(v, ' ');
      return v;
    };
    if (!a.sat && compact(a.type) == compact(antenna) &&
        (!a.ts.time || timediff(at, a.ts) >= 0) &&
        (!a.te.time || timediff(at, a.te) <= 0)) {
      s.rtk->opt.pcvr[0] = a;
      found = true;
      break;
    }
  }
  free_pcvs(&antennas);
  if (!found)
    throw std::runtime_error(
        "No exact receiver antenna/radome match in ANTEX: " + antenna);
  s.biases.clear();
  for (const auto &b : p.at("biases"))
    s.biases[{b.at("prn"), b.at("signal")}].push_back(
        {b.at("start_ns"), b.at("end_ns"), b.at("meters")});
  s.metadata = p.at("metadata");
  s.ready = true;
}
PppResult PppFloat::process(const ObservationBatch &batch) {
  std::lock_guard guard(rtklib_mutex);
  auto &s = *state_;
  auto &r = *s.rtk;
  auto &nav = *s.nav;
  PppResult result;
  if (!s.ready)
    throw std::runtime_error("Load PPP products before processing");
  for (const auto &e : batch.epochs) {
    if (e.gpst_ns <= s.previous)
      throw std::runtime_error("Non-increasing measurement time in PPP input");
    s.previous = e.gpst_ns;
    std::map<int, obsd_t> rows;
    for (const auto &m : e.signals) {
      int slot = m.signal == s.code1 ? 0 : (m.signal == s.code2 ? 1 : -1);
      if (slot < 0 || m.antenna != 0)
        continue;
      int sat = satno(SYS_GPS, m.prn);
      if (!sat)
        throw std::runtime_error("Unsupported GPS PRN");
      auto key = std::make_pair(m.prn, m.signal);
      auto old = s.tracks.find(key);
      bool slip = e.clock_reset;
      if (old != s.tracks.end())
        slip |= e.gpst_ns - old->second.time > s.gap_ns ||
                (m.lock_valid && old->second.lock_valid &&
                 m.lock_seconds < old->second.lock) ||
                m.half_cycle != old->second.half ||
                m.sub_half_cycle != old->second.sub_half;
      if (slip)
        r.ssat[sat - 1].slip[slot] |= 1;
      if (m.phase_valid)
        s.tracks[key] = {e.gpst_ns, m.lock_seconds, m.lock_valid, m.half_cycle,
                         m.sub_half_cycle};
      auto &o = rows[sat];
      o.time = gpstime(e.gpst_ns);
      o.sat = sat;
      o.rcv = 1;
      if (o.code[slot])
        throw std::runtime_error("Duplicate selected observation in epoch");
      o.code[slot] = obs2code(m.signal.c_str());
      if (m.code_valid)
        o.P[slot] = m.pseudorange_m;
      if (m.phase_valid && !m.half_cycle)
        o.L[slot] = m.phase_cycles;
      if (m.doppler_valid)
        o.D[slot] = m.doppler_hz;
      if (m.cn0_valid)
        o.SNR[slot] = m.cn0_dbhz;
      o.LLI[slot] = r.ssat[sat - 1].slip[slot] & 1;
    }
    if (s.solved_time >= 0 && e.gpst_ns - s.solved_time < s.step_ns - 1000000) {
      ++s.skipped;
      continue;
    }
    s.solved_time = e.gpst_ns;
    ++s.attempted;
    std::vector<obsd_t> observations;
    for (auto &[sat, o] : rows) {
      if (!o.P[0] || !o.P[1] || !o.L[0] || !o.L[1])
        continue;
      if (!nav.pcvs[sat - 1].type[0])
        throw std::runtime_error("Missing satellite antenna calibration");
      for (int k = 0; k < 2; ++k)
        o.P[k] -= s.bias(sat, k ? s.code2 : s.code1, e.gpst_ns);
      // No extrapolation beyond actual product coverage or SP3-clock fallback.
      if (timediff(o.time, nav.peph[0].time) < 0 ||
          timediff(o.time, nav.peph[nav.ne - 1].time) > 0 ||
          timediff(o.time, nav.pclk[0].time) < 0 ||
          timediff(o.time, nav.pclk[nav.nc - 1].time) > 0)
        throw std::runtime_error(
            "Observation outside precise product coverage");
      double rs[6], dts[2], var;
      // peph2pos ignores a failed precise-clock lookup and can leave its
      // SP3 clock in place. Require bracketing CLK values ourselves.
      const auto transmit = timeadd(o.time, -o.P[0] / CLIGHT);
      auto upper = std::lower_bound(
          nav.pclk, nav.pclk + nav.nc, transmit,
          [](const pclk_t &a, gtime_t t) { return timediff(a.time, t) < 0; });
      if (upper == nav.pclk || upper == nav.pclk + nav.nc ||
          timediff(upper->time, (upper - 1)->time) > 60 ||
          upper->clk[sat - 1][0] == 0 || (upper - 1)->clk[sat - 1][0] == 0)
        throw std::runtime_error(
            "Missing bracketing 30-second CLK samples; no SP3-clock fallback");
      if (!peph2pos(o.time, sat, &nav, 1, rs, dts, &var) || dts[0] == 0)
        throw std::runtime_error(
            "Missing precise orbit/clock for GPS satellite");
      observations.push_back(o);
    }
    int n = observations.size();
    if (n > MAXOBS)
      throw std::runtime_error("PPP observation capacity exceeded");
    r.sol.stat = SOLQ_NONE;
    if (n >= 4)
      rtkpos(&r, observations.data(), n, &nav);
    bool ok = r.sol.stat == SOLQ_PPP;
    if (ok)
      ++s.solved;
    PppEpoch row{};
    row.gpst_ns = e.gpst_ns;
    row.status = ok ? 6 : 0;
    row.satellites = ok ? r.sol.ns : 0;
    double xyz[3], q[9], pos[3], enu[3], qe[9];
    for (int i = 0; i < 3; ++i)
      xyz[i] = ok ? r.sol.rr[i] : NAN;
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        q[i + j * 3] = ok ? r.P[i + j * r.nx] : NAN;
    ecef2pos(r.opt.ru, pos);
    double delta[3];
    for (int i = 0; i < 3; ++i)
      delta[i] = xyz[i] - r.opt.ru[i];
    ecef2enu(pos, delta, enu);
    covenu(pos, q, qe);
    row.x = xyz[0];
    row.y = xyz[1];
    row.z = xyz[2];
    row.qxx = q[0];
    row.qyy = q[4];
    row.qzz = q[8];
    row.qxy = q[3];
    row.qyz = q[7];
    row.qzx = q[2];
    row.east = enu[0];
    row.north = enu[1];
    row.up = enu[2];
    row.sigma_e = sigma(qe[0]);
    row.sigma_n = sigma(qe[4]);
    row.sigma_u = sigma(qe[8]);
    // Pinned PPP layout: static XYZ, NSYS clocks, estimated ZTD (nf=2, IFLC).
    constexpr int clock = 3, trop = 3 + NSYS;
    row.clock_ns = ok ? r.x[clock] / CLIGHT * 1e9 : NAN;
    row.clock_sigma_ns =
        ok ? sigma(r.P[clock + clock * r.nx]) / CLIGHT * 1e9 : NAN;
    row.ztd_m = ok ? r.x[trop] : NAN;
    row.ztd_sigma_m = ok ? sigma(r.P[trop + trop * r.nx]) : NAN;
    result.epochs.push_back(row);
    for (const auto &o : observations) {
      const auto &sat = r.ssat[o.sat - 1];
      result.satellites.push_back(
          {e.gpst_ns, int32_t(o.sat), int32_t(ok && sat.vsat[0]),
           int32_t((o.LLI[0] | o.LLI[1] | sat.slip[0] | sat.slip[1]) & 1),
           ok ? sat.azel[0] * R2D : NAN, ok ? sat.azel[1] * R2D : NAN,
           ok && sat.vsat[0] ? sat.resc[0] : NAN,
           ok && sat.vsat[0] ? sat.resp[0] : NAN});
    }
    for (auto &sat : r.ssat)
      for (int k = 0; k < 2; ++k)
        sat.slip[k] = 0;
  }
  return result;
}
Json PppFloat::summary() const {
  const auto &s = *state_;
  return {{"mode", "static_forward_float"},
          {"engine", std::string("RTKLIB-") + VER_RTKLIB + " " + PATCH_LEVEL},
          {"systems", "GPS"},
          {"attempted_epochs", s.attempted},
          {"solved_epochs", s.solved},
          {"decimated_epochs", s.skipped},
          {"settings", s.settings},
          {"products", s.metadata},
          {"uncertainty", "formal 1-sigma"},
          {"residuals", "post-fit ionosphere-free L1/L2, meters"},
          {"models",
           {"precise SP3/CLK", "satellite code OSB",
            "GPS L1/L2 antenna PCO and NOAZI PCV", "phase wind-up",
            "solid Earth tides", "estimated ZTD, Niell mapping"}},
          {"limitations",
           {"GPS only", "float only", "no ocean loading",
            "no azimuth-dependent PCV", "no troposphere gradients",
            "no PPP receiver-bias calibration", "not equivalent to CSRS-PPP"}}};
}
} // namespace neognss_obs
