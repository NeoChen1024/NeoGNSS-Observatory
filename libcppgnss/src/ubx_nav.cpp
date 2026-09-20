// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Kelei Chen

#include <cppgnss/ubx_nav.hpp>

#include <cppgnss/ubx_ids_gen.hpp>

#include <format>

namespace UBX {

bool ubx_nav_pvt_semantically_valid(const ubx_nav_pvt &pvt) {

    const _ubx_nav_pvt &data = pvt.data;
    // UBX-NAV-PVT valid bits 0 and 1 indicate a valid date and time.
    if ((data.valid_bit & 0x03) != 0x03)
        return false;
    if (data.month < 1 || data.month > 12)
        return false;
    if (data.day < 1 || data.day > 31)
        return false;
    if (data.hour > 23 || data.min > 59)
        return false;
    // A leap second may be represented as 60.
    return data.second <= 60;
}

bool ubx_nav_pvt_fix_ok(const ubx_nav_pvt &pvt) {
    return ubx_nav_pvt_semantically_valid(pvt) && (pvt.data.flags_bit & 1) &&
           pvt.data.fixType >= 2 && pvt.data.fixType <= 5;
}

bool ubx_nav_eoe_semantically_valid(const ubx_nav_eoe &eoe) {
    return eoe.data.iTOW <= UINT32_C(86400) * 1000 * 7;
}

std::string ubx_nav_pvt_fix_type(const ubx_nav_pvt &pvt) {
    if (!ubx_nav_pvt_semantically_valid(pvt))
        return "INVALID";

    std::string fix_type;
    switch (pvt.data.fixType) {
    case 0:
        fix_type = "NO";
        break;
    case 1:
        fix_type = "DR";
        break;
    case 2:
        fix_type = "2D";
        break;
    case 3:
        fix_type = "3D";
        break;
    case 4:
        fix_type = "G+DR";
        break;
    case 5:
        fix_type = "TIME";
        break;
    default:
        fix_type = "?";
        break;
    }
    if (pvt.data.flags_bit & 0x02)
        fix_type += "/DGNSS";
    return fix_type;
}

} // namespace UBX
