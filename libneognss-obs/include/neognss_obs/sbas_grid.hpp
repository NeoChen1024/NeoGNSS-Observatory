// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <map>
#include <memory>
#include <neognss_obs/cnex_engine.hpp>
#include <string>

namespace neognss_obs {
class SbasGridProcessor {
  public:
    using Output = std::map<std::string, std::shared_ptr<CnexBatch>>;
    SbasGridProcessor(std::string setup, int64_t interval_s = 3600,
                      int64_t correction_age_s = 600,
                      int64_t mask_age_s = 1200);
    ~SbasGridProcessor();
    Output feed(ArrowSchema *, ArrowArray *);
    Output advance(int64_t gpst_seconds, int64_t fraction_ps = 0);
    void discontinuity();
    std::map<std::string, uint64_t> diagnostics() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace neognss_obs
