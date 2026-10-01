// SPDX-License-Identifier: GPL-3.0-only
#include "broadcast_beidou.hpp"
#include "broadcast_bits.hpp"
#include <neognss_obs/raw_bits.hpp>

namespace neognss_obs::broadcast_detail {
std::vector<DecodedMessage>
decode_pppb2b(std::span<const uint8_t> body, int64_t satellite,
              std::optional<Tick> time,
              std::map<std::string, uint64_t> &counts) {
    BroadcastBits b(body, 474);
    std::vector<DecodedMessage> out;
    if (!bds_ppp_b2b_code_assigned(satellite)) {
        ++counts["bds_pppb2b_unassigned_code"];
        return out;
    }
    if (b.u(0, 6) != uint64_t(satellite)) {
        ++counts["bds_prn_mismatch"];
        return out;
    }
    const bool available = b.u(6, 1) == 0;
    const int type = int(b.u(12, 6));
    Row base{{"message_type", int64_t(type)}, {"service_available", available}};
    auto emit = [&](std::string kind, Row r, bool candidate) {
        r.insert(base.begin(), base.end());
        out.push_back(
            {"bds_pppb2b_" + kind, std::move(r), candidate && available, {}});
    };
    emit("status", {}, false);
    if (type == 63) {
        ++counts["bds_pppb2b_null_messages"];
        ++counts["decoded_messages"];
        return out;
    }
    if (type < 1 || type > 7) {
        ++counts["bds_reserved_messages"];
        ++counts["decoded_messages"];
        return out;
    }
    size_t p = 18;
    auto u = [&](int n) {
        auto v = int64_t(b.u(p, n));
        p += n;
        return v;
    };
    auto s = [&](int n) {
        auto v = b.s(p, n);
        p += n;
        return v;
    };
    auto header = [&](int validity) {
        Row r;
        int64_t sod = u(17);
        u(4);
        r["iod_ssr"] = u(2);
        r["epoch_bdt_sod_s"] = seconds(sod);
        Value epoch;
        if (time && sod < 86400) {
            Tick t = (*time - seconds(14)) / seconds(86400) * seconds(86400) +
                     seconds(sod + 14);
            if (t > *time + seconds(43200))
                t -= seconds(86400);
            if (*time > t + seconds(43200))
                t += seconds(86400);
            epoch = t;
        }
        r["epoch_gpst"] = epoch;
        r["valid_until_gpst"] =
            validity && std::holds_alternative<Tick>(epoch)
                ? Value(std::get<Tick>(epoch) + seconds(validity))
                : Value{};
        return r;
    };
    // Slots are the ICD's common namespace, not receiver-specific SV IDs.
    // Retain unexpanded mask ordinals: a consumer must pair the exact IODP
    // mask.
    auto orbit = [&](int n, Row r) {
        Records rows{{{"slot", int64_t{}},
                      {"iodn", int64_t{}},
                      {"iod_corr", int64_t{}},
                      {"radial_m", double{}},
                      {"along_m", double{}},
                      {"cross_m", double{}},
                      {"urai", int64_t{}}},
                     {}};
        for (int i = 0; i < n; ++i) {
            Fields x;
            x["slot"] = u(9);
            x["iodn"] = u(10);
            x["iod_corr"] = u(3);
            auto a = s(15), c = s(13), d = s(13);
            x["radial_m"] = a == -16384 ? Scalar{} : Scalar(double(a) * 0.0016);
            x["along_m"] = c == -4096 ? Scalar{} : Scalar(double(c) * 0.0064);
            x["cross_m"] = d == -4096 ? Scalar{} : Scalar(double(d) * 0.0064);
            x["urai"] = u(6);
            auto slot = std::get<int64_t>(x["slot"]);
            if (slot >= 1 && slot <= 137)
                rows.rows.push_back(std::move(x));
        }
        r["entries"] = rows;
        emit("orbit", std::move(r), true);
    };
    auto clocks = [&](int n, Row r, bool direct, int64_t start) {
        Records rows{{{"slot", int64_t{}},
                      {"mask_ordinal", int64_t{}},
                      {"iod_corr", int64_t{}},
                      {"c0_m", double{}}},
                     {}};
        for (int i = 0; i < n; ++i) {
            Fields x{{"slot", Scalar{}}, {"mask_ordinal", Scalar{}}};
            if (direct)
                x["slot"] = u(9);
            else
                x["mask_ordinal"] = start + i;
            x["iod_corr"] = u(3);
            auto value = s(15);
            x["c0_m"] =
                value == -16384 ? Scalar{} : Scalar(double(value) * 0.0016);
            if (!direct || (std::get<int64_t>(x["slot"]) >= 1 &&
                            std::get<int64_t>(x["slot"]) <= 137))
                rows.rows.push_back(std::move(x));
        }
        r["entries"] = rows;
        emit(direct ? "clock_direct" : "clock_mask", std::move(r), true);
    };
    try {
        if (type == 1) {
            Row r = header(0);
            r["iodp"] = u(4);
            std::vector<int64_t> slots;
            for (int i = 1; i <= 255; ++i)
                if (u(1))
                    slots.push_back(i);
            // All mask bits remain necessary for compact-index resolution,
            // including systems excluded from scientific processing.
            r["slots"] = slots;
            emit("mask", r, true);
        } else if (type == 2)
            orbit(6, header(96));
        else if (type == 3) {
            Row r = header(86400);
            int n = int(u(5));
            Records rows{{{"slot", int64_t{}},
                          {"signal_index", int64_t{}},
                          {"bias_m", double{}}},
                         {}};
            for (int i = 0; i < n; ++i) {
                auto slot = u(9);
                int m = int(u(4));
                for (int j = 0; j < m; ++j) {
                    auto signal = u(4), bias = s(12);
                    if (slot >= 1 && slot <= 137)
                        rows.rows.push_back({{"slot", slot},
                                             {"signal_index", signal},
                                             {"bias_m", double(bias) * 0.017}});
                }
            }
            r["entries"] = rows;
            emit("code_bias", r, true);
        } else if (type == 4) {
            Row r = header(12);
            r["iodp"] = u(4);
            auto sub = u(5);
            if (sub > 11)
                throw std::out_of_range("Reserved PPP subtype");
            clocks(23, r, false, sub * 23 + 1);
        } else if (type == 5) {
            Row r = header(96);
            r["iodp"] = u(4);
            auto sub = u(3);
            if (sub > 3)
                throw std::out_of_range("Reserved PPP subtype");
            Records rows{{{"mask_ordinal", int64_t{}}, {"urai", int64_t{}}},
                         {}};
            for (int i = 0; i < 70; ++i)
                rows.rows.push_back(
                    {{"mask_ordinal", sub * 70 + i + 1}, {"urai", u(6)}});
            r["entries"] = rows;
            emit("ura", r, true);
        } else {
            int nc = int(u(5)), no = int(u(3));
            if (nc > (type == 6 ? 22 : 15) || no > 6)
                throw std::out_of_range("Invalid PPP group count");
            if (nc) {
                Row r = header(12);
                int64_t start = 0;
                if (type == 6) {
                    r["iodp"] = u(4);
                    start = u(9);
                    if (start < 1 || start + nc - 1 > 255)
                        throw std::out_of_range("Invalid PPP mask ordinal");
                }
                clocks(nc, r, type == 7, start);
            }
            if (no)
                orbit(no, header(96));
        }
    } catch (const std::out_of_range &) {
        // No partially decoded correction is published from a malformed frame.
        out.resize(1);
        ++counts["bds_pppb2b_invalid_layout"];
    }
    counts["decoded_messages"] += out.size();
    return out;
}
} // namespace neognss_obs::broadcast_detail
