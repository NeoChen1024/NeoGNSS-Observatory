// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "broadcast_bits.hpp"
#include <span>

namespace neognss_obs::broadcast_detail {
// GPS and QZSS CNAV parameter decoding and source-local, fresh three-part
// assemblies.
class CnavDecoder {
    std::map<std::string, PartAssembly> assemblies_;

  public:
    void clear() { assemblies_.clear(); }
    std::vector<DecodedMessage> decode(std::span<const uint8_t> body,
                                       const std::string &source, bool gps,
                                       int64_t satellite, bool l2, bool l5,
                                       std::optional<Tick> time,
                                       std::map<std::string, uint64_t> &counts);
};
// Returns false for time-ineligible candidates, without selecting a source.
bool cnav_snapshot(Row &row, const std::string &kind, Tick time);
bool cnav_expired(const Row &row, const std::string &kind, Tick time);
} // namespace neognss_obs::broadcast_detail
