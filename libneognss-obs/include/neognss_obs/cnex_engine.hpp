// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <memory>
#include <nanoarrow/nanoarrow.h>
#include <nlohmann/json.hpp>
#include <span>
#include <string>

namespace neognss_obs {
// Owned Arrow buffers; consumers may retain them after the engine advances.
struct CnexBatch {
    CnexBatch() = default;
    CnexBatch(const CnexBatch &) = delete;
    CnexBatch &operator=(const CnexBatch &) = delete;
    ArrowSchema schema{};
    ArrowArray array{};
    virtual ~CnexBatch();
};
using CnexBatches = std::array<std::shared_ptr<CnexBatch>, 4>;
class CnexEngine {
  public:
    CnexEngine(const std::string &protocol, const std::string &setup_id,
               unsigned antenna, int64_t period_seconds, int64_t period_ps,
               unsigned workers = 4);
    ~CnexEngine();
    CnexBatches feed(std::span<const uint8_t> bytes);
    CnexBatches finish_telemetry();
    nlohmann::json summary();
    nlohmann::json checkpoint();
    nlohmann::json time_error();
    void restore(const nlohmann::json &state);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class CnexTimeProbe {
  public:
    explicit CnexTimeProbe(const std::string &protocol);
    ~CnexTimeProbe();
    void feed(std::span<const uint8_t> bytes);
    nlohmann::json result();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace neognss_obs
