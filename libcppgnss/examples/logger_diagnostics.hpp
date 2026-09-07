// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string_view>

// Application diagnostics only: never change stream bytes or recording policy.
struct LoggerDiagnostics {
    using Clock = std::chrono::steady_clock;
    int64_t interval_ms = 1000, tolerance_percent = 20, previous_ms = -1;
    bool expect_clock = false;
    uint64_t epochs = 0, gaps = 0, estimated_missing = 0, irregular = 0;
    uint64_t missing_clock = 0, mismatched_clock = 0, duplicate_clock = 0;
    uint64_t timeouts = 0, reconnects = 0, invalid_checksums = 0;
    Clock::time_point started = Clock::now(), previous_arrival = started;

    static std::string label(int64_t ms) {
        if(ms < 0) return "unavailable";
        using namespace std::chrono;
        const auto date = sys_days{year{1980}/1/6} + milliseconds{ms};
        return std::format("GPST-{:%Y-%m-%d--%H-%M-%S}-{:03}", floor<seconds>(date), ms % 1000);
    }
    void transport(std::string_view kind) const {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        const auto text = std::format("\n[logger qc] {} elapsed_ms={} last_epoch={}\n", kind, elapsed, label(previous_ms));
        fputs(text.c_str(), stderr);
    }
    void epoch(int64_t ms, uint32_t clock_count, uint32_t clock_tow) {
        const auto now = Clock::now();
        if(previous_ms >= 0) {
            const int64_t dt = ms - previous_ms;
            if(std::abs(dt - interval_ms) * 100 > interval_ms * tolerance_percent) {
                const auto arrival = std::chrono::duration_cast<std::chrono::milliseconds>(now - previous_arrival).count();
                const auto slots = std::llround(double(dt) / interval_ms);
                const bool aligned = slots >= 1 && std::abs(dt - slots * interval_ms) * 100 <= interval_ms * tolerance_percent;
                const bool gap = dt * 100 > interval_ms * (100 + tolerance_percent);
                if(gap) ++gaps;
                if(!gap || !aligned) ++irregular;
                if(gap && aligned) estimated_missing += slots - 1;
                const auto text = std::format(
                    "\n[logger qc] {} previous={} current={} interval_ms={} expected_ms={} "
                    "arrival_interval_ms={} estimated_missing={}\n",
                    gap ? "epoch_gap" : "irregular_interval", label(previous_ms), label(ms), dt,
                    interval_ms, arrival, aligned && gap ? std::to_string(slots - 1) : "unknown");
                fputs(text.c_str(), stderr);
            }
        }
        if(expect_clock && !clock_count) {
            ++missing_clock;
            const auto text = std::format("\n[logger qc] missing_NAV-CLOCK epoch={}\n", label(ms));
            fputs(text.c_str(), stderr);
        }
        if(clock_count && clock_tow % 604800000 != ms % 604800000) {
            ++mismatched_clock;
            const auto text = std::format("\n[logger qc] mismatched_NAV-CLOCK epoch={} clock_iTOW_ms={}\n", label(ms), clock_tow);
            fputs(text.c_str(), stderr);
        }
        if(clock_count > 1) {
            ++duplicate_clock;
            const auto text = std::format("\n[logger qc] duplicate_NAV-CLOCK epoch={} count={}\n", label(ms), clock_count);
            fputs(text.c_str(), stderr);
        }
        ++epochs;
        previous_ms = ms;
        previous_arrival = now;
    }
    void summary() const {
        const auto text = std::format(
            "\n[logger qc totals] epochs={} gaps={} estimated_missing={} irregular={} "
            "missing_NAV-CLOCK={} mismatched_NAV-CLOCK={} duplicate_NAV-CLOCK={} "
            "timeouts={} reconnects={} invalid_checksums={} last_epoch={} expected_ms={} tolerance_percent={}\n",
            epochs, gaps, estimated_missing, irregular, missing_clock, mismatched_clock,
            duplicate_clock, timeouts, reconnects, invalid_checksums, label(previous_ms), interval_ms, tolerance_percent);
        fputs(text.c_str(), stderr);
    }
    ~LoggerDiagnostics() { summary(); }
};
