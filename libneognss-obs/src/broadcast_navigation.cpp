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
        std::array<uint8_t, 90> bytes{};
        std::array<int64_t, 3> times{-1, -1, -1};
    };
    struct Entry {
        eph_t eph;
        int64_t available;
    };
    std::string setup;
    mutable std::mutex mutex;
    std::map<int, Assembly> pending;
    std::map<int, std::deque<Entry>> ephemerides;
    uint64_t count = 0;
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
        if (fam != "GPS_LNAV" && fam != "QZS_LNAV")
            continue;
        auto t = timestamp(time, row);
        if (!t)
            continue;
        auto sys = text(system, row);
        if (sys != (fam == "GPS_LNAV" ? "G" : "J"))
            throw std::runtime_error("LNAV satellite system mismatch");
        int prn = int(ArrowArrayViewGetUIntUnsafe(number, row));
        int sat =
            satno(sys == "G" ? SYS_GPS : SYS_QZS, sys == "G" ? prn : prn + 192);
        if (ArrowArrayViewIsNull(number, row) ||
            ArrowArrayViewIsNull(length, row) ||
            text(f("completeness"), row) != "complete" ||
            text(f("content_kind"), row) != "navigation_bits" || !sat ||
            text(format, row) != "LNAV_300_V1" ||
            ArrowArrayViewGetUIntUnsafe(length, row) != 300)
            throw std::runtime_error("Invalid LNAV canonical identity/layout");
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
            throw std::runtime_error("Null LNAV body");
        auto bits = ArrowArrayViewGetBytesUnsafe(body, row);
        if (bits.size_bytes != 38 || (bits.data.as_uint8[37] & 15))
            throw std::runtime_error("Invalid LNAV body size/padding");
        uint8_t data[30]{};
        for (int word = 0; word < 10; ++word)
            setbitu(data, word * 24, 24,
                    getbitu(bits.data.as_uint8, word * 30, 24));
        if (data[0] != 0x8b)
            continue;
        int id = int(getbitu(data, 43, 3));
        if (id < 1 || id > 3)
            continue;
        auto &a = state_->pending[sat];
        if (a.times[id - 1] > *t)
            continue;
        std::copy(data, data + 30, a.bytes.begin() + (id - 1) * 30);
        a.times[id - 1] = *t;
        auto [lo, hi] = std::minmax_element(a.times.begin(), a.times.end());
        if (*lo < 0 || *hi - *lo > 120 * second)
            continue;
        eph_t eph{};
        if (!decode_frame(a.bytes.data(), sys == "G" ? SYS_GPS : SYS_QZS, &eph,
                          nullptr, nullptr, nullptr))
            continue;
        // Replace RTKLIB's host-date week expansion with the received context.
        int reference = int(*hi / (604800 * second));
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
        eph.sat = sat;
        if (std::abs(timediff(time_of(*hi), eph.ttr)) > 120 || eph.A < 1e7 ||
            eph.A > 5e7 || eph.e < 0 || eph.e >= 1)
            continue;
        auto &cache = state_->ephemerides[sat];
        const auto unchanged = [&](const auto &e) {
            return e.eph.iode == eph.iode && e.eph.iodc == eph.iodc &&
                   e.eph.svh == eph.svh && e.eph.fit == eph.fit &&
                   timediff(e.eph.toe, eph.toe) == 0 &&
                   timediff(e.eph.toc, eph.toc) == 0;
        };
        if (!cache.empty() && unchanged(cache.back()))
            continue;
        cache.push_back({eph, *hi});
        if (cache.size() > 8)
            cache.pop_front();
        ++state_->count;
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
    for (const auto &e : found->second) {
        double age = std::abs(timediff(tx, e.eph.toe));
        double limit =
            std::min(7200., e.eph.fit > 0 ? e.eph.fit * 1800. : 7200.);
        if (e.available > ns || age > limit)
            continue;
        if (!selected || e.available > selected->available)
            selected = &e;
    }
    if (!selected || satexclude(sat, 0, selected->eph.svh, nullptr))
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
                              : 0;
    return sat && position(sat, ns, 0, xyz);
}
} // namespace neognss_obs
