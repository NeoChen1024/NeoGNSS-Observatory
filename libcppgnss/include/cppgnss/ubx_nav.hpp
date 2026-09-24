// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Kelei Chen

#pragma once

#include <cppgnss/ubx_nav_gen.hpp>

#include <cstdio>
#include <string>

namespace UBX {

// Generated parser validity only covers frame identity, payload length and
// structural decoding. These helpers apply application-level NAV semantics.
bool ubx_nav_pvt_semantically_valid(const ubx_nav_pvt &pvt);
// A usable GNSS solution includes TIME-only, not just a position fix.
bool ubx_nav_pvt_fix_ok(const ubx_nav_pvt &pvt);

std::string ubx_nav_pvt_fix_type(const ubx_nav_pvt &pvt);

} // namespace UBX
