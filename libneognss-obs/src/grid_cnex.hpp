// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "arrow_batch.hpp"
#include <algorithm>
#include <boost/int128/int128.hpp>
#include <cstring>
#include <limits>
#include <mutex>
#include <neognss_obs/processing.hpp>

namespace neognss_obs {
// One ordered station reader. All buffers are bounded by a day of navigation
// context, one input batch, and 8192 pending frames per active satellite.
class GridCnexProcessor {
    struct Column {
        ArrowArrayView *v;
        const ArrowSchema *s;
        Column child(const char *name) const {
            for (int64_t i = 0; i < s->n_children; ++i)
                if (s->children[i]->name &&
                    std::strcmp(s->children[i]->name, name) == 0)
                    return {v->children[i], s->children[i]};
            throw std::runtime_error(std::string("Missing CommonNEX field: ") +
                                     name);
        }
        bool null(int64_t i) const { return ArrowArrayViewIsNull(v, i); }
        std::string_view text(int64_t i) const {
            if (v->storage_type != NANOARROW_TYPE_STRING || null(i))
                throw std::runtime_error("Expected non-null CommonNEX string");
            auto x = ArrowArrayViewGetStringUnsafe(v, i);
            return {x.data, size_t(x.size_bytes)};
        }
        int64_t integer(int64_t i) const {
            if (null(i) || (v->storage_type != NANOARROW_TYPE_INT8 &&
                            v->storage_type != NANOARROW_TYPE_INT16 &&
                            v->storage_type != NANOARROW_TYPE_INT32 &&
                            v->storage_type != NANOARROW_TYPE_INT64 &&
                            v->storage_type != NANOARROW_TYPE_UINT8 &&
                            v->storage_type != NANOARROW_TYPE_UINT16 &&
                            v->storage_type != NANOARROW_TYPE_UINT32))
                throw std::runtime_error("Expected non-null CommonNEX integer");
            return ArrowArrayViewGetIntUnsafe(v, i);
        }
        int64_t time(int64_t i, int64_t day) const {
            if ((std::strcmp(s->format, "d:38,12") != 0 &&
                 std::strcmp(s->format, "d:38,12,128") != 0) ||
                null(i))
                throw std::runtime_error(
                    "Expected non-null GPST decimal128(38,12)");
            ArrowDecimal d;
            ArrowDecimalInit(&d, 128, 38, 12);
            ArrowArrayViewGetDecimalUnsafe(v, i, &d);
            using Tick = boost::int128::int128;
            if (d.words[d.high_word_index] >> 63)
                throw std::runtime_error("Negative GPST");
            Tick ticks = (Tick(d.words[d.high_word_index]) << 64) +
                         d.words[d.low_word_index];
            if (ticks < Tick(day) * 1000000000 ||
                ticks >= (Tick(day) + 86400000) * 1000000000)
                throw std::runtime_error("Record outside its GPST day");
            // Existing grid policy truncates decimal seconds to milliseconds.
            return int64_t(ticks / 1000000000);
        }
    };
    struct Stream {
        int64_t satellite, id, last;
        GridProcessor processor;
        std::vector<GridFrame> pending;
        Stream(int64_t sat, int64_t stream, int64_t time)
            : satellite(sat), id(stream), last(time) {}
    };
    std::string setup_;
    int64_t gap_, day_ = -1, next_stream_ = 0, next_frame_ = 0;
    std::optional<int64_t> previous_nav_, previous_raw_;
    std::vector<std::unique_ptr<Stream>> streams_;
    std::vector<int64_t> events_;
    size_t event_ = 0;
    bool raw_started_ = false, finished_ = false;
    Json diagnostics_ = Json::object();
    std::vector<GridInterval> output_;
    void emit(Stream &s, std::vector<GridInterval> rows) {
        for (auto &r : rows) {
            r.satellite_number = s.satellite;
            r.stream_id = s.id;
            output_.push_back(r);
        }
    }
    void flush(Stream &s) {
        if (s.pending.empty())
            return;
        emit(s,
             s.processor.process_frames(std::span<const GridFrame>(s.pending)));
        s.pending.clear();
    }
    void close(size_t i, int64_t time) {
        auto &s = *streams_[i];
        flush(s);
        emit(s, s.processor.finish_intervals(time));
        auto counts = s.processor.diagnostics();
        for (auto it = counts.begin(); it != counts.end(); ++it)
            diagnostics_[it.key()] = diagnostics_.value(it.key(), uint64_t(0)) +
                                     it.value().get<uint64_t>();
        streams_.erase(streams_.begin() + i);
    }
    void count(const char *name) {
        diagnostics_[name] = diagnostics_.value(name, uint64_t(0)) + 1;
    }
    void advance(int64_t time) {
        if (previous_nav_) {
            if (time < *previous_nav_)
                throw std::runtime_error("Reversed navigation context; select "
                                         "a non-overlapping recording path");
            if (time - *previous_nav_ > gap_) {
                while (!streams_.empty())
                    close(0, std::max(*previous_nav_, streams_[0]->last));
                count("navigation_gaps");
            }
        }
        previous_nav_ = time;
        for (size_t i = 0; i < streams_.size();) {
            if (time - streams_[i]->last > gap_) {
                close(i, streams_[i]->last);
                count("signal_gaps");
            } else
                ++i;
        }
    }
    std::vector<GridInterval> drain() {
        std::vector<GridInterval> r;
        r.swap(output_);
        return r;
    }
    void active() const {
        if (day_ < 0 || finished_)
            throw std::runtime_error("SBAS reader requires an active GPST day");
    }

  public:
    std::mutex mutex;
    GridCnexProcessor(std::string setup, int64_t gap_ms)
        : setup_(std::move(setup)), gap_(gap_ms) {
        if (gap_ms <= 0)
            throw std::invalid_argument("Invalid SBAS gap timeout");
    }
    void begin_day(int64_t day) {
        if (day_ >= 0 || finished_ || day < 0 || day % 86400000)
            throw std::runtime_error("Invalid SBAS day boundary");
        day_ = day;
        events_.clear();
        event_ = 0;
        raw_started_ = false;
    }
    void events(ArrowSchema *schema, ArrowArray *array) {
        active();
        if (raw_started_)
            throw std::runtime_error("Navigation context must precede RawBits");
        ArrowBatchView view(schema, array);
        Column root{&view.value, schema};
        auto scope = root.child("scope"), setup = root.child("setup_id"),
             kind = root.child("kind"), time = root.child("gpst");
        auto payload = root.child("payload"),
             completion = payload.child("epoch_completion");
        auto status = completion.child("completion");
        for (int64_t i = 0; i < array->length; ++i) {
            if (scope.null(i) || scope.text(i) != "NAVIGATION")
                continue;
            if (setup.text(i) != setup_)
                throw std::runtime_error("Mixed Setup identity");
            if (kind.text(i) != "EPOCH_COMPLETION")
                throw std::runtime_error("Unsupported navigation event");
            if (payload.null(i) || completion.null(i) ||
                status.text(i) != "COMPLETE")
                throw std::runtime_error(
                    "Incomplete navigation closure requires an explicit "
                    "downstream policy");
            events_.push_back(time.time(i, day_));
        }
    }
    std::vector<GridInterval> feed(ArrowSchema *schema, ArrowArray *array) {
        active();
        if (!raw_started_) {
            std::sort(events_.begin(), events_.end());
            events_.erase(std::unique(events_.begin(), events_.end()),
                          events_.end());
            raw_started_ = true;
        }
        ArrowBatchView view(schema, array);
        Column root{&view.value, schema};
        auto family = root.child("message_family"),
             setup = root.child("setup_id"),
             time = root.child("nav_epoch_gpst");
        auto system = root.child("satellite_system"),
             satellite = root.child("satellite_number"),
             format = root.child("body_format");
        auto bits = root.child("bit_length"), body = root.child("body"),
             complete = root.child("completeness"),
             checks = root.child("checks");
        if (checks.v->storage_type != NANOARROW_TYPE_LIST ||
            checks.s->n_children != 1 ||
            body.v->storage_type != NANOARROW_TYPE_BINARY)
            throw std::runtime_error("Invalid SBAS body/checks Arrow type");
        Column check{checks.v->children[0], checks.s->children[0]};
        auto origin = check.child("origin"), kind = check.child("kind"),
             scope = check.child("scope"), result = check.child("result");
        for (int64_t i = 0; i < array->length; ++i) {
            if (family.null(i) || family.text(i) != "SBAS_L1")
                continue;
            if (setup.text(i) != setup_)
                throw std::runtime_error("Mixed Setup identity");
            if (time.null(i))
                continue;
            auto t = time.time(i, day_);
            if (previous_raw_ && t < *previous_raw_)
                throw std::runtime_error("Reversed SBAS occurrence time; "
                                         "select non-overlapping inputs");
            previous_raw_ = t;
            while (event_ < events_.size() && events_[event_] <= t)
                advance(events_[event_++]);
            if (system.text(i) != "S" || format.text(i) != "SBAS_L1_250_V1" ||
                bits.integer(i) != 250)
                throw std::runtime_error(
                    "Grid requires canonical SBAS L1 250-bit bodies");
            if (body.null(i) || complete.text(i) != "complete")
                throw std::runtime_error("Invalid complete SBAS body");
            auto bytes = ArrowArrayViewGetBytesUnsafe(body.v, i);
            if (bytes.size_bytes != 32 ||
                (uint8_t(bytes.data.as_uint8[31]) & 63))
                throw std::runtime_error("Invalid complete SBAS body");
            auto sat = satellite.integer(i);
            auto it = std::find_if(
                streams_.begin(), streams_.end(),
                [&](const auto &s) { return s->satellite == sat; });
            if (it != streams_.end() && t - (*it)->last > gap_) {
                close(size_t(it - streams_.begin()), (*it)->last);
                count("signal_gaps");
                it = streams_.end();
            }
            if (it == streams_.end()) {
                streams_.push_back(
                    std::make_unique<Stream>(sat, next_stream_++, t));
                it = std::prev(streams_.end());
            }
            auto &stream = **it;
            std::string_view independent, receiver;
            if (checks.null(i))
                throw std::runtime_error(
                    "Missing independently checked SBAS CRC");
            auto first = ArrowArrayViewListChildOffset(checks.v, i),
                 end = ArrowArrayViewListChildOffset(checks.v, i + 1);
            for (auto j = first; j < end; ++j) {
                if (check.null(j))
                    throw std::runtime_error("Invalid SBAS check");
                if (kind.text(j) != "crc" || scope.text(j) != "message")
                    continue;
                auto o = origin.text(j);
                if (o == "independent")
                    independent = result.text(j);
                if (o == "receiver")
                    receiver = result.text(j);
            }
            if (independent != "pass" && independent != "fail")
                throw std::runtime_error(
                    "Missing independently checked SBAS CRC");
            GridFrame f{t,
                        next_frame_++,
                        {},
                        independent == "pass",
                        receiver != "fail"};
            std::memcpy(f.bytes.data(), bytes.data.as_uint8, 32);
            stream.pending.push_back(f);
            stream.last = t;
            if (stream.pending.size() >= 8192)
                flush(stream);
        }
        return drain();
    }
    std::vector<GridInterval> end_day() {
        active();
        if (!raw_started_) {
            std::sort(events_.begin(), events_.end());
            events_.erase(std::unique(events_.begin(), events_.end()),
                          events_.end());
        }
        while (event_ < events_.size())
            advance(events_[event_++]);
        for (auto &s : streams_)
            flush(*s);
        day_ = -1;
        return drain();
    }
    std::vector<GridInterval> finish() {
        if (day_ >= 0 || finished_)
            throw std::runtime_error("End SBAS day before finish");
        while (!streams_.empty())
            close(0, std::max(streams_[0]->last,
                              previous_nav_.value_or(streams_[0]->last)));
        finished_ = true;
        diagnostics_["raw_bits_frames"] = next_frame_;
        return drain();
    }
    Json diagnostics() const { return diagnostics_; }
};
} // namespace neognss_obs
