// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <boost/int128/int128.hpp>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace neognss_obs {
// Exact GPST picoseconds since 1980-01-06T00:00:00.
using GpstTicks = boost::int128::int128;

// Picoseconds to int64 nanoseconds, rounded half to even.
inline int64_t nearest_ns(GpstTicks ps) {
    GpstTicks ns = ps / 1000, remainder = ps % 1000;
    if (remainder > 500 || (remainder == 500 && (ns & 1) != 0))
        ++ns;
    if (ns > std::numeric_limits<int64_t>::max() ||
        ns < std::numeric_limits<int64_t>::min())
        throw std::runtime_error("Time outside int64 nanoseconds");
    return int64_t(ns);
}
} // namespace neognss_obs
