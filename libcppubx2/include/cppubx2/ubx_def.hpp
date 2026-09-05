// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Kelei Chen

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <map>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <cppubx2/ubx_ids_gen.hpp>

#pragma once

namespace UBX
{
using std::vector;
using std::map;
using std::string;

constexpr uint8_t UBX_SYNC1	= 0xB5;
constexpr uint8_t UBX_SYNC2	= 0x62;
constexpr uint8_t UBX_CLASS_OFFSET	= 0;
constexpr uint8_t UBX_MSG_OFFSET	= 1;
constexpr uint8_t UBX_LENGTH_OFFSET	= 2;
constexpr uint8_t UBX_HEADER_SIZE	= 4;
constexpr uint8_t UBX_CKSUM_SIZE	= 2;

typedef vector<uint8_t> ubx_buf_t;
typedef map<uint8_t, string> ubx_name_map_t;

template<typename T>
concept UbxScalar =
	(std::integral<T> || std::floating_point<T>) &&
	(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

template<UbxScalar T>
T little_to_native(T value)
{
	if constexpr(sizeof(T) == 1 || std::endian::native == std::endian::little)
	{
		return value;
	}
	else
	{
		static_assert(std::endian::native == std::endian::big,
			"mixed-endian targets are not supported");
		auto bytes = std::bit_cast<std::array<uint8_t, sizeof(T)>>(value);
		std::reverse(bytes.begin(), bytes.end());
		return std::bit_cast<T>(bytes);
	}
}

template<UbxScalar T>
T read_le(std::span<const uint8_t> payload, size_t offset)
{
	if(offset > payload.size() || sizeof(T) > payload.size() - offset)
		throw std::out_of_range("UBX scalar exceeds payload bounds");

	std::array<uint8_t, sizeof(T)> bytes{};
	std::copy_n(payload.begin() + offset, sizeof(T), bytes.begin());
	if constexpr(std::endian::native == std::endian::big && sizeof(T) > 1)
		std::reverse(bytes.begin(), bytes.end());
	return std::bit_cast<T>(bytes);
}

// Diagnostics are silent by default and scoped to the calling thread.
// The handler must not retain detail; restore the returned previous handler
// after a scoped operation.
using parse_error_handler = void (*)(std::string_view, std::source_location);
parse_error_handler set_parse_error_handler(parse_error_handler handler);

void report_parse_error(
	std::string_view detail,
	std::source_location location = std::source_location::current());

class ubx_frame
{
public:
	uint8_t class_id;
	uint8_t msg_id;
	uint16_t length;
	ubx_buf_t payload;
	// CK_A is high byte, CK_B is low byte
	uint16_t cksum;

	bool valid;

	ubx_frame();
	ubx_frame(std::span<const uint8_t> buf);
	void clear();
	void dump(FILE *fp) const;
	int write(FILE *fp) const;
private:
	bool validate(std::span<const uint8_t> buf);
};

class ubx_any_msg
{
public:
	bool valid;
	uint8_t class_id;
	uint8_t msg_id;
	ubx_buf_t payload;

	ubx_any_msg();
	ubx_any_msg(const ubx_frame &frame);
	bool parse(const ubx_frame &frame);
	void clear();
	void dump(FILE *fp) const;
};

} // namespace UBX
