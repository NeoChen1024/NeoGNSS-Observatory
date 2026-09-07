// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Kelei Chen

#pragma once

#include <cppubx2/ubx_nav_gen.hpp>

#include <cstdio>
#include <string>

namespace UBX
{

// Generated parser validity only covers frame identity, payload length and
// structural decoding. These helpers apply application-level NAV semantics.
bool ubx_nav_pvt_semantically_valid(const ubx_nav_pvt &pvt);
// A usable GNSS solution includes TIME-only, not just a position fix.
bool ubx_nav_pvt_fix_ok(const ubx_nav_pvt &pvt);
bool ubx_nav_eoe_semantically_valid(const ubx_nav_eoe &eoe);

std::string ubx_nav_pvt_fix_type(const ubx_nav_pvt &pvt);

void ubx_nav_pvt_dump(const ubx_nav_pvt &pvt, FILE *fp);
void ubx_nav_eoe_dump(const ubx_nav_eoe &eoe, FILE *fp);

// Returns true when the frame is a NAV message with a custom dump function.
bool ubx_nav_dump_custom(const ubx_frame &frame, FILE *fp);

} // namespace UBX
