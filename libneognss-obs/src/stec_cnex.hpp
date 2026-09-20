// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <boost/int128/int128.hpp>
#include <cstring>
#include <limits>
#include <mutex>
#include <nanoarrow/nanoarrow.h>
#include <neognss_obs/ppp.hpp>

namespace neognss_obs {
// Bounded Arrow replay adapter. It owns only the last not-yet-closed epoch.
class StecCnexReader {
    using Tick = boost::int128::int128;
    std::string setup_, first_, second_;
    std::optional<Tick> last_;
    std::optional<neognss_obs::ObservationEpoch> pending_;
    uint64_t rounded_ = 0, smoothed_ = 0;
    struct View {
        ArrowArrayView value{};
        View(ArrowSchema *schema, ArrowArray *array) {
            ArrowError error{};
            if (ArrowArrayViewInitFromSchema(&value, schema, &error) ||
                ArrowArrayViewSetArray(&value, array, &error) ||
                ArrowArrayViewValidate(&value, NANOARROW_VALIDATION_LEVEL_FULL,
                                       &error)) {
                ArrowArrayViewReset(&value);
                throw std::runtime_error(
                    std::string("Invalid CommonNEX Arrow batch: ") +
                    error.message);
            }
        }
        ~View() { ArrowArrayViewReset(&value); }
    };
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
    StecCnexReader(std::string setup, std::string first, std::string second)
        : setup_(std::move(setup)), first_(std::move(first)),
          second_(std::move(second)) {}
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
        auto tracking = field("phase_tracking");
        auto ts = schema_child(schema, "phase_tracking");
        auto quality = [&](const char *n, int64_t row) {
            auto q = field(n);
            if (ArrowArrayViewIsNull(q, row))
                return false;
            auto status = text(child(q, schema_child(schema, n), "status"),
                               row + q->offset);
            if (status != "valid" && status != "invalid" && status != "unknown")
                throw std::runtime_error(
                    "Unsupported CommonNEX quality status");
            return status == "invalid";
        };
        ObservationBatch out;
        for (int64_t i = 0; i < v->length; ++i) {
            int64_t row = i + v->offset;
            if (text(field("setup_id"), row) != setup_)
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
                pending_->gpst_ns = int64_t(ns);
                last_ = t;
                rounded_ += remainder != 0;
            }
            auto code = text(signal, row);
            if (text(system, row) != "G" || (code != first_ && code != second_))
                continue;
            if (sat->storage_type != NANOARROW_TYPE_UINT16 ||
                ArrowArrayViewIsNull(sat, row))
                throw std::runtime_error("Expected CommonNEX satellite number");
            neognss_obs::Observation m;
            m.prn = static_cast<int>(ArrowArrayViewGetUIntUnsafe(sat, row));
            if (m.prn < 1 || m.prn > 32)
                throw std::runtime_error("Unsupported GPS satellite number");
            m.signal = code;
            m.frequency_hz = code == first_ ? 1575.42e6 : 1227.60e6;
            m.pseudorange_m = number(field("pseudorange_m"), row);
            m.phase_cycles = number(field("carrier_phase_cycles"), row);
            m.code_valid = std::isfinite(m.pseudorange_m) &&
                           m.pseudorange_m > 0 && !quality("code_quality", row);
            m.phase_valid =
                std::isfinite(m.phase_cycles) && !quality("phase_quality", row);
            if (!ArrowArrayViewIsNull(tracking, row)) {
                auto r = row + tracking->offset;
                m.half_cycle =
                    boolean(child(tracking, ts, "half_cycle_ambiguity"), r);
                m.sub_half_cycle =
                    boolean(child(tracking, ts, "half_cycle_subtracted"), r);
                m.loss_of_lock =
                    boolean(child(tracking, ts, "loss_of_lock"), r);
                auto counter = child(tracking, ts, "continuity_counter");
                auto modulus =
                    child(tracking, ts, "continuity_counter_modulus");
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
                auto l = child(tracking, ts, "lock");
                if (!ArrowArrayViewIsNull(l, r)) {
                    auto ls = schema_child(ts, "lock");
                    auto lower_schema = schema_child(ls, "lower_s");
                    if (std::strcmp(lower_schema->format, "d:38,12") != 0 &&
                        std::strcmp(lower_schema->format, "d:38,12,128") != 0)
                        throw std::runtime_error(
                            "Expected decimal128(38,12) lock duration");
                    auto lower = child(l, ls, "lower_s");
                    auto tick = ticks(lower, r + l->offset);
                    m.lock_seconds = double(tick) / 1e12;
                    m.lock_valid = true;
                }
            }
            auto corrections = field("receiver_corrections");
            if (!ArrowArrayViewIsNull(corrections, row)) {
                auto cs = schema_child(schema, "receiver_corrections");
                smoothed_ +=
                    boolean(child(corrections, cs, "code_smoothing_applied"),
                            row + corrections->offset);
            }
            for (const auto &old : pending_->signals)
                if (old.prn == m.prn && old.signal == m.signal)
                    throw std::runtime_error(
                        "Duplicate selected CommonNEX observation");
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
    Json summary() const {
        return {{"rounded_epochs_to_ns", rounded_},
                {"smoothed_selected_observations", smoothed_}};
    }
};
} // namespace neognss_obs
