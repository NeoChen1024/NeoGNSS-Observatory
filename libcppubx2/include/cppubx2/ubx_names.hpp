// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Kelei Chen

#include <cppubx2/ubx_def.hpp>

#pragma once

namespace UBX
{
string ubx_msg_name(uint8_t class_id, uint8_t msg_id);
// MGA message subtypes use the first payload byte; without it return the family.
string ubx_msg_name(uint8_t class_id, uint8_t msg_id, std::span<const uint8_t> payload);
string ubx_gnssid_name(uint8_t gnssid);
string ubx_gnssid_abbr_name(uint8_t gnssid);
} // namespace UBX
