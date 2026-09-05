// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppubx2/ubx_def.hpp>
#include <functional>
#include <limits>

namespace UBX {
constexpr int64_t unknown_utc = std::numeric_limits<int64_t>::min();
enum ArchiveFlags : uint32_t {
    archive_eoe = 1, archive_pvt = 2, archive_inferred_time = 4,
    archive_no_nav = 8, archive_partial = 16, archive_noise = 32,
    archive_time_conflict = 64
};
struct ArchiveEpoch {
    uint64_t begin = 0, end = 0;
    int64_t utc = unknown_utc;
    int64_t tow_ms = -1;
    uint64_t fingerprint = 0;
    uint32_t flags = 0, frames = 0, nav_frames = 0;
    int32_t gps_week = -1;
};
// Input is borrowed. Emits an exhaustive, nonoverlapping partition of bytes.
// Valid-frame runs are grouped by EOE; missing EOE falls back to known timestamp
// transitions. Untimed messages retain order and have no claimed transmit time.
// UTC mapping is learned only from valid NAV-PVT date/time; it is never taken
// from filenames. Leap-second labels are rejected pending explicit support.
void scan_archive(std::span<const uint8_t> bytes,
                  const std::function<void(const ArchiveEpoch &)> &emit);
}
