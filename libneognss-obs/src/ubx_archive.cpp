// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cppgnss/stream.hpp>
#include <cppgnss/ubx_nav.hpp>
#include <neognss_obs/ubx_archive.hpp>
#include <openssl/evp.h>
#include <string_view>

namespace neognss_obs {
using namespace UBX;
namespace {
constexpr int64_t week_ms = 604800000;
int64_t nav_tow(uint8_t id, std::span<const uint8_t> p) {
    // Explicit timestamp offsets: not all NAV messages begin with iTOW.
    switch (id) {
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x04:
    case 0x07:
    case 0x11:
    case 0x12:
    case 0x20:
    case 0x21:
    case 0x22:
    case 0x32:
    case 0x35:
    case 0x39:
    case 0x43:
    case 0x61:
        if (p.size() >= 4)
            return read_le<uint32_t>(p, 0);
        break;
    case 0x13:
    case 0x14:
        if (p.size() >= 8)
            return read_le<uint32_t>(p, 4);
        break;
    }
    return -1;
}
// A TOW difference folded into [-half week, +half week].
int64_t wrap_week_ms(int64_t delta) {
    if (delta < -week_ms / 2)
        delta += week_ms;
    if (delta > week_ms / 2)
        delta -= week_ms;
    return delta;
}
} // namespace
void EpochAssembler::finish(
    uint32_t extra, const std::function<void(const ArchiveEpoch &)> &emit) {
    if (!active_)
        return;
    auto mapped = [&](int64_t tow) {
        if (tow < 0 || anchor_gpst_ == unknown_gpst)
            return unknown_gpst;
        const int64_t delta = wrap_week_ms(tow - anchor_tow_);
        if (std::abs(delta) > 60000)
            return unknown_gpst;
        return anchor_gpst_ + delta;
    };
    // NAV/EOE describes the navigation epoch; RAWX is a measurement on
    // the steered receiver clock and need not have an identical TOW.
    if (epoch_.gpst_ms == unknown_gpst && rawx_gpst_ != unknown_gpst) {
        epoch_.gpst_ms = rawx_gpst_;
        if (epoch_.tow_ms >= 0)
            epoch_.gpst_ms +=
                wrap_week_ms(epoch_.tow_ms - rawx_gpst_ % week_ms);
        else
            epoch_.tow_ms = rawx_gpst_ % week_ms;
    }
    if (epoch_.gpst_ms == unknown_gpst && epoch_.tow_ms >= 0) {
        epoch_.gpst_ms = mapped(epoch_.tow_ms);
        if (epoch_.gpst_ms != unknown_gpst)
            epoch_.flags |= archive_inferred_time;
    }
    if (epoch_.gpst_ms != unknown_gpst) {
        if (rawx_gpst_ != unknown_gpst &&
            std::abs(epoch_.gpst_ms - rawx_gpst_) > 500)
            epoch_.flags |= archive_time_conflict;
        epoch_.gps_week = epoch_.gpst_ms / week_ms;
        anchor_tow_ = epoch_.tow_ms;
        anchor_gpst_ = epoch_.gpst_ms;
    }
    epoch_.flags |= extra;
    if (!epoch_.nav_frames)
        epoch_.flags |= archive_no_nav;

    emit(epoch_);
    epoch_ = {};
    rawx_gpst_ = unknown_gpst;
    active_ = false;
}
void EpochAssembler::accept(
    const cppgnss::FrameView &frame,
    const std::function<void(const ArchiveEpoch &)> &emit) {
    const auto p = frame.payload;
    const uint8_t cls = frame.id() >> 8, id = frame.id() & 255;
    const auto pos = frame.offset;
    const auto length = frame.wire.size();
    const int64_t raw_nav_tow = cls == 1 ? nav_tow(id, p) : -1;
    const int64_t tow =
        raw_nav_tow >= 0 && raw_nav_tow < week_ms ? raw_nav_tow : -1;
    // Empty measurement reports can carry startup week/TOW placeholders.
    // Preserve their bytes, but never use them to split or anchor epochs.
    const bool timed_rawx = cls == 2 && id == 0x15 && p.size() >= 16 &&
                            p[11] > 0 && p[13] == 1 &&
                            p.size() == 16 + size_t(p[11]) * 32;
    int64_t measurement = unknown_gpst;
    if (timed_rawx) {
        const double t = read_le<double>(p, 0);
        if (std::isfinite(t) && t >= 0 && t < 604800)
            measurement = int64_t(read_le<uint16_t>(p, 8)) * week_ms +
                          std::llround(t * 1000);
    }
    const auto timegps = ubx_nav_timegps_time(frame);
    const int64_t gpst = timegps ? timegps->gpst_ms : unknown_gpst;
    const int64_t measurement_delta =
        measurement == unknown_gpst || epoch_.tow_ms < 0
            ? 0
            : wrap_week_ms(measurement % week_ms - epoch_.tow_ms);
    if (active_ &&
        ((epoch_.tow_ms >= 0 && tow >= 0 && epoch_.tow_ms != tow) ||
         (measurement != unknown_gpst &&
          (rawx_gpst_ != unknown_gpst || std::abs(measurement_delta) > 500))))
        finish(archive_partial, emit);
    if (!active_) {
        epoch_.begin = pos;
        active_ = true;
    }
    epoch_.end = pos + length;
    if (measurement != unknown_gpst)
        rawx_gpst_ = measurement;
    if (cls == 2 && id == 0x15)
        epoch_.flags |= archive_rawx;
    if (cls == 1 && id == 7 && p.size() == 92)
        epoch_.flags |= archive_pvt;
    ++epoch_.frames;
    if (cls == 1)
        ++epoch_.nav_frames;
    if (tow >= 0)
        epoch_.tow_ms = tow;
    if (gpst != unknown_gpst) {
        if (epoch_.gpst_ms != unknown_gpst && epoch_.gpst_ms != gpst)
            epoch_.flags |= archive_time_conflict;
        epoch_.gpst_ms = gpst;
    }

    if (fingerprint_) {
        if (epoch_.frames == 1)
            epoch_.fingerprint = 14695981039346656037ull;
        for (auto b : frame.wire) {
            epoch_.fingerprint ^= b;
            epoch_.fingerprint *= 1099511628211ull;
        }
    }
    if (cls == 1 && id == 0x61 && p.size() == 4)
        finish(archive_eoe, emit);
}
void scan_archive(std::span<const uint8_t> b,
                  const std::function<void(const ArchiveEpoch &)> &emit) {
    EpochAssembler assembler(true);
    size_t pos = 0, noise = 0;
    const auto lost = [&](size_t end) {
        if (noise < end) {
            ArchiveEpoch e;
            e.begin = noise;
            e.end = end;
            e.flags = archive_noise;
            emit(e);
        }
    };
    while (pos < b.size()) {
        // The whole archive is present, so an incomplete candidate is noise.
        // NMEA and RTCM3 frames are noise too; SBF frames are recorded as
        // separate foreign-protocol spans.
        const auto m = cppgnss::match_frame(b.subspan(pos), pos);
        const bool ubx = m.status == cppgnss::FrameMatch::frame &&
                         m.view.protocol() == cppgnss::Protocol::ubx;
        const bool sbf = m.status == cppgnss::FrameMatch::frame &&
                         m.view.protocol() == cppgnss::Protocol::sbf;
        if (!ubx)
            assembler.finish(archive_partial, emit);
        if (!ubx && !sbf) {
            pos += std::max<size_t>(m.length, 1);
            continue;
        }
        lost(pos);
        if (sbf) {
            ArchiveEpoch foreign;
            foreign.begin = pos;
            foreign.end = pos + m.length;
            foreign.flags = archive_noise | archive_foreign_protocol;
            emit(foreign);
        } else
            assembler.accept(m.view, emit);
        pos += m.length;
        noise = pos;
    }
    assembler.finish(archive_partial, emit);
    lost(b.size());
}
std::vector<uint8_t> archive_index(std::span<const uint8_t> bytes) {
    std::vector<uint8_t> out{'U', 'B', 'X', 'I', 'D', 'X', '0', '4'};
    unsigned char digest[32];
    unsigned length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(),
                   nullptr) != 1 ||
        length != 32)
        throw std::runtime_error("Archive SHA256 failed");
    out.insert(out.end(), digest, digest + 32);
    const auto append = [&]<class T>(T value) {
        value = little_to_native(value);
        const auto *p = reinterpret_cast<const uint8_t *>(&value);
        out.insert(out.end(), p, p + sizeof value);
    };
    append(uint64_t(bytes.size()));
    scan_archive(bytes, [&](const ArchiveEpoch &e) {
        append(e.begin);
        append(e.end);
        append(e.gpst_ms);
        append(e.tow_ms);
        append(e.fingerprint);
        append(e.flags);
        append(e.frames);
        append(e.nav_frames);
        append(e.gps_week);
    });
    return out;
}
void read_archive_index(std::span<const uint8_t> index,
                        const std::function<void(ArchiveEpoch)> &visit) {
    if (index.size() < archive_header_size ||
        std::string_view(reinterpret_cast<const char *>(index.data()), 8) !=
            "UBXIDX04" ||
        (index.size() - archive_header_size) % archive_record_size)
        throw std::runtime_error("Expected UBXIDX04 epoch index");
    for (size_t pos = archive_header_size; pos < index.size();
         pos += archive_record_size)
        visit({read_le<uint64_t>(index, pos), read_le<uint64_t>(index, pos + 8),
               read_le<int64_t>(index, pos + 16),
               read_le<int64_t>(index, pos + 24),
               read_le<uint64_t>(index, pos + 32),
               read_le<uint32_t>(index, pos + 40),
               read_le<uint32_t>(index, pos + 44),
               read_le<uint32_t>(index, pos + 48),
               read_le<int32_t>(index, pos + 52)});
}
} // namespace neognss_obs
