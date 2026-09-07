// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Kelei Chen

#include <cppgnss/ubx_nav.hpp>

#include <cppgnss/ubx_ids_gen.hpp>

#include <format>

namespace UBX
{

bool ubx_nav_pvt_semantically_valid(const ubx_nav_pvt &pvt)
{
	if(!pvt.valid)
		return false;

	const _ubx_nav_pvt &data = pvt.data;
	// UBX-NAV-PVT valid bits 0 and 1 indicate a valid date and time.
	if((data.valid_bit & 0x03) != 0x03)
		return false;
	if(data.month < 1 || data.month > 12)
		return false;
	if(data.day < 1 || data.day > 31)
		return false;
	if(data.hour > 23 || data.min > 59)
		return false;
	// A leap second may be represented as 60.
	return data.second <= 60;
}

bool ubx_nav_pvt_fix_ok(const ubx_nav_pvt &pvt)
{
	return ubx_nav_pvt_semantically_valid(pvt) && (pvt.data.flags_bit & 1) &&
		pvt.data.fixType >= 2 && pvt.data.fixType <= 5;
}

bool ubx_nav_eoe_semantically_valid(const ubx_nav_eoe &eoe)
{
	return eoe.valid && eoe.data.iTOW <= UINT32_C(86400) * 1000 * 7;
}

std::string ubx_nav_pvt_fix_type(const ubx_nav_pvt &pvt)
{
	if(!ubx_nav_pvt_semantically_valid(pvt))
		return "INVALID";

	std::string fix_type;
	switch(pvt.data.fixType)
	{
	case 0: fix_type = "NO"; break;
	case 1: fix_type = "DR"; break;
	case 2: fix_type = "2D"; break;
	case 3: fix_type = "3D"; break;
	case 4: fix_type = "G+DR"; break;
	case 5: fix_type = "TIME"; break;
	default: fix_type = "?"; break;
	}
	if(pvt.data.flags_bit & 0x02)
		fix_type += "/DGNSS";
	return fix_type;
}

void ubx_nav_pvt_dump(const ubx_nav_pvt &pvt, FILE *fp)
{
	const _ubx_nav_pvt &data = pvt.data;
	auto dump = std::format(
		"(NAV-PVT, iTOW={}, year={}, month={}, day={}, hour={}, min={}, sec={}, valid={}, "
		"tAcc={}, nano={}, fixType={}, flags={}, flags2={}, numSV={}, lon={}, lat={}, "
		"height={}, hMSL={}, hAcc={}, vAcc={}, velN={}, velE={}, velD={}, gSpeed={}, "
		"headMot={}, sAcc={}, headAcc={}, pDOP={}, headVeh={})\n",
		data.iTOW, data.year, data.month, data.day, data.hour, data.min, data.second,
		data.valid_bit, data.tAcc, data.nano, data.fixType, data.flags_bit, data.flags2_bit,
		data.numSV, data.lon, data.lat, data.height, data.hMSL, data.hAcc, data.vAcc,
		data.velN, data.velE, data.velD, data.gSpeed, data.headMot, data.sAcc,
		data.headAcc, data.pDOP, data.headVeh);
	fputs(dump.c_str(), fp);
}

void ubx_nav_eoe_dump(const ubx_nav_eoe &eoe, FILE *fp)
{
	auto dump = std::format("(NAV-EOE, iTOW={})\n", eoe.data.iTOW);
	fputs(dump.c_str(), fp);
}

bool ubx_nav_dump_custom(const ubx_frame &frame, FILE *fp)
{
	if(frame.class_id != UBX_CLASS_NAV)
		return false;

	switch(frame.msg_id)
	{
	case UBX_NAV_PVT:
	{
		ubx_nav_pvt pvt(frame);
		if(pvt.valid) ubx_nav_pvt_dump(pvt, fp);
		else fputs("(NAV-PVT, invalid payload)\n", fp);
		return true;
	}
	case UBX_NAV_EOE:
	{
		ubx_nav_eoe eoe(frame);
		if(eoe.valid) ubx_nav_eoe_dump(eoe, fp);
		else fputs("(NAV-EOE, invalid payload)\n", fp);
		return true;
	}
	default:
		return false;
	}
}

} // namespace UBX
