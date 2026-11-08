// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "broadcast_fields.hpp"
#include <cmath>
#include <initializer_list>
#include <span>

namespace neognss_obs::broadcast_detail {
// RAW * 2^-FRACTIONAL_BITS seconds as picoseconds, rounded half to even
// without an intermediate double.
inline Tick binary_seconds(int64_t raw, unsigned fractional_bits) {
    return binary_scaled(Tick(raw), int(fractional_bits), Tick(ps));
}
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
        return binary_seconds(s(p, n), unsigned(-exp));
    }
    Bytes bytes(size_t p, size_t n) const {
        Bytes out((n + 7) / 8, 0);
        for (size_t i = 0; i < n; ++i)
            out[i / 8] |= uint8_t(u(p + i, 1) << (7 - i % 8));
        return out;
    }
};
// Sequential fields: each read starts where the previous one ended.
class BitCursor {
    const BroadcastBits &bits_;
    size_t pos_;

  public:
    explicit BitCursor(const BroadcastBits &bits, size_t pos = 0)
        : bits_(bits), pos_(pos) {}
    int64_t u(size_t n) {
        auto v = int64_t(bits_.u(pos_, n));
        pos_ += n;
        return v;
    }
    int64_t s(size_t n) {
        auto v = bits_.s(pos_, n);
        pos_ += n;
        return v;
    }
};
// Table-driven fields at consecutive positions. Type 'u'/'s' is an integer,
// 't' unsigned seconds times factor, 'd' a binary time offset, 'U' an unsigned
// and 'f' a signed float scaled by 2^exp * factor.
struct Field {
    const char *name;
    int width;
    int exp = 0;
    char type = 'u';
    double factor = 1;
};
inline Row fields(const BroadcastBits &b, size_t p,
                  std::initializer_list<Field> list) {
    Row r;
    for (const auto &f : list) {
        if (f.type == 'u')
            r[f.name] = int64_t(b.u(p, f.width));
        else if (f.type == 's')
            r[f.name] = b.s(p, f.width);
        else if (f.type == 't')
            r[f.name] = seconds(int64_t(b.u(p, f.width)) * int64_t(f.factor));
        else if (f.type == 'd')
            r[f.name] = b.offset(p, f.width, f.exp);
        else
            r[f.name] = b.f(p, f.width, f.exp, f.type != 'U', f.factor);
        p += f.width;
    }
    return r;
}
// Adds fields of `x` not already in `r`.
inline void merge(Row &r, const Row &x) { r.insert(x.begin(), x.end()); }
inline int64_t integer(const Row &r, const char *name) {
    return std::get<int64_t>(r.at(name));
}
// Parts of one source-local data set, collected from the first part's
// reception time.
struct PartAssembly {
    std::map<int, Row> parts;
    std::optional<Tick> start;
    // Starts over when more than TIMEOUT has passed since the first part, or
    // when CONFLICT(id, part) holds for a held part, counting
    // "<prefix>assembly_timeout" or "<prefix>assembly_changed"; then holds
    // PART as ID.
    template <class Conflict>
    void add(Tick t, Tick timeout, int id, const Row &part, Conflict conflict,
             std::map<std::string, uint64_t> &counts,
             const std::string &prefix) {
        if (start && t - *start > timeout) {
            *this = {};
            ++counts[prefix + "assembly_timeout"];
        }
        for (const auto &[held, old] : parts)
            if (conflict(held, old)) {
                *this = {};
                ++counts[prefix + "assembly_changed"];
                break;
            }
        if (!start)
            start = t;
        parts[id] = part;
    }
};
// Data bits of consecutive 30-bit navigation words with parity removed: the
// first `first` bits of word 0 and the first `rest` bits of each later word.
inline Bytes strip_parity(std::span<const uint8_t> body, size_t words,
                          size_t first, size_t rest) {
    const BroadcastBits b(body, words * 30);
    Bytes out((first + (words - 1) * rest + 7) / 8, 0);
    size_t n = 0;
    for (size_t w = 0; w < words; ++w)
        for (size_t k = 0; k < (w ? rest : first); ++k, ++n)
            out[n / 8] |= uint8_t(b.u(w * 30 + k, 1) << (7 - n % 8));
    return out;
}
// WEEK, a week number truncated to MODULO, completed to the full week nearest
// the reception context NOW (never the host clock), plus TOW.
inline Tick reference(Tick now, int64_t week, int modulo, Tick tow) {
    auto w = int64_t(now / seconds(604800));
    auto full = w + (week - w % modulo);
    if (full - w > modulo / 2)
        full -= modulo;
    if (w - full > modulo / 2)
        full += modulo;
    return seconds(full * 604800) + tow;
}
// reference() for a broadcast week/TOW pair; null without reception context or
// for a TOW outside the week.
inline Value week_reference(std::optional<Tick> now, int64_t week, int modulo,
                            Tick tow) {
    if (!now || tow < 0 || tow >= seconds(604800))
        return {};
    return reference(*now, week, modulo, tow);
}
// GST week 0 is GPST week 1024.
inline Value gst_reference(std::optional<Tick> now, int64_t week, Tick tow,
                           int modulo = 4096) {
    return week_reference(now, (week + 1024) % modulo, modulo, tow);
}
inline Value nearest_week(std::optional<Tick> now, Tick tow) {
    if (!now || tow < 0 || tow >= seconds(604800))
        return {};
    return neognss_obs::nearest_week(*now, tow);
}
} // namespace neognss_obs::broadcast_detail
