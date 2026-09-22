// SPDX-License-Identifier: GPL-3.0-only
#include "rtklib.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <map>
#include <neognss_obs/antenna.hpp>
#include <neognss_obs/rtklib_lock.hpp>
#include <neognss_obs/stec.hpp>
#include <set>
#include <tuple>

extern "C" void ngo_combine_precise_clocks(nav_t *nav);

namespace neognss_obs {
namespace {
constexpr int64_t second = 1000000000LL;
unsigned signal_slot(std::string_view code) {
    if (code.size() != 2 || code[0] < '1' || code[0] > '9' || code[1] < 'A' ||
        code[1] > 'Z')
        return 0;
    return unsigned(code[0] - '0') * 32 + unsigned(code[1] - 'A');
}
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
    deviations.reserve(x.size());
    for (double v : x)
        deviations.push_back(std::abs(v - out.value));
    double scale = std::max(floor, 1.4826 * median(deviations));
    out.weights.resize(x.size());
    for (int k = 0; k < 20; ++k) {
        double sum = 0, norm = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            out.weights[i] =
                weights[i] *
                std::min(1.0, 1.5 * scale /
                                  std::max(1e-12, std::abs(x[i] - out.value)));
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

// Reuse RTKLIB's merge after each file, exactly as readrnxc() does.
void append_clocks(nav_t &nav, const std::vector<pclk_t> &clocks) {
    size_t count = size_t(nav.nc) + clocks.size();
    if (count > size_t(INT_MAX))
        throw std::overflow_error("Too many precise clock records");
    auto buffer =
        static_cast<pclk_t *>(std::realloc(nav.pclk, count * sizeof(pclk_t)));
    if (!buffer)
        throw std::bad_alloc();
    nav.pclk = buffer;
    std::copy(clocks.begin(), clocks.end(), nav.pclk + nav.nc);
    nav.nc = nav.ncmax = int(count);
    ngo_combine_precise_clocks(&nav);
    if (!nav.pclk || nav.nc <= 0)
        throw std::bad_alloc();
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
            // readtec uses zero for unavailable values; conservatively reject
            // both.
            if (!(m.data[n] > 0) || !std::isfinite(m.data[n]) ||
                !(m.rms[n] > 0))
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
    struct Pair {
        int id, family;
        char system;
        std::string first, second;
        double f1, f2, k;
        unsigned first_slot = 0, second_slot = 0;
    };
    struct Track {
        int pair_id = -1, prn = 0;
        double first_gf = 0;
        int64_t id, start, last, emitted = -1, samples = 0;
        int reason;
        double gf;
        neognss_obs::Observation a, b;
        std::vector<double> offsets, weights;
        int64_t first_level = -1, last_level = -1;
        int64_t receiver_segment_start_ns = 0;
    };
    Json settings;
    std::vector<Pair> pairs;
    std::map<char, std::map<int, std::vector<const Pair *>>> pair_families;
    std::array<std::vector<int>, MAXSAT> health_index;
    std::map<int, ReceiverAntenna> antennas;
    bool antenna_required;
    std::unique_ptr<nav_t> nav = std::make_unique<nav_t>();
    std::map<std::string, std::vector<pclk_t>> clock_cache;
    std::map<std::pair<int, int>, std::vector<Bias>> biases;
    std::map<int, Track> tracks;
    double rr[3], pos[3], elevation, level_elevation, mapping_height;
    double gf_jump, gf_rate;
    int64_t previous = -1, last_emit = -1, next_arc = 0, interval, gap;
    std::vector<int64_t> restart_boundaries;
    size_t next_restart = 0;
    int64_t receiver_segment_start_ns = 0;
    std::map<int, uint64_t> product_gaps{{1, 0}, {2, 0},  {4, 0},
                                         {8, 0}, {16, 0}, {32, 0}};
    uint64_t epochs = 0, sample_count = 0, arcs = 0, valid_arcs = 0,
             unhealthy = 0, missing_gim = 0;
    bool ready = false, finished = false;
    explicit State(const Json &s)
        : settings(s), antenna_required(s.at("antenna_required")) {
        for (const auto &[key, model] : s.at("antennas").items())
            antennas.emplace(std::stoi(key), ReceiverAntenna(model));
        for (const auto &p : s.at("pairs")) {
            const std::string sys = p.at("system");
            Pair pair{p.at("id"),
                      p.at("family_id"),
                      sys.at(0),
                      p.at("signal1"),
                      p.at("signal2"),
                      p.at("frequency1_hz"),
                      p.at("frequency2_hz"),
                      p.at("meters_per_tecu")};
            if (pair.id != int(pairs.size()) || sys.size() != 1 ||
                std::string("GECJ").find(pair.system) == std::string::npos ||
                pair.family < 0 || pair.family >= 16 ||
                !antennas.contains(pair.family) ||
                !obs2code(pair.first.c_str()) ||
                !obs2code(pair.second.c_str()) || !std::isfinite(pair.f1) ||
                !std::isfinite(pair.f2) || pair.f1 <= pair.f2 || pair.f2 <= 0 ||
                !std::isfinite(pair.k) ||
                std::abs(pair.k - 40.3e16 * (1 / (pair.f2 * pair.f2) -
                                             1 / (pair.f1 * pair.f1))) > 1e-12)
                throw std::invalid_argument(
                    "Invalid automatic STEC pair contract");
            pair.first_slot = signal_slot(pair.first);
            pair.second_slot = signal_slot(pair.second);
            if (!pair.first_slot || !pair.second_slot)
                throw std::invalid_argument("Invalid STEC signal code");
            pairs.push_back(std::move(pair));
        }
        if (pairs.empty())
            throw std::invalid_argument("Empty STEC pair selection");
        for (const auto &pair : pairs)
            pair_families[pair.system][pair.family].push_back(&pair);
        interval = std::llround(positive(s, "interval") * second);
        gap = std::llround(positive(s, "gap_timeout") * second);
        if (interval < 1 || gap < 1)
            throw std::invalid_argument("Interval below nanosecond resolution");
        elevation = positive(s, "elevation_deg");
        level_elevation = positive(s, "level_elevation_deg");
        if (elevation >= 90 || level_elevation >= 90 ||
            level_elevation < elevation)
            throw std::invalid_argument("Invalid elevation masks");
        mapping_height = positive(s, "mapping_height_km");
        gf_jump = positive(s, "gf_jump_m");
        gf_rate = positive(s, "gf_rate_m_s");
        positive(s, "min_arc_seconds");
        positive(s, "min_level_samples");
        for (int i = 0; i < 3; ++i)
            rr[i] = s.at("position_ecef_m").at(i);
        if (!std::isfinite(norm(rr, 3)) || norm(rr, 3) < 6e6 ||
            norm(rr, 3) > 7e6)
            throw std::invalid_argument("Invalid antenna ECEF position");
        ecef2pos(rr, pos);
    }
    ~State() { free_products(*nav); }
    double bias(int pair, int prn, int64_t t) const {
        auto found = biases.find({pair, prn});
        if (found != biases.end())
            for (const auto &b : found->second)
                if (b.start <= t && t < b.end)
                    return b.meters;
        return NAN;
    }
    StecArc estimate(int prn, const Track &a, int reason,
                     bool provisional) const {
        auto fit = robust(a.offsets, a.weights, 0.1);
        bool valid = a.offsets.size() >=
                         settings.at("min_level_samples").get<size_t>() &&
                     double(a.last_level - a.first_level) / second >=
                         settings.at("min_arc_seconds").get<double>();
        return {a.start,
                a.last,
                a.id,
                a.samples,
                int64_t(a.offsets.size()),
                a.receiver_segment_start_ns,
                a.prn,
                int(pairs.at(a.pair_id).system),
                a.pair_id,
                int(valid),
                a.reason,
                reason,
                int(provisional),
                valid ? fit.value : NAN,
                fit.scatter};
    }
    void close(int prn, int reason, std::vector<StecArc> &out) {
        auto found = tracks.find(prn);
        if (found == tracks.end())
            return;
        auto value = estimate(prn, found->second, reason, false);
        out.push_back(value);
        ++arcs;
        valid_arcs += value.valid;
        tracks.erase(found);
    }
    bool geometry(int sat, int64_t ns, double &az, double &el, double &lat,
                  double &lon, int32_t &issues) {

        gtime_t t = time_of(ns);
        const eph_t *health = nullptr;
        double age = 7201;
        const auto &indices = health_index.at(sat - 1);
        auto lower = [&](gtime_t when) {
            return std::lower_bound(indices.begin(), indices.end(), when,
                                    [&](int i, gtime_t v) {
                                        return timediff(nav->eph[i].toe, v) < 0;
                                    });
        };
        auto consider = [&](int i) {
            auto candidate = nav->eph + i;
            double d = std::abs(timediff(t, candidate->toe));
            // Preserve the original scan's first-index tie rule.
            if (d < age || (health && d == age && candidate < health)) {
                health = candidate;
                age = d;
            }
        };
        auto next = lower(t);
        if (next != indices.end())
            consider(*next);
        if (next != indices.begin())
            consider(*lower(nav->eph[*std::prev(next)].toe));
        if (!health || age > 7200) {
            issues |= 4;
            return true;
        }
        // Reuse constellation-specific health interpretation, including the
        // QZSS LEX-only bit which must not reject L1/L2/L5 ranging
        // observations.
        if (satexclude(sat, 0.0, health->svh, nullptr)) {
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
            // Match RTKLIB's 11-point orbit stencil. Do not interpolate across
            // a missing day merely because two distant products bracket the
            // epoch.
            auto orbit = std::lower_bound(nav->peph, nav->peph + nav->ne, tx,
                                          [](const peph_t &a, gtime_t b) {
                                              return timediff(a.time, b) < 0;
                                          });
            int index = std::max(0, int(orbit - nav->peph) - 1);
            int first = std::clamp(index - 5, 0, nav->ne - 11);
            for (int j = first + 1; j < first + 11; ++j) {
                double separation =
                    timediff(nav->peph[j].time, nav->peph[j - 1].time);
                if (separation <= 0 || separation > 301) {
                    issues |= 1;
                    return true;
                }
            }
            auto upper = std::lower_bound(nav->pclk, nav->pclk + nav->nc, tx,
                                          [](const pclk_t &a, gtime_t b) {
                                              return timediff(a.time, b) < 0;
                                          });
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
    std::set<std::string> clock_files;
    for (const auto &f : p.at("clk"))
        clock_files.insert(f.get<std::string>());
    std::erase_if(s.clock_cache, [&](const auto &entry) {
        return !clock_files.contains(entry.first);
    });
    for (const auto &f : p.at("clk")) {
        auto path = f.get<std::string>();
        auto found = s.clock_cache.find(path);
        if (found == s.clock_cache.end()) {
            auto parsed = std::unique_ptr<nav_t, void (*)(nav_t *)>(
                new nav_t{}, [](nav_t *v) {
                    free_products(*v);
                    delete v;
                });
            if (!readrnxc(path.c_str(), parsed.get()))
                throw std::runtime_error("Cannot read STEC CLK");
            found =
                s.clock_cache
                    .emplace(path, std::vector<pclk_t>(
                                       parsed->pclk, parsed->pclk + parsed->nc))
                    .first;
        }
        append_clocks(*s.nav, found->second);
    }
    obs_t unused{};
    sta_t sta{};
    for (const auto &f : p.at("nav")) {
        int ok = readrnx(f.get<std::string>().c_str(), 1, "", &unused,
                         s.nav.get(), &sta);
        freeobs(&unused);
        if (ok <= 0)
            throw std::runtime_error("Cannot read STEC BRDC");
    }
    uniqnav(s.nav.get());
    for (auto &indices : s.health_index)
        indices.clear();
    for (int i = 0; i < s.nav->n; ++i)
        if (s.nav->eph[i].sat >= 1 && s.nav->eph[i].sat <= MAXSAT)
            s.health_index[s.nav->eph[i].sat - 1].push_back(i);
    for (auto &indices : s.health_index)
        std::sort(indices.begin(), indices.end(), [&](int a, int b) {
            double delta = timediff(s.nav->eph[a].toe, s.nav->eph[b].toe);
            return delta < 0 || (delta == 0 && a < b);
        });
    for (const auto &f : p.at("ionex")) {
        int before = s.nav->nt;
        readtec(f.get<std::string>().c_str(), s.nav.get(), 1);
        if (s.nav->nt <= before)
            throw std::runtime_error("Cannot read nonempty STEC IONEX: " +
                                     f.get<std::string>());
    }
    for (int i = 0; i < s.nav->nt; ++i) {
        auto &m = s.nav->tec[i];
        if (m.ndata[2] != 1 || m.rb != 6371 || m.hgts[0] != 450 ||
            m.lons[2] <= 0 || m.lats[2] >= 0)
            throw std::runtime_error(
                "Expected CODE 450 km single-layer IONEX grid");
        // readtec leaves external UT calendar epochs unchanged. Convert once
        // here.
        m.time = utc2gpst(m.time);
    }
    s.biases.clear();
    auto utc_ns = [](int64_t ns) -> int64_t {
        auto utc = time_of(ns);
        return ns + std::llround(timediff(utc2gpst(utc), utc) * second);
    };
    for (const auto &b : p.at("biases")) {
        int64_t start = b.at("start_ns"), end = b.at("end_ns");
        if (b.contains("start_utc_ns")) {
            start = std::max(start, utc_ns(b.at("start_utc_ns")));
            end = std::min(end, utc_ns(b.at("end_utc_ns")));
        }
        double meters = b.at("meters");
        int pair = b.at("pair_id"), prn = b.at("prn");
        if (pair < 0 || pair >= int(s.pairs.size()) || prn < 1 || prn > 99 ||
            !std::isfinite(meters))
            throw std::invalid_argument("Invalid satellite pair bias");
        if (start < end)
            s.biases[{pair, prn}].push_back({start, end, meters});
    }
    for (auto &[key, values] : s.biases) {
        std::sort(
            values.begin(), values.end(),
            [](const auto &a, const auto &b) { return a.start < b.start; });
        for (size_t i = 1; i < values.size(); ++i)
            if (values[i].start < values[i - 1].end)
                throw std::invalid_argument(
                    "Overlapping satellite pair bias intervals");
    }
    s.ready = true;
}
void StecProcessor::restarts(std::span<const int64_t> boundaries) {
    std::lock_guard guard(rtklib_mutex);
    auto &s = *state_;
    if (s.finished)
        throw std::runtime_error("Cannot schedule restart after STEC finish");
    auto merged = s.restart_boundaries;
    for (int64_t ns : boundaries) {
        if (ns < 0)
            throw std::invalid_argument("Negative receiver restart GPST");
        if (std::binary_search(s.restart_boundaries.begin(),
                               s.restart_boundaries.end(), ns))
            continue;
        if (ns <= s.previous)
            throw std::runtime_error(
                "New receiver restart precedes processed observations; rebuild "
                "the complete selection");
        merged.push_back(ns);
    }
    std::sort(merged.begin(), merged.end());
    merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
    s.restart_boundaries = std::move(merged);
}

StecResult StecProcessor::process(const ObservationBatch &batch) {
    std::lock_guard guard(rtklib_mutex);
    auto &s = *state_;
    StecResult out;
    if (!s.ready || s.finished)
        throw std::runtime_error("STEC processor is not ready");
    std::vector<const Observation *> observations;
    std::vector<double> pair_bias(s.pairs.size());
    std::vector<uint64_t> bias_generation(s.pairs.size(), 0);
    uint64_t generation = 0;
    for (const auto &epoch : batch.epochs) {
        if (epoch.gpst_ns <= s.previous)
            throw std::runtime_error("Non-increasing STEC observation time");
        bool restarted = false;
        while (s.next_restart < s.restart_boundaries.size() &&
               s.restart_boundaries[s.next_restart] <= epoch.gpst_ns) {
            while (!s.tracks.empty())
                s.close(s.tracks.begin()->first, 11, out.arcs);
            s.receiver_segment_start_ns =
                s.restart_boundaries[s.next_restart++];
            s.last_emit = -1;
            restarted = true;
        }
        s.previous = epoch.gpst_ns;
        ++s.epochs;
        bool emit =
            s.last_emit < 0 || epoch.gpst_ns - s.last_emit >= s.interval;
        if (emit)
            s.last_emit = epoch.gpst_ns;
        std::set<int> timed_out;
        for (auto it = s.tracks.begin(); it != s.tracks.end();) {
            auto current = it++;
            if (epoch.gpst_ns - current->second.last > s.gap) {
                timed_out.insert(current->first);
                s.close(current->first, 1, out.arcs);
            }
        }
        observations.clear();
        observations.reserve(epoch.signals.size());
        for (const auto &m : epoch.signals) {
            if (m.antenna != 0)
                continue;
            observations.push_back(&m);
        }
        std::sort(observations.begin(), observations.end(), [](auto a, auto b) {
            return std::tie(a->system, a->prn, a->signal) <
                   std::tie(b->system, b->prn, b->signal);
        });
        for (size_t i = 1; i < observations.size(); ++i) {
            auto a = observations[i - 1], b = observations[i];
            if (std::tie(a->system, a->prn, a->signal) ==
                std::tie(b->system, b->prn, b->signal))
                throw std::runtime_error("Duplicate STEC observation identity");
        }
        for (size_t begin = 0, end; begin < observations.size(); begin = end) {
            auto system = observations[begin]->system;
            auto prn = observations[begin]->prn;
            for (end = begin + 1; end < observations.size() &&
                                  observations[end]->system == system &&
                                  observations[end]->prn == prn;
                 ++end) {
            }
            std::array<const Observation *, 320> signals{};
            ++generation;
            auto bias_value = [&](int id) {
                if (bias_generation[id] != generation) {
                    pair_bias[id] = s.bias(id, prn, epoch.gpst_ns);
                    bias_generation[id] = generation;
                }
                return pair_bias[id];
            };
            for (size_t i = begin; i < end; ++i)
                if (auto slot = signal_slot(observations[i]->signal))
                    signals[slot] = observations[i];
            int sys = system == 'G'   ? SYS_GPS
                      : system == 'E' ? SYS_GAL
                      : system == 'C' ? SYS_CMP
                      : system == 'J' ? SYS_QZS
                                      : 0;
            int sat = satno(sys, system == 'J' ? prn + 192 : prn);
            if (!sat)
                throw std::runtime_error("Unsupported STEC satellite identity");
            // Geometry and GIM are shared by all pair families for this
            // satellite.
            bool computed = false, usable = true;
            double az = NAN, el = NAN, lat = NAN, lon = NAN, mapping = NAN,
                   gim = NAN, rms = NAN;
            int32_t geometry_issues = 0;
            auto families = s.pair_families.find(system);
            if (families == s.pair_families.end())
                continue;
            for (const auto &[family, candidates] : families->second) {
                int key = sat * 16 + family;
                auto old = s.tracks.find(key);
                auto available = [&](const State::Pair &p) {
                    auto a = signals[p.first_slot], b = signals[p.second_slot];
                    return a && b && a->phase_valid && b->phase_valid &&
                           !a->half_cycle && !b->half_cycle;
                };
                const State::Pair *selected = nullptr;
                auto calibrated = [&](const State::Pair &p) {
                    return available(p) && signals[p.first_slot]->code_valid &&
                           signals[p.second_slot]->code_valid &&
                           std::isfinite(bias_value(p.id));
                };
                // Sticky among covered codes. If coverage disappears, prefer
                // a covered alternative; otherwise retain raw phase continuity.
                if (old != s.tracks.end() &&
                    calibrated(s.pairs[old->second.pair_id]))
                    selected = &s.pairs[old->second.pair_id];
                if (!selected)
                    for (auto candidate : candidates)
                        if (calibrated(*candidate)) {
                            selected = candidate;
                            break;
                        }
                if (!selected && old != s.tracks.end() &&
                    available(s.pairs[old->second.pair_id]))
                    selected = &s.pairs[old->second.pair_id];
                if (!selected)
                    for (auto candidate : candidates)
                        if (available(*candidate)) {
                            selected = candidate;
                            break;
                        }
                if (!selected) {
                    if (old != s.tracks.end()) {
                        const auto &active = s.pairs[old->second.pair_id];
                        for (auto slot :
                             {active.first_slot, active.second_slot}) {
                            auto found = signals[slot];
                            if (found && found->half_cycle) {
                                s.close(key, 3, out.arcs);
                                break;
                            }
                        }
                    }
                    continue;
                }
                const auto &p = *selected;
                const auto *a = signals[p.first_slot],
                           *b = signals[p.second_slot];
                double gf = CLIGHT / p.f1 * a->phase_cycles -
                            CLIGHT / p.f2 * b->phase_cycles;
                int reason = restarted ? 11 : timed_out.contains(key) ? 1 : 0;
                auto &antenna = s.antennas.at(family);
                auto antenna_record = [&](int64_t ns) -> int64_t {
                    if (!antenna.enabled())
                        return -1;
                    try {
                        return antenna.record_index(ns);
                    } catch (const std::runtime_error &) {
                        return -1;
                    }
                };
                if (old != s.tracks.end()) {
                    auto &t = old->second;
                    double dt = double(epoch.gpst_ns - t.last) / second;
                    if (epoch.gpst_ns - t.last > s.gap)
                        reason = 1;
                    else if (a->loss_of_lock || b->loss_of_lock ||
                             (a->lock_valid && t.a.lock_valid &&
                              a->lock_seconds < t.a.lock_seconds) ||
                             (b->lock_valid && t.b.lock_valid &&
                              b->lock_seconds < t.b.lock_seconds))
                        reason = 2;
                    else if ((a->continuity_counter && t.a.continuity_counter &&
                              a->continuity_counter !=
                                  t.a.continuity_counter) ||
                             (b->continuity_counter && t.b.continuity_counter &&
                              b->continuity_counter != t.b.continuity_counter))
                        reason = 6;
                    else if (a->sub_half_cycle != t.a.sub_half_cycle ||
                             b->sub_half_cycle != t.b.sub_half_cycle)
                        reason = 3;
                    else if (std::abs(gf - t.gf) > s.gf_jump + s.gf_rate * dt)
                        reason = 4;

                    if (p.id != t.pair_id)
                        reason = 9;
                    if (epoch.clock_reset)
                        reason = 10;
                    if (antenna_record(t.last) != antenna_record(epoch.gpst_ns))
                        reason = 8;
                    if (reason)
                        s.close(key, reason, out.arcs);
                }
                auto [it, inserted] = s.tracks.try_emplace(key);
                auto &t = it->second;
                if (inserted) {
                    t.id = s.next_arc++;
                    t.start = epoch.gpst_ns;
                    t.reason = reason;
                    t.pair_id = p.id;
                    t.prn = prn;
                    t.first_gf = gf;
                    t.receiver_segment_start_ns = s.receiver_segment_start_ns;
                }
                t.last = epoch.gpst_ns;
                t.a = *a;
                t.b = *b;
                t.gf = gf;
                if (!emit)
                    continue;
                t.emitted = epoch.gpst_ns;
                if (!computed) {
                    computed = true;
                    usable = s.geometry(sat, epoch.gpst_ns, az, el, lat, lon,
                                        geometry_issues);
                    double rp = 6371 / (6371 + s.mapping_height) *
                                std::sin(.9782 * (PI / 2 - el * D2R));
                    mapping = 1 / std::sqrt(1 - rp * rp);
                    if (std::isfinite(mapping)) {
                        std::tie(gim, rms) =
                            s.gim(epoch.gpst_ns, lat, lon, mapping);
                        if (!std::isfinite(gim)) {
                            geometry_issues |= 16;
                            ++s.missing_gim;
                        }
                    }
                }
                if (!usable)
                    continue;
                double raw = gf;
                int32_t issues = geometry_issues;
                if (s.antenna_required &&
                    (!antenna.enabled() || antenna_record(epoch.gpst_ns) < 0)) {
                    gf = NAN;
                    issues |= 32;
                } else if (!antenna.covers(epoch.gpst_ns, el)) {
                    gf = NAN;
                    if (std::isfinite(el))
                        issues |= 32;
                } else
                    gf -= antenna.correction(0, epoch.gpst_ns, az, el) -
                          antenna.correction(1, epoch.gpst_ns, az, el);
                double code = NAN;
                if (a->code_valid && b->code_valid)
                    code =
                        b->pseudorange_m - a->pseudorange_m - bias_value(p.id);
                if (!std::isfinite(bias_value(p.id)))
                    issues |= 8;
                for (auto &[bit, count] : s.product_gaps)
                    if (issues & bit)
                        ++count;
                out.samples.push_back(
                    {epoch.gpst_ns, t.id, s.receiver_segment_start_ns, prn,
                     int(system), p.id, issues, raw, (raw - t.first_gf) / p.k,
                     gf, code, el, az, lat, lon, mapping, gim, rms});
                ++t.samples;
                ++s.sample_count;
                if (std::isfinite(code) && std::isfinite(gf) &&
                    el >= s.level_elevation) {
                    t.offsets.push_back(code - gf);
                    t.weights.push_back(std::pow(std::sin(el * D2R), 2));
                    if (t.first_level < 0)
                        t.first_level = epoch.gpst_ns;
                    t.last_level = epoch.gpst_ns;
                }
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
            {"receiver_restarts_applied", s.next_restart},
            {"receiver_restarts_pending",
             s.restart_boundaries.size() - s.next_restart},
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
              {"gim", s.product_gaps.at(16)},
              {"receiver_antenna_direction", s.product_gaps.at(32)}}},
            {"pairs", s.settings.at("pairs")},
            {"arc_reasons",
             {{"0", "start"},
              {"1", "timeout"},
              {"2", "lock_decrease"},
              {"3", "half_cycle_change"},
              {"4", "geometry_free_jump_candidate"},
              {"5", "stream_end"},
              {"6", "continuity_counter_change"},
              {"7", "open_incremental_tail"},
              {"8", "receiver_antenna_calibration_change"},
              {"9", "exact_signal_pair_change"},
              {"10", "receiver_clock_reset"},
              {"11", "receiver_restart"}}}};
}

std::vector<StecArc> StecProcessor::preview() const {
    std::vector<StecArc> out;
    for (const auto &[prn, track] : state_->tracks)
        out.push_back(state_->estimate(prn, track, 7, true));
    return out;
}
Json StecProcessor::checkpoint() const {
    const auto &s = *state_;
    Json tracks = Json::array();
    auto tracking = [](const neognss_obs::Observation &m) {
        return Json{{"lock_seconds", m.lock_seconds},
                    {"lock_valid", m.lock_valid},
                    {"sub_half_cycle", m.sub_half_cycle},
                    {"counter", m.continuity_counter
                                    ? Json(*m.continuity_counter)
                                    : Json(nullptr)}};
    };
    for (const auto &[prn, t] : s.tracks)
        tracks.push_back(
            {{"key", prn},
             {"prn", t.prn},
             {"pair_id", t.pair_id},
             {"first_gf", t.first_gf},
             {"receiver_segment_start_ns", t.receiver_segment_start_ns},
             {"id", t.id},
             {"start", t.start},
             {"last", t.last},
             {"emitted", t.emitted},
             {"samples", t.samples},
             {"reason", t.reason},
             {"gf", t.gf},
             {"a", tracking(t.a)},
             {"b", tracking(t.b)},
             {"offsets", t.offsets},
             {"weights", t.weights},
             {"first_level", t.first_level},
             {"last_level", t.last_level}});
    return {{"version", 4},
            {"restart_boundaries", s.restart_boundaries},
            {"next_restart", s.next_restart},
            {"receiver_segment_start_ns", s.receiver_segment_start_ns},
            {"last_emit", s.last_emit},
            {"settings", s.settings},
            {"previous", s.previous},
            {"next_arc", s.next_arc},
            {"epochs", s.epochs},
            {"samples", s.sample_count},
            {"arcs", s.arcs},
            {"valid_arcs", s.valid_arcs},
            {"unhealthy", s.unhealthy},
            {"missing_gim", s.missing_gim},
            {"product_gaps", s.product_gaps},
            {"tracks", tracks}};
}
void StecProcessor::restore(const Json &j) {
    auto &s = *state_;
    if (j.at("version") != 4 || j.at("settings") != s.settings ||
        s.previous != -1)
        throw std::runtime_error("Incompatible STEC continuation state");
    s.previous = j.at("previous");
    s.restart_boundaries =
        j.at("restart_boundaries").get<std::vector<int64_t>>();
    s.next_restart = j.at("next_restart");
    s.receiver_segment_start_ns = j.at("receiver_segment_start_ns");
    if (!std::is_sorted(s.restart_boundaries.begin(),
                        s.restart_boundaries.end()) ||
        std::adjacent_find(s.restart_boundaries.begin(),
                           s.restart_boundaries.end()) !=
            s.restart_boundaries.end() ||
        (!s.restart_boundaries.empty() && s.restart_boundaries.front() < 0) ||
        s.next_restart !=
            size_t(std::upper_bound(s.restart_boundaries.begin(),
                                    s.restart_boundaries.end(), s.previous) -
                   s.restart_boundaries.begin()) ||
        s.receiver_segment_start_ns !=
            (s.next_restart ? s.restart_boundaries[s.next_restart - 1] : 0))
        throw std::runtime_error("Invalid receiver restart checkpoint");
    s.last_emit = j.at("last_emit");
    s.next_arc = j.at("next_arc");
    s.epochs = j.at("epochs");
    s.sample_count = j.at("samples");
    s.arcs = j.at("arcs");
    s.valid_arcs = j.at("valid_arcs");
    s.unhealthy = j.at("unhealthy");
    s.missing_gim = j.at("missing_gim");
    s.product_gaps = j.at("product_gaps").get<std::map<int, uint64_t>>();
    auto tracking = [](const Json &v) {
        neognss_obs::Observation m;
        m.lock_seconds = v.at("lock_seconds");
        m.lock_valid = v.at("lock_valid");
        m.sub_half_cycle = v.at("sub_half_cycle");
        if (!v.at("counter").is_null())
            m.continuity_counter = v.at("counter").get<uint32_t>();
        return m;
    };
    for (const auto &v : j.at("tracks")) {
        State::Track t;
        t.prn = v.at("prn");
        t.pair_id = v.at("pair_id");
        t.first_gf = v.at("first_gf");
        t.receiver_segment_start_ns = v.at("receiver_segment_start_ns");
        if (t.receiver_segment_start_ns != s.receiver_segment_start_ns)
            throw std::runtime_error("STEC track crosses a receiver restart");
        if (t.pair_id < 0 || t.pair_id >= int(s.pairs.size()) ||
            !std::isfinite(t.first_gf))
            throw std::runtime_error("Invalid STEC pair checkpoint");
        t.id = v.at("id");
        t.start = v.at("start");
        t.last = v.at("last");
        t.emitted = v.at("emitted");
        t.samples = v.at("samples");
        t.reason = v.at("reason");
        t.gf = v.at("gf");
        t.a = tracking(v.at("a"));
        t.b = tracking(v.at("b"));
        t.offsets = v.at("offsets").get<std::vector<double>>();
        t.weights = v.at("weights").get<std::vector<double>>();
        t.first_level = v.at("first_level");
        t.last_level = v.at("last_level");
        if (t.offsets.size() != t.weights.size() ||
            t.start < t.receiver_segment_start_ns || t.start > t.last ||
            t.last > s.previous || !std::isfinite(t.gf) || t.id < 0 ||
            t.id >= s.next_arc)
            throw std::runtime_error("Invalid STEC track checkpoint");
        s.tracks.emplace(v.at("key").get<int>(), std::move(t));
    }
}

Json fit_receiver_dcb(std::span<const DcbSample> rows, const Json &settings) {
    double K = positive(settings, "meters_per_tecu");
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
