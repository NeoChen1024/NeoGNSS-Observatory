// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <boost/int128/int128.hpp>
#include <optional>

namespace neognss_obs {
// Receiver evidence only: never use host elapsed time to date archive records.
struct ReceiverTime {
    using Tick = boost::int128::int128;
    static constexpr int64_t second = 1000000000000LL;
    Tick period, timeout, restart_threshold;
    std::optional<Tick> navigation, progress, uptime, anchor_uptime;
    std::optional<Tick> uptime_gpst, paired_offset;
    int64_t archive_day = 0;
    enum class Restart { none, uptime_decrease, offset_jump };
    explicit ReceiverTime(Tick cadence)
        : period(cadence), timeout(cadence * 10),
          restart_threshold(std::max(Tick(5) * second, cadence * 2)) {}

    void advance(Tick t) {
        if (!progress || t > *progress)
            progress = t;
        archive_day = int64_t(t / second / 86400);
    }
    std::optional<Tick> anchor() const {
        if (!navigation || (progress && *progress - *navigation > timeout) ||
            (uptime && anchor_uptime && *uptime - *anchor_uptime > timeout))
            return {};
        return navigation;
    }
    std::optional<Tick> associated_uptime() const {
        if (!uptime ||
            (uptime_gpst && progress && *progress - *uptime_gpst > timeout))
            return {};
        return uptime;
    }
    void set_navigation(std::optional<Tick> t) {
        navigation = t;
        if (t) {
            advance(*t);
            anchor_uptime = associated_uptime();
        } else
            anchor_uptime.reset();
    }
    Restart report_uptime(Tick t, std::optional<Tick> report_gpst) {
        auto reason = Restart::none;
        if (uptime && t < *uptime)
            reason = Restart::uptime_decrease;
        // A direct status timestamp, or a fresh navigation context whose
        // progress is independently known, is required for offset comparison.
        if (reason == Restart::none && report_gpst && navigation && progress &&
            *progress - *navigation <= period) {
            auto offset = *report_gpst - t;
            if (paired_offset && (offset - *paired_offset > restart_threshold ||
                                  *paired_offset - offset > restart_threshold))
                reason = Restart::offset_jump;
            paired_offset = offset;
        }
        if (reason != Restart::none) {
            navigation.reset();
            anchor_uptime.reset();
            paired_offset.reset();
        }
        uptime = t;
        uptime_gpst = progress;
        if (!anchor_uptime && navigation && report_gpst &&
            *report_gpst - *navigation <= period)
            anchor_uptime = t;
        return reason;
    }
};
} // namespace neognss_obs
