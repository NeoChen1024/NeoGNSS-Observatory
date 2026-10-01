// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "broadcast_fields.hpp"
#include <cmath>
#include <span>

namespace neognss_obs::broadcast_detail {
class BroadcastBits {
    std::span<const uint8_t> data_;
    size_t size_;

  public:
    BroadcastBits(std::span<const uint8_t> data, size_t size)
        : data_(data), size_(size) {
        if (size > data.size() * 8)
            throw std::invalid_argument("Truncated broadcast bits");
    }
    uint64_t u(size_t p, size_t n) const {
        if (n > 63 || p > size_ || n > size_ - p)
            throw std::out_of_range("Broadcast field outside body");
        uint64_t v = 0;
        for (size_t i = p; i < p + n; ++i)
            v = (v << 1) | ((data_[i / 8] >> (7 - i % 8)) & 1);
        return v;
    }
    int64_t s(size_t p, size_t n) const {
        if (!n)
            throw std::out_of_range("Empty signed field");
        uint64_t v = u(p, n);
        if (v & (uint64_t(1) << (n - 1)))
            return -int64_t((uint64_t(1) << n) - v);
        return int64_t(v);
    }
    double f(size_t p, size_t n, int exp, bool sign = true,
             double factor = 1) const {
        return std::ldexp(sign ? double(s(p, n)) : double(u(p, n)), exp) *
               factor;
    }
    Tick offset(size_t p, size_t n, int exp) const {
        Tick x = Tick(s(p, n)) * ps, d = Tick(1) << -exp;
        bool neg = x < 0;
        if (neg)
            x = -x;
        Tick q = x / d, r = x % d;
        if (r * 2 > d || (r * 2 == d && (q & 1) != 0))
            ++q;
        return neg ? -q : q;
    }
    Bytes bytes(size_t p, size_t n) const {
        Bytes out((n + 7) / 8, 0);
        for (size_t i = 0; i < n; ++i)
            out[i / 8] |= uint8_t(u(p + i, 1) << (7 - i % 8));
        return out;
    }
};
Tick reference(Tick now, int64_t week, int modulo, int64_t tow);
inline Value gst_reference(std::optional<Tick> now, int64_t week, int64_t tow,
                           int modulo = 4096) {
    if (!now || tow < 0 || tow >= 604800)
        return {};
    return reference(*now, (week + 1024) % modulo, modulo, tow);
}
inline Value nearest_week(std::optional<Tick> now, Tick tow) {
    if (!now || tow < 0 || tow >= seconds(604800))
        return {};
    Tick t = (*now / seconds(604800)) * seconds(604800) + tow;
    if (t - *now > seconds(302400))
        t -= seconds(604800);
    if (*now - t > seconds(302400))
        t += seconds(604800);
    return t;
}
} // namespace neognss_obs::broadcast_detail
