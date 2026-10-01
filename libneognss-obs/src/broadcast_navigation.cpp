// SPDX-License-Identifier: GPL-3.0-only
#include "arrow_batch.hpp"
#include "broadcast_beidou.hpp"
#include "broadcast_galileo.hpp"
#include "rtklib.h"
#include <algorithm>
#include <array>
#include <boost/int128/int128.hpp>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <neognss_obs/broadcast_navigation.hpp>
#include <neognss_obs/rtklib_lock.hpp>
#include <optional>

namespace neognss_obs {
namespace {
constexpr int64_t second = 1000000000;
ArrowArrayView *field(ArrowArrayView *v, const ArrowSchema *s,
                      const char *name) {
    for (int64_t i = 0; i < s->n_children; ++i)
        if (s->children[i]->name &&
            std::strcmp(s->children[i]->name, name) == 0)
            return v->children[i];
    throw std::runtime_error(std::string("Missing RawBits field: ") + name);
}
std::string_view text(ArrowArrayView *v, int64_t row) {
    if (v->storage_type != NANOARROW_TYPE_STRING ||
        ArrowArrayViewIsNull(v, row))
        throw std::runtime_error("Expected RawBits string");
    auto x = ArrowArrayViewGetStringUnsafe(v, row);
    return {x.data, size_t(x.size_bytes)};
}
std::optional<int64_t> timestamp(ArrowArrayView *v, int64_t row) {
    if (v->storage_type != NANOARROW_TYPE_DECIMAL128)
        throw std::runtime_error("Expected RawBits decimal128(38,12) GPST");
    if (ArrowArrayViewIsNull(v, row))
        return {};
    ArrowDecimal d;
    ArrowDecimalInit(&d, 128, 38, 12);
    ArrowArrayViewGetDecimalUnsafe(v, row, &d);
    using Tick = boost::int128::int128;
    Tick ticks =
        (Tick(d.words[d.high_word_index]) << 64) + d.words[d.low_word_index];
    if (ticks < 0 || ticks / 1000 > INT64_MAX)
        throw std::runtime_error("RawBits time outside processing range");
    auto ns = ticks / 1000, rem = ticks % 1000;
    if (rem > 500 || (rem == 500 && (ns & 1) != 0))
        ++ns;
    if (ns > INT64_MAX)
        throw std::runtime_error("RawBits time overflow");
    return int64_t(ns);
}
gtime_t time_of(int64_t ns) {
    return gpst2time(int(ns / (604800 * second)),
                     double(ns % (604800 * second)) / second);
}
} // namespace
struct BroadcastNavigation::State {
    struct Assembly {
        std::array<uint8_t, 380> bytes{};
        std::array<int64_t, 10> times;
        Assembly() { times.fill(-1); }
    };
    struct Entry {
        eph_t eph;
        int64_t available;
        std::string family;
    };
    std::string setup;
    mutable std::mutex mutex;
    std::map<std::pair<int, std::string>, Assembly> pending;
    broadcast_detail::GalileoDecoder galileo;
    broadcast_detail::BeidouDecoder beidou;
    std::map<std::string, uint64_t> galileo_diagnostics;
    std::map<int, std::deque<Entry>> ephemerides;
    uint64_t count = 0;
    std::map<std::string, uint64_t> family_counts;
    explicit State(std::string id) : setup(std::move(id)) {}
    void publish(eph_t eph, int sat, int64_t hi, std::string_view fam) {
        eph.sat = sat;
        if (eph.toes < 0 || eph.toes >= 604800 ||
            std::abs(timediff(time_of(hi), eph.ttr)) > 120 ||
            !std::isfinite(eph.A) || eph.A < 1e7 || eph.A > 5e7 ||
            !std::isfinite(eph.e) || eph.e < 0 || eph.e >= 1)
            return;
        auto &cache = ephemerides[sat];
        auto previous =
            std::find_if(cache.rbegin(), cache.rend(),
                         [&](const auto &e) { return e.family == fam; });
        if (previous != cache.rend() && previous->eph.iode == eph.iode &&
            previous->eph.iodc == eph.iodc && previous->eph.svh == eph.svh &&
            previous->eph.fit == eph.fit &&
            timediff(previous->eph.toe, eph.toe) == 0 &&
            timediff(previous->eph.toc, eph.toc) == 0)
            return;
        cache.push_back({eph, hi, std::string(fam)});
        if (std::count_if(cache.begin(), cache.end(),
                          [&](const auto &e) { return e.family == fam; }) > 8)
            cache.erase(
                std::find_if(cache.begin(), cache.end(),
                             [&](const auto &e) { return e.family == fam; }));
        ++count;
        ++family_counts[std::string(fam)];
    }
};
BroadcastNavigation::BroadcastNavigation(std::string setup)
    : state_(std::make_unique<State>(std::move(setup))) {}
BroadcastNavigation::~BroadcastNavigation() = default;
void BroadcastNavigation::clear() {
    std::lock_guard lock(state_->mutex);
    state_->pending.clear();
    state_->galileo.clear();
    state_->beidou.clear();
    state_->ephemerides.clear();
}
uint64_t BroadcastNavigation::decoded() const {
    std::lock_guard lock(state_->mutex);
    return state_->count;
}
std::map<std::string, uint64_t> BroadcastNavigation::decoded_by_family() const {
    std::lock_guard lock(state_->mutex);
    return state_->family_counts;
}
void BroadcastNavigation::feed(ArrowSchema *schema, ArrowArray *array) {
    std::lock_guard rtklib_guard(rtklib_mutex);
    std::lock_guard guard(state_->mutex);
    ArrowBatchView owner(schema, array);
    for (int64_t i = 0; i < schema->n_children; ++i)
        if (schema->children[i]->name &&
            std::strcmp(schema->children[i]->name, "nav_epoch_gpst") == 0 &&
            std::strcmp(schema->children[i]->format, "d:38,12") != 0 &&
            std::strcmp(schema->children[i]->format, "d:38,12,128") != 0)
            throw std::runtime_error(
                "Expected decimal128(38,12) navigation time");
    auto v = &owner.value;
    auto f = [&](const char *n) { return field(v, schema, n); };
    auto setup = f("setup_id"), family = f("message_family"),
         format = f("body_format"), system = f("satellite_system"),
         number = f("satellite_number"), time = f("nav_epoch_gpst"),
         body = f("body"), length = f("bit_length"), checks = f("checks");
    if (body->storage_type != NANOARROW_TYPE_BINARY ||
        checks->storage_type != NANOARROW_TYPE_LIST ||
        number->storage_type != NANOARROW_TYPE_UINT16 ||
        length->storage_type != NANOARROW_TYPE_UINT32)
        throw std::runtime_error("Invalid RawBits column types");
    const ArrowSchema *cs = nullptr;
    for (int64_t i = 0; i < schema->n_children; ++i)
        if (schema->children[i]->name &&
            std::strcmp(schema->children[i]->name, "checks") == 0)
            cs = schema->children[i]->children[0];
    auto check = checks->children[0];
    auto result = field(check, cs, "result");
    for (int64_t i = 0; i < v->length; ++i) {
        auto row = i + v->offset;
        if (text(setup, row) != state_->setup)
            throw std::runtime_error("RawBits Setup mismatch");
        auto fam = text(family, row);
        const bool lnav = fam == "GPS_LNAV" || fam == "QZS_LNAV";
        const bool inav = fam == "GAL_INAV", fnav = fam == "GAL_FNAV";
        const bool d1 = fam == "BDS_D1", d2 = fam == "BDS_D2";
        if (!lnav && !inav && !fnav && !d1 && !d2)
            continue;
        auto t = timestamp(time, row);
        if (!t)
            continue;
        auto sys = text(system, row);
        std::string_view expected = fam == "GPS_LNAV"   ? "G"
                                    : fam == "QZS_LNAV" ? "J"
                                    : (inav || fnav)    ? "E"
                                                        : "C";
        if (sys != expected)
            throw std::runtime_error("Broadcast satellite system mismatch");
        int prn = int(ArrowArrayViewGetUIntUnsafe(number, row));
        int native_system = sys == "G"   ? SYS_GPS
                            : sys == "J" ? SYS_QZS
                            : sys == "E" ? SYS_GAL
                                         : SYS_CMP;
        int sat = satno(native_system, sys == "J" ? prn + 192 : prn);
        std::string_view expected_format = lnav   ? "LNAV_300_V1"
                                           : inav ? "INAV_228_V1"
                                           : fnav ? "FNAV_238_V1"
                                                  : "D1D2_300_V1";
        const unsigned bit_count = inav ? 228 : fnav ? 238 : 300;
        if (ArrowArrayViewIsNull(number, row) ||
            ArrowArrayViewIsNull(length, row) || !sat ||
            text(f("completeness"), row) != "complete" ||
            text(f("content_kind"), row) != "navigation_bits" ||
            text(format, row) != expected_format ||
            ArrowArrayViewGetUIntUnsafe(length, row) != bit_count)
            throw std::runtime_error(
                "Invalid broadcast canonical identity/layout");
        bool pass = false, fail = false;
        auto listrow = row + checks->offset;
        for (auto j = checks->buffer_views[1].data.as_int32[listrow];
             j < checks->buffer_views[1].data.as_int32[listrow + 1]; ++j) {
            auto status = text(result, j + check->offset);
            pass |= status == "pass";
            fail |= status == "fail";
        }
        if (!pass || fail)
            continue;
        if (ArrowArrayViewIsNull(body, row))
            throw std::runtime_error("Null broadcast body");
        auto bits = ArrowArrayViewGetBytesUnsafe(body, row);
        const auto *raw = bits.data.as_uint8;
        if (bits.size_bytes != (bit_count + 7) / 8 ||
            (raw[bits.size_bytes - 1] &
             ((1u << ((8 - bit_count % 8) % 8)) - 1)))
            throw std::runtime_error("Invalid broadcast body size/padding");
        if (inav || fnav || d1 || d2) {
            using namespace broadcast_detail;
            auto sources = f("bitstream_source");
            if (sources->storage_type != NANOARROW_TYPE_LIST ||
                ArrowArrayViewIsNull(sources, row))
                throw std::runtime_error(
                    "Missing broadcast signal contributors");
            std::vector<std::string> signals;
            for (auto j = ArrowArrayViewListChildOffset(sources,
                                                        row + sources->offset);
                 j < ArrowArrayViewListChildOffset(sources,
                                                   row + sources->offset + 1);
                 ++j)
                signals.emplace_back(text(sources->children[0], j));
            if (signals.empty())
                throw std::runtime_error("Empty broadcast signal contributors");
            std::sort(signals.begin(), signals.end());
            std::string key = std::to_string(sat) + "/" + std::string(fam);
            for (const auto &signal : signals)
                key += "/" + signal;
            auto decoded =
                (d1 || d2)
                    ? state_->beidou.decode(
                          {raw, size_t(bits.size_bytes)}, std::string(fam), key,
                          prn, Tick(*t) * 1000, state_->galileo_diagnostics)
                    : state_->galileo.decode(
                          {raw, size_t(bits.size_bytes)}, std::string(fam), key,
                          prn, signals.size() == 1 && signals[0] == "GAL_E1_B",
                          Tick(*t) * 1000, state_->galileo_diagnostics);
            for (const auto &message : decoded) {
                if (message.kind != "gal_inav_ephemeris" &&
                    message.kind != "gal_fnav_ephemeris" &&
                    message.kind != "bds_d1_ephemeris" &&
                    message.kind != "bds_d2_ephemeris")
                    continue;
                const auto &r = message.fields;
                if (!std::holds_alternative<Tick>(r.at("toe_gpst")) ||
                    !std::holds_alternative<Tick>(r.at("toc_gpst")) ||
                    !std::holds_alternative<Tick>(r.at("transmission_gpst")))
                    continue;
                eph_t eph{};
                auto d = [&](const char *n) {
                    return std::get<double>(r.at(n));
                };
                auto u = [&](const char *n) {
                    return int(std::get<int64_t>(r.at(n)));
                };
                auto ts = [&](const char *n) {
                    auto tick = std::get<Tick>(r.at(n));
                    return gpst2time(int(tick / seconds(604800)),
                                     double(tick % seconds(604800)) /
                                         double(ps));
                };
                if (d1 || d2) {
                    eph.iodc = u("aodc");
                    // Preserve RTKLIB's legacy issue convention at this adapter
                    // only.
                    eph.iode =
                        int(std::get<Tick>(r.at("toc_bdt_s")) / seconds(720)) %
                        240;
                } else
                    eph.iode = eph.iodc = u("iod_nav");
                eph.A = d("sqrt_a") * d("sqrt_a");
                eph.e = d("eccentricity");
                eph.M0 = d("m0_rad");
                eph.OMG0 = d("omega0_rad");
                eph.i0 = d("i0_rad");
                eph.omg = d("omega_rad");
                eph.OMGd = d("omega_dot_rad_s");
                eph.idot = d("idot_rad_s");
                eph.deln = d("delta_n_rad_s");
                eph.cuc = d("cuc_rad");
                eph.cus = d("cus_rad");
                eph.crc = d("crc_m");
                eph.crs = d("crs_m");
                eph.cic = d("cic_rad");
                eph.cis = d("cis_rad");
                eph.f0 = double(std::get<Tick>(r.at("af0_s"))) / ps;
                eph.f1 = d("af1_s_s");
                eph.f2 = d("af2_s_s2");
                eph.toe = ts("toe_gpst");
                eph.toc = ts("toc_gpst");
                eph.ttr = ts("transmission_gpst");
                eph.toes = time2gpst(eph.toe, &eph.week);
                if (d1 || d2) {
                    eph.toes = double(std::get<Tick>(r.at("toe_bdt_s"))) / ps;
                    eph.week = int((std::get<Tick>(r.at("toe_gpst")) -
                                    seconds(1356LL * 604800 + 14)) /
                                   seconds(604800));
                    eph.sva = u("urai");
                    eph.svh = u("health");
                    eph.flag = d2 ? 2 : 1;
                    eph.tgd[0] = double(std::get<Tick>(r.at("tgd_b1i_s"))) / ps;
                    eph.tgd[1] = double(std::get<Tick>(r.at("tgd_b2i_s"))) / ps;
                } else {
                    eph.sva = u("sisa_index");
                    eph.tgd[0] =
                        double(std::get<Tick>(r.at("bgd_e1_e5a_s"))) / ps;
                    if (inav) {
                        eph.tgd[1] =
                            double(std::get<Tick>(r.at("bgd_e1_e5b_s"))) / ps;
                        eph.svh = (u("e5b_health") << 7) |
                                  (u("e5b_data_invalid") << 6) |
                                  (u("e1b_health") << 1) |
                                  u("e1b_data_invalid");
                        eph.code = 1 << 9;
                    } else {
                        eph.svh = (u("e5a_health") << 4) |
                                  (u("e5a_data_invalid") << 3);
                        eph.code = 1 << 8;
                    }
                }
                state_->publish(eph, sat, *t, fam);
            }
            continue;
        }
        auto &a = state_->pending[{sat, std::string(fam)}];
        int id = 0, count = 0, stride = 0, offset = 0;
        uint8_t data[38]{};
        if (lnav) {
            for (int word = 0; word < 10; ++word)
                setbitu(data, word * 24, 24, getbitu(raw, word * 30, 24));
            if (data[0] != 0x8b)
                continue;
            id = int(getbitu(data, 43, 3));
            count = 3;
            stride = 30;
            offset = (id - 1) * stride;
        }
        if (id < 1 || id > count || a.times[id - 1] > *t)
            continue;
        std::copy(data, data + stride, a.bytes.begin() + offset);
        a.times[id - 1] = *t;
        int64_t lo = INT64_MAX, hi = -1;
        bool complete = true;
        for (int j = 0; j < count; ++j) {
            if (a.times[j] < 0) {
                complete = false;
                break;
            }
            lo = std::min(lo, a.times[j]);
            hi = std::max(hi, a.times[j]);
        }
        if (!complete || hi - lo > 120 * second)
            continue;
        eph_t eph{};
        int decoded = 0;
        if (lnav)
            decoded = decode_frame(a.bytes.data(), native_system, &eph, nullptr,
                                   nullptr, nullptr);
        if (!decoded)
            continue;
        if (lnav) {
            int reference = int(hi / (604800 * second));
            int week = int(getbitu(a.bytes.data(), 48, 10));
            week += int(std::llround(double(reference - week) / 1024)) * 1024;
            double tow = getbitu(a.bytes.data(), 24, 17) * 6.;
            eph.ttr = gpst2time(week, tow);
            if (eph.toes < tow - 302400)
                ++week;
            else if (eph.toes > tow + 302400)
                --week;
            int ignored;
            double toc = time2gpst(eph.toc, &ignored);
            eph.week = week;
            eph.toe = gpst2time(week, eph.toes);
            eph.toc = gpst2time(week, toc);
        }
        state_->publish(eph, sat, hi, fam);
    }
}
bool BroadcastNavigation::position(int sat, int64_t ns, double travel,
                                   double *xyz) const {
    // Caller owns the shared RTKLIB lock (also used by the STEC engine).
    std::lock_guard guard(state_->mutex);
    auto found = state_->ephemerides.find(sat);
    if (found == state_->ephemerides.end())
        return false;
    auto tx = timeadd(time_of(ns), -travel);
    const State::Entry *selected = nullptr;
    std::map<std::string, const State::Entry *> latest;
    for (const auto &e : found->second) {
        double age = std::abs(timediff(tx, e.eph.toe));
        double limit =
            std::min(7200., e.eph.fit > 0 ? e.eph.fit * 1800. : 7200.);
        // Equal reception-context time does not order a late navigation page
        // against the observation. Activate only for a later measurement.
        if (e.available >= ns || age > limit)
            continue;
        if (!selected || e.available > selected->available)
            selected = &e;
        auto &family = latest[e.family];
        if (!family || e.available > family->available)
            family = &e;
    }
    if (!selected)
        return false;
    // I/NAV and F/NAV report different signal health. Do not mask an
    // available unhealthy report by choosing the other family's orbit.
    for (const auto &[family, entry] : latest)
        if (satexclude(sat, 0, entry->eph.svh, nullptr))
            return false;
    double clock, variance;
    eph2pos(tx, &selected->eph, xyz, &clock, &variance);
    return std::isfinite(xyz[0]) && std::isfinite(xyz[1]) &&
           std::isfinite(xyz[2]) && norm(xyz, 3) > 1e7;
}
bool BroadcastNavigation::ecef(char system, int number, int64_t ns,
                               double *xyz) const {
    std::lock_guard lock(rtklib_mutex);
    int sat = system == 'G'   ? satno(SYS_GPS, number)
              : system == 'J' ? satno(SYS_QZS, number + 192)
              : system == 'E' ? satno(SYS_GAL, number)
              : system == 'C' ? satno(SYS_CMP, number)
                              : 0;
    return sat && position(sat, ns, 0, xyz);
}
} // namespace neognss_obs
