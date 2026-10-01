// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "broadcast_fields.hpp"
#include <span>

namespace neognss_obs::broadcast_detail {
class GalileoDecoder {
    struct State;
    std::unique_ptr<State> state_;

  public:
    GalileoDecoder();
    ~GalileoDecoder();
    void clear();
    std::vector<DecodedMessage>
    decode(std::span<const uint8_t> body, const std::string &family,
           const std::string &source, int64_t satellite, bool e1,
           std::optional<Tick> time, std::map<std::string, uint64_t> &counts);
};
bool galileo_snapshot(Row &row, const std::string &kind, Tick time);
bool galileo_expired(const Row &row, const std::string &kind, Tick time);
} // namespace neognss_obs::broadcast_detail
