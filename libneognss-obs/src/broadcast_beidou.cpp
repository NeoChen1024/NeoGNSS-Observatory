// SPDX-License-Identifier: GPL-3.0-only
#include "broadcast_beidou.hpp"
#include "broadcast_bits.hpp"
#include <algorithm>
#include <numbers>

namespace neognss_obs::broadcast_detail {
namespace {
constexpr double pi = std::numbers::pi;
int64_t integer(const Row &r, const char *n) {
    return std::get<int64_t>(r.at(n));
}
struct Field {
    const char *name;
    int width;
    int exp = 0;
    char type = 'u';
    double factor = 1;
};
Row fields(const BroadcastBits &b, size_t p,
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
void merge(Row &a, const Row &b) { a.insert(b.begin(), b.end()); }
Value bdt(std::optional<Tick> now, int64_t week, int64_t tow,
          int modulo = 8192) {
    if (!now || tow < 0 || tow >= 604800)
        return {};
    return reference(*now - seconds(14), (week + 1356) % modulo, modulo, tow) +
           seconds(14);
}
Value bdt_nearest(std::optional<Tick> now, Tick tow) {
    auto t = nearest_week(
        now ? std::optional<Tick>(*now - seconds(14)) : std::nullopt, tow);
    if (auto p = std::get_if<Tick>(&t))
        return *p + seconds(14);
    return {};
}
Row orbit1(const BroadcastBits &b, size_t p) {
    return fields(b, p,
                  {{"toe_bdt_s", 11, 0, 't', 300},
                   {"satellite_type", 2},
                   {"delta_a_m", 26, -9, 'f'},
                   {"a_dot_m_s", 25, -21, 'f'},
                   {"delta_n_rad_s", 17, -44, 'f', pi},
                   {"delta_n_dot_rad_s2", 23, -57, 'f', pi},
                   {"m0_rad", 33, -32, 'f', pi},
                   {"eccentricity", 33, -34, 'U'},
                   {"omega_rad", 33, -32, 'f', pi}});
}
Row orbit2(const BroadcastBits &b, size_t p) {
    return fields(b, p,
                  {{"omega0_rad", 33, -32, 'f', pi},
                   {"i0_rad", 33, -32, 'f', pi},
                   {"omega_dot_rad_s", 19, -44, 'f', pi},
                   {"idot_rad_s", 15, -44, 'f', pi},
                   {"cis_rad", 16, -30, 'f'},
                   {"cic_rad", 16, -30, 'f'},
                   {"crs_m", 24, -8, 'f'},
                   {"crc_m", 24, -8, 'f'},
                   {"cus_rad", 21, -30, 'f'},
                   {"cuc_rad", 21, -30, 'f'}});
}
Row clock(const BroadcastBits &b, size_t p) {
    return fields(b, p,
                  {{"toc_bdt_s", 11, 0, 't', 300},
                   {"af0_s", 25, -34, 'd'},
                   {"af1_s_s", 22, -50, 'f'},
                   {"af2_s_s2", 11, -66, 'f'}});
}
Row sisai(const BroadcastBits &b, size_t p) {
    return fields(b, p,
                  {{"top_bdt_s", 11, 0, 't', 300},
                   {"sisai_ocb", 5},
                   {"sisai_oc1", 3},
                   {"sisai_oc2", 3}});
}
Row iono(const BroadcastBits &b, size_t p) {
    std::vector<double> a{b.f(p, 10, -3, false)};
    for (int i = 0; i < 8; ++i)
        a.push_back(
            b.f(p + 10 + i * 8, 8, -3, i == 0 || i >= 4, i == 3 ? -1 : 1));
    return {{"alpha_tecu", a}};
}
Row utc(const BroadcastBits &b, size_t p, std::optional<Tick> time) {
    auto r = fields(b, p,
                    {{"a0_s", 16, -35, 'd'},
                     {"a1_s_s", 13, -51, 'f'},
                     {"a2_s_s2", 7, -68, 'f'},
                     {"delta_tls_s", 8, 0, 's'},
                     {"tot_bdt_s", 16, 0, 't', 16},
                     {"week_raw", 13},
                     {"wn_lsf_raw", 13},
                     {"dn", 3},
                     {"delta_tlsf_s", 8, 0, 's'}});
    r["reference_gpst"] = bdt(time, integer(r, "week_raw"),
                              int64_t(std::get<Tick>(r.at("tot_bdt_s")) / ps));
    return r;
}
Row eop(const BroadcastBits &b, size_t p) {
    return fields(b, p,
                  {{"teop_bdt_s", 16, 0, 't', 16},
                   {"pm_x_arcsec", 21, -20, 'f'},
                   {"pm_x_dot_arcsec_day", 15, -21, 'f'},
                   {"pm_y_arcsec", 21, -20, 'f'},
                   {"pm_y_dot_arcsec_day", 15, -21, 'f'},
                   {"ut1_minus_utc_s", 31, -24, 'd'},
                   {"ut1_dot_s_day", 19, -25, 'f'}});
}
Row bgto(const BroadcastBits &b, size_t p, std::optional<Tick> time) {
    auto r = fields(b, p,
                    {{"gnss_id", 3},
                     {"week_raw", 13},
                     {"t0_bdt_s", 16, 0, 't', 16},
                     {"a0_s", 16, -35, 'd'},
                     {"a1_s_s", 13, -51, 'f'},
                     {"a2_s_s2", 7, -68, 'f'}});
    r["reference_gpst"] = bdt(time, integer(r, "week_raw"),
                              int64_t(std::get<Tick>(r.at("t0_bdt_s")) / ps));
    r["available"] = integer(r, "gnss_id") >= 1 && integer(r, "gnss_id") <= 3;
    return r;
}
Row midi(const BroadcastBits &b, size_t p, std::optional<Tick> time) {
    auto r = fields(b, p,
                    {{"subject_sv_id", 6},
                     {"satellite_type", 2},
                     {"week_raw", 13},
                     {"toa_bdt_s", 8, 0, 't', 4096},
                     {"eccentricity", 11, -16, 'U'},
                     {"delta_i_rad", 11, -14, 'f', pi},
                     {"sqrt_a", 17, -4, 'U'},
                     {"omega0_rad", 16, -15, 'f', pi},
                     {"omega_dot_rad_s", 11, -33, 'f', pi},
                     {"omega_rad", 16, -15, 'f', pi},
                     {"m0_rad", 16, -15, 'f', pi},
                     {"af0_s", 11, -20, 'd'},
                     {"af1_s_s", 10, -37, 'f'},
                     {"health", 8}});
    r["reference_gpst"] = bdt(time, integer(r, "week_raw"),
                              int64_t(std::get<Tick>(r.at("toa_bdt_s")) / ps));
    return r;
}
Row reduced(const BroadcastBits &b, size_t p, int64_t week, int64_t tow,
            std::optional<Tick> time) {
    auto r = fields(b, p,
                    {{"subject_sv_id", 6},
                     {"satellite_type", 2},
                     {"delta_a_m", 8, 9, 'f'},
                     {"omega0_rad", 7, -6, 'f', pi},
                     {"phi0_rad", 7, -6, 'f', pi},
                     {"health", 8}});
    r["week_raw"] = week;
    r["toa_bdt_s"] = seconds(tow);
    r["reference_gpst"] = bdt(time, week, tow);
    return r;
}
Bytes legacy_data(std::span<const uint8_t> data) {
    BroadcastBits b(data, 300);
    Bytes out(28);
    size_t n = 0;
    for (size_t w = 0; w < 10; ++w)
        for (size_t k = 0; k < (w ? 22 : 26); ++k, ++n)
            out[n / 8] |= uint8_t(b.u(w * 30 + k, 1) << (7 - n % 8));
    return out;
}
Row klobuchar(const BroadcastBits &b, size_t p) {
    std::vector<double> alpha, beta;
    const int ae[] = {-30, -27, -24, -24}, be[] = {11, 14, 16, 16};
    for (int i = 0; i < 4; ++i) {
        alpha.push_back(b.f(p + i * 8, 8, ae[i]));
        beta.push_back(b.f(p + 32 + i * 8, 8, be[i]));
    }
    return {{"alpha", alpha}, {"beta", beta}};
}
Row legacy_clock(const BroadcastBits &b, size_t p) {
    auto r = fields(b, p,
                    {{"health", 1},
                     {"aodc", 5},
                     {"urai", 4},
                     {"week_raw", 13},
                     {"toc_bdt_s", 17, 0, 't', 8}});
    r["tgd_b1i_s"] = Tick(b.s(p + 40, 10)) * 100;
    r["tgd_b2i_s"] = Tick(b.s(p + 50, 10)) * 100;
    return r;
}
} // namespace
struct BeidouDecoder::State {
    struct Part {
        Row row;
        Tick received;
        int64_t sow;
    };
    std::map<std::string, std::map<int, Part>> pending;
    struct AlmanacContext {
        int64_t week = 0, toa = 0;
        std::optional<Tick> reference_seen, expanded_seen;
        bool expanded = false;
    };
    std::map<std::string, AlmanacContext> almanacs;
};
BeidouDecoder::BeidouDecoder() : state_(std::make_unique<State>()) {}
BeidouDecoder::~BeidouDecoder() = default;
void BeidouDecoder::clear() {
    state_->pending.clear();
    state_->almanacs.clear();
}

std::vector<DecodedMessage>
BeidouDecoder::decode(std::span<const uint8_t> body, const std::string &family,
                      const std::string &source, int64_t satellite,
                      std::optional<Tick> time,
                      std::map<std::string, uint64_t> &counts) {
    std::vector<DecodedMessage> out;
    if (satellite < 1 || satellite > 63) {
        ++counts["bds_invalid_satellite"];
        return out;
    }
    if (family == "BDS_PPP_B2B")
        return decode_pppb2b(body, satellite, time, counts);
    const bool b1 = family == "BDS_BCNAV1", b2 = family == "BDS_BCNAV2",
               b3 = family == "BDS_BCNAV3";
    if (family == "BDS_D1" || family == "BDS_D2") {
        const bool d2 = family == "BDS_D2";
        auto bytes = legacy_data(body);
        BroadcastBits b(bytes, 224);
        const std::string pre = d2 ? "bds_d2_" : "bds_d1_";
        auto emit = [&](std::string kind, Row r, bool candidate = false) {
            ++counts["decoded_messages"];
            out.push_back({pre + kind, std::move(r), candidate, {}});
        };
        int sf = int(b.u(15, 3));
        int64_t sow = b.u(18, 20);
        if (b.u(0, 11) != 0x712 || sf < 1 || sf > 5 || sow >= 604800) {
            ++counts["bds_invalid_header"];
            return out;
        }
        int page = d2 ? (sf == 1   ? int(b.u(38, 4))
                         : sf == 5 ? int(b.u(39, 7))
                                   : int(b.u(38, 4)))
                      : (sf >= 4 ? int(b.u(39, 7)) : 0);
        emit("header", {{"subframe_id", int64_t(sf)},
                        {"page", int64_t(page)},
                        {"sow_bdt_s", seconds(sow)}});
        if ((!d2 && sf == 1) || (d2 && sf == 1 && page == 2))
            emit("klobuchar", klobuchar(b, d2 ? 42 : 98), true);
        if (sf == 5 && page == (d2 ? 102 : 10)) {
            auto r = fields(b, 46,
                            {{"delta_tls_s", 8, 0, 's'},
                             {"delta_tlsf_s", 8, 0, 's'},
                             {"wn_lsf_raw", 8},
                             {"a0_s", 32, -30, 'd'},
                             {"a1_s_s", 24, -50, 'f'},
                             {"dn", 8}});
            // No reference week is present in this page. Keep the native SOW.
            r["sow_bdt_s"] = seconds(sow);
            emit("utc", r, true);
        }
        if (!state_->almanacs.contains(source) &&
            state_->almanacs.size() >= 2048)
            throw std::runtime_error(
                "BeiDou almanac context capacity exceeded");
        auto &ac = state_->almanacs[source];
        auto fresh = [&](std::optional<Tick> seen) {
            return time && seen && *time >= *seen &&
                   *time - *seen <= seconds(2250);
        };
        if (sf == 5 && (page == (d2 ? 35 : 7) || page == (d2 ? 36 : 8))) {
            bool upper = page == (d2 ? 36 : 8);
            int n = upper ? 11 : 19;
            Records entries{
                {{"subject_sv_id", int64_t{}}, {"health", int64_t{}}}, {}};
            for (int i = 0; i < n; ++i)
                entries.rows.push_back(
                    {{"subject_sv_id", int64_t(i + (upper ? 20 : 1))},
                     {"health", int64_t(b.u(46 + i * 9, 9))}});
            emit("almanac_health", {{"entries", entries}});
            if (upper && time) {
                ac.week = b.u(145, 8);
                ac.toa = b.u(153, 8) * 4096;
                ac.reference_seen = time;
            }
        }
        if (sf == 5 && page == (d2 ? 101 : 9)) {
            // Legacy BDT-minus-GNSS model uses nanoseconds and native SOW.
            emit("bgto",
                 {{"a0_gps_s", Tick(b.s(76, 14)) * 100},
                  {"a1_gps_s_s", double(b.s(90, 16)) * 1e-10},
                  {"a0_gal_s", Tick(b.s(106, 14)) * 100},
                  {"a1_gal_s_s", double(b.s(120, 16)) * 1e-10}},
                 true);
        }
        int subject = 0;
        if (!d2 && sf == 4 && page >= 1 && page <= 24)
            subject = page;
        if (!d2 && sf == 5 && page >= 1 && page <= 6)
            subject = page + 24;
        if (d2 && sf == 5 && page >= 37 && page <= 60)
            subject = page - 36;
        if (d2 && sf == 5 && page >= 95 && page <= 100)
            subject = page - 70;
        if (subject && time) {
            ac.expanded = b.u(222, 2) == 3;
            ac.expanded_seen = time;
        }
        int extended = d2 ? page - 103 : page - 11;
        if (sf == 5 && extended >= 0 && extended < 13 &&
            fresh(ac.expanded_seen) && ac.expanded) {
            int amid = int(b.u(222, 2));
            if (amid >= 1 && amid <= 3)
                subject = 31 + 13 * (amid - 1) + extended;
            if (subject > 63)
                subject = 0;
        }
        if (subject) {
            auto r = fields(b, 46,
                            {{"sqrt_a", 24, -11, 'U'},
                             {"af1_s_s", 11, -38, 'f'},
                             {"af0_s", 11, -20, 'd'},
                             {"omega0_rad", 24, -23, 'f', pi},
                             {"eccentricity", 17, -21, 'U'},
                             {"delta_i_rad", 16, -19, 'f', pi},
                             {"toa_bdt_s", 8, 0, 't', 4096},
                             {"omega_dot_rad_s", 17, -38, 'f', pi},
                             {"omega_rad", 24, -23, 'f', pi},
                             {"m0_rad", 24, -23, 'f', pi}});
            r["subject_sv_id"] = int64_t(subject);
            r["reference_gpst"] =
                fresh(ac.reference_seen) &&
                        std::get<Tick>(r.at("toa_bdt_s")) == seconds(ac.toa)
                    ? bdt(time, ac.week, ac.toa, 256)
                    : Value{};
            if (std::get<double>(r.at("sqrt_a")) != 0)
                emit("almanac", r,
                     std::holds_alternative<Tick>(r.at("reference_gpst")));
        }
        if ((d2 && (sf != 1 || page == 2 || page < 1 || page > 10)) ||
            (!d2 && sf > 3)) {
            if (out.size() == 1)
                ++counts["bds_legacy_unhandled_payloads"];
            return out;
        }
        if (!time)
            return out;
        if (!state_->pending.contains(source) && state_->pending.size() >= 2048)
            throw std::runtime_error("BeiDou assembly capacity exceeded");
        auto &parts = state_->pending[source];
        int id = d2 ? page : sf;
        std::erase_if(parts, [&](const auto &x) {
            return *time < x.second.received ||
                   *time - x.second.received > seconds(90);
        });
        Row fragment{{"fragment", bytes}};
        const bool duplicate = parts.contains(id) && parts[id].sow == sow &&
                               parts[id].row == fragment;
        if (!duplicate) {
            if (id == 1 || (parts.contains(id) && parts[id].sow == sow))
                parts.clear();
            parts[id] = {fragment, *time, sow};
        }
        for (int i = 1; i <= (d2 ? 10 : 3); ++i)
            if ((!d2 || i != 2) && !parts.contains(i))
                return out;
        const int64_t start = parts[1].sow;
        for (int i = 1; i <= (d2 ? 10 : 3); ++i)
            if (!d2 || i != 2) {
                if ((parts[i].sow - start + 604800) % 604800 !=
                    (i - 1) * (d2 ? 3 : 6)) {
                    ++counts["bds_assembly_conflicts"];
                    return out;
                }
            }
        auto get = [&](int n) {
            return BroadcastBits(
                std::get<Bytes>(parts.at(n).row.at("fragment")), 224);
        };
        auto b1 = get(1);
        Row r = legacy_clock(b1, d2 ? 42 : 38);
        if (!d2) {
            merge(r, fields(b1, 162,
                            {{"af2_s_s2", 11, -66, 'f'},
                             {"af0_s", 24, -33, 'd'},
                             {"af1_s_s", 22, -50, 'f'},
                             {"aode", 5}}));
            auto b2 = get(2), b3 = get(3);
            merge(r, fields(b2, 38,
                            {{"delta_n_rad_s", 16, -43, 'f', pi},
                             {"cuc_rad", 18, -31, 'f'},
                             {"m0_rad", 32, -31, 'f', pi},
                             {"eccentricity", 32, -33, 'U'},
                             {"cus_rad", 18, -31, 'f'},
                             {"crc_m", 18, -6, 'f'},
                             {"crs_m", 18, -6, 'f'},
                             {"sqrt_a", 32, -19, 'U'}}));
            r["toe_bdt_s"] =
                seconds(int64_t((b2.u(222, 2) << 15) | b3.u(38, 15)) * 8);
            merge(r, fields(b3, 53,
                            {{"i0_rad", 32, -31, 'f', pi},
                             {"cic_rad", 18, -31, 'f'},
                             {"omega_dot_rad_s", 24, -43, 'f', pi},
                             {"cis_rad", 18, -31, 'f'},
                             {"idot_rad_s", 14, -43, 'f', pi},
                             {"omega0_rad", 32, -31, 'f', pi},
                             {"omega_rad", 32, -31, 'f', pi}}));
        } else {
            auto p3 = get(3), p4 = get(4), p5 = get(5), p6 = get(6),
                 p7 = get(7), p8 = get(8), p9 = get(9), p10 = get(10);
            auto join = [](uint64_t hi, int nh, uint64_t lo, int nl) {
                uint64_t u = (hi << nl) | lo;
                return (u & (uint64_t(1) << (nh + nl - 1)))
                           ? -int64_t((uint64_t(1) << (nh + nl)) - u)
                           : int64_t(u);
            };
            r["af0_s"] = p3.offset(80, 24, -33);
            r["af1_s_s"] = std::ldexp(
                double(join(p3.u(104, 4), 4, p4.u(42, 18), 18)), -50);
            r["af2_s_s2"] = p4.f(60, 11, -66);
            r["aode"] = int64_t(p4.u(71, 5));
            r["delta_n_rad_s"] = p4.f(76, 16, -43, true, pi);
            r["cuc_rad"] =
                std::ldexp(double(join(p4.u(92, 14), 14, p5.u(42, 4), 4)), -31);
            r["m0_rad"] = p5.f(46, 32, -31, true, pi);
            r["cus_rad"] = p5.f(78, 18, -31);
            r["eccentricity"] =
                std::ldexp(double((p5.u(96, 10) << 22) | p6.u(42, 22)), -33);
            r["sqrt_a"] = p6.f(64, 32, -19, false);
            r["cic_rad"] =
                std::ldexp(double(join(p6.u(96, 10), 10, p7.u(42, 8), 8)), -31);
            r["cis_rad"] = p7.f(50, 18, -31);
            r["toe_bdt_s"] = seconds(int64_t(p7.u(68, 17)) * 8);
            r["i0_rad"] =
                std::ldexp(double(join(p7.u(85, 21), 21, p8.u(42, 11), 11)),
                           -31) *
                pi;
            r["crc_m"] = p8.f(53, 18, -6);
            r["crs_m"] = p8.f(71, 18, -6);
            r["omega_dot_rad_s"] =
                std::ldexp(double(join(p8.u(89, 19), 19, p9.u(42, 5), 5)),
                           -43) *
                pi;
            r["omega0_rad"] = p9.f(47, 32, -31, true, pi);
            r["omega_rad"] =
                std::ldexp(double(join(p9.u(79, 27), 27, p10.u(42, 5), 5)),
                           -31) *
                pi;
            r["idot_rad_s"] = p10.f(47, 14, -43, true, pi);
        }
        if (r.at("toe_bdt_s") != r.at("toc_bdt_s")) {
            ++counts["bds_reference_mismatch"];
            return out;
        }
        auto transmit = bdt(time, integer(r, "week_raw"), start);
        std::optional<Tick> context;
        if (auto t = std::get_if<Tick>(&transmit))
            context = *t;
        r["toe_gpst"] = bdt_nearest(context, std::get<Tick>(r.at("toe_bdt_s")));
        r["toc_gpst"] = bdt_nearest(context, std::get<Tick>(r.at("toc_bdt_s")));
        r["transmission_gpst"] = transmit;
        emit("ephemeris", r, true);
        parts.clear();
        return out;
    }
    if (!b1 && !b2 && !b3) {
        ++counts["bds_unsupported_family"];
        return out;
    }
    BroadcastBits b(body, b1 ? 1800 : b2 ? 576 : 984);
    std::string prefix = b1   ? "bds_bcnav1_"
                         : b2 ? "bds_bcnav2_"
                              : "bds_bcnav3_";
    auto emit = [&](std::string kind, Row r, bool candidate = false,
                    std::string discriminator = "") {
        ++counts["decoded_messages"];
        out.push_back(
            {prefix + kind, std::move(r), candidate, std::move(discriminator)});
    };
    auto alm = [&](std::string kind, Row r) {
        if (integer(r, "subject_sv_id") == 0 ||
            integer(r, "satellite_type") == 0) {
            ++counts["bds_dummy_almanacs"];
            return;
        }
        auto id = std::to_string(integer(r, "subject_sv_id"));
        emit(kind, std::move(r), true, id);
    };
    auto bg = [&](size_t p) {
        auto r = bgto(b, p, time);
        auto id = std::to_string(integer(r, "gnss_id"));
        emit("bgto", std::move(r), true, id);
    };
    Row first, second, clk;
    int64_t sow = 0;
    auto clocks = [&](size_t p, int iodp) {
        clk = clock(b, p);
        clk["iodc"] = int64_t(b.u(iodp, 10));
        emit("clock", clk);
    };
    if (b1) {
        // SF1 remains BCH encoded; do not interpret its first bits as PRN/SOH.
        first = orbit1(b, 111);
        merge(first, orbit2(b, 314));
        merge(first, clock(b, 536));
        first["week_raw"] = int64_t(b.u(72, 13));
        first["how"] = int64_t(b.u(85, 8));
        first["iodc"] = int64_t(b.u(93, 10));
        first["iode"] = int64_t(b.u(103, 8));
        first["tgd_b2ap_s"] = b.offset(605, 12, -34);
        first["isc_b1cd_s"] = b.offset(617, 12, -34);
        first["tgd_b1cp_s"] = b.offset(629, 12, -34);
        if ((integer(first, "iodc") & 255) == integer(first, "iode")) {
            auto epoch = bdt(time, integer(first, "week_raw"),
                             integer(first, "how") * 3600);
            std::optional<Tick> context;
            if (auto t = std::get_if<Tick>(&epoch))
                context = *t;
            first["toe_gpst"] =
                bdt_nearest(context, std::get<Tick>(first.at("toe_bdt_s")));
            first["toc_gpst"] =
                bdt_nearest(context, std::get<Tick>(first.at("toc_bdt_s")));
            emit("ephemeris", first, true);
        } else
            ++counts["bds_issue_mismatch"];
        int type = int(b.u(1272, 6));
        if (type >= 1 && type <= 4) {
            auto status = fields(b, 1278,
                                 {{"health", 2},
                                  {"dif", 1},
                                  {"sif", 1},
                                  {"aif", 1},
                                  {"sismai", 4}});
            status["page_type"] = int64_t(type);
            emit("status", status);
        }
        if (type == 1) {
            emit("orbit_accuracy", {{"sisai_oe", int64_t(b.u(1287, 5))}});
            emit("sisai", sisai(b, 1292));
            emit("bdgim", iono(b, 1314), true);
            emit("utc", utc(b, 1388, time), true);
        } else if (type == 2) {
            emit("sisai", sisai(b, 1287));
            auto week = b.u(1309, 13), toa = b.u(1322, 8) * 4096;
            for (int i = 0; i < 4; ++i)
                alm("reduced_almanac",
                    reduced(b, 1330 + i * 38, week, toa, time));
        } else if (type == 3) {
            emit("orbit_accuracy", {{"sisai_oe", int64_t(b.u(1287, 5))}});
            emit("eop", eop(b, 1292), true);
            bg(1430);
        } else if (type == 4) {
            emit("sisai", sisai(b, 1287));
            alm("midi_almanac", midi(b, 1309, time));
        } else
            ++counts["bds_reserved_pages"];
        return out;
    }
    if (b.u(0, 6) != uint64_t(satellite)) {
        ++counts["bds_prn_mismatch"];
        return out;
    }
    int type = int(b.u(b2 ? 6 : 12, 6));
    sow = int64_t(b.u(b2 ? 12 : 18, b2 ? 18 : 20)) * (b2 ? 3 : 1);
    if (sow >= 604800) {
        ++counts["bds_invalid_sow"];
        return out;
    }
    if (b2) {
        if (type == 10) {
            first = orbit1(b, 61);
            first["week_raw"] = int64_t(b.u(30, 13));
            first["iode"] = int64_t(b.u(53, 8));
            emit("orbit_part1", first);
        } else if (type == 11) {
            second = orbit2(b, 42);
            emit("orbit_part2", second);
        } else if (type >= 30 && type <= 34) {
            if (type == 34) {
                clocks(64, 133);
                emit("sisai", sisai(b, 42));
                emit("utc", utc(b, 143, time), true);
            } else if (type == 33) {
                clocks(42, 217);
                bg(111);
                alm("reduced_almanac",
                    reduced(b, 179, b.u(227, 13), b.u(240, 8) * 4096, time));
            } else {
                clocks(42, 111);
                if (type == 30) {
                    emit("group_delay", fields(b, 121,
                                               {{"tgd_b2ap_s", 12, -34, 'd'},
                                                {"isc_b2ad_s", 12, -34, 'd'}}));
                    emit("bdgim", iono(b, 145), true);
                    emit("b1c_group_delay",
                         fields(b, 219, {{"tgd_b1cp_s", 12, -34, 'd'}}));
                } else if (type == 31) {
                    for (int i = 0; i < 3; ++i)
                        alm("reduced_almanac",
                            reduced(b, 142 + i * 38, b.u(121, 13),
                                    b.u(134, 8) * 4096, time));
                } else
                    emit("eop", eop(b, 121), true);
            }
        } else if (type == 40) {
            emit("orbit_accuracy", {{"sisai_oe", int64_t(b.u(42, 5))}});
            emit("sisai", sisai(b, 47));
            alm("midi_almanac", midi(b, 69, time));
        } else {
            ++counts["bds_reserved_messages"];
            return out;
        }
        Row status;
        size_t p = type == 10 ? 43 : 32;
        status = fields(b, p,
                        {{"dif_b2a", 1},
                         {"sif_b2a", 1},
                         {"aif_b2a", 1},
                         {"sismai", 4},
                         {"dif_b1c", 1},
                         {"sif_b1c", 1},
                         {"aif_b1c", 1}});
        // Type 10 has no health field. Keep it in a distinct typed output.
        status["message_type"] = int64_t(type);
        status["sow_bdt_s"] = seconds(sow);
        if (type != 10)
            status["health"] = int64_t(b.u(30, 2));
        emit(type == 10 ? "integrity" : "status", status);
    } else {
        if (type == 10) {
            first = orbit1(b, 42);
            merge(first, orbit2(b, 245));
            emit("orbit", first);
            emit("integrity",
                 fields(b, 467,
                        {{"dif", 1}, {"sif", 1}, {"aif", 1}, {"sismai", 4}}));
        } else if (type == 30) {
            clk = clock(b, 55);
            clk["week_raw"] = int64_t(b.u(38, 13));
            clk["tgd_b2bi_s"] = b.offset(124, 12, -34);
            emit("clock", clk);
            emit("bdgim", iono(b, 136), true);
            emit("utc", utc(b, 210, time), true);
            emit("eop", eop(b, 307), true);
            auto r = sisai(b, 445);
            r["sisai_oe"] = int64_t(b.u(467, 5));
            r["health"] = int64_t(b.u(472, 2));
            emit("status", r);
        } else if (type == 40) {
            bg(38);
            alm("midi_almanac", midi(b, 106, time));
            for (int i = 0; i < 5; ++i)
                alm("reduced_almanac", reduced(b, 283 + i * 38, b.u(262, 13),
                                               b.u(275, 8) * 4096, time));
        } else {
            ++counts["bds_reserved_messages"];
            return out;
        }
    }
    if (!time)
        return out;
    if (!state_->pending.contains(source) && state_->pending.size() >= 2048)
        throw std::runtime_error("BeiDou assembly capacity exceeded");
    auto &parts = state_->pending[source];
    std::erase_if(parts, [&](const auto &x) {
        return *time < x.second.received ||
               *time - x.second.received > seconds(90);
    });
    if (!first.empty()) {
        // A fresh first orbit fragment starts a new cycle, never a mixed cache.
        if (!parts.contains(1) || parts[1].row != first ||
            parts[1].sow != sow) {
            parts.clear();
            parts[1] = {first, *time, sow};
        }
    }
    if (!second.empty() &&
        (!parts.contains(2) || parts[2].row != second || parts[2].sow != sow))
        parts[2] = {second, *time, sow};
    if (!clk.empty() &&
        (!parts.contains(3) || parts[3].row != clk || parts[3].sow != sow))
        parts[3] = {clk, *time, sow};
    if (!parts.contains(1) || !parts.contains(3) || (b2 && !parts.contains(2)))
        return out;
    if (b2 && ((parts[2].sow - parts[1].sow + 604800) % 604800 != 3 ||
               (integer(parts[3].row, "iodc") & 255) !=
                   integer(parts[1].row, "iode"))) {
        ++counts["bds_issue_mismatch"];
        return out;
    }
    Row r = parts[1].row;
    if (b2)
        merge(r, parts[2].row);
    merge(r, parts[3].row);
    if (r.at("toe_bdt_s") != r.at("toc_bdt_s")) {
        ++counts["bds_reference_mismatch"];
        return out;
    }
    auto epoch = bdt(time, integer(r, "week_raw"), parts[b2 ? 1 : 3].sow);
    std::optional<Tick> context;
    if (auto t = std::get_if<Tick>(&epoch))
        context = *t;
    r["toe_gpst"] = bdt_nearest(context, std::get<Tick>(r.at("toe_bdt_s")));
    r["toc_gpst"] = bdt_nearest(context, std::get<Tick>(r.at("toc_bdt_s")));
    emit("ephemeris", std::move(r), true);
    parts.clear();
    return out;
}
bool beidou_snapshot(Row &r, const std::string &kind, Tick t) {
    if (kind.starts_with("bds_pppb2b_") && kind != "bds_pppb2b_mask") {
        auto from = std::get_if<Tick>(&r.at("epoch_gpst")),
             to = std::get_if<Tick>(&r.at("valid_until_gpst"));
        if (!from || !to || t < *from || t >= *to)
            return false;
        r["applicability"] = std::string("TIME_WINDOW_IOD_MATCH_REQUIRED");
    }
    if (kind.ends_with("_bgto") && r.contains("available") &&
        !std::get<bool>(r.at("available")))
        return false;
    if (kind.ends_with("_ephemeris")) {
        for (auto n : {"toe_gpst", "toc_gpst"}) {
            auto v = std::get_if<Tick>(&r.at(n));
            if (!v || t < *v - seconds(3600) || t > *v + seconds(3600))
                return false;
        }
        r["applicability"] = std::string("AGE_WINDOW");
    }
    return true;
}
bool beidou_expired(const Row &r, const std::string &kind, Tick t) {
    if (kind.starts_with("bds_pppb2b_") && kind != "bds_pppb2b_mask") {
        auto v = std::get_if<Tick>(&r.at("valid_until_gpst"));
        return !v || t >= *v;
    }
    if (!kind.ends_with("_ephemeris"))
        return false;
    auto v = std::get_if<Tick>(&r.at("toe_gpst"));
    return !v || t > *v + seconds(3600);
}
} // namespace neognss_obs::broadcast_detail
