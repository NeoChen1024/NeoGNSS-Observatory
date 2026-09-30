// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "broadcast_fields.hpp"
#include <span>

namespace neognss_obs::broadcast_detail {
struct CnavOutput {
    std::string kind;
    Row fields;
    bool candidate = false;
    std::string discriminator;
};

// QZSS CNAV parameter decoding and source-local, fresh three-part assemblies.
class QzsCnavDecoder {
    struct Assembly {
        std::map<int, Row> parts;
        std::optional<Tick> start;
    };
    std::map<std::string, Assembly> assemblies_;

  public:
    void clear() { assemblies_.clear(); }
    std::vector<CnavOutput> decode(std::span<const uint8_t> body,
                                   const std::string &source, int64_t satellite,
                                   bool l2, bool l5, std::optional<Tick> time,
                                   std::map<std::string, uint64_t> &counts);
};
// Returns false for time-ineligible candidates, without selecting a source.
bool cnav_snapshot(Row &row, const std::string &kind, Tick time);
bool cnav_expired(const Row &row, const std::string &kind, Tick time);
Tick reference(Tick now, int64_t week, int modulo, int64_t tow);
} // namespace neognss_obs::broadcast_detail
