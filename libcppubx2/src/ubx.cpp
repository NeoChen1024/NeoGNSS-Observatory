// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Kelei Chen

#include <cstdint>
#include <cstring>
#include <format>
#include <stdio.h>
#include <cppubx2/ubx.hpp>

namespace UBX
{
using std::string;

static thread_local parse_error_handler error_handler = nullptr;

parse_error_handler set_parse_error_handler(parse_error_handler handler)
{
	auto previous = error_handler;
	error_handler = handler;
	return previous;
}

void report_parse_error(std::string_view detail, std::source_location location)
{
	if(error_handler) error_handler(detail, location);
}

void ubx_frame::clear()
{
	this->valid = false;
	this->class_id = 0;
	this->msg_id = 0;
	this->length = 0;
	this->payload.clear();
	this->cksum = 0;
}

ubx_frame::ubx_frame()
{
	clear();
}

ubx_frame::ubx_frame(std::span<const uint8_t> buf)
{
	clear();
	if (buf.size() < UBX_HEADER_SIZE + UBX_CKSUM_SIZE)
	{
		return;
	}
	this->class_id = buf[UBX_CLASS_OFFSET];
	this->msg_id = buf[UBX_MSG_OFFSET];
	this->length = buf[UBX_LENGTH_OFFSET] | (buf[UBX_LENGTH_OFFSET + 1] << 8);
	this->cksum = (buf[buf.size() - 2]<<8) | buf[buf.size() - 1];
	if(validate(buf))
	{
		this->valid = true;
	}
	this->payload = ubx_buf_t(buf.begin() + UBX_HEADER_SIZE, buf.end() - UBX_CKSUM_SIZE);
}

bool ubx_frame::validate(std::span<const uint8_t> buf)
{
	if(buf.size() < UBX_HEADER_SIZE + UBX_CKSUM_SIZE)
	{
		report_parse_error("frame is shorter than header and checksum");
		return false;
	}
	if(buf.size() != (size_t)this->length + UBX_HEADER_SIZE + UBX_CKSUM_SIZE)
	{
		report_parse_error(std::format("buffer size {} does not match payload length {}",
			buf.size(), this->length));
		return false;
	}
	uint8_t ck_a = 0, ck_b = 0;
	for (size_t i = 0; i < buf.size() - 2; i++)
	{
		ck_a += buf[i];
		ck_b += ck_a;
	}
	uint16_t buf_cksum = (ck_a << 8) | ck_b;
	if(buf_cksum != this->cksum)
	{
		report_parse_error(std::format("checksum {:04x} does not match {:04x}",
			buf_cksum, this->cksum));
		return false;
	}

	return true;
}

void ubx_frame::dump(FILE *fp) const
{
	fprintf(fp, "=========\n");
	fprintf(fp, "class_id: %02x\n", this->class_id);
	fprintf(fp, "msg_id: %02x\n", this->msg_id);
	fprintf(fp, "length: %d\n", this->length);
	fprintf(fp, "cksum: %04x\n", this->cksum);
	fprintf(fp, "buf: ");
	for (unsigned int i = 0; i < this->payload.size(); i++)
	{
		fprintf(fp, "%02x ", this->payload[i]);
	}
	fprintf(fp, "\n");
	fprintf(fp, "valid: %d\n", this->valid);
}

// Returns EOF on error
// TODO: make it more elegant
int ubx_frame::write(FILE *fp) const
{
	if(fputc(UBX_SYNC1, fp) == EOF ||
	   fputc(UBX_SYNC2, fp) == EOF ||
	   fputc(this->class_id, fp) == EOF ||
	   fputc(this->msg_id, fp) == EOF ||
	   fputc(this->length & 0xff, fp) == EOF ||
	   fputc((this->length >> 8) & 0xff, fp) == EOF)
		return EOF;
	for(size_t i = 0; i < this->payload.size(); i++)
	{
		if(fputc(this->payload[i], fp) == EOF)
			return EOF;
	}
	if(fputc(this->cksum >> 8, fp) == EOF ||
	   fputc(this->cksum & 0xff, fp) == EOF)
		return EOF;
	return 0;
}

ubx_any_msg::ubx_any_msg()
{
	clear();
}

ubx_any_msg::ubx_any_msg(const ubx_frame &frame)
{
	parse(frame);
}

void ubx_any_msg::clear()
{
	this->valid = false;
	this->class_id = 0;
	this->msg_id = 0;
	this->payload.clear();
}

bool ubx_any_msg::parse(const ubx_frame &frame)
{
	clear();
	if(frame.valid == false)
	{
		return false;
	}

	this->class_id = frame.class_id;
	this->msg_id = frame.msg_id;
	this->payload = frame.payload;
	this->valid = true;
	return true;
}

void ubx_any_msg::dump(FILE *fp) const
{
	fprintf(fp, "%s (%zd)\t> ",
		("UBX-" + ubx_msg_name(this->class_id, this->msg_id, this->payload)).c_str(),
		this->payload.size());
	for(auto i: this->payload)
	{
		fprintf(fp, "%02x", i);
	}
	fprintf(fp, "\n");
}

} // namespace UBX
