// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Kelei Chen

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <cppgnss/parse.hpp>
#include <cppgnss/ubx_ids_gen.hpp>

#pragma once

namespace UBX {
using std::string;
using std::vector;

template <typename T>
concept UbxScalar =
    (std::integral<T> || std::floating_point<T>) &&
    (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

template <UbxScalar T> T little_to_native(T value) {
    if constexpr (sizeof(T) == 1 ||
                  std::endian::native == std::endian::little) {
        return value;
    } else {
        static_assert(std::endian::native == std::endian::big,
                      "mixed-endian targets are not supported");
        auto bytes = std::bit_cast<std::array<uint8_t, sizeof(T)>>(value);
        std::reverse(bytes.begin(), bytes.end());
        return std::bit_cast<T>(bytes);
    }
}

template <UbxScalar T>
T read_le(std::span<const uint8_t> payload, size_t offset) {
    if (offset > payload.size() || sizeof(T) > payload.size() - offset)
        throw std::out_of_range("UBX scalar exceeds payload bounds");

    std::array<uint8_t, sizeof(T)> bytes{};
    std::copy_n(payload.begin() + offset, sizeof(T), bytes.begin());
    if constexpr (std::endian::native == std::endian::big && sizeof(T) > 1)
        std::reverse(bytes.begin(), bytes.end());
    return std::bit_cast<T>(bytes);
}

} // namespace UBX
