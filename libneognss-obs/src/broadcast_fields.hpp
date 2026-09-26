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
struct Column {
    ArrowArrayView *v;
    const ArrowSchema *s;
    Column child(const char *name) const {
        for (int64_t i = 0; i < s->n_children; ++i)
            if (s->children[i]->name &&
                std::string_view(s->children[i]->name) == name)
                return {v->children[i], s->children[i]};
        throw std::runtime_error(std::string("Missing RawBits field: ") + name);
    }
    bool null(int64_t i) const { return ArrowArrayViewIsNull(v, i); }
    std::string str(int64_t i) const {
        if (null(i) || v->storage_type != NANOARROW_TYPE_STRING)
            throw std::runtime_error("Expected string");
        auto x = ArrowArrayViewGetStringUnsafe(v, i);
        return {x.data, size_t(x.size_bytes)};
    }
    int64_t integer(int64_t i) const {
        if (null(i) || (v->storage_type != NANOARROW_TYPE_UINT16 &&
                        v->storage_type != NANOARROW_TYPE_UINT32))
            throw std::runtime_error("Invalid RawBits integer");
        return int64_t(ArrowArrayViewGetUIntUnsafe(v, i));
    }
    std::optional<Tick> time(int64_t i) const {
        if (std::string_view(s->format) != "d:38,12" &&
            std::string_view(s->format) != "d:38,12,128")
            throw std::runtime_error("Expected GPST decimal128(38,12)");
        if (null(i))
            return {};
        ArrowDecimal d;
        ArrowDecimalInit(&d, 128, 38, 12);
        ArrowArrayViewGetDecimalUnsafe(v, i, &d);
        if (d.words[d.high_word_index] >> 63)
            throw std::runtime_error("Negative GPST");
        Tick t = (Tick(d.words[d.high_word_index]) << 64) +
                 d.words[d.low_word_index];
        if (t / ps > INT64_MAX)
            throw std::runtime_error("GPST out of range");
        return t;
    }
};
std::shared_ptr<CnexBatch> batch(const std::vector<Row> &rows);
Tick seconds(int64_t s);
Tick duration(double s);
std::pair<std::string, Row> sbas_fields(const SBAS::Message &message);
} // namespace neognss_obs::broadcast_detail
