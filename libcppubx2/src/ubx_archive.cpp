// SPDX-License-Identifier: GPL-3.0-only
#include <cppubx2/ubx_archive.hpp>
#include <chrono>
#include <cmath>

namespace UBX {
namespace {
constexpr int64_t week_ms = 604800000;
int64_t nav_tow(uint8_t id, std::span<const uint8_t> p) {
    // Explicit timestamp offsets: not all NAV messages begin with iTOW.
    switch(id) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x07:
    case 0x11: case 0x12: case 0x20: case 0x21: case 0x22:
    case 0x32: case 0x35: case 0x39: case 0x43: case 0x61:
        if(p.size() >= 4) return read_le<uint32_t>(p, 0);
        break;
    case 0x13: case 0x14:
        if(p.size() >= 8) return read_le<uint32_t>(p, 4);
        break;
    }
    return -1;
}
int64_t pvt_utc(std::span<const uint8_t> p) {
    using namespace std::chrono;
    if(p.size() != 92 || (p[11] & 3) != 3 || p[8] > 23 || p[9] > 59 || p[10] > 59)
        return unknown_utc;
    const year_month_day date{year{read_le<uint16_t>(p, 4)}, month{p[6]}, day{p[7]}};
    if(!date.ok()) return unknown_utc;
    // Keep the receiver's integer-second label. nano is a signed subsecond
    // correction and is not used to move nominal 1 Hz epochs across seconds.
    return duration_cast<seconds>(sys_days{date}.time_since_epoch()).count()
        + p[8] * 3600 + p[9] * 60 + p[10];
}
}
void scan_archive(std::span<const uint8_t> b, const std::function<void(const ArchiveEpoch &)> &emit) {
    ArchiveEpoch epoch;
    bool active = false;
    int64_t anchor_tow = -1, anchor_utc = unknown_utc;
    auto finish = [&](uint32_t extra) {
        if(!active) return;
        epoch.flags |= extra;
        if(!epoch.nav_frames) epoch.flags |= archive_no_nav;
        uint64_t h = 14695981039346656037ull;
        for(auto x : b.subspan(epoch.begin, epoch.end - epoch.begin)) { h ^= x; h *= 1099511628211ull; }
        epoch.fingerprint = h;
        emit(epoch);
        epoch = {};
        active = false;
    };
    auto mapped = [&](int64_t tow) {
        if(tow < 0 || anchor_utc == unknown_utc) return unknown_utc;
        int64_t delta = tow - anchor_tow;
        if(delta < -week_ms / 2) delta += week_ms;
        if(delta > week_ms / 2) delta -= week_ms;
        // Do not extrapolate across long unanchored outages.
        if(std::abs(delta) > 60000 || delta % 1000) return unknown_utc;
        return anchor_utc + delta / 1000;
    };
    size_t pos = 0, noise = 0;
    while(pos < b.size()) {
        bool valid = false;
        size_t length = 0;
        if(pos + 8 <= b.size() && b[pos] == 0xb5 && b[pos+1] == 0x62) {
            length = read_le<uint16_t>(b, pos+4) + 8;
            if(length <= b.size() - pos) {
                uint8_t a = 0, c = 0;
                for(size_t i = pos+2; i < pos+length-2; ++i) { a += b[i]; c += a; }
                valid = a == b[pos+length-2] && c == b[pos+length-1];
            }
        }
        if(!valid) {
            finish(archive_partial);
            ++pos;
            continue;
        }
        if(noise < pos) {
            ArchiveEpoch lost;
            lost.begin = noise; lost.end = pos; lost.flags = archive_noise;
            emit(lost);
        }
        const auto p = b.subspan(pos+6, length-8);
        const uint8_t cls = b[pos+2], id = b[pos+3];
        int64_t tow = cls == 1 ? nav_tow(id, p) : -1;
        if(cls == 2 && id == 0x15 && p.size() >= 16) {
            double t = read_le<double>(p, 0);
            if(std::isfinite(t) && t >= 0 && t < 604800)
                // RAWX receiver clock steering can put the measurement just
                // below/above the nominal NAV epoch. Group by nearest second,
                // retaining the original floating timestamp in the source bytes.
                tow = (std::llround(t) * 1000) % week_ms;
        }
        if(tow >= week_ms) tow = -1;
        const int64_t utc = cls == 1 && id == 7 ? pvt_utc(p) : unknown_utc;
        if(active && epoch.tow_ms >= 0 && tow >= 0 && epoch.tow_ms / 1000 != tow / 1000)
            finish(archive_partial);
        if(!active) { epoch.begin = pos; active = true; }
        epoch.end = pos + length;
        if(cls == 2 && id == 0x15 && p.size() >= 16)
            epoch.gps_week = read_le<uint16_t>(p, 8);
        ++epoch.frames;
        if(cls == 1) ++epoch.nav_frames;
        if(tow >= 0) epoch.tow_ms = tow;
        if(utc != unknown_utc && tow >= 0) {
            if(epoch.utc != unknown_utc && epoch.utc != utc) epoch.flags |= archive_time_conflict;
            epoch.utc = utc; epoch.flags |= archive_pvt;
            anchor_tow = tow; anchor_utc = utc;
        } else if(epoch.utc == unknown_utc && tow >= 0) {
            epoch.utc = mapped(tow);
            if(epoch.utc != unknown_utc) epoch.flags |= archive_inferred_time;
        }
        pos += length;
        noise = pos;
        if(cls == 1 && id == 0x61 && p.size() == 4) finish(archive_eoe);
    }
    finish(archive_partial);
    if(noise < b.size()) {
        ArchiveEpoch lost;
        lost.begin = noise; lost.end = b.size(); lost.flags = archive_noise;
        emit(lost);
    }
}
}
