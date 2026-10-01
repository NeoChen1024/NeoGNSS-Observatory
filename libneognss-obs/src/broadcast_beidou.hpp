// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "broadcast_fields.hpp"
#include <span>

namespace neognss_obs::broadcast_detail {
std::vector<DecodedMessage>
decode_pppb2b(std::span<const uint8_t> body, int64_t satellite,
              std::optional<Tick> time,
              std::map<std::string, uint64_t> &counts);
class BeidouDecoder {
    struct State;
    std::unique_ptr<State> state_;

  public:
    BeidouDecoder();
    ~BeidouDecoder();
    void clear();
    std::vector<DecodedMessage>
    decode(std::span<const uint8_t> body, const std::string &family,
           const std::string &source, int64_t satellite,
           std::optional<Tick> time, std::map<std::string, uint64_t> &counts);
};
bool beidou_snapshot(Row &row, const std::string &kind, Tick time);
bool beidou_expired(const Row &row, const std::string &kind, Tick time);
} // namespace neognss_obs::broadcast_detail
