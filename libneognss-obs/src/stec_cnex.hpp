// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "arrow_batch.hpp"
#include <bitset>
#include <boost/int128/int128.hpp>
#include <cstring>
#include <limits>
#include <mutex>
#include <nanoarrow/nanoarrow.h>
#include <neognss_obs/ppp.hpp>
#include <unordered_map>
#include <vector>

namespace neognss_obs {
// Bounded Arrow replay adapter. It owns only the last not-yet-closed epoch.
class StecCnexReader {
    using Tick = boost::int128::int128;
    std::string setup_;
    struct Signal {
        double frequency;
        size_t index;
    };
    std::unordered_map<uint32_t, Signal> signals_;
    std::vector<std::bitset<100>> seen_;
    static uint32_t signal_key(std::string_view sys, std::string_view code) {
        if (sys.size() != 1 || code.size() != 2)
            return 0;
        return (uint32_t(uint8_t(sys[0])) << 16) |
               (uint32_t(uint8_t(code[0])) << 8) | uint8_t(code[1]);
    }
    std::optional<Tick> last_;
    std::optional<neognss_obs::ObservationEpoch> pending_;
    uint64_t rounded_ = 0, smoothed_ = 0;
    using View = ArrowBatchView;
    static ArrowArrayView *child(ArrowArrayView *v, const ArrowSchema *s,
                                 const char *name) {
        for (int64_t k = 0; k < s->n_children; ++k)
            if (s->children[k]->name &&
                std::strcmp(s->children[k]->name, name) == 0)
                return v->children[k];
        throw std::runtime_error(std::string("Missing CommonNEX field: ") +
                                 name);
    }
    static const ArrowSchema *schema_child(const ArrowSchema *s,
                                           const char *name) {
        for (int64_t k = 0; k < s->n_children; ++k)
            if (s->children[k]->name &&
                std::strcmp(s->children[k]->name, name) == 0)
                return s->children[k];
        throw std::runtime_error(std::string("Missing CommonNEX field: ") +
                                 name);
    }
    static std::string_view text(ArrowArrayView *v, int64_t i) {
        if (v->storage_type != NANOARROW_TYPE_STRING ||
            ArrowArrayViewIsNull(v, i))
            throw std::runtime_error("Expected non-null CommonNEX string");
        auto x = ArrowArrayViewGetStringUnsafe(v, i);
        return {x.data, size_t(x.size_bytes)};
    }
    static double number(ArrowArrayView *v, int64_t i) {
        if (v->storage_type != NANOARROW_TYPE_DOUBLE)
            throw std::runtime_error("Expected float64 observation");
        return ArrowArrayViewIsNull(v, i) ? NAN
                                          : ArrowArrayViewGetDoubleUnsafe(v, i);
    }
    static bool boolean(ArrowArrayView *v, int64_t i, bool missing = false) {
        if (v->storage_type != NANOARROW_TYPE_BOOL)
            throw std::runtime_error("Expected boolean quality flag");
        return ArrowArrayViewIsNull(v, i)
                   ? missing
                   : ArrowArrayViewGetIntUnsafe(v, i) != 0;
    }
    static Tick ticks(ArrowArrayView *v, int64_t i) {
        if (v->storage_type != NANOARROW_TYPE_DECIMAL128 ||
            ArrowArrayViewIsNull(v, i))
            throw std::runtime_error("Expected non-null decimal GPST");
        ArrowDecimal d;
        ArrowDecimalInit(&d, 128, 38, 12);
        ArrowArrayViewGetDecimalUnsafe(v, i, &d);
        if (d.words[d.high_word_index] >> 63)
            throw std::runtime_error("Negative GPST");
        return (Tick(d.words[d.high_word_index]) << 64) +
               d.words[d.low_word_index];
    }

  public:
    std::mutex mutex;
    StecCnexReader(std::string setup, const Json &pairs)
        : setup_(std::move(setup)) {
        for (const auto &p : pairs) {
            const std::string sys = p.at("system");
            for (auto suffix : {"1", "2"}) {
                std::string code = p.at(std::string("signal") + suffix);
                auto key = signal_key(sys, code);
                if (!key)
                    throw std::invalid_argument("Invalid STEC signal identity");
                auto [it, inserted] =
                    signals_.try_emplace(key, Signal{0, signals_.size()});
                it->second.frequency =
                    p.at(std::string("frequency") + suffix + "_hz");
            }
        }
        seen_.resize(signals_.size());
    }
    ObservationBatch feed(ArrowSchema *schema, ArrowArray *array) {
        View owner(schema, array);
        auto *v = &owner.value;
        if (v->storage_type != NANOARROW_TYPE_STRUCT)
            throw std::runtime_error("Expected Arrow RecordBatch");
        auto field = [&](const char *n) { return child(v, schema, n); };
        auto time = field("gpst"), system = field("satellite_system"),
             sat = field("satellite_number"), signal = field("signal");
        auto gpst_schema = schema_child(schema, "gpst");
        if (std::strcmp(gpst_schema->format, "d:38,12") != 0 &&
            std::strcmp(gpst_schema->format, "d:38,12,128") != 0)
            throw std::runtime_error("Expected GPST decimal128(38,12) seconds");
        auto tracking = field("tracking");
        auto ts = schema_child(schema, "tracking");
        auto setup = field("setup_id"), code_value = field("pseudorange_m"),
             phase_value = field("carrier_phase_cycles");
        auto quality = field("quality");
        auto qs = schema_child(schema, "quality");
        auto code_valid = child(quality, qs, "code_valid");
        auto phase_valid = child(quality, qs, "phase_valid");
        auto half = child(tracking, ts, "half_cycle_ambiguity"),
             sub_half = child(tracking, ts, "half_cycle_subtracted"),
             counter = child(tracking, ts, "continuity_counter"),
             modulus = child(tracking, ts, "continuity_counter_modulus"),
             lock = child(tracking, ts, "lock_duration_s"),
             lock_bound = child(tracking, ts, "lock_duration_is_lower_bound");
        auto lower_schema = schema_child(ts, "lock_duration_s");
        const bool valid_lock_type =
            std::strcmp(lower_schema->format, "d:38,12") == 0 ||
            std::strcmp(lower_schema->format, "d:38,12,128") == 0;
        auto corrections = field("receiver_corrections");
        auto smoothed =
            child(corrections, schema_child(schema, "receiver_corrections"),
                  "code_smoothing_applied");
        auto invalid = [&](ArrowArrayView *valid, int64_t row) {
            if (ArrowArrayViewIsNull(quality, row))
                return false;
            // Unknown source validity is not a veto; numerical acceptance
            // remains this processor's policy, not a stored validity claim.
            return !boolean(valid, row + quality->offset, true);
        };
        ObservationBatch out;
        for (int64_t i = 0; i < v->length; ++i) {
            int64_t row = i + v->offset;
            if (text(setup, row) != setup_)
                throw std::runtime_error("Mixed CommonNEX Setup");
            Tick t = ticks(time, row);
            if (last_ && t < *last_)
                throw std::runtime_error("CommonNEX observation time reversal");
            if (last_ && t == *last_ && !pending_)
                throw std::runtime_error(
                    "Repeated already-closed CommonNEX epoch");
            if (!last_ || t != *last_) {
                Tick ns = t / 1000, remainder = t % 1000;
                if (remainder > 500 || (remainder == 500 && (ns & 1) != 0))
                    ++ns;
                if (ns > std::numeric_limits<int64_t>::max())
                    throw std::runtime_error(
                        "STEC epoch outside int64 nanoseconds");
                if (pending_) {
                    if (ns <= pending_->gpst_ns)
                        throw std::runtime_error(
                            "Distinct GPST epochs collide at STEC nanosecond "
                            "precision");
                    out.epochs.push_back(std::move(*pending_));
                }
                pending_ = neognss_obs::ObservationEpoch{};
                for (auto &seen : seen_)
                    seen.reset();
                pending_->gpst_ns = int64_t(ns);
                last_ = t;
                rounded_ += remainder != 0;
            }
            auto code = text(signal, row);
            auto sys = text(system, row);
            auto frequency = signals_.find(signal_key(sys, code));
            if (frequency == signals_.end())
                continue;
            if (sat->storage_type != NANOARROW_TYPE_UINT16 ||
                ArrowArrayViewIsNull(sat, row))
                throw std::runtime_error("Expected CommonNEX satellite number");
            neognss_obs::Observation m;
            m.prn = static_cast<int>(ArrowArrayViewGetUIntUnsafe(sat, row));
            m.system = sys[0];
            if (m.prn < 1 || m.prn > 99)
                throw std::runtime_error("Unsupported satellite number");
            m.signal = code;
            m.frequency_hz = frequency->second.frequency;
            m.pseudorange_m = number(code_value, row);
            m.phase_cycles = number(phase_value, row);
            m.code_valid = std::isfinite(m.pseudorange_m) &&
                           m.pseudorange_m > 0 && !invalid(code_valid, row);
            m.phase_valid =
                std::isfinite(m.phase_cycles) && !invalid(phase_valid, row);
            if (!ArrowArrayViewIsNull(tracking, row)) {
                auto r = row + tracking->offset;
                m.half_cycle = boolean(half, r);
                m.sub_half_cycle = boolean(sub_half, r);
                if (ArrowArrayViewIsNull(counter, r) !=
                    ArrowArrayViewIsNull(modulus, r))
                    throw std::runtime_error(
                        "Incomplete continuity counter/modulus");
                if (!ArrowArrayViewIsNull(counter, r)) {
                    if (counter->storage_type != NANOARROW_TYPE_UINT32 ||
                        modulus->storage_type != NANOARROW_TYPE_UINT32)
                        throw std::runtime_error("Invalid continuity counter");
                    m.continuity_counter =
                        ArrowArrayViewGetUIntUnsafe(counter, r);
                    auto mod = ArrowArrayViewGetUIntUnsafe(modulus, r);
                    if (mod < 2 || *m.continuity_counter >= mod)
                        throw std::runtime_error(
                            "Continuity counter outside modulus");
                }
                if (ArrowArrayViewIsNull(lock, r) !=
                    ArrowArrayViewIsNull(lock_bound, r))
                    throw std::runtime_error(
                        "Incomplete lock duration/bound flag");
                if (!ArrowArrayViewIsNull(lock, r)) {
                    if (!valid_lock_type)
                        throw std::runtime_error(
                            "Expected decimal128(38,12) lock duration");
                    (void)boolean(lock_bound, r);
                    auto tick = ticks(lock, r);
                    m.lock_seconds = double(tick) / 1e12;
                    m.lock_valid = true;
                }
            }
            if (!ArrowArrayViewIsNull(corrections, row)) {
                smoothed_ += boolean(smoothed, row + corrections->offset);
            }
            auto &seen = seen_[frequency->second.index];
            if (seen.test(m.prn))
                throw std::runtime_error(
                    "Duplicate selected CommonNEX observation");
            seen.set(m.prn);
            pending_->signals.push_back(std::move(m));
        }
        return out;
    }
    ObservationBatch flush() {
        ObservationBatch out;
        if (pending_) {
            out.epochs.push_back(std::move(*pending_));
            pending_.reset();
        }
        return out;
    }
    ObservationBatch complete(int64_t through_ns) {
        if (pending_ && pending_->gpst_ns <= through_ns)
            return flush();
        return {};
    }
    Json summary() const {
        return {{"rounded_epochs_to_ns", rounded_},
                {"smoothed_selected_observations", smoothed_}};
    }
};
} // namespace neognss_obs
