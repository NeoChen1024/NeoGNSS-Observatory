// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <boost/int128/int128.hpp>
#include <cstdint>
#include <limits>
#include <nanoarrow/nanoarrow.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace neognss_obs {
struct ArrowBatchView {
    ArrowArrayView value{};
    ArrowBatchView(ArrowSchema *schema, ArrowArray *array) {
        ArrowError error{};
        if (ArrowArrayViewInitFromSchema(&value, schema, &error) ||
            ArrowArrayViewSetArray(&value, array, &error) ||
            ArrowArrayViewValidate(&value, NANOARROW_VALIDATION_LEVEL_FULL,
                                   &error)) {
            ArrowArrayViewReset(&value);
            throw std::runtime_error(
                std::string("Invalid CommonNEX Arrow batch: ") + error.message);
        }
    }
    ArrowBatchView(const ArrowBatchView &) = delete;
    ArrowBatchView &operator=(const ArrowBatchView &) = delete;
    ~ArrowBatchView() { ArrowArrayViewReset(&value); }
};

// A named view into a validated CommonNEX batch or one of its nested children.
// Row indices are passed through to nanoarrow, which applies this view's
// offset.
struct Column {
    using Tick = boost::int128::int128; // picoseconds
    ArrowArrayView *v;
    const ArrowSchema *s;
    Column child(const char *name) const {
        for (int64_t i = 0; i < s->n_children; ++i)
            if (s->children[i]->name &&
                std::string_view(s->children[i]->name) == name)
                return {v->children[i], s->children[i]};
        throw std::runtime_error(std::string("Missing CommonNEX field: ") +
                                 name);
    }
    bool null(int64_t i) const { return ArrowArrayViewIsNull(v, i); }
    std::string_view text(int64_t i) const {
        if (null(i) || v->storage_type != NANOARROW_TYPE_STRING)
            throw std::runtime_error("Expected non-null CommonNEX string");
        auto x = ArrowArrayViewGetStringUnsafe(v, i);
        return {x.data, size_t(x.size_bytes)};
    }
    std::string str(int64_t i) const { return std::string(text(i)); }
    int64_t integer(int64_t i) const {
        if (null(i) || (v->storage_type != NANOARROW_TYPE_UINT16 &&
                        v->storage_type != NANOARROW_TYPE_UINT32))
            throw std::runtime_error("Invalid CommonNEX unsigned integer");
        return int64_t(ArrowArrayViewGetUIntUnsafe(v, i));
    }
    // CommonNEX GPST and durations: decimal128(38,12) seconds.
    bool decimal_seconds() const {
        return std::string_view(s->format) == "d:38,12" ||
               std::string_view(s->format) == "d:38,12,128";
    }
    // Nonnegative decimal seconds as exact picoseconds; null is nullopt.
    std::optional<Tick> seconds_ps(int64_t i) const {
        if (!decimal_seconds())
            throw std::runtime_error("Expected decimal128(38,12) seconds");
        if (null(i))
            return {};
        ArrowDecimal d;
        ArrowDecimalInit(&d, 128, 38, 12);
        ArrowArrayViewGetDecimalUnsafe(v, i, &d);
        if (d.words[d.high_word_index] >> 63)
            throw std::runtime_error("Negative CommonNEX seconds");
        Tick t = (Tick(d.words[d.high_word_index]) << 64) +
                 d.words[d.low_word_index];
        if (t / 1000000000000 > std::numeric_limits<int64_t>::max())
            throw std::runtime_error("CommonNEX seconds out of range");
        return t;
    }
};

// Picoseconds to int64 nanoseconds, rounded half to even.
inline int64_t nearest_ns(Column::Tick ps) {
    Column::Tick ns = ps / 1000, remainder = ps % 1000;
    if (remainder > 500 || (remainder == 500 && (ns & 1) != 0))
        ++ns;
    if (ns > std::numeric_limits<int64_t>::max() ||
        ns < std::numeric_limits<int64_t>::min())
        throw std::runtime_error("Time outside int64 nanoseconds");
    return int64_t(ns);
}
} // namespace neognss_obs
