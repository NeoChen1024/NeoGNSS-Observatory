// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppgnss/stream.hpp>
#include <memory>
#include <nlohmann/json.hpp>

namespace neognss_obs {
using Json = nlohmann::json;
class DatasetScan {
  public:
    DatasetScan(const std::string &protocol, double gap_timeout = 50);
    ~DatasetScan();
    void feed(std::span<const uint8_t>);
    void finish();
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
