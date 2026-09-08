// SPDX-License-Identifier: GPL-3.0-only
#include <chrono>
#include <cmath>
#include <cppgnss/stream.hpp>
#include <neognss_obs/ubx_archive.hpp>

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
} // namespace
void EpochAssembler::finish(
    uint32_t extra, const std::function<void(const ArchiveEpoch &)> &emit) {
  if (!active_)
    return;
  auto mapped = [&](int64_t tow) {
    if (tow < 0 || anchor_gpst_ == unknown_gpst)
      return unknown_gpst;
    int64_t delta = tow - anchor_tow_;
    if (delta < -week_ms / 2)
      delta += week_ms;
    if (delta > week_ms / 2)
      delta -= week_ms;
    if (std::abs(delta) > 60000)
      return unknown_gpst;
    return anchor_gpst_ + delta;
  };
  // NAV/EOE describes the navigation epoch; RAWX is a measurement on
  // the steered receiver clock and need not have an identical TOW.
  if (epoch_.gpst_ms == unknown_gpst && rawx_gpst_ != unknown_gpst) {
    epoch_.gpst_ms = rawx_gpst_;
    if (epoch_.tow_ms >= 0) {
      int64_t delta = epoch_.tow_ms - rawx_gpst_ % week_ms;
      if (delta < -week_ms / 2)
        delta += week_ms;
      if (delta > week_ms / 2)
        delta -= week_ms;
      epoch_.gpst_ms += delta;
    } else
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
    const std::function<void(const ArchiveEpoch &)> &emit,
    const std::function<void()> &on_frame) {
  const auto p = frame.payload;
  const uint8_t cls = frame.id >> 8, id = frame.id & 255;
  const auto pos = frame.offset, length = frame.wire.size();
  const int64_t raw_nav_tow = cls == 1 ? nav_tow(id, p) : -1;
  const int64_t tow =
      raw_nav_tow >= 0 && raw_nav_tow <= week_ms ? raw_nav_tow % week_ms : -1;
  // Empty measurement reports can carry startup week/TOW placeholders.
  // Preserve their bytes, but never use them to split or anchor epochs.
  const bool timed_rawx = cls == 2 && id == 0x15 && p.size() >= 16 &&
                          p[11] > 0 && p[13] == 1 &&
                          p.size() == 16 + size_t(p[11]) * 32;
  int64_t measurement = unknown_gpst;
  if (timed_rawx) {
    const double t = read_le<double>(p, 0);
    if (std::isfinite(t) && t >= 0 && t < 604800)
      measurement =
          int64_t(read_le<uint16_t>(p, 8)) * week_ms + std::llround(t * 1000);
  }
  int64_t gpst = unknown_gpst;
  int32_t full_week = -1;
  if (cls == 1 && id == 0x20 && p.size() == 16 && (p[11] & 3) == 3 &&
      tow >= 0) {
    full_week = read_le<int16_t>(p, 8);
    if (full_week >= 0) {
      gpst = int64_t(full_week) * week_ms + raw_nav_tow;
    }
  }
  int64_t measurement_delta = measurement == unknown_gpst || epoch_.tow_ms < 0
                                  ? 0
                                  : measurement % week_ms - epoch_.tow_ms;
  if (measurement_delta < -week_ms / 2)
    measurement_delta += week_ms;
  if (measurement_delta > week_ms / 2)
    measurement_delta -= week_ms;
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
  if (gpst != unknown_gpst && tow >= 0) {
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
  if (on_frame)
    on_frame();
  if (cls == 1 && id == 0x61 && p.size() == 4)
    finish(archive_eoe, emit);
}
void scan_archive(std::span<const uint8_t> b,
                  const std::function<void(const ArchiveEpoch &)> &emit) {
  EpochAssembler assembler(true);
  size_t pos = 0, noise = 0;
  while (pos < b.size()) {
    if (pos + 8 <= b.size() && b[pos] == 0x24 && b[pos + 1] == 0x40) {
      const size_t size = read_le<uint16_t>(b, pos + 6);
      if (size >= 8 && size % 4 == 0 && size <= b.size() - pos &&
          cppgnss::sbf_crc(b.subspan(pos + 4, size - 4)) ==
              read_le<uint16_t>(b, pos + 2)) {
        assembler.finish(archive_partial, emit);
        if (noise < pos) {
          ArchiveEpoch lost;
          lost.begin = noise;
          lost.end = pos;
          lost.flags = archive_noise;
          emit(lost);
        }
        ArchiveEpoch foreign;
        foreign.begin = pos;
        foreign.end = pos + size;
        foreign.flags = archive_noise | archive_foreign_protocol;
        emit(foreign);
        pos += size;
        noise = pos;
        continue;
      }
    }
    bool valid = false;
    size_t length = 0;
    if (pos + 8 <= b.size() && b[pos] == 0xb5 && b[pos + 1] == 0x62) {
      length = read_le<uint16_t>(b, pos + 4) + 8;
      if (length <= b.size() - pos) {
        uint8_t a = 0, c = 0;
        for (size_t i = pos + 2; i < pos + length - 2; ++i) {
          a += b[i];
          c += a;
        }
        valid = a == b[pos + length - 2] && c == b[pos + length - 1];
      }
    }
    if (!valid) {
      assembler.finish(archive_partial, emit);
      ++pos;
      continue;
    }
    if (noise < pos) {
      ArchiveEpoch lost;
      lost.begin = noise;
      lost.end = pos;
      lost.flags = archive_noise;
      emit(lost);
    }
    assembler.accept({cppgnss::Protocol::ubx, pos,
                      uint16_t((b[pos + 2] << 8) | b[pos + 3]), 0,
                      b.subspan(pos, length), b.subspan(pos + 6, length - 8)},
                     emit);
    pos += length;
    noise = pos;
  }
  assembler.finish(archive_partial, emit);
  if (noise < b.size()) {
    ArchiveEpoch lost;
    lost.begin = noise;
    lost.end = b.size();
    lost.flags = archive_noise;
    emit(lost);
  }
}
} // namespace neognss_obs
