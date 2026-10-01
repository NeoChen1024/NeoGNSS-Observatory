// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "broadcast_fields.hpp"
#include <span>
namespace neognss_obs::broadcast_detail {
class HasDecoder {
    struct State;
    std::unique_ptr<State> state_;

  public:
    HasDecoder();
    ~HasDecoder();
    void clear();
    std::vector<DecodedMessage> decode(std::span<const uint8_t>,
                                       const std::string &source,
                                       std::optional<Tick>,
                                       std::map<std::string, uint64_t> &);
};
} // namespace neognss_obs::broadcast_detail
