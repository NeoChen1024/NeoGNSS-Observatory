// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Kelei Chen

#include <cppubx2/ubx.hpp>
#include <cppubx2/ubx_reader.hpp>
#include <cppubx2/ubx_nav.hpp>
#include <cppubx2/ubx_dump_gen.hpp>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <endian.h>
#include <sys/stat.h>
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <charconv>
#include <ctime>
#include <format>

#define RETURN_ERR \
	return 1

using namespace UBX;

static ByteReadResult file_read_byte(void *context)
{
	FILE *fp = static_cast<FILE *>(context);
	int c = fgetc(fp);
	if(c != EOF)
		return {.result = ReadResult::ok, .byte = static_cast<uint8_t>(c)};
	return {.result = feof(fp) ? ReadResult::end : ReadResult::error};
}

static ByteReadResult tcp_read_byte(void *context)
{
	int fd = *static_cast<int *>(context);
	unsigned char c;
	while(true)
	{
		ssize_t n = read(fd, &c, 1);
		if(n == 1) return {.result = ReadResult::ok, .byte = c};
		if(n == 0) return {.result = ReadResult::end};
		if(errno == EINTR) continue;
		if(errno == EAGAIN || errno == EWOULDBLOCK)
			return {.result = ReadResult::timeout};
		return {.result = ReadResult::error};
	}
}

static ReadResult read_logged_frame(void *context, read_byte_fn read_byte, ubx_buf_t &buf)
{
	size_t discarded = 0;
	auto result = UBX::read_ubx_frame(context, read_byte, buf, &discarded);
	if(discarded)
		fprintf(stderr, "read_ubx_frame(): WASTED %zd Bytes\n", discarded);
	return result;
}

static void log_parse_error(std::string_view detail, std::source_location location)
{
	auto message = std::format("{}:{} {}: {}\n",
		location.file_name(), location.line(), location.function_name(), detail);
	fputs(message.c_str(), stderr);
}

void print_status_line(const ubx_nav_pvt &pvt)
{
	char buf[128];
	fputc('\r', stderr);
	for(int i = 0; i < 80; i++)
		buf[i] = ' ';
	buf[80] = '\0';
	fputs(buf, stderr);
	auto status = std::format("\riTOW={:06}.{:03} {:>10} {:04}/{:02}/{:02} {:02}:{:02}:{:02}, Sats: {:02}",
		pvt.data.iTOW / 1000, pvt.data.iTOW % 1000,
		ubx_nav_pvt_fix_type(pvt).c_str(),
		pvt.data.year, pvt.data.month, pvt.data.day, pvt.data.hour, pvt.data.min, pvt.data.second,
		pvt.data.numSV);
	fputs(status.c_str(), stderr);
}

static int tcp_connect(const char *host, int port)
{
	struct addrinfo hints, *res, *rp;
	char port_str[16];

	snprintf(port_str, sizeof(port_str), "%d", port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	int err = getaddrinfo(host, port_str, &hints, &res);
	if(err != 0)
	{
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
		return -1;
	}

	int sockfd = -1;
	for(rp = res; rp != NULL; rp = rp->ai_next)
	{
		sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if(sockfd < 0) continue;

		struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
		setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		if(connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
			break;
		close(sockfd);
		sockfd = -1;
	}

	freeaddrinfo(res);
	return sockfd;
}

struct Stats
{
	time_t last_print = 0;
	uint64_t period_bytes = 0;
	uint64_t period_frames = 0;
	uint64_t period_pvt = 0;
	uint64_t period_fix = 0;
};

static void print_stats(Stats &st, time_t now)
{
	double elapsed = difftime(now, st.last_print);
	if(elapsed < 1) return;
	double rate = st.period_bytes / elapsed / 1024.0;
	if(st.period_pvt > 0)
	{
		int pct = (int)(st.period_fix * 100 / st.period_pvt);
		auto message = std::format(
			"\n[neoubxlogger stats] avg rate: {:.1f} KiB/s, frames: {}, FIX {}%\n",
			rate, st.period_frames, pct);
		fputs(message.c_str(), stderr);
	}
	else
	{
		auto message = std::format(
			"\n[neoubxlogger stats] avg rate: {:.1f} KiB/s, frames: {}, FIX ---%\n",
			rate, st.period_frames);
		fputs(message.c_str(), stderr);
	}
	st.last_print = now;
	st.period_bytes = 0;
	st.period_frames = 0;
	st.period_pvt = 0;
	st.period_fix = 0;
}

// Own the output stream so early failures also close it. Success paths must
// explicitly check close(): buffered write errors may only surface at fclose().
class OutputFile
{
public:
	OutputFile() = default;
	OutputFile(const OutputFile &) = delete;
	OutputFile &operator=(const OutputFile &) = delete;
	~OutputFile() { close(); }
	FILE *get() const { return fp; }
	bool close()
	{
		FILE *closing = fp;
		fp = nullptr;
		if(closing && fclose(closing) == EOF)
		{
			perror("UBX output close");
			return false;
		}
		return true;
	}
	bool open(const char *filename)
	{
		if(!close()) return false;
		fp = fopen(filename, "wb");
		if(!fp) perror(filename);
		return fp != nullptr;
	}
private:
	FILE *fp = nullptr;
};

int main(int argc, char *argv[])
{
	set_parse_error_handler(log_parse_error);
	bool debug = false;
	bool no_write = false;
	bool quiet = false;
	FILE *readin = stdin;
	OutputFile output;
	const char *input_path = nullptr;

	bool tcp_mode = false;
	std::string tcp_host;
	int tcp_port = 0;
	int sockfd = -1;

	Stats stats{.last_print = time(NULL)};

	setvbuf(stderr, NULL, _IONBF, 0);

	int opt;

	while((opt = getopt(argc, argv, "f:t:dnq")) != -1)
	{
		switch(opt)
		{
		case 'f':
			input_path = optarg;
			break;
		case 't':
		{
			std::string spec(optarg);
			size_t colon = spec.rfind(':');
			if(colon == std::string::npos || colon == 0 || colon == spec.size() - 1)
			{
				fprintf(stderr, "Invalid TCP spec '%s'. Expected HOST:PORT\n", optarg);
				RETURN_ERR;
			}
			tcp_host = spec.substr(0, colon);
			const char *port_begin = spec.data() + colon + 1;
			const char *port_end = spec.data() + spec.size();
			auto [end, error] = std::from_chars(port_begin, port_end, tcp_port);
			if(error != std::errc{} || end != port_end || tcp_port < 1 || tcp_port > 65535)
			{
				fprintf(stderr, "Invalid TCP port in '%s'. Expected 1..65535\n", optarg);
				RETURN_ERR;
			}
			if(tcp_host.front() == '[')
			{
				if(tcp_host.size() <= 2 || tcp_host.back() != ']')
				{
					fprintf(stderr, "Invalid TCP spec '%s'. Expected HOST:PORT\n", optarg);
					RETURN_ERR;
				}
				tcp_host = tcp_host.substr(1, tcp_host.size() - 2);
			}
			tcp_mode = true;
			break;
		}
		case 'd':
			debug = true;
			break;
		case 'n':
			no_write = true;
			break;
		case 'q':
			quiet = true;
			break;
		default:
			fprintf(stderr, "Usage: %s [-f input_file] [-t HOST:PORT] [-n] [-d] [-q]\n", argv[0]);
			RETURN_ERR;
		}
	}

	if(tcp_mode && input_path != nullptr)
	{
		fprintf(stderr, "-f and -t are mutually exclusive\n");
		RETURN_ERR;
	}
	if(debug && quiet)
	{
		fprintf(stderr, "-d and -q are mutually exclusive\n");
		RETURN_ERR;
	}
	if(optind != argc)
	{
		fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
		RETURN_ERR;
	}
	if(input_path)
	{
		readin = fopen(input_path, "rb");
		if(!readin) { perror(input_path); RETURN_ERR; }
	}
	if(tcp_mode)
	{
		sockfd = tcp_connect(tcp_host.c_str(), tcp_port);
		if(sockfd < 0)
		{
			fprintf(stderr, "Failed to connect to %s:%d\n", tcp_host.c_str(), tcp_port);
			RETURN_ERR;
		}
		fprintf(stderr, "Connected to %s:%d\n", tcp_host.c_str(), tcp_port);
	}

	ubx_nav_pvt current_pvt, last_pvt;
	time_t stats_next = time(NULL) + 60;

	while(1)
	{
		ubx_buf_t buf;

		if(tcp_mode)
		{
			ReadResult ret = read_logged_frame(&sockfd, tcp_read_byte, buf);
			if(ret == ReadResult::timeout)
			{
				fprintf(stderr, "\nTCP %s:%d: read timeout; discarding partial frame and resynchronizing\n",
					tcp_host.c_str(), tcp_port);
				continue;
			}
			if(ret != ReadResult::ok)
			{
				if(ret == ReadResult::error) perror("TCP read");
				fprintf(stderr, "\nTCP %s:%d: connection lost, reconnecting in 2s...\n",
					tcp_host.c_str(), tcp_port);
				close(sockfd);
				sleep(2);
				sockfd = tcp_connect(tcp_host.c_str(), tcp_port);
				if(sockfd < 0)
				{
					fprintf(stderr, "Reconnect to %s:%d failed, retrying...\n",
						tcp_host.c_str(), tcp_port);
				}
				continue;
			}
		}
		else
		{
			ReadResult ret = read_logged_frame(readin, file_read_byte, buf);
			if(ret == ReadResult::end) break;
			if(ret == ReadResult::truncated)
			{
				fputs("Truncated UBX frame at EOF\n", stderr);
				RETURN_ERR;
			}
			if(ret != ReadResult::ok)
			{
				perror("UBX input");
				RETURN_ERR;
			}
		}

		ubx_frame frame(buf);
		if(!frame.valid)
		{
			fprintf(stderr, "Invalid frame!\n");
			frame.dump(stderr);
			continue;
		}

		if(debug && !ubx_nav_dump_custom(frame, stderr))
			ubx_dump_any(frame, stderr);

		ubx_nav_pvt pvt(frame);
		if(ubx_nav_pvt_semantically_valid(pvt))
		{
			stats.period_pvt++;
			if((pvt.data.flags_bit & 1) && pvt.data.fixType >= 2 && pvt.data.fixType <= 4)
				stats.period_fix++;
			current_pvt = pvt;
			if(!quiet) print_status_line(pvt);
			if((output.get() == nullptr ||
				current_pvt.data.year != last_pvt.data.year ||
				current_pvt.data.month != last_pvt.data.month ||
				current_pvt.data.day != last_pvt.data.day) && !no_write)
			{
				if(!output.close()) RETURN_ERR;
				char dirname[64];
				snprintf(dirname, sizeof(dirname), "%04u-%02hhu",
					current_pvt.data.year, current_pvt.data.month);
				if(mkdir(dirname, 0755) != 0 && errno != EEXIST)
				{
					perror(dirname);
					RETURN_ERR;
				}
				char filename[128];
				snprintf(filename, 128, "%s/%04u%02hhu%02hhuT%02hhu%02hhu%02hhu.ubx",
					dirname,
					current_pvt.data.year, current_pvt.data.month, current_pvt.data.day, current_pvt.data.hour, current_pvt.data.min, current_pvt.data.second);
				if(!output.open(filename))
				{
					fprintf(stderr, "Unable to open file %s!\n", filename);
					RETURN_ERR;
				}
				if(!quiet)
					fprintf(stderr, "\nOpened file %s\n", filename);
			}
			last_pvt = current_pvt;
		}

		ubx_nav_eoe eoe(frame);
		if(ubx_nav_eoe_semantically_valid(eoe))
		{
			if(!current_pvt.valid)
			{
				fprintf(stderr, "\nIgnoring NAV-EOE without a valid NAV-PVT\n");
			}
			else
			{
				if(eoe.data.iTOW != current_pvt.data.iTOW)
				{
					fprintf(stderr, "\nEOE iTOW mismatch! %u != %u\n", eoe.data.iTOW, current_pvt.data.iTOW);
				}

				if(!quiet) fputs(" EOE", stderr);
			}
		}

		if(output.get() && frame.write(output.get()) == EOF)
		{
			perror("UBX output");
			RETURN_ERR;
		}

		stats.period_bytes += 8 + frame.length;
		stats.period_frames++;

		time_t now = time(NULL);
		if(now >= stats_next)
		{
			print_stats(stats, now);
			stats_next = now + 60;
		}
	}

	if(!output.close()) RETURN_ERR;
	if(readin != stdin) fclose(readin);
	if(!quiet)
		fputs("\nEOF!?\n", stderr);
	return 0;
}
