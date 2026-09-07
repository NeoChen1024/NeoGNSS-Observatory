// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppgnss/ubx_def.hpp>

namespace UBX
{
enum class ReadResult
{
	ok,
	end,
	truncated,
	timeout,
	error,
};

struct ByteReadResult
{
	ReadResult result;
	uint8_t byte = 0;
};

using read_byte_fn = ByteReadResult (*)(void *context);


// Reads one frame excluding sync bytes; validate its checksum with ubx_frame.
// A callback failure discards partial framing state. The buffer is unspecified
// unless the result is ok. discarded_bytes counts noise preceding a found sync;
// it remains zero if no complete sync was found. No I/O or diagnostics are owned
// by this function. Length-corrupt frames may consume subsequent sync bytes.
ReadResult read_ubx_frame(void *context, read_byte_fn read_byte, ubx_buf_t &buf,
                        size_t *discarded_bytes = nullptr);
} // namespace UBX
