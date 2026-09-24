// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Kelei Chen

#include "logger_diagnostics.hpp"
#include "logger_output.hpp"
#include <array>
#include <cerrno>
#include <charconv>
#include <cppgnss/parse.hpp>
#include <cppgnss/sbf.hpp>
#include <cppgnss/sbf_ids_gen.hpp>
#include <cppgnss/ubx.hpp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <getopt.h>
#include <memory>
#include <netdb.h>
#include <optional>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

using namespace UBX;
using cppgnss::FrameView;
using cppgnss::Protocol;

static void fail(const std::string &message) {
    throw std::runtime_error(message);
}
static void io_error(const char *what) {
    fail(std::string(what) + ": " + strerror(errno));
}
static int64_t integer_arg(const char *text, int64_t minimum, int64_t maximum) {
    int64_t value;
    auto end = text + strlen(text);
    auto result = std::from_chars(text, end, value);
    if (result.ec != std::errc{} || result.ptr != end || value < minimum ||
        value > maximum)
        fail("Invalid numeric option: " + std::string(text));
    return value;
}
static int tcp_connect(const std::string &host, int port) {
    addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    auto service = std::to_string(port);
    auto error = getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    if (error) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error));
        return -1;
    }
    int fd = -1;
    for (auto address = result; address; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype,
                    address->ai_protocol);
        if (fd < 0)
            continue;
        timeval timeout{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    return fd;
}
struct Stats {
    std::chrono::steady_clock::time_point last =
        std::chrono::steady_clock::now();
    uint64_t bytes = 0, frames = 0, pvt = 0, fixes = 0;
    void print() {
        auto now = std::chrono::steady_clock::now();
        double seconds = std::chrono::duration<double>(now - last).count();
        auto text =
            std::format("\n[neognsslogger stats] avg rate: {:.1f} KiB/s, "
                        "frames: {}, FIX {}%\n",
                        bytes / std::max(seconds, .001) / 1024, frames,
                        pvt ? std::to_string(fixes * 100 / pvt) : "---");
        fputs(text.c_str(), stderr);
        bytes = frames = pvt = fixes = 0;
        last = now;
    }
};
static std::optional<int64_t> sbf_time(std::span<const uint8_t> p) {
    if (p.size() < 6)
        return {};
    auto tow = read_le<uint32_t>(p, 0);
    auto week = read_le<uint16_t>(p, 4);
    if (tow >= 604800000 || week == 65535)
        return {};
    return int64_t(week) * 604800000 + tow;
}
static void usage(const char *name) {
    fprintf(
        stderr,
        "Usage: %s [-p PROTOCOL] [-f FILE | -t HOST:PORT] [-n] [-d | -q] "
        "[--expected-period-ms 1000] [--epoch-tolerance-percent 20] "
        "[--expected-measurement-period-ms N] [--expect-nav-clock] "
        "[--disk-buffer-mib N] [OUTPUT_DIR]\n\n"
        "  -p, --protocol ubx|sbf|rtcm3|nmea|mixed (default: ubx)\n"
        "                        rtcm3/nmea/mixed require -n (inspection "
        "only)\n"
        "  -f FILE                Read file (default input: stdin)\n"
        "  -t HOST:PORT           Read TCP (default: disabled; timeout 5 s, "
        "reconnect 2 s)\n"
        "  -n                     Disable recording (default: recording "
        "enabled)\n"
        "  -d                     Dump messages (default: off)\n"
        "  -q                     Suppress live status (default: off)\n"
        "  --expected-period-ms N Navigation cadence (default: 1000 ms)\n"
        "  --epoch-interval-ms N  Alias for --expected-period-ms\n"
        "  --epoch-tolerance-percent N  Cadence tolerance (default: +/-20%%)\n"
        "  --expected-measurement-period-ms N  SBF cadence (default: "
        "navigation cadence)\n"
        "  --expect-nav-clock     UBX clock presence check (default: off)\n"
        "  --disk-buffer-mib N    Background disk buffer (default: 64 MiB; "
        "minimum: 1)\n"
        "  OUTPUT_DIR             Recording directory (default: ./)\n"
        "  -h, --help             Show this help\n",
        name);
}

int main(int argc, char **argv) try {
    bool debug = false, no_write = false, quiet = false;
    Protocol protocol = Protocol::ubx;
    bool mixed = false;
    const char *input = nullptr;
    std::string host;
    int port = 0;
    std::filesystem::path root = ".";
    size_t disk_buffer = 64 * 1024 * 1024;
    LoggerDiagnostics navigation;
    std::optional<int64_t> measurement_period;
    const option options[] = {
        {"protocol", required_argument, nullptr, 'p'},
        {"help", no_argument, nullptr, 'h'},
        {"expected-period-ms", required_argument, nullptr, 1000},
        {"epoch-interval-ms", required_argument, nullptr, 1000},
        {"epoch-tolerance-percent", required_argument, nullptr, 1001},
        {"expect-nav-clock", no_argument, nullptr, 1002},
        {"expected-measurement-period-ms", required_argument, nullptr, 1003},
        {"disk-buffer-mib", required_argument, nullptr, 1004},
        {nullptr, 0, nullptr, 0}};
    int option;
    while ((option = getopt_long(argc, argv, "p:f:t:dnqh", options, nullptr)) !=
           -1) {
        switch (option) {
        case 'p':
            mixed = std::string_view(optarg) == "mixed";
            if (std::string_view(optarg) == "ubx")
                protocol = Protocol::ubx;
            else if (std::string_view(optarg) == "sbf")
                protocol = Protocol::sbf;
            else if (std::string_view(optarg) == "rtcm3")
                protocol = Protocol::rtcm3;
            else if (std::string_view(optarg) == "nmea" || mixed)
                protocol = Protocol::nmea;
            else
                fail("Expected protocol ubx, sbf, rtcm3, nmea or mixed");
            break;
        case 'f':
            input = optarg;
            break;
        case 't': {
            std::string text(optarg);
            auto colon = text.rfind(':');
            if (colon == std::string::npos || colon == 0)
                fail("Expected HOST:PORT");
            host = text.substr(0, colon);
            port = int(integer_arg(text.c_str() + colon + 1, 1, 65535));
            if (host.front() == '[') {
                if (host.size() <= 2 || host.back() != ']')
                    fail("Expected [IPv6]:PORT");
                host = host.substr(1, host.size() - 2);
            }
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
        case 'h':
            usage(argv[0]);
            return 0;
        case 1000:
            navigation.interval_ms = integer_arg(optarg, 1, 604799999);
            break;
        case 1001:
            navigation.tolerance_percent = integer_arg(optarg, 0, 99);
            break;
        case 1002:
            navigation.expect_clock = true;
            break;
        case 1003:
            measurement_period = integer_arg(optarg, 1, 604799999);
            break;
        case 1004:
            disk_buffer =
                size_t(integer_arg(optarg, 1, SIZE_MAX / (1024 * 1024))) *
                1024 * 1024;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }
    if (input && !host.empty())
        fail("-f and -t are mutually exclusive");
    if (debug && quiet)
        fail("-d and -q are mutually exclusive");
    if (argc - optind > 1)
        fail("Only one OUTPUT_DIR is allowed");
    if (argc > optind)
        root = argv[optind];
    if (!no_write &&
        (mixed || protocol == Protocol::rtcm3 || protocol == Protocol::nmea))
        fail("RTCM3/NMEA/mixed recording has no rotation policy; use -n to "
             "inspect");
    if (protocol == Protocol::sbf && navigation.expect_clock)
        fail("--expect-nav-clock is UBX-only");
    if (protocol == Protocol::ubx && measurement_period)
        fail("--expected-measurement-period-ms is SBF-only");
    std::optional<LoggerDiagnostics> measurement;
    if (protocol == Protocol::sbf) {
        measurement.emplace();
        measurement->axis = "measurement";
        measurement->interval_ms =
            measurement_period.value_or(navigation.interval_ms);
        measurement->tolerance_percent = navigation.tolerance_percent;
    }
    std::unique_ptr<LoggerOutput> output;
    if (!no_write)
        output = std::make_unique<LoggerOutput>(disk_buffer, quiet);
    std::filesystem::path output_path;
    Stats stats;
    std::vector<uint8_t> pending;
    std::optional<int64_t> timegps;
    int64_t last = -1, output_day = -1;
    uint32_t clock_count = 0, clock_tow = 0;
    std::optional<int64_t> meas_epoch;
    auto record = [&](int64_t time) {
        if (time <= last) {
            if (!no_write)
                fail("Non-increasing GPST epoch: " +
                     LoggerDiagnostics::label(time));
            fputs("\n[logger qc] non_increasing_epoch\n", stderr);
        }
        navigation.epoch(time, clock_count, clock_tow);
        clock_count = 0;
        timegps.reset();
        last = time;
        if (no_write)
            return;
        auto day = time / 86400000;
        if (day != output_day) {
            using namespace std::chrono;
            auto calendar = sys_days{year{1980} / 1 / 6} + milliseconds(time);
            auto directory = root / std::format("{:%Y-%m}", calendar);
            auto filename =
                directory / (LoggerDiagnostics::label(time) +
                             (protocol == Protocol::ubx ? ".ubx" : ".sbf"));
            output_path = filename;
            output_day = day;
        }
        output->submit(output_path, std::move(pending));
        pending = {};
    };
    auto consume = [&](const FrameView &f) {
        ++stats.frames;
        stats.bytes += f.wire.size();
        if (mixed || protocol == Protocol::rtcm3 ||
            protocol == Protocol::nmea) {
            if (debug)
                fputs(cppgnss::dump(f).c_str(), stderr);
            return;
        }
        if (!no_write) {
            if (pending.size() + f.wire.size() > 64 * 1024 * 1024)
                fail("Missing epoch boundary: buffer exceeds 64 MiB");
            pending.insert(pending.end(), f.wire.begin(), f.wire.end());
        }
        if (protocol == Protocol::sbf) {
            const auto id = static_cast<cppgnss::SbfMessageId>(f.id());
            using enum cppgnss::SbfMessageId;
            if (debug)
                fputs(cppgnss::dump(f).c_str(), stderr);
            auto time = sbf_time(f.payload);
            if ((id == PVT_CARTESIAN || id == PVT_GEODETIC) &&
                f.payload.size() >= 77) {
                const auto mode = f.payload[6] & 15;
                const auto error = f.payload[7];
                ++stats.pvt;
                if (!error && ((mode >= 1 && mode <= 8) || mode == 10))
                    ++stats.fixes;
                if (!quiet && !debug)
                    fprintf(stderr, "\rSBF PVT mode=%u error=%u sats=%u", mode,
                            error, f.payload[66]);
            }
            if (id == MEAS_EPOCH && time) {
                if (meas_epoch && meas_epoch != time)
                    fputs("\n[logger qc] missing_EndOfMeas\n", stderr);
                meas_epoch = time;
            }
            if (id == END_OF_MEAS) {
                if (time) {
                    if (meas_epoch && meas_epoch != time)
                        fputs("\n[logger qc] mismatched_EndOfMeas\n", stderr);
                    measurement->epoch(*time, 0, 0);
                } else
                    fputs("\n[logger qc] invalid_EndOfMeas_time\n", stderr);
                meas_epoch.reset();
            }
            if (id == END_OF_PVT) {
                if (!time) {
                    if (!no_write)
                        fail("EndOfPVT requires valid GPST WNc/TOW");
                    fputs("\n[logger qc] invalid_EndOfPVT_time\n", stderr);
                } else
                    record(*time);
            }
            return;
        }
        const auto id = static_cast<cppgnss::UbxMessageId>(f.id());
        using enum cppgnss::UbxMessageId;
        if (debug)
            fputs(cppgnss::dump(f).c_str(), stderr);
        if (id == NAV_CLOCK && f.payload.size() == 20) {
            ++clock_count;
            clock_tow = read_le<uint32_t>(f.payload, 0);
        }
        if (id == NAV_PVT) {
            auto parsed = cppgnss::parse<ubx_nav_pvt>(f);
            if (parsed && ubx_nav_pvt_semantically_valid(parsed.value())) {
                const auto &pvt = parsed.value();
                ++stats.pvt;
                if (ubx_nav_pvt_fix_ok(pvt))
                    ++stats.fixes;
                if (!quiet && !debug)
                    fprintf(stderr, "\riTOW=%u GPST %s sats=%u", pvt.data.iTOW,
                            ubx_nav_pvt_fix_type(pvt).c_str(), pvt.data.numSV);
            }
        }
        if (id == NAV_TIMEGPS) {
            auto p = f.payload;
            if (p.size() != 16 || (p[11] & 3) != 3 ||
                read_le<int16_t>(p, 8) < 0 ||
                read_le<uint32_t>(p, 0) >= 604800000) {
                if (!no_write)
                    fail("Invalid NAV-TIMEGPS");
                timegps.reset();
                return;
            }
            auto time = int64_t(read_le<int16_t>(p, 8)) * 604800000 +
                        read_le<uint32_t>(p, 0);
            if (timegps && *timegps != time && !no_write)
                fail("Conflicting NAV-TIMEGPS before EOE");
            timegps = time;
        }
        if (id == NAV_EOE) {
            if (f.payload.size() != 4 || !timegps ||
                *timegps % 604800000 != read_le<uint32_t>(f.payload, 0)) {
                if (!no_write)
                    fail("EOE requires matching valid NAV-TIMEGPS");
                fputs("\n[logger qc] unmatched_EOE\n", stderr);
                timegps.reset();
                return;
            }
            record(*timegps);
        }
    };
    auto make_decoder = [&] {
        return mixed ? cppgnss::StreamDecoder({Protocol::ubx, Protocol::sbf,
                                               Protocol::rtcm3, Protocol::nmea})
                     : cppgnss::StreamDecoder(protocol);
    };
    auto decoder = make_decoder();
    auto discard = [&] {
        fprintf(stderr,
                "\n[logger qc] transport_break discarded_frame_bytes=%zu "
                "discarded_epoch_bytes=%zu\n",
                decoder.pending_bytes(), pending.size());
        decoder = make_decoder();
        pending.clear();
        timegps.reset();
        meas_epoch.reset();
        clock_count = 0;
    };
    std::array<uint8_t, 65536> buffer;
    FILE *file = stdin;
    int socket = -1;
    if (input) {
        file = fopen(input, "rb");
        if (!file)
            io_error(input);
    }
    if (!host.empty()) {
        socket = tcp_connect(host, port);
        if (socket < 0)
            fail("Initial TCP connection failed");
    }
    for (;;) {
        if (output)
            output->check();
        ssize_t count;
        if (!host.empty()) {
            if (socket < 0) {
                ++navigation.reconnects;
                navigation.transport("TCP_reconnect_attempt");
                sleep(2);
                socket = tcp_connect(host, port);
                continue;
            }
            count = read(socket, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ++navigation.timeouts;
                navigation.transport("TCP_timeout");
                discard();
                continue;
            }
            if (count <= 0) {
                navigation.transport("TCP_disconnect");
                discard();
                close(socket);
                socket = -1;
                continue;
            }
        } else {
            count = read(fileno(file), buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
                io_error("Input read");
            if (!count)
                break;
        }
        auto invalid = decoder.invalid, noise = decoder.noise,
             foreign = decoder.skipped_protocol_frames;
        decoder.feed(std::span(buffer).first(count), consume);
        if (decoder.invalid != invalid) {
            navigation.invalid_checksums += decoder.invalid - invalid;
            navigation.transport("invalid_frame_or_checksum");
        }
        if (decoder.noise != noise)
            fprintf(stderr, "\n[logger qc] discarded_bytes=%llu\n",
                    static_cast<unsigned long long>(decoder.noise - noise));
        if (decoder.skipped_protocol_frames != foreign)
            fprintf(stderr, "\n[logger qc] skipped_foreign_frames=%llu\n",
                    static_cast<unsigned long long>(
                        decoder.skipped_protocol_frames - foreign));
        if (std::chrono::steady_clock::now() - stats.last >=
            std::chrono::seconds(60)) {
            stats.print();
            navigation.summary();
            if (measurement)
                measurement->summary();
        }
    }
    decoder.finish();
    if (!no_write && !pending.empty())
        fail("Incomplete recording interval at EOF: missing epoch boundary; "
             "pending bytes not written");
    if (output)
        output->finish();
    if (file != stdin)
        fclose(file);
    stats.print();
    return 0;
} catch (const std::exception &error) {
    fprintf(stderr, "Logger error: %s\n", error.what());
    return 1;
}
