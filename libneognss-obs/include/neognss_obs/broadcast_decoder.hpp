// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <map>
#include <memory>
#include <neognss_obs/cnex_engine.hpp>
#include <string>

namespace neognss_obs {
// Canonical RawBits consumer. No receiver protocol or orbit backend interface.
class BroadcastMessageDecoder {
  public:
    using Output = std::map<std::string, std::shared_ptr<CnexBatch>>;
    BroadcastMessageDecoder(std::string setup, int64_t snapshot_seconds);
    ~BroadcastMessageDecoder();
    Output feed(ArrowSchema *, ArrowArray *);
    void discontinuity();
    std::map<std::string, uint64_t> diagnostics() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace neognss_obs
