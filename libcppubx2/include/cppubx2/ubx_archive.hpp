// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppubx2/ubx_def.hpp>
#include <functional>
#include <limits>

namespace UBX {
constexpr int64_t unknown_gpst = std::numeric_limits<int64_t>::min();
enum ArchiveFlags : uint32_t {
    archive_eoe = 1, archive_pvt = 2, archive_inferred_time = 4,
    archive_no_nav = 8, archive_partial = 16, archive_noise = 32,
    archive_time_conflict = 64, archive_rawx = 128
};
struct ArchiveEpoch {
    uint64_t begin = 0, end = 0;
    int64_t gpst = unknown_gpst;
    int64_t tow_ms = -1;
    uint64_t fingerprint = 0;
    uint32_t flags = 0, frames = 0, nav_frames = 0;
    int32_t gps_week = -1;
};
// Input is borrowed. Emits an exhaustive, nonoverlapping partition of bytes.
// Valid-frame runs are grouped by EOE; missing EOE falls back to known timestamp
// transitions. Untimed messages retain order and have no claimed transmit time.
// GPST seconds since 1980-01-06 are anchored by NAV-TIMEGPS or RXM-RAWX.
// NAV-PVT calendar fields and filenames never define this time axis.
void scan_archive(std::span<const uint8_t> bytes,
                  const std::function<void(const ArchiveEpoch &)> &emit);
}
