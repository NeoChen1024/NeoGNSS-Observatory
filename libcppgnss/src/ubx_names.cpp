// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Kelei Chen

#include <cppgnss/ubx_names.hpp>

namespace UBX
{

// u-blox GNSS id
static ubx_name_map_t ubx_gnssid_names =
{
	{0, "GPS"},
	{1, "SBAS"},
	{2, "GAL"},
	{3, "BDS"},
	{4, "IMES"},
	{5, "QZSS"},
	{6, "GLO"},
	{7, "NavIC"}
};

static ubx_name_map_t ubx_gnssid_abbr_names =
{
	{0, "G"},
	{1, "S"},
	{2, "E"},
	{3, "B"},
	{4, "I"},
	{5, "Q"},
	{6, "R"},
	{7, "N"}
};

string ubx_gnssid_name(uint8_t gnssid)
{
	if(ubx_gnssid_names.count(gnssid) == 0)
	{
		return "?";
	}
	return ubx_gnssid_names[gnssid];
}

string ubx_gnssid_abbr_name(uint8_t gnssid)
{
	if(ubx_gnssid_abbr_names.count(gnssid) == 0)
	{
		return "?";
	}
	return ubx_gnssid_abbr_names[gnssid];
}

} // namespace UBX
