// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Kelei Chen
#include <cppubx2/ubx_reader.hpp>

namespace UBX
{
ReadResult read_ubx_frame(void *context, read_byte_fn read_byte, ubx_buf_t &buf, size_t *discarded_bytes)
{
	if(discarded_bytes) *discarded_bytes = 0;
	bool have_sync1 = false;
	size_t wasted_bytes = 0;

	while(true)
	{
		auto read_result = read_byte(context);
		if(read_result.result != ReadResult::ok)
			return read_result.result == ReadResult::end && have_sync1
				? ReadResult::truncated : read_result.result;
		uint8_t c = read_result.byte;
		if(!have_sync1)
		{
			have_sync1 = c == UBX_SYNC1;
			if(!have_sync1) wasted_bytes++;
			continue;
		}
		if(c == UBX_SYNC2) break;

		// The previous SYNC1 was noise. Retain a new SYNC1 so B5 B5 62
		// resynchronizes at the second byte instead of dropping the frame.
		wasted_bytes++;
		have_sync1 = c == UBX_SYNC1;
		if(!have_sync1) wasted_bytes++;
	}

	buf.clear();
	if(discarded_bytes) *discarded_bytes = wasted_bytes;

	for(size_t i = 0; i < UBX_HEADER_SIZE; i++)
	{
		auto read_result = read_byte(context);
		if(read_result.result != ReadResult::ok)
			return read_result.result == ReadResult::end
				? ReadResult::truncated : read_result.result;
		buf.push_back(read_result.byte);
	}

	size_t length = size_t(buf[UBX_LENGTH_OFFSET]) |
		(size_t(buf[UBX_LENGTH_OFFSET + 1]) << 8);
	for(size_t i = 0; i < length + UBX_CKSUM_SIZE; i++)
	{
		auto read_result = read_byte(context);
		if(read_result.result != ReadResult::ok)
			return read_result.result == ReadResult::end
				? ReadResult::truncated : read_result.result;
		buf.push_back(read_result.byte);
	}
	return ReadResult::ok;
}

} // namespace UBX
