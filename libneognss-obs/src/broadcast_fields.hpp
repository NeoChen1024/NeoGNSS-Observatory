// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "arrow_batch.hpp"
#include <boost/int128/int128.hpp>
#include <map>
#include <neognss_obs/cnex_engine.hpp>
#include <neognss_obs/sbas.hpp>
#include <optional>
#include <stdexcept>
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
// A null whose Arrow type the producer declares. An untyped null
// (std::monostate) is a null GPST time or duration: decimal128(38,12) seconds.
template <class T> struct Null {
    bool operator==(const Null &) const = default;
};
using Value = std::variant<std::monostate, int64_t, double, std::string, bool,
                           Tick, Bytes, std::vector<int64_t>,
                           std::vector<std::string>, std::vector<double>,
                           Records, Record, Null<double>, Null<bool>>;
using Row = std::map<std::string, Value>;
inline Value time_value(std::optional<Tick> t) {
    return t ? Value(*t) : Value{};
}
// The entry for KEY in a bounded map. A new key is admitted only while USED,
// the entries counted against CAPACITY, is below it.
template <class Map>
typename Map::mapped_type &bounded(Map &m, const typename Map::key_type &key,
                                   size_t capacity, const char *error,
                                   size_t used) {
    if (!m.contains(key) && used >= capacity)
        throw std::runtime_error(error);
    return m[key];
}
template <class Map>
typename Map::mapped_type &bounded(Map &m, const typename Map::key_type &key,
                                   size_t capacity, const char *error) {
    return bounded(m, key, capacity, error, m.size());
}
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
