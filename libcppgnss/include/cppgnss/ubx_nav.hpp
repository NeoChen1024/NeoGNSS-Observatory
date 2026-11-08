// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Kelei Chen

#pragma once

#include <cppgnss/ubx_nav_gen.hpp>

#include <cstdio>
#include <optional>
#include <string>

namespace UBX {

// Generated parser validity only covers frame identity, payload length and
// structural decoding. These helpers apply application-level NAV semantics.
bool ubx_nav_pvt_semantically_valid(const ubx_nav_pvt &pvt);
// Fix classification of a semantically valid NAV-PVT; the caller owns that
// validity gate. A usable GNSS solution includes TIME-only, not just a
// position fix.
bool ubx_nav_pvt_fix_ok(const ubx_nav_pvt &pvt);
std::string ubx_nav_pvt_fix_type(const ubx_nav_pvt &pvt);

struct NavTimeGps {
    int64_t gpst_ms; // week * 604800000 + iTOW
    int32_t ftow_ns; // signed fraction to add to gpst_ms
};
// GPS time of a UBX-NAV-TIMEGPS frame whose validity flags mark both TOW and
// week valid, with a non-negative week and iTOW inside the week. Other frames,
// other lengths and invalid times have none.
std::optional<NavTimeGps> ubx_nav_timegps_time(const cppgnss::FrameView &frame);

} // namespace UBX
