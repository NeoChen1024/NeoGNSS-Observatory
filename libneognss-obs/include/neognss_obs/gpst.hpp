// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <bit>
#include <boost/int128/int128.hpp>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace neognss_obs {
// Exact GPST picoseconds since 1980-01-06T00:00:00.
using GpstTicks = boost::int128::int128;
inline constexpr int64_t gpst_ticks_per_second = 1000000000000;

// GPST day number (days since the GPS epoch) of a non-negative time.
inline int64_t gpst_day(GpstTicks t) {
    return int64_t(t / gpst_ticks_per_second / 86400);
}

// TOW (ticks within a week) placed in the GPST week nearest to NOW.
inline GpstTicks nearest_week(GpstTicks now, GpstTicks tow) {
    constexpr int64_t week = 604800 * gpst_ticks_per_second;
    GpstTicks t = now / week * week + tow;
    if (t - now > week / 2)
        t -= week;
    if (now - t > week / 2)
        t += week;
    return t;
}

// RAW * SCALE * 2^-SHIFT exactly, without an intermediate double. The
// magnitude is rounded half to even, so the result is symmetric in sign.
inline GpstTicks binary_scaled(GpstTicks raw, int shift, GpstTicks scale) {
    GpstTicks x = raw * scale;
    const bool negative = x < 0;
    if (negative)
        x = -x;
    GpstTicks q = 0;
    if (shift <= 0) {
        if (-shift > 126 || x > (GpstTicks(1) << (126 + shift)))
            throw std::range_error("Exact binary value outside int128");
        q = x << -shift;
    } else if (shift < 127) {
        q = x >> shift;
        const GpstTicks r = x - (q << shift), half = GpstTicks(1)
                                                     << (shift - 1);
        if (r > half || (r == half && (q & 1) != 0))
            ++q;
    }
    return negative ? -q : q;
}
// A finite binary64 VALUE * SCALE as an exact integer, rounded half to even.
inline GpstTicks binary_scaled(double value, int64_t scale) {
    const auto bits = std::bit_cast<uint64_t>(value);
    const int exponent = int((bits >> 52) & 2047);
    uint64_t mantissa = bits & ((uint64_t(1) << 52) - 1);
    if (exponent)
        mantissa |= uint64_t(1) << 52;
    const GpstTicks raw = GpstTicks(mantissa);
    return binary_scaled(bits >> 63 ? -raw : raw,
                         52 - (exponent ? exponent - 1023 : -1022), scale);
}

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
