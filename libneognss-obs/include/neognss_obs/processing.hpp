// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppgnss/stream.hpp>
#include <memory>
#include <neognss_obs/sbas.hpp>
#include <nlohmann/json.hpp>

namespace neognss_obs {
using Json = nlohmann::json;
std::vector<uint8_t> archive_index(std::span<const uint8_t>);
Json sbas_message(const neognss_obs::SBAS::Result &);
class DatasetScan {
  public:
    DatasetScan(const std::string &protocol, double gap_timeout = 50);
    ~DatasetScan();
    Json feed(std::span<const uint8_t>);
    Json finish();
    Json summary() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
class SegmentPlanner {
  public:
    SegmentPlanner(const Json &joins, int64_t gap_timeout_ms);
    ~SegmentPlanner();
    void feed(const Json &source, std::span<const uint8_t> index);
    Json finish();

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace neognss_obs
