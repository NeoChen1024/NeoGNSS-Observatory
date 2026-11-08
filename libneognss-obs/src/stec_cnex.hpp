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
        ArrowBatchView owner(schema, array);
        auto *v = &owner.value;
        if (v->storage_type != NANOARROW_TYPE_STRUCT)
            throw std::runtime_error("Expected Arrow RecordBatch");
        Column root{v, schema};
        auto time = root.child("gpst"), system = root.child("satellite_system"),
             signal = root.child("signal"), setup = root.child("setup_id");
        if (!time.decimal_seconds())
            throw std::runtime_error("Expected GPST decimal128(38,12) seconds");
        auto tracking_column = root.child("tracking"),
             quality_column = root.child("quality"),
             corrections_column = root.child("receiver_corrections");
        auto lock = tracking_column.child("lock_duration_s");
        auto sat = root.child("satellite_number").v,
             code_value = root.child("pseudorange_m").v,
             phase_value = root.child("carrier_phase_cycles").v,
             tracking = tracking_column.v, quality = quality_column.v,
             corrections = corrections_column.v;
        auto code_valid = quality_column.child("code_valid").v,
             phase_valid = quality_column.child("phase_valid").v,
             half = tracking_column.child("half_cycle_ambiguity").v,
             sub_half = tracking_column.child("half_cycle_subtracted").v,
             counter = tracking_column.child("continuity_counter").v,
             modulus = tracking_column.child("continuity_counter_modulus").v,
             lock_bound =
                 tracking_column.child("lock_duration_is_lower_bound").v;
        auto smoothed = corrections_column.child("code_smoothing_applied").v;
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
            if (setup.text(row) != setup_)
                throw std::runtime_error("Mixed CommonNEX Setup");
            auto stamp = time.seconds_ps(row);
            if (!stamp)
                throw std::runtime_error("Expected non-null CommonNEX GPST");
            Tick t = *stamp;
            if (last_ && t < *last_)
                throw std::runtime_error("CommonNEX observation time reversal");
            if (last_ && t == *last_ && !pending_)
                throw std::runtime_error(
                    "Repeated already-closed CommonNEX epoch");
            if (!last_ || t != *last_) {
                int64_t ns = nearest_ns(t);
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
                pending_->gpst_ns = ns;
                last_ = t;
                rounded_ += t % 1000 != 0;
            }
            auto code = signal.text(row);
            auto sys = system.text(row);
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
                if (lock.null(r) != ArrowArrayViewIsNull(lock_bound, r))
                    throw std::runtime_error(
                        "Incomplete lock duration/bound flag");
                if (!lock.null(r)) {
                    (void)boolean(lock_bound, r);
                    m.lock_seconds = double(*lock.seconds_ps(r)) / 1e12;
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
