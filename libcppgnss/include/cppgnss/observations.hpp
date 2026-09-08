// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppgnss/stream.hpp>
#include <optional>
#include <string>

namespace cppgnss {
// Initial supported observation family: GPS L1/L2 RAWX and SBF MeasEpoch.
// No RTKLIB types, filtering policy or terminal I/O in this representation.
struct Observation {
  int prn = 0, antenna = 0;
  std::string signal;
  double frequency_hz = 0, pseudorange_m = 0, phase_cycles = 0, doppler_hz = 0,
         cn0_dbhz = 0;
  bool code_valid = false, phase_valid = false, doppler_valid = false,
       cn0_valid = false;
  bool half_cycle = false, sub_half_cycle = false, lock_valid = false;
  double lock_seconds = 0;
};
struct ObservationEpoch {
  int64_t gpst_ns = 0;
  std::vector<Observation> signals;
  bool clock_reset = false;
  uint64_t unselected_signals = 0;
};
// Other systems/signals are counted, never remapped to a supported identity.
// Meas3 blocks are ignored here; the application diagnoses Meas3-only inputs.
std::optional<ObservationEpoch> decode_gps_observations(const FrameView &frame);
} // namespace cppgnss
