// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppgnss/observations.hpp>
#include <neognss_obs/processing.hpp>

namespace neognss_obs {
struct ObservationBatch {
  std::vector<cppgnss::ObservationEpoch> epochs;
};
class ObservationReader {
public:
  explicit ObservationReader(const std::string &protocol);
  ObservationBatch feed(std::span<const uint8_t>);
  void finish();
  Json summary() const;

private:
  cppgnss::StreamDecoder reader_;
  uint64_t epochs_ = 0, unselected_ = 0, meas3_ = 0;
};
struct PppEpoch {
  int64_t gpst_ns;
  int32_t status, satellites;
  double x, y, z, qxx, qyy, qzz, qxy, qyz, qzx;
  double east, north, up, sigma_e, sigma_n, sigma_u;
  double clock_ns, clock_sigma_ns, ztd_m, ztd_sigma_m;
};
struct PppSatellite {
  int64_t gpst_ns;
  int32_t prn, used, slip;
  double azimuth_deg, elevation_deg, phase_residual_m, code_residual_m;
};
struct PppResult {
  std::vector<PppEpoch> epochs;
  std::vector<PppSatellite> satellites;
};
class PppFloat {
public:
  explicit PppFloat(const Json &settings);
  ~PppFloat();
  void products(const Json &products);
  PppResult process(const ObservationBatch &batch);
  Json summary() const;

private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace neognss_obs
