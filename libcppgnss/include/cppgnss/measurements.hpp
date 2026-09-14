// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cmath>
#include <cppgnss/stream.hpp>
#include <optional>
#include <string>

namespace cppgnss {
// Protocol quantities retain their native time representation at this boundary.
struct Measurement {
  std::string system, signal;
  uint16_t satellite = 0;
  uint8_t antenna = 0;
  double code = NAN, phase = NAN, doppler = NAN, cn0 = NAN;
  float code_sigma = NAN, phase_sigma = NAN, doppler_sigma = NAN;
  int code_status = 2, phase_status = 2; // valid, invalid, unknown
  bool half_ambiguity = false;
  std::optional<bool> half_subtracted;
  std::optional<uint32_t> lock_ms;
  bool lock_lower_bound = false;
};
struct Measurements {
  uint16_t week = 0;
  double tow_seconds = 0;
  std::optional<uint32_t> tow_ms;
  std::vector<Measurement> rows;
  uint64_t unsupported = 0;
  uint64_t excluded = 0;
};
// RAWX v1 and MeasEpoch revisions 0/1. Completion policy belongs to the caller.
std::optional<Measurements> decode_measurements(const FrameView &);
} // namespace cppgnss
