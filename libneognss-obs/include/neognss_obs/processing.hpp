// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppgnss/sbas.hpp>
#include <cppgnss/stream.hpp>
#include <cppgnss/ubx_subframe.hpp>
#include <memory>
#include <nlohmann/json.hpp>

namespace neognss_obs {
using Json = nlohmann::json;
std::vector<uint8_t> archive_index(std::span<const uint8_t>);
Json sbas_message(const cppgnss::SBAS::Result &);
class DatasetScan {
  public:
    DatasetScan(const std::string &protocol, bool qa = true, double gap_timeout = 50);
    ~DatasetScan();
    Json feed(std::span<const uint8_t>);
    Json finish();
    Json summary() const;
  private:
    struct State;
    std::unique_ptr<State> state_;
};
class SubframeProcessor {
  public:
    explicit SubframeProcessor(bool sbas_only = true) : sbas_only_(sbas_only) {}
    Json feed(std::span<const uint8_t>);
    Json finish();
    Json summary() const;

  private:
    bool sbas_only_;
    cppgnss::StreamDecoder reader_{cppgnss::Protocol::ubx};
    std::map<UBX::SignalKey, uint64_t> counts_;
    std::map<std::string, uint64_t> statuses_, types_;
    uint64_t malformed_ = 0;
};
class ClockProcessor {
  public:
    ClockProcessor(double max_gap = 50, double tolerance = 50000, double temperature_max_age = 5,
                   const std::string &protocol = "ubx");
    ~ClockProcessor();
    Json feed(std::span<const uint8_t>, const std::string &source);
    // Only close framing at a file boundary. Buffered epoch/state survive.
    Json end_file();
    Json finish();
    Json summary() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
class GridProcessor {
  public:
    GridProcessor(double correction_age = 600, double mask_age = 1200, double gap_timeout = 0);
    ~GridProcessor();
    // Rows contain gpst_ms, offset, and a decoded SBAS message. One signal
    // per instance; caller explicitly chooses continuous-group boundaries.
    Json process(const Json &rows);
    Json process_frames(const Json &rows);
    Json finish(int64_t gpst_ms);
    Json diagnostics() const;

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
