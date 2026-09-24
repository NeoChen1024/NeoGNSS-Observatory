// SPDX-License-Identifier: GPL-3.0-only
#include "arrow_batch.hpp"
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
    std::map<int, std::deque<Entry>> ephemerides;
    uint64_t count = 0;
    std::map<std::string, uint64_t> family_counts;
    explicit State(std::string id) : setup(std::move(id)) {}
};
BroadcastNavigation::BroadcastNavigation(std::string setup)
    : state_(std::make_unique<State>(std::move(setup))) {}
BroadcastNavigation::~BroadcastNavigation() = default;
void BroadcastNavigation::clear() {
    std::lock_guard lock(state_->mutex);
    state_->pending.clear();
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
        } else if (inav) {
            // Canonical I/NAV has 114 even bits followed by 114 odd bits.
            if (getbitu(raw, 0, 1) != 0 || getbitu(raw, 114, 1) != 1 ||
                getbitu(raw, 1, 1) || getbitu(raw, 115, 1))
                continue;
            id = int(getbitu(raw, 2, 6));
            count = 5;
            stride = 16;
            offset = id * stride;
            for (int j = 0; j < 112; ++j)
                setbitu(data, j, 1, getbitu(raw, j + 2, 1));
            for (int j = 0; j < 16; ++j)
                setbitu(data, 112 + j, 1, getbitu(raw, 116 + j, 1));
        } else if (fnav) {
            id = int(getbitu(raw, 0, 6));
            count = 4;
            stride = 31;
            offset = (id - 1) * stride;
            std::copy(raw, raw + 30,
                      data); // decoder does not read the omitted tail
        } else {
            if (getbitu(raw, 0, 11) != 0x712)
                continue;
            id = int(getbitu(raw, 15, 3));
            count = d1 ? 3 : 10;
            stride = 38;
            if (d2) {
                if (id != 1)
                    continue;
                id = int(getbitu(raw, 42, 4));
                if (id == 2)
                    continue;
            }
            offset = (id - 1) * stride;
            std::copy(raw, raw + 38, data);
        }
        if (id < 1 || id > count || a.times[id - 1] > *t)
            continue;
        std::copy(data, data + stride, a.bytes.begin() + offset);
        a.times[id - 1] = *t;
        int64_t lo = INT64_MAX, hi = -1;
        bool complete = true;
        for (int j = 0; j < count; ++j) {
            if (d2 && j == 1)
                continue; // page 2 is not required for the D2 ephemeris
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
        else if (inav)
            decoded = decode_gal_inav(a.bytes.data(), &eph, nullptr, nullptr);
        else if (fnav)
            decoded = decode_gal_fnav(a.bytes.data(), &eph, nullptr, nullptr);
        else if (d1)
            decoded = decode_bds_d1(a.bytes.data(), &eph, nullptr, nullptr);
        else
            decoded = decode_bds_d2(a.bytes.data(), &eph, nullptr);
        if (!decoded || ((inav || fnav) && eph.sat != sat))
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
        } else {
            // Decoder dates are GST/BDT-derived. Resolve finite week fields
            // against reception, never host time. BDT conversion includes +14
            // s.
            const double rollover = (inav || fnav ? 4096. : 8192.) * 604800;
            const double shift =
                std::round(timediff(time_of(hi), eph.ttr) / rollover) *
                rollover;
            // RTKLIB timeadd narrows whole seconds to int; an entire GST/BDT
            // rollover exceeds that range. This shift is integral seconds.
            eph.ttr.time += static_cast<time_t>(shift);
            eph.toe.time += static_cast<time_t>(shift);
            eph.toc.time += static_cast<time_t>(shift);
            eph.week += int(shift / 604800);
            // Resolve each model epoch around transmission time independently
            // of a source decoder's opposite-sign week carry.
            auto near_week = [&](gtime_t value) {
                return timeadd(value,
                               std::round(timediff(eph.ttr, value) / 604800) *
                                   604800);
            };
            auto adjusted_toe = near_week(eph.toe);
            eph.week +=
                int(std::llround(timediff(adjusted_toe, eph.toe) / 604800));
            eph.toe = adjusted_toe;
            eph.toc = near_week(eph.toc);
        }
        eph.sat = sat;
        if (eph.toes < 0 || eph.toes >= 604800 ||
            std::abs(timediff(time_of(hi), eph.ttr)) > 120 ||
            !std::isfinite(eph.A) || eph.A < 1e7 || eph.A > 5e7 ||
            !std::isfinite(eph.e) || eph.e < 0 || eph.e >= 1)
            continue;
        auto &cache = state_->ephemerides[sat];
        const auto unchanged = [&](const auto &e) {
            return e.eph.iode == eph.iode && e.eph.iodc == eph.iodc &&
                   e.eph.svh == eph.svh && e.eph.fit == eph.fit &&
                   timediff(e.eph.toe, eph.toe) == 0 &&
                   timediff(e.eph.toc, eph.toc) == 0;
        };
        auto previous =
            std::find_if(cache.rbegin(), cache.rend(),
                         [&](const auto &e) { return e.family == fam; });
        if (previous != cache.rend() && unchanged(*previous))
            continue;
        cache.push_back({eph, hi, std::string(fam)});
        if (std::count_if(cache.begin(), cache.end(),
                          [&](const auto &e) { return e.family == fam; }) > 8)
            cache.erase(
                std::find_if(cache.begin(), cache.end(),
                             [&](const auto &e) { return e.family == fam; }));
        ++state_->count;
        ++state_->family_counts[std::string(fam)];
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
