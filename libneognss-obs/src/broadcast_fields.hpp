// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "arrow_batch.hpp"
#include <boost/int128/int128.hpp>
#include <map>
#include <neognss_obs/cnex_engine.hpp>
#include <neognss_obs/sbas.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace neognss_obs::broadcast_detail {
using Tick = boost::int128::int128;
inline constexpr int64_t ps = 1000000000000;
using Bytes = std::vector<uint8_t>;
using Scalar = std::variant<std::monostate, int64_t, double, std::string, bool,
                            Tick, Bytes, std::vector<int64_t>,
                            std::vector<std::string>, std::vector<double>>;
using Fields = std::map<std::string, Scalar>;
struct Records {
    // Explicit field types even when the list is empty or a field is all-null.
    Fields prototype;
    std::vector<Fields> rows;
    bool operator==(const Records &) const = default;
};
struct Record {
    Fields prototype;
    std::optional<Fields> row;
    bool operator==(const Record &) const = default;
};
using Value =
    std::variant<std::monostate, int64_t, double, std::string, bool, Tick,
                 Bytes, std::vector<int64_t>, std::vector<std::string>,
                 std::vector<double>, Records, Record>;
using Row = std::map<std::string, Value>;
struct DecodedMessage {
    std::string kind;
    Row fields;
    bool candidate = false;
    std::string discriminator;
};
std::shared_ptr<CnexBatch> batch(const std::vector<Row> &rows);
Tick seconds(int64_t s);
std::pair<std::string, Row> sbas_fields(const SBAS::Message &message);
} // namespace neognss_obs::broadcast_detail
