// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <bit>
#include <cppgnss/parse.hpp>
#include <cppgnss/sbf.hpp>
#include <cppgnss/sbf_ids_gen.hpp>
#include <format>
#include <stdexcept>
#include <type_traits>

namespace cppgnss::SBF {
namespace detail {
struct PayloadError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Cursor {
    std::span<const uint8_t> bytes;
    size_t offset = 0;
    std::span<const uint8_t> take(size_t n) {
        if (n > bytes.size() - offset)
            throw PayloadError("SBF field exceeds payload at " +
                               std::to_string(offset));
        auto result = bytes.subspan(offset, n);
        offset += n;
        return result;
    }
    template <class T, size_t N, bool Signed = false>
    T scalar(double scale = 1) {
        auto b = take(N);
        if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
            return {b.begin(), b.end()};
        else if constexpr (std::is_same_v<T, WideInteger>) {
            if (scale != 1)
                throw PayloadError("Scaled wide SBF integer is unsupported");
            return {{b.begin(), b.end()}, Signed};
        } else {
            uint64_t value = 0;
            for (size_t i = 0; i < N; ++i)
                value |= uint64_t(b[i]) << (8 * i);
            if constexpr (std::is_floating_point_v<T> && !Signed) {
                static_assert(N == 4 || N == 8);
                if constexpr (N == 4)
                    return double(std::bit_cast<float>(uint32_t(value))) *
                           scale;
                else
                    return std::bit_cast<double>(value) * scale;
            } else {
                if constexpr (Signed) {
                    if constexpr (N > 0 && N < 8)
                        if (b.back() & 128)
                            value |= ~uint64_t{0} << (8 * N);
                    return T(std::bit_cast<int64_t>(value));
                } else
                    return T(value);
            }
        }
    }
    std::vector<uint8_t> remaining() {
        auto b = take(bytes.size() - offset);
        return {b.begin(), b.end()};
    }
    void padding(size_t base, uint64_t length) {
        if (offset - base > length)
            throw PayloadError("SBF sub-block shorter than its defined fields");
        take(length - (offset - base));
    }
    size_t count(uint64_t n) const {
        if (n > bytes.size())
            throw PayloadError("SBF repetition count exceeds payload");
        return size_t(n);
    }
};
inline uint64_t bits(std::span<const uint8_t> data, size_t first,
                     size_t count) {
    if (count > 64 || first + count > data.size() * 8)
        throw PayloadError("Invalid SBF bit schema");
    uint64_t value = 0;
    for (size_t j = 0; j < count; ++j)
        value |= uint64_t((data[(first + j) / 8] >> ((first + j) % 8)) & 1)
                 << j;
    return value;
}
struct NoFields {
    static constexpr bool enabled = false, hierarchical = false;
};
inline std::string hex(std::span<const uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 15]);
    }
    return out;
}
using cppgnss::detail::TextDump;
template <class Sink> struct Group {
    Sink &sink;
    Group(Sink &sink, const char *name,
          std::optional<size_t> index = std::nullopt)
        : sink(sink) {
        if constexpr (Sink::hierarchical)
            sink.begin(name, index);
    }
    ~Group() {
        if constexpr (Sink::hierarchical)
            sink.end();
    }
};
template <class Sink, class T>
void emit(Sink &sink, const char *name, const std::string &suffix,
          const T &value) {
    if constexpr (Sink::enabled)
        sink.field(name, suffix, value);
}
template <class Sink> std::string suffix(const std::string &parent, size_t i) {
    if constexpr (Sink::enabled) {
        char index[32];
        snprintf(index, sizeof(index), "_%02zu", i + 1);
        return parent + index;
    } else
        return {};
}
template <class T>
uint64_t reference(T value, const char *name, const std::string &suffix) {
    if constexpr (std::is_integral_v<T>) {
        if constexpr (std::is_unsigned_v<T>)
            return value;
        else if (value >= 0)
            return uint64_t(value);
    }
    throw PayloadError("Noninteger SBF count/condition: " + std::string(name) +
                       suffix);
}
template <class T>
BlockInfo info(uint16_t id, uint8_t revision, std::string_view name,
               const ParseResult<T> &parsed) {
    BlockInfo result{id, revision, name, Status::decoded, 0, {}, {}, {}};
    if (!parsed) {
        result.status =
            parsed.error().code == ParseErrorCode::UNSUPPORTED_LAYOUT
                ? Status::unsupported_schema
                : Status::invalid_payload;
        result.consumed = parsed.error().offset.value_or(0);
        result.error = parsed.error().detail;
    } else {
        result.consumed = parsed.consumed();
        if constexpr (requires {
                          parsed.value().TOW;
                          parsed.value().WNc;
                      }) {
            result.tow_ms = parsed.value().TOW;
            result.week = parsed.value().WNc;
        }
    }
    return result;
}
} // namespace detail
} // namespace cppgnss::SBF
