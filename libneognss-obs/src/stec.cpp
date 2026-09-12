// SPDX-License-Identifier: GPL-3.0-only
#include "rtklib.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <neognss_obs/rtklib_lock.hpp>
#include <neognss_obs/stec.hpp>
#include <set>
#include <tuple>

namespace neognss_obs {
namespace {
constexpr double K = 40.3e16 * (1 / (FREQL2 * FREQL2) - 1 / (FREQL1 * FREQL1));
constexpr int64_t second = 1000000000LL;
gtime_t time_of(int64_t ns) {
  return gpst2time(int(ns / (604800 * second)),
                   double(ns % (604800 * second)) / second);
}
double positive(const Json &s, const char *key) {
  double v = s.at(key);
  if (!std::isfinite(v) || v <= 0)
    throw std::invalid_argument(std::string("Invalid ") + key);
  return v;
}
double median(std::vector<double> x) {
  if (x.empty())
    return NAN;
  auto mid = x.begin() + x.size() / 2;
  std::nth_element(x.begin(), mid, x.end());
  double v = *mid;
  return x.size() % 2 ? v : (v + *std::max_element(x.begin(), mid)) / 2;
}
struct Fit {
  double value = NAN, scatter = NAN;
  std::vector<double> weights;
};
Fit robust(const std::vector<double> &x, const std::vector<double> &weights,
           double floor) {
  Fit out;
  if (x.empty())
    return out;
  out.value = median(x);
  std::vector<double> deviations;
  for (double v : x)
    deviations.push_back(std::abs(v - out.value));
  double scale = std::max(floor, 1.4826 * median(deviations));
  out.weights.resize(x.size());
  for (int k = 0; k < 20; ++k) {
    double sum = 0, norm = 0;
    for (size_t i = 0; i < x.size(); ++i) {
      out.weights[i] =
          weights[i] *
          std::min(1.0,
                   1.5 * scale / std::max(1e-12, std::abs(x[i] - out.value)));
      sum += out.weights[i] * x[i];
      norm += out.weights[i];
    }
    double next = sum / norm;
    if (std::abs(next - out.value) < 1e-8) {
      out.value = next;
      break;
    }
    out.value = next;
  }
  double sum = 0, norm = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    sum += out.weights[i] * std::pow(x[i] - out.value, 2);
    norm += out.weights[i];
  }
  out.scatter = std::sqrt(sum / norm);
  return out;
}
void free_products(nav_t &nav) {
  // RTKLIB freenav frees the tec array, but not each map's grids.
  for (int i = 0; i < nav.nt; ++i) {
    free(nav.tec[i].data);
    free(nav.tec[i].rms);
  }
  freenav(&nav, 0x7f);
  free(nav.erp.data);
  nav = nav_t{};
}
// Strict bilinear interpolation: no nearest-neighbour fill or zero
// substitution.
std::pair<double, double> grid(const tec_t &m, double latitude,
                               double longitude) {
  double a = (latitude - m.lats[0]) / m.lats[2];
  double lon = std::fmod(longitude - m.lons[0], 360.0);
  if (lon < 0)
    lon += 360;
  double b = lon / m.lons[2];
  int i = int(std::floor(a)), j = int(std::floor(b));
  if (i < 0 || i >= m.ndata[0] - 1 || j < 0 || j >= m.ndata[1] - 1)
    return {NAN, NAN};
  a -= i;
  b -= j;
  double v = 0, rms = 0;
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 2; ++x) {
      int n = i + x + m.ndata[0] * (j + y);
      // readtec uses zero for unavailable values; conservatively reject both.
      if (!(m.data[n] > 0) || !std::isfinite(m.data[n]) || !(m.rms[n] > 0))
        return {NAN, NAN};
      double w = (x ? a : 1 - a) * (y ? b : 1 - b);
      v += w * m.data[n];
      rms += w * m.rms[n];
    }
  return {v, rms};
}
} // namespace

struct StecProcessor::State {
  struct Bias {
    int64_t start, end;
    double meters;
  };
  struct Track {
    int64_t id, start, last, emitted = -1, samples = 0;
    int reason;
    double gf;
    cppgnss::Observation a, b;
    std::vector<double> offsets, weights;
    int64_t first_level = -1, last_level = -1;
  };
  Json settings;
  std::unique_ptr<nav_t> nav = std::make_unique<nav_t>();
  std::map<int, std::vector<Bias>> biases;
  std::map<int, std::vector<Bias>> gim_biases;
  std::map<int, Track> tracks;
  std::string signal1, signal2;
  double rr[3], pos[3], elevation, level_elevation, mapping_height;
  int64_t previous = -1, next_arc = 0, interval, gap;
  std::map<int, uint64_t> product_gaps{{1, 0}, {2, 0}, {4, 0}, {8, 0}, {16, 0}};
  uint64_t epochs = 0, sample_count = 0, arcs = 0, valid_arcs = 0,
           unhealthy = 0, missing_gim = 0;
  bool ready = false, finished = false;
  explicit State(const Json &s) : settings(s) {
    signal1 = s.at("signal1");
    signal2 = s.at("signal2");
    if (signal1.size() != 2 || signal2.size() != 2 || signal1[0] != '1' ||
        signal2[0] != '2' || !obs2code(signal1.c_str()) ||
        !obs2code(signal2.c_str()))
      throw std::invalid_argument("STEC requires an exact GPS L1/L2 pair");
    interval = std::llround(positive(s, "interval") * second);
    gap = std::llround(positive(s, "gap_timeout") * second);
    if (interval < 1 || gap < 1)
      throw std::invalid_argument("Interval below nanosecond resolution");
    elevation = positive(s, "elevation_deg");
    level_elevation = positive(s, "level_elevation_deg");
    if (elevation >= 90 || level_elevation >= 90 || level_elevation < elevation)
      throw std::invalid_argument("Invalid elevation masks");
    mapping_height = positive(s, "mapping_height_km");
    positive(s, "gf_jump_m");
    positive(s, "gf_rate_m_s");
    positive(s, "min_arc_seconds");
    positive(s, "min_level_samples");
    for (int i = 0; i < 3; ++i)
      rr[i] = s.at("position_ecef_m").at(i);
    if (!std::isfinite(norm(rr, 3)) || norm(rr, 3) < 6e6 || norm(rr, 3) > 7e6)
      throw std::invalid_argument("Invalid antenna ECEF position");
    ecef2pos(rr, pos);
  }
  ~State() { free_products(*nav); }
  double bias(int prn, int64_t t) const {
    auto find = [&](const auto &values) -> double {
      auto found = values.find(prn);
      if (found != values.end())
        for (auto b : found->second)
          if (b.start <= t && t < b.end)
            return b.meters;
      return NAN;
    };
    double intra = find(biases), reference = find(gim_biases);
    if (std::isfinite(intra) && std::isfinite(reference))
      return intra - reference;
    return NAN;
  }
  void close(int prn, int reason, std::vector<StecArc> &out) {
    auto found = tracks.find(prn);
    if (found == tracks.end())
      return;
    auto &a = found->second;
    auto fit = robust(a.offsets, a.weights, 0.1);
    bool valid =
        a.offsets.size() >= settings.at("min_level_samples").get<size_t>() &&
        double(a.last_level - a.first_level) / second >=
            settings.at("min_arc_seconds").get<double>();
    out.push_back({a.start, a.last, a.id, a.samples, int64_t(a.offsets.size()),
                   prn, int(valid), a.reason, reason, valid ? fit.value : NAN,
                   fit.scatter});
    ++arcs;
    valid_arcs += valid;
    tracks.erase(found);
  }
  bool geometry(int prn, int64_t ns, double &az, double &el, double &lat,
                double &lon, int32_t &issues) {
    int sat = satno(SYS_GPS, prn);
    if (!sat)
      throw std::runtime_error("Unsupported GPS PRN");
    gtime_t t = time_of(ns);
    const eph_t *health = nullptr;
    double age = 7201;
    for (int i = 0; i < nav->n; ++i)
      if (nav->eph[i].sat == sat) {
        double d = std::abs(timediff(t, nav->eph[i].toe));
        if (d < age) {
          health = nav->eph + i;
          age = d;
        }
      }
    if (!health || age > 7200) {
      issues |= 4;
      return true;
    }
    if (health->svh) {
      ++unhealthy;
      return false;
    }
    double tau = .075, rs[6], dts[2], variance, e[3];
    if (nav->ne < 11)
      issues |= 1;
    if (nav->nc < 2)
      issues |= 2;
    if (issues)
      return true;
    for (int k = 0; k < 3; ++k) {
      auto tx = timeadd(t, -tau);
      if (timediff(tx, nav->peph[0].time) < 0 ||
          timediff(tx, nav->peph[nav->ne - 1].time) > 0) {
        issues |= 1;
        return true;
      }
      // Match RTKLIB's 11-point orbit stencil. Do not interpolate across a
      // missing day merely because two distant products bracket the epoch.
      auto orbit = std::lower_bound(
          nav->peph, nav->peph + nav->ne, tx,
          [](const peph_t &a, gtime_t b) { return timediff(a.time, b) < 0; });
      int index = std::max(0, int(orbit - nav->peph) - 1);
      int first = std::clamp(index - 5, 0, nav->ne - 11);
      for (int j = first + 1; j < first + 11; ++j) {
        double separation = timediff(nav->peph[j].time, nav->peph[j - 1].time);
        if (separation <= 0 || separation > 301) {
          issues |= 1;
          return true;
        }
      }
      auto upper = std::lower_bound(
          nav->pclk, nav->pclk + nav->nc, tx,
          [](const pclk_t &a, gtime_t b) { return timediff(a.time, b) < 0; });
      if (upper == nav->pclk || upper == nav->pclk + nav->nc ||
          timediff(upper->time, (upper - 1)->time) > 60 ||
          !upper->clk[sat - 1][0] || !(upper - 1)->clk[sat - 1][0]) {
        issues |= 2;
        return true;
      }
      // Orbit/LOS only: use satellite centre of mass, not RTKLIB antenna
      // offsets.
      if (!peph2pos(tx, sat, nav.get(), 0, rs, dts, &variance)) {
        issues |= 1;
        return true;
      }
      double angle = OMGE * tau, x = rs[0], y = rs[1];
      rs[0] = std::cos(angle) * x + std::sin(angle) * y;
      rs[1] = -std::sin(angle) * x + std::cos(angle) * y;
      for (int j = 0; j < 3; ++j)
        e[j] = rs[j] - rr[j];
      tau = norm(e, 3) / CLIGHT;
    }
    double range = norm(e, 3), azel[2];
    for (double &v : e)
      v /= range;
    satazel(pos, e, azel);
    az = azel[0] * R2D;
    el = azel[1] * R2D;
    if (el < elevation)
      return false;
    // Intersect reception-frame LOS with CODE's 6821 km geocentric shell.
    double dot = dot3(rr, e), radius = 6821000;
    double distance =
        -dot + std::sqrt(dot * dot + radius * radius - dot3(rr, rr));
    double p[3];
    for (int j = 0; j < 3; ++j)
      p[j] = rr[j] + distance * e[j];
    lat = std::atan2(p[2], std::hypot(p[0], p[1])) * R2D;
    lon = std::atan2(p[1], p[0]) * R2D;
    return true;
  }
  static double dot3(const double *a, const double *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  }
  std::pair<double, double> gim(int64_t ns, double lat, double lon,
                                double mapping) {
    auto t = time_of(ns);
    if (nav->nt < 2)
      return {NAN, NAN};
    auto upper = std::upper_bound(
        nav->tec, nav->tec + nav->nt, t,
        [](gtime_t a, const tec_t &b) { return timediff(a, b.time) < 0; });
    if (upper == nav->tec || upper == nav->tec + nav->nt)
      return {NAN, NAN};
    auto &a = *(upper - 1);
    auto &b = *upper;
    double span = timediff(b.time, a.time);
    if (span <= 0 || span > 3601)
      return {NAN, NAN};
    // Sun-fixed temporal interpolation, with both map epochs normalized to
    // GPST.
    auto va = grid(a, lat, lon + timediff(t, a.time) * 360 / 86400);
    auto vb = grid(b, lat, lon + timediff(t, b.time) * 360 / 86400);
    double w = timediff(t, a.time) / span;
    if (!std::isfinite(va.first) || !std::isfinite(vb.first)) {
      return {NAN, NAN};
    }
    return {mapping * ((1 - w) * va.first + w * vb.first),
            mapping * ((1 - w) * va.second + w * vb.second)};
  }
};

StecProcessor::StecProcessor(const Json &s) {
  std::lock_guard guard(rtklib_mutex);
  state_ = std::make_unique<State>(s);
}
StecProcessor::~StecProcessor() {
  std::lock_guard guard(rtklib_mutex);
  state_.reset();
}
void StecProcessor::products(const Json &p) {
  std::lock_guard guard(rtklib_mutex);
  auto &s = *state_;
  s.ready = false;
  free_products(*s.nav);
  for (const auto &f : p.at("sp3")) {
    int before = s.nav->ne;
    readsp3(f.get<std::string>().c_str(), s.nav.get(), 0);
    if (s.nav->ne <= before)
      throw std::runtime_error("Cannot read nonempty STEC SP3: " +
                               f.get<std::string>());
  }
  for (const auto &f : p.at("clk"))
    if (!readrnxc(f.get<std::string>().c_str(), s.nav.get()))
      throw std::runtime_error("Cannot read STEC CLK");
  obs_t unused{};
  sta_t sta{};
  for (const auto &f : p.at("nav")) {
    int ok = readrnx(f.get<std::string>().c_str(), 1, "", &unused, s.nav.get(),
                     &sta);
    freeobs(&unused);
    if (ok <= 0)
      throw std::runtime_error("Cannot read STEC BRDC");
  }
  uniqnav(s.nav.get());
  for (const auto &f : p.at("ionex")) {
    int before = s.nav->nt;
    readtec(f.get<std::string>().c_str(), s.nav.get(), 1);
    if (s.nav->nt <= before)
      throw std::runtime_error("Cannot read nonempty STEC IONEX: " +
                               f.get<std::string>());
  }
  for (int i = 0; i < s.nav->nt; ++i) {
    auto &m = s.nav->tec[i];
    if (m.ndata[2] != 1 || m.rb != 6371 || m.hgts[0] != 450 || m.lons[2] <= 0 ||
        m.lats[2] >= 0)
      throw std::runtime_error("Expected CODE 450 km single-layer IONEX grid");
    // readtec leaves external UT calendar epochs unchanged. Convert once here.
    m.time = utc2gpst(m.time);
  }
  s.biases.clear();
  for (const auto &b : p.at("biases"))
    s.biases[b.at("prn")].push_back(
        {b.at("start_ns"), b.at("end_ns"), b.at("meters")});
  s.gim_biases.clear();
  auto utc_ns = [](int64_t ns) {
    auto utc = time_of(ns);
    return ns + std::llround(timediff(utc2gpst(utc), utc) * second);
  };
  for (const auto &b : p.at("gim_biases"))
    s.gim_biases[b.at("prn")].push_back({utc_ns(b.at("start_utc_ns")),
                                         utc_ns(b.at("end_utc_ns")),
                                         b.at("meters")});
  s.ready = true;
}
StecResult StecProcessor::process(const ObservationBatch &batch) {
  std::lock_guard guard(rtklib_mutex);
  auto &s = *state_;
  StecResult out;
  if (!s.ready || s.finished)
    throw std::runtime_error("STEC processor is not ready");
  for (const auto &epoch : batch.epochs) {
    if (epoch.gpst_ns <= s.previous)
      throw std::runtime_error("Non-increasing STEC observation time");
    s.previous = epoch.gpst_ns;
    ++s.epochs;
    std::map<int, std::pair<const cppgnss::Observation *,
                            const cppgnss::Observation *>>
        pairs;
    for (const auto &m : epoch.signals)
      if (m.antenna == 0 && (m.signal == s.signal1 || m.signal == s.signal2)) {
        auto &slot =
            m.signal == s.signal1 ? pairs[m.prn].first : pairs[m.prn].second;
        if (slot)
          throw std::runtime_error("Duplicate selected STEC signal");
        slot = &m;
      }
    for (auto [prn, pair] : pairs) {
      auto [a, b] = pair;
      if (!a || !b || !a->phase_valid || !b->phase_valid)
        continue;
      if (a->half_cycle || b->half_cycle) {
        s.close(prn, 3, out.arcs);
        continue;
      }
      double gf = CLIGHT / a->frequency_hz * a->phase_cycles -
                  CLIGHT / b->frequency_hz * b->phase_cycles;
      int reason = 0;
      auto old = s.tracks.find(prn);
      if (old != s.tracks.end()) {
        auto &t = old->second;
        double dt = double(epoch.gpst_ns - t.last) / second;
        if (epoch.gpst_ns - t.last > s.gap)
          reason = 1;
        else if ((a->lock_valid && t.a.lock_valid &&
                  a->lock_seconds < t.a.lock_seconds) ||
                 (b->lock_valid && t.b.lock_valid &&
                  b->lock_seconds < t.b.lock_seconds))
          reason = 2;
        else if (a->sub_half_cycle != t.a.sub_half_cycle ||
                 b->sub_half_cycle != t.b.sub_half_cycle)
          reason = 3;
        else if (std::abs(gf - t.gf) >
                 s.settings.at("gf_jump_m").get<double>() +
                     s.settings.at("gf_rate_m_s").get<double>() * dt)
          reason = 4;
        if (reason)
          s.close(prn, reason, out.arcs);
      }
      auto [it, inserted] = s.tracks.try_emplace(prn);
      auto &t = it->second;
      if (inserted) {
        t.id = s.next_arc++;
        t.start = epoch.gpst_ns;
        t.reason = reason;
      }
      t.last = epoch.gpst_ns;
      t.a = *a;
      t.b = *b;
      t.gf = gf;
      if (t.emitted >= 0 && epoch.gpst_ns - t.emitted < s.interval)
        continue;
      t.emitted = epoch.gpst_ns;
      double az = NAN, el = NAN, lat = NAN, lon = NAN;
      int32_t issues = 0;
      if (!s.geometry(prn, epoch.gpst_ns, az, el, lat, lon, issues))
        continue;
      // CODE MSLM mapping height is distinct from the 450 km IPP shell.
      double rp = 6371 / (6371 + s.mapping_height) *
                  std::sin(.9782 * (PI / 2 - el * D2R));
      double mapping = 1 / std::sqrt(1 - rp * rp);
      double gim = NAN, rms = NAN;
      if (std::isfinite(mapping)) {
        std::tie(gim, rms) = s.gim(epoch.gpst_ns, lat, lon, mapping);
        if (!std::isfinite(gim)) {
          issues |= 16;
          ++s.missing_gim;
        }
      }
      double code = NAN;
      if (a->code_valid && b->code_valid) {
        code = b->pseudorange_m - a->pseudorange_m - s.bias(prn, epoch.gpst_ns);
        if (!std::isfinite(code))
          issues |= 8;
      }
      for (auto &[bit, count] : s.product_gaps)
        if (issues & bit)
          ++count;
      out.samples.push_back({epoch.gpst_ns, t.id, prn, issues, gf, code, el, az,
                             lat, lon, mapping, gim, rms});
      ++t.samples;
      ++s.sample_count;
      if (std::isfinite(code) && el >= s.level_elevation) {
        t.offsets.push_back(code - gf);
        t.weights.push_back(std::pow(std::sin(el * D2R), 2));
        if (t.first_level < 0)
          t.first_level = epoch.gpst_ns;
        t.last_level = epoch.gpst_ns;
      }
    }
  }
  return out;
}
std::vector<StecArc> StecProcessor::finish() {
  std::lock_guard guard(rtklib_mutex);
  auto &s = *state_;
  std::vector<StecArc> out;
  while (!s.tracks.empty())
    s.close(s.tracks.begin()->first, 5, out);
  s.finished = true;
  return out;
}
Json StecProcessor::summary() const {
  const auto &s = *state_;
  return {{"epochs", s.epochs},
          {"samples", s.sample_count},
          {"arcs", s.arcs},
          {"valid_arcs", s.valid_arcs},
          {"unhealthy_samples", s.unhealthy},
          {"missing_gim_samples", s.missing_gim},
          {"product_gaps",
           {{"orbit", s.product_gaps.at(1)},
            {"clock", s.product_gaps.at(2)},
            {"health", s.product_gaps.at(4)},
            {"satellite_bias", s.product_gaps.at(8)},
            {"gim", s.product_gaps.at(16)}}},
          {"meters_per_tecu", K},
          {"arc_reasons",
           {{"0", "start"},
            {"1", "timeout"},
            {"2", "lock_decrease"},
            {"3", "half_cycle_change"},
            {"4", "geometry_free_jump_candidate"},
            {"5", "stream_end"}}}};
}

Json fit_receiver_dcb(std::span<const DcbSample> rows, const Json &settings) {
  double bin_seconds = positive(settings, "bin_seconds"),
         min_hours = positive(settings, "min_hours");
  auto bin_ns = std::llround(bin_seconds * second);
  if (bin_ns < 1)
    throw std::invalid_argument("Invalid DCB bin interval");
  double cutoff = positive(settings, "elevation_deg"),
         rms_floor = positive(settings, "gim_rms_floor_tecu");
  double max_scatter = positive(settings, "max_scatter_tecu");
  int min_sat = positive(settings, "min_satellites"),
      min_arcs = positive(settings, "min_arcs");
  std::map<std::pair<int64_t, int64_t>, std::vector<const DcbSample *>> bins;
  for (const auto &r : rows)
    if (std::isfinite(r.residual_tecu) && std::isfinite(r.gim_rms_tecu) &&
        r.gim_rms_tecu > 0 && r.elevation_deg >= cutoff &&
        r.elevation_deg < 90 && std::isfinite(r.azimuth_deg))
      bins[{r.arc_id, r.gpst_ns / bin_ns}].push_back(&r);
  std::vector<double> x, w;
  std::vector<const DcbSample *> representatives;
  std::map<int64_t, double> arc_weight;
  for (const auto &[key, bin] : bins) {
    std::vector<double> v;
    double weight = 0;
    for (auto r : bin) {
      v.push_back(r->residual_tecu);
      weight += std::pow(std::sin(r->elevation_deg * D2R), 2) /
                std::pow(std::max(rms_floor, r->gim_rms_tecu), 2);
    }
    weight /= bin.size();
    x.push_back(median(v));
    w.push_back(weight);
    representatives.push_back(bin[bin.size() / 2]);
    arc_weight[key.first] += weight;
  }
  // Each arc has at most one unit of total weight; high-rate/long tracks do
  // not become thousands of independent bias measurements.
  for (size_t i = 0; i < w.size(); ++i)
    w[i] /= arc_weight.at(representatives[i]->arc_id);
  auto fit = robust(x, w, .5);
  std::set<int> satellites, quadrants;
  std::set<int64_t> arcs, time_bins;
  double low = 90, high = 0;
  for (size_t i = 0; i < x.size(); ++i)
    if (std::abs(x[i] - fit.value) <= std::max(3.0, 3 * fit.scatter)) {
      auto r = representatives[i];
      satellites.insert(r->prn);
      arcs.insert(r->arc_id);
      time_bins.insert(r->gpst_ns / bin_ns);
      quadrants.insert(int(r->azimuth_deg / 90) % 4);
      low = std::min(low, r->elevation_deg);
      high = std::max(high, r->elevation_deg);
    }
  double coverage = time_bins.size() * bin_seconds / 3600;
  bool valid = satellites.size() >= size_t(min_sat) &&
               arcs.size() >= size_t(min_arcs) && quadrants.size() >= 3 &&
               high - low >= 20 && coverage >= min_hours &&
               fit.scatter <= max_scatter;
  return {{"status", valid ? "estimated" : "insufficient_or_inconsistent"},
          {"receiver_bias_tecu", valid ? Json(fit.value) : Json(nullptr)},
          {"receiver_dcb_ns",
           valid ? Json(fit.value * K / CLIGHT * 1e9) : Json(nullptr)},
          {"candidate_bias_tecu",
           std::isfinite(fit.value) ? Json(fit.value) : Json(nullptr)},
          {"residual_scatter_tecu",
           std::isfinite(fit.scatter) ? Json(fit.scatter) : Json(nullptr)},
          {"bins", bins.size()},
          {"satellites", satellites.size()},
          {"arcs", arcs.size()},
          {"azimuth_quadrants", quadrants.size()},
          {"elevation_span_deg", std::max(0.0, high - low)},
          {"covered_hours", coverage}};
}
} // namespace neognss_obs
