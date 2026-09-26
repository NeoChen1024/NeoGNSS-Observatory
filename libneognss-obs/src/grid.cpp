// SPDX-License-Identifier: GPL-3.0-only
#include "broadcast_fields.hpp"
#include <algorithm>
#include <mutex>
#include <neognss_obs/sbas_grid.hpp>
#include <set>

namespace neognss_obs {
using namespace broadcast_detail;
struct SbasGridProcessor::State {
    struct Mask {
        std::vector<int> bits;
        Tick report;
    };
    struct Cell {
        std::optional<Tick> report;
        int delay = 0, givei = 15, iodi = -1;
        bool present = false, changed = false;
        std::string reason = "MISSING";
        Tick valid = 0, restricted = 0;
        double integral = 0;
    };
    struct Source {
        int64_t sat = 0;
        std::vector<std::string> signals;
        int iodi = -1, band_count = 0;
        std::map<int, Mask> masks;
        std::map<std::pair<int, int>, Cell> cells;
        std::optional<Tick> mt0;
        bool mt0_untimed = false;
    };
    std::string setup;
    Tick interval, correction_age, mask_age;
    std::optional<Tick> progress, next, segment_start;
    std::map<std::string, Source> sources;
    std::map<std::string, uint64_t> counts;
    std::map<std::string, std::vector<Row>> output;
    mutable std::mutex mutex;
    bool failed = false;
    static constexpr double tecu_per_m = 1575.42e6 * 1575.42e6 / (40.3 * 1e16);
    State(std::string id, int64_t n, int64_t age, int64_t mask)
        : setup(std::move(id)), interval(seconds(n)),
          correction_age(seconds(age)), mask_age(seconds(mask)) {
        if (n <= 0 || age <= 0 || mask <= 0)
            throw std::invalid_argument(
                "SBAS intervals and ages must be positive");
    }
    static Value optional_time(std::optional<Tick> t) {
        return t ? Value(*t) : Value{};
    }
    std::optional<Tick> expiry(const Source &s, int band, const Cell &c) const {
        auto m = s.masks.find(band);
        if (!c.present || !c.report || m == s.masks.end() || c.iodi != s.iodi)
            return {};
        return std::min(*c.report + correction_age,
                        m->second.report + mask_age);
    }
    void integrate(Tick end) {
        if (!progress || end <= *progress)
            return;
        for (auto &[key, s] : sources)
            for (auto &[point, c] : s.cells) {
                auto deadline = expiry(s, point.first, c);
                if (!deadline || c.delay == 511 || c.givei == 15)
                    continue;
                const Tick stop = std::min(end, *deadline);
                if (stop <= *progress)
                    continue;
                const Tick elapsed = stop - *progress;
                c.valid += elapsed;
                c.integral +=
                    double(elapsed) / double(ps) * c.delay * .125 * tecu_per_m;
                if (s.mt0_untimed)
                    c.restricted += elapsed;
                else if (s.mt0) {
                    const Tick restricted_end =
                        std::min(stop, *s.mt0 + seconds(60));
                    if (restricted_end > *progress)
                        c.restricted += restricted_end - *progress;
                }
            }
        progress = end;
    }
    void emit(const std::string &kind, Row row) {
        if (++counts["output_in_call"] > 1000000)
            throw std::runtime_error("SBAS snapshot output bound exceeded; use "
                                     "smaller progress steps");
        row["output_sequence"] = int64_t(counts["output_records"]++);
        output[kind].push_back(std::move(row));
    }
    void snapshot(Tick t) {
        int64_t rows = 0, usable = 0;
        for (auto &[key, s] : sources) {
            int live_masks = 0;
            for (const auto &[band, m] : s.masks)
                live_masks += m.report + mask_age > t;
            const bool complete =
                s.band_count > 0 && live_masks == s.band_count;
            for (auto it = s.cells.begin(); it != s.cells.end();) {
                auto &[point, c] = *it;
                auto deadline = expiry(s, point.first, c);
                const bool fresh = deadline && *deadline > t;
                std::string status = c.reason;
                if (c.present && !fresh)
                    status = "EXPIRED";
                if (fresh)
                    status = c.delay == 511  ? "DO_NOT_USE"
                             : c.givei == 15 ? "NOT_MONITORED"
                                             : "USABLE";
                const bool good = fresh && status == "USABLE";
                if (c.changed || c.valid > 0 || fresh) {
                    const auto coordinate =
                        *SBAS::igp_coordinate(point.first, point.second);
                    const bool mt0_active = s.mt0 && *s.mt0 + seconds(60) > t;
                    const double valid_s = double(c.valid) / double(ps);
                    Row r{{"setup_id", setup},
                          {"snapshot_gpst", t},
                          {"window_start_gpst", t - interval},
                          {"satellite_system", std::string("S")},
                          {"satellite_number", s.sat},
                          {"bitstream_source", s.signals},
                          {"band", int64_t(point.first)},
                          {"mask_bit", int64_t(point.second)},
                          {"latitude", double(coordinate.first)},
                          {"longitude", double(coordinate.second)},
                          {"iodi", int64_t(c.iodi)},
                          {"givei", int64_t(c.givei)},
                          {"reported_gpst", optional_time(c.report)},
                          {"expiry_gpst", optional_time(deadline)},
                          {"status", status},
                          {"mask_set_complete", complete},
                          {"last_mt0_gpst", optional_time(s.mt0)},
                          {"mt0_restriction",
                           std::string(s.mt0_untimed ? "UNKNOWN"
                                       : mt0_active  ? "ACTIVE"
                                       : s.mt0       ? "ELAPSED"
                                                     : "NOT_OBSERVED")},
                          {"delay_m", good ? Value(c.delay * .125) : Value{}},
                          {"vtec_tecu",
                           good ? Value(c.delay * .125 * tecu_per_m) : Value{}},
                          {"valid_duration_s", c.valid},
                          {"mt0_duration_s", c.restricted},
                          {"coverage", double(c.valid) / double(interval)},
                          {"mean_vtec_tecu", c.valid > 0
                                                 ? Value(c.integral / valid_s)
                                                 : Value{}}};
                    emit("grid", std::move(r));
                    ++rows;
                    usable += good;
                }
                c.valid = c.restricted = 0;
                c.integral = 0;
                c.changed = false;
                if (!fresh)
                    it = s.cells.erase(it);
                else
                    ++it;
            }
        }
        emit("snapshots", {{"setup_id", setup},
                           {"snapshot_gpst", t},
                           {"window_start_gpst", t - interval},
                           {"segment_start_gpst", *segment_start},
                           {"partial_window", *segment_start > t - interval},
                           {"grid_rows", rows},
                           {"usable_cells", usable},
                           {"sources", int64_t(sources.size())}});
        ++counts["snapshots"];
    }
    void advance(Tick t) {
        if (progress && t < *progress)
            throw std::runtime_error(
                "SBAS navigation context reversed; declare discontinuity");
        if (!progress) {
            progress = t;
            segment_start = t;
            next = (t / interval + 1) * interval;
            return;
        }
        int steps = 0;
        while (*next <= t) {
            if (++steps > 4096)
                throw std::runtime_error(
                    "SBAS progress crosses too many snapshots");
            integrate(*next);
            snapshot(*next);
            *next += interval;
        }
        integrate(t);
    }
    void invalidate(Source &s, const std::string &reason) {
        for (auto &[point, c] : s.cells) {
            c.present = false;
            c.changed = true;
            c.reason = reason;
        }
        s.masks.clear();
    }
    void accept(const SBAS::Message &m, Source &s, std::optional<Tick> t) {
        if (m.type == 0) {
            if (t) {
                s.mt0 = t;
                s.mt0_untimed = false;
            } else
                s.mt0_untimed = true;
            ++counts["mt0_messages"];
            return;
        }
        if (!t) {
            ++counts["untimed_grid_messages"];
            return;
        }
        if (auto v = std::get_if<SBAS::IonosphericMask>(&m.content)) {
            if (v->number_of_bands_raw == 0) {
                invalidate(s, "MASK_REMOVED");
                s.iodi = v->iodi;
                s.band_count = 0;
                ++counts["mask_messages"];
                return;
            }
            std::vector<int> bits;
            for (size_t i = 0; i < v->mask.size(); ++i)
                if (v->mask[i])
                    bits.push_back(int(i + 1));
            if (s.iodi != v->iodi || s.band_count != v->number_of_bands_raw) {
                invalidate(s, "MASK_CHANGED");
                s.iodi = v->iodi;
                s.band_count = v->number_of_bands_raw;
            }
            auto old = s.masks.find(v->band);
            if (old != s.masks.end() && old->second.bits != bits) {
                ++counts["same_issue_mask_changed"];
                invalidate(s, "MASK_CHANGED");
            } else if (old != s.masks.end() &&
                       old->second.report + mask_age <= *t) {
                for (auto &[point, c] : s.cells)
                    if (point.first == v->band) {
                        c.present = false;
                        c.changed = true;
                        c.reason = "MASK_CHANGED";
                    }
            }
            s.masks[v->band] = {std::move(bits), *t};
            ++counts["mask_messages"];
        } else if (auto v = std::get_if<SBAS::IonosphericDelay>(&m.content)) {
            auto mask = s.masks.find(v->band);
            if (mask == s.masks.end() || s.iodi != v->iodi ||
                mask->second.report + mask_age <= *t) {
                ++counts["correction_without_mask"];
                return;
            }
            for (size_t i = 0; i < v->corrections.size(); ++i) {
                const size_t ordinal = v->block * 15 + i;
                if (ordinal >= mask->second.bits.size())
                    continue;
                auto &c = s.cells[{v->band, mask->second.bits[ordinal]}];
                const auto &e = v->corrections[i];
                c.report = t;
                c.delay = e.delay_raw;
                c.givei = e.givei;
                c.iodi = v->iodi;
                c.present = true;
                c.changed = true;
            }
            ++counts["correction_messages"];
        }
    }
    Output drain() {
        Output result;
        for (auto &[kind, rows] : output)
            result[kind] = batch(rows);
        output.clear();
        return result;
    }
};
SbasGridProcessor::SbasGridProcessor(std::string setup, int64_t interval,
                                     int64_t age, int64_t mask)
    : state_(std::make_unique<State>(std::move(setup), interval, age, mask)) {}
SbasGridProcessor::~SbasGridProcessor() = default;
SbasGridProcessor::Output SbasGridProcessor::feed(ArrowSchema *schema,
                                                  ArrowArray *array) {
    auto &s = *state_;
    std::lock_guard lock(s.mutex);
    if (s.failed)
        throw std::runtime_error(
            "SBAS processor failed; construct a new instance");
    try {
        s.counts["output_in_call"] = 0;
        ArrowBatchView owner(schema, array);
        Column root{&owner.value, schema};
        auto time = root.child("nav_epoch_gpst"),
             family = root.child("message_family");
        for (int64_t i = 0; i < array->length; ++i) {
            auto row = i + owner.value.offset;
            if (root.child("setup_id").str(row) != s.setup)
                throw std::runtime_error("Mixed SBAS Setup");
            auto t = time.time(row);
            if (t)
                s.advance(*t);
            if (family.str(row) != "SBAS_L1")
                continue;
            auto sat = root.child("satellite_number").integer(row);
            if (root.child("satellite_system").str(row) != "S" || sat < 1 ||
                sat > 255 ||
                root.child("body_format").str(row) != "SBAS_L1_250_V1" ||
                root.child("content_kind").str(row) != "navigation_bits" ||
                root.child("bit_length").integer(row) != 250 ||
                root.child("completeness").str(row) != "complete")
                throw std::runtime_error("Invalid SBAS identity/layout");
            auto checks = root.child("checks"),
                 signals = root.child("bitstream_source"),
                 body = root.child("body");
            if (checks.v->storage_type != NANOARROW_TYPE_LIST ||
                signals.v->storage_type != NANOARROW_TYPE_LIST ||
                body.v->storage_type != NANOARROW_TYPE_BINARY)
                throw std::runtime_error("Invalid SBAS Arrow fields");
            bool pass = false, fail = false;
            Column check{checks.v->children[0], checks.s->children[0]};
            auto result = check.child("result");
            if (!checks.null(row))
                for (auto j = ArrowArrayViewListChildOffset(
                         checks.v, row + checks.v->offset);
                     j < ArrowArrayViewListChildOffset(
                             checks.v, row + checks.v->offset + 1);
                     ++j) {
                    auto value = result.str(j + check.v->offset);
                    pass |= value == "pass";
                    fail |= value == "fail";
                }
            if (!pass || fail) {
                ++s.counts["rejected_checks"];
                continue;
            }
            if (body.null(row) || signals.null(row))
                throw std::runtime_error("Null SBAS body/signals");
            auto bytes = ArrowArrayViewGetBytesUnsafe(body.v, row);
            if (bytes.size_bytes != 32 || (bytes.data.as_uint8[31] & 63))
                throw std::runtime_error("Invalid SBAS body");
            auto decoded = SBAS::parse_l1({bytes.data.as_uint8, 32});
            if (decoded.status != SBAS::Status::decoded) {
                ++s.counts["rejected_content"];
                continue;
            }
            if (decoded.message->type != 0 && decoded.message->type != 18 &&
                decoded.message->type != 26)
                continue;
            Column signal{signals.v->children[0], signals.s->children[0]};
            std::vector<std::string> names;
            for (auto j = ArrowArrayViewListChildOffset(
                     signals.v, row + signals.v->offset);
                 j < ArrowArrayViewListChildOffset(signals.v,
                                                   row + signals.v->offset + 1);
                 ++j)
                names.push_back(signal.str(j));
            if (names.empty())
                throw std::runtime_error("Empty SBAS signal identity");
            std::sort(names.begin(), names.end());
            std::string key = std::to_string(sat);
            for (auto &v : names)
                key += "/" + v;
            if (!s.sources.contains(key) && s.sources.size() >= 64)
                throw std::runtime_error("SBAS source bound exceeded");
            auto &source = s.sources[key];
            source.sat = sat;
            source.signals = std::move(names);
            s.accept(*decoded.message, source, t);
        }
        return s.drain();
    } catch (...) {
        s.failed = true;
        throw;
    }
}
SbasGridProcessor::Output SbasGridProcessor::advance(int64_t sec,
                                                     int64_t fraction) {
    auto &s = *state_;
    std::lock_guard lock(s.mutex);
    if (s.failed)
        throw std::runtime_error("SBAS processor failed");
    try {
        if (sec < 0 || fraction < 0 || fraction >= ps)
            throw std::invalid_argument("Invalid GPST");
        s.counts["output_in_call"] = 0;
        s.advance(seconds(sec) + fraction);
        return s.drain();
    } catch (...) {
        s.failed = true;
        throw;
    }
}
void SbasGridProcessor::discontinuity() {
    auto &s = *state_;
    std::lock_guard lock(s.mutex);
    if (s.failed)
        throw std::runtime_error("SBAS processor failed");
    s.sources.clear();
    s.progress.reset();
    s.next.reset();
    s.segment_start.reset();
    ++s.counts["discontinuities"];
}
std::map<std::string, uint64_t> SbasGridProcessor::diagnostics() const {
    std::lock_guard lock(state_->mutex);
    auto r = state_->counts;
    r.erase("output_in_call");
    return r;
}
} // namespace neognss_obs
