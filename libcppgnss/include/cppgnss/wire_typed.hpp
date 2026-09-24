// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <bit>
#include <charconv>
#include <cmath>
#include <cppgnss/parse.hpp>
#include <format>
#include <limits>
#include <type_traits>

namespace cppgnss {
// NMEA hex integers may exceed 64 bits (for example receiver unique IDs).
// Retain exact digits, including leading zeroes, without fixed-width overflow.
struct HexInteger {
    std::string digits;
};
} // namespace cppgnss
namespace cppgnss::detail {
struct WireError {
    size_t offset;
    std::string detail;
};
struct BitCursor {
    std::span<const uint8_t> bytes;
    size_t bit = 0;
    uint64_t u(size_t width) {
        if (width > 64 || width > bytes.size() * 8 - bit)
            throw WireError{bit / 8, "Invalid or truncated bit field"};
        uint64_t value = 0;
        for (size_t i = 0; i < width; ++i, ++bit)
            value = (value << 1) | ((bytes[bit / 8] >> (7 - bit % 8)) & 1);
        return value;
    }
    int64_t s(size_t width) {
        auto value = u(width);
        if (width && width < 64 && (value & (uint64_t{1} << (width - 1))))
            value |= (~uint64_t{0}) << width;
        return std::bit_cast<int64_t>(value);
    }
    size_t count(uint64_t value) const {
        if (value > bytes.size() * 8 - bit)
            throw WireError{bit / 8, "Repetition exceeds remaining payload"};
        return size_t(value);
    }
};
struct TextCursor {
    std::string_view bytes;
    size_t offset = 0;
    bool ended = false;
    bool brackets = false;
    explicit TextCursor(std::span<const uint8_t> input)
        : bytes(reinterpret_cast<const char *>(input.data()), input.size()),
          ended(input.empty()) {}
    size_t remaining() const {
        if (ended)
            return 0;
        size_t n = 1;
        for (size_t i = offset; i < bytes.size(); ++i)
            n += bytes[i] == ',';
        return n;
    }
    std::string_view take() {
        if (ended)
            return {};
        auto start = offset;
        auto end = bytes.find(',', start);
        if (end == std::string_view::npos) {
            end = bytes.size();
            ended = true;
        }
        offset = ended ? end : end + 1;
        auto value = bytes.substr(start, end - start);
        if (brackets) {
            while (value.starts_with('['))
                value.remove_prefix(1);
            while (value.ends_with(']'))
                value.remove_suffix(1);
        }
        return value;
    }
    template <class T> std::optional<T> number(int base = 10) {
        auto start = offset;
        auto text = take();
        if (text.empty())
            return {};
        if (text.front() == '+')
            text.remove_prefix(1);
        T value{};
        std::from_chars_result r;
        if constexpr (std::is_floating_point_v<T>)
            r = std::from_chars(text.data(), text.data() + text.size(), value,
                                std::chars_format::general);
        else
            r = std::from_chars(text.data(), text.data() + text.size(), value,
                                base);
        if (r.ec != std::errc{} || r.ptr != text.data() + text.size())
            throw WireError{start, "Invalid numeric NMEA field"};
        if constexpr (std::is_floating_point_v<T>)
            if (!std::isfinite(value))
                throw WireError{start, "Non-finite NMEA field"};
        return value;
    }
    std::optional<std::string> text(bool character = false) {
        auto start = offset;
        auto value = take();
        if (value.empty())
            return {};
        if (character && value.size() != 1)
            throw WireError{start, "Expected one NMEA character"};
        return std::string(value);
    }
    std::optional<HexInteger> hex() {
        auto start = offset;
        auto value = take();
        if (value.empty())
            return {};
        for (char c : value)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F')))
                throw WireError{start, "Invalid hexadecimal NMEA field"};
        return HexInteger{std::string(value)};
    }
    size_t count(std::optional<int64_t> n) const {
        if (!n || *n < 0 || uint64_t(*n) > remaining())
            throw WireError{offset, "Invalid NMEA repetition count"};
        return size_t(*n);
    }
};
inline std::string quoted(std::string_view value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 32 || c >= 127)
            out += std::format("\\x{:02x}", c);
        else
            out += char(c);
    }
    return out + '"';
}
inline std::string show(const std::string &v) { return quoted(v); }
inline std::string show(const HexInteger &v) { return quoted(v.digits); }
template <class T> std::string show(T v) { return std::format("{}", v); }
template <class T> std::string show(const std::optional<T> &v) {
    return v ? show(*v) : "null";
}
inline std::string nmea_identity(const FrameView &frame) {
    const auto &h = std::get<NmeaHeader>(frame.header);
    TextCursor c(frame.payload);
    auto count = c.remaining();
    auto first = c.take();
    if (!h.address.starts_with('P'))
        return h.address.size() > 2 ? std::string(h.address.substr(2))
                                    : std::string{};
    std::string key(h.address.substr(1));
    if (key == "ASHR" || key == "GPPADV" || key == "FEC" || key == "SSN" ||
        key == "TNL" || key == "UBX") {
        if (key != "ASHR" || first.empty() || first.front() < '0' ||
            first.front() > '9')
            key += first;
    }
    if (key.starts_with("QTM")) {
        if (count == 1 && first == "OK")
            return "QTMACK";
        if (count == 2 && first == "ERROR")
            return "QTMNAK";
        if (key == "QTMCFGMSGRATE") {
            if (count == 3)
                key += "_NOVER";
            else if (count == 5)
                key += "_INTFNOVER";
            else if (count == 6)
                key += "_INTF";
        } else if (key == "QTMCFGSAT" && count == 4)
            key += "_LOW";
        else if (key == "QTMCFGGEOFENCE" && count == 13)
            key += "_POLY";
        else if (key == "QTMSN" && !first.empty() && first.front() >= '0' &&
                 first.front() <= '9')
            key += "_ALT";
    }
    if (key == "STMDRSENMSG")
        key += "_" + std::string(first);
    return key;
}
inline bool rtcm_matches(const FrameView &frame, uint16_t id,
                         int subtype = -1) {
    if (frame.id() != id)
        return false;
    if (subtype < 0)
        return true;
    if (frame.payload.size() < 3)
        return false;
    // IGS SSR: message number (12), version (3), subtype (8).
    return (((frame.payload[1] & 1) << 7) | (frame.payload[2] >> 1)) == subtype;
}
} // namespace cppgnss::detail
