// SPDX-License-Identifier: GPL-3.0-only
#include "broadcast_galileo.hpp"
#include "broadcast_bits.hpp"
#include "broadcast_has.hpp"
#include <algorithm>
#include <numbers>
#include <set>

namespace neognss_obs::broadcast_detail {
namespace {
constexpr double pi = std::numbers::pi;
struct Field {
    const char *name;
    int width;
    int exp = 0;
    char type = 'u';
    double factor = 1;
};
Row fields(const BroadcastBits &b, int p,
           std::initializer_list<Field> definition) {
    Row r;
    for (const auto &d : definition) {
        if (d.type == 'u')
            r[d.name] = int64_t(b.u(p, d.width));
        else if (d.type == 't')
            r[d.name] = seconds(int64_t(b.u(p, d.width)) * int64_t(d.factor));
        else if (d.type == 'd')
            r[d.name] = b.offset(p, d.width, d.exp);
        else
            r[d.name] = b.f(p, d.width, d.exp, d.type != 'U', d.factor);
        p += d.width;
    }
    return r;
}
void merge(Row &r, Row x) { r.insert(x.begin(), x.end()); }
Row iono(const BroadcastBits &b, int p) {
    return fields(b, p,
                  {{"ai0_sfu", 11, -2, 'U'},
                   {"ai1_sfu_degree", 11, -8, 'f'},
                   {"ai2_sfu_degree2", 14, -15, 'f'},
                   {"storm_flags", 5}});
}
Row utc(const BroadcastBits &b, int p, std::optional<Tick> t) {
    auto r = fields(b, p,
                    {{"a0_s", 32, -30, 'd'},
                     {"a1_s_s", 24, -50, 'f'},
                     {"delta_tls_s", 8, 0, 'u'},
                     {"tot_s", 8, 0, 't', 3600},
                     {"week_raw", 8},
                     {"wn_lsf_raw", 8},
                     {"dn", 3},
                     {"delta_tlsf_s", 8}});
    r["delta_tls_s"] = b.s(p + 56, 8);
    r["delta_tlsf_s"] = b.s(p + 91, 8);
    r["reference_gpst"] =
        gst_reference(t, std::get<int64_t>(r.at("week_raw")),
                      int64_t(std::get<Tick>(r.at("tot_s")) / ps), 256);
    return r;
}
Row ggto(const BroadcastBits &b, int p, bool fnav, std::optional<Tick> t) {
    auto r = fnav ? fields(b, p,
                           {{"t0g_s", 8, 0, 't', 3600},
                            {"a0g_s", 16, -35, 'd'},
                            {"a1g_s_s", 12, -51, 'f'},
                            {"week_raw", 6}})
                  : fields(b, p,
                           {{"a0g_s", 16, -35, 'd'},
                            {"a1g_s_s", 12, -51, 'f'},
                            {"t0g_s", 8, 0, 't', 3600},
                            {"week_raw", 6}});
    bool unavailable = b.u(p, 42) == ((uint64_t(1) << 42) - 1);
    r["available"] = !unavailable;
    r["reference_gpst"] =
        unavailable
            ? Value{}
            : gst_reference(t, std::get<int64_t>(r.at("week_raw")),
                            int64_t(std::get<Tick>(r.at("t0g_s")) / ps), 64);
    return r;
}
void prefix(Row &to, const Row &from, const std::string &p) {
    for (const auto &[k, v] : from)
        to[p + k] = v;
}
Row orbit_almanac(const BroadcastBits &b, int p) {
    return fields(b, p,
                  {{"subject_sv_id", 6},
                   {"delta_sqrt_a", 13, -9, 'f'},
                   {"eccentricity", 11, -16, 'U'},
                   {"omega_rad", 16, -15, 'f', pi},
                   {"delta_i_rad", 11, -14, 'f', pi},
                   {"omega0_rad", 16, -15, 'f', pi},
                   {"omega_dot_rad_s", 11, -33, 'f', pi},
                   {"m0_rad", 16, -15, 'f', pi}});
}
Row tail_almanac(const BroadcastBits &b, int p, bool f) {
    auto r = fields(b, p, {{"af0_s", 16, -19, 'd'}, {"af1_s_s", 13, -38, 'f'}});
    r[f ? "e5a_health" : "e5b_health"] = int64_t(b.u(p + 29, 2));
    if (!f)
        r["e1b_health"] = int64_t(b.u(p + 31, 2));
    return r;
}
Row strip(const Row &r, std::string p) {
    Row out;
    for (const auto &[k, v] : r)
        if (k.starts_with(p))
            out[k.substr(p.size())] = v;
    return out;
}
bool in_window(const Row &r, const char *n, Tick t, int64_t before,
               int64_t after) {
    auto v = std::get_if<Tick>(&r.at(n));
    return v && t >= *v - seconds(before) && t <= *v + seconds(after);
}
} // namespace
struct GalileoDecoder::State {
    struct Assembly {
        std::map<int, Row> parts;
        std::map<int, Tick> times;
        std::optional<Tick> start;
        std::set<int> emitted;
    };
    struct Sar {
        Bytes bits;
        bool long_message = false;
        std::optional<Tick> first, last;
    };
    std::map<std::string, Assembly> ephemerides, almanacs;
    std::map<std::string, Sar> sar;
    HasDecoder has;
};
GalileoDecoder::GalileoDecoder() : state_(std::make_unique<State>()) {}
GalileoDecoder::~GalileoDecoder() = default;
void GalileoDecoder::clear() {
    state_->ephemerides.clear();
    state_->almanacs.clear();
    state_->sar.clear();
    state_->has.clear();
}

std::vector<DecodedMessage>
GalileoDecoder::decode(std::span<const uint8_t> data, const std::string &family,
                       const std::string &source, int64_t satellite, bool e1,
                       std::optional<Tick> time,
                       std::map<std::string, uint64_t> &counts) {
    if (family == "GAL_CNAV")
        return state_->has.decode(data, source, time, counts);
    bool f = family == "GAL_FNAV";
    if (satellite < 1 || satellite > 36) {
        ++counts["gal_invalid_satellite"];
        return {};
    }
    BroadcastBits raw(data, f ? 238 : 228);
    Bytes unpacked;
    std::vector<DecodedMessage> out;
    auto add = [&](std::string kind, Row r, bool candidate = false,
                   std::string id = "") {
        out.push_back(
            {std::move(kind), std::move(r), candidate, std::move(id)});
    };
    if (!f) {
        if (raw.u(0, 1) || !raw.u(114, 1) || raw.u(1, 1) || raw.u(115, 1)) {
            ++counts["gal_non_nominal_page"];
            return out;
        }
        unpacked = raw.bytes(2, 112);
        auto odd = raw.bytes(116, 16);
        unpacked.insert(unpacked.end(), odd.begin(), odd.end());
    } else
        unpacked.assign(data.begin(), data.end());
    BroadcastBits b(unpacked, f ? 214 : 128);
    int type = int(b.u(0, 6));
    if (!f && type != 63) {
        // The E5b reserved field is not OSNMA/SAR. Mixed contributors do not
        // identify which signal supplied the odd half, so expose uninterpreted
        // bits.
        if (e1) {
            add("gal_osnma_fragment", {{"payload", raw.bytes(132, 40)}});
            bool start = raw.u(172, 1), long_message = raw.u(173, 1);
            add("gal_sar_fragment", {{"start", start},
                                     {"long_message", long_message},
                                     {"data", int64_t(raw.u(174, 20))}});
            if (time) {
                if (!state_->sar.contains(source) && state_->sar.size() >= 128)
                    throw std::runtime_error(
                        "Galileo SAR source capacity exceeded");
                auto &s = state_->sar[source];
                if (start) {
                    s = {};
                    s.first = time;
                    s.long_message = long_message;
                } else if (!s.first || s.long_message != long_message ||
                           !s.last || *time - *s.last > seconds(3)) {
                    s = {};
                }
                if (s.first) {
                    for (int i = 0; i < 20; ++i)
                        s.bits.push_back(uint8_t(raw.u(174 + i, 1)));
                    s.last = time;
                    if (s.bits.size() == size_t(long_message ? 160 : 80)) {
                        Bytes p(s.bits.size() / 8);
                        for (size_t i = 0; i < s.bits.size(); ++i)
                            p[i / 8] |= s.bits[i] << (7 - i % 8);
                        BroadcastBits r(p, s.bits.size());
                        add("gal_sar_rlm",
                            {{"long_message", long_message},
                             {"beacon_id", int64_t(r.u(0, 60))},
                             {"message_code", int64_t(r.u(60, 4))},
                             {"parameters", r.bytes(64, s.bits.size() - 64)},
                             {"parameter_bit_length",
                              int64_t(s.bits.size() - 64)},
                             {"first_received_gpst", *s.first}});
                        s = {};
                    }
                }
            }
        } else
            add("gal_inav_auxiliary_bits", {{"payload", raw.bytes(132, 64)}});
    }
    Row r;
    std::string name =
        (f ? "gal_fnav_page_" : "gal_inav_word_") + std::to_string(type);
    if (!f) {
        switch (type) {
        case 1:
            r = fields(b, 6,
                       {{"iod_nav", 10},
                        {"toe_s", 14, 0, 't', 60},
                        {"m0_rad", 32, -31, 'f', pi},
                        {"eccentricity", 32, -33, 'U'},
                        {"sqrt_a", 32, -19, 'U'}});
            break;
        case 2:
            r = fields(b, 6,
                       {{"iod_nav", 10},
                        {"omega0_rad", 32, -31, 'f', pi},
                        {"i0_rad", 32, -31, 'f', pi},
                        {"omega_rad", 32, -31, 'f', pi},
                        {"idot_rad_s", 14, -43, 'f', pi}});
            break;
        case 3:
            r = fields(b, 6,
                       {{"iod_nav", 10},
                        {"omega_dot_rad_s", 24, -43, 'f', pi},
                        {"delta_n_rad_s", 16, -43, 'f', pi},
                        {"cuc_rad", 16, -29, 'f'},
                        {"cus_rad", 16, -29, 'f'},
                        {"crc_m", 16, -5, 'f'},
                        {"crs_m", 16, -5, 'f'},
                        {"sisa_index", 8}});
            break;
        case 4:
            r = fields(b, 6,
                       {{"iod_nav", 10},
                        {"svid", 6},
                        {"cic_rad", 16, -29, 'f'},
                        {"cis_rad", 16, -29, 'f'},
                        {"toc_s", 14, 0, 't', 60},
                        {"af0_s", 31, -34, 'd'},
                        {"af1_s_s", 21, -46, 'f'},
                        {"af2_s_s2", 6, -59, 'f'}});
            break;
        case 5:
            r = iono(b, 6);
            merge(r, fields(b, 47,
                            {{"bgd_e1_e5a_s", 10, -32, 'd'},
                             {"bgd_e1_e5b_s", 10, -32, 'd'},
                             {"e5b_health", 2},
                             {"e1b_health", 2},
                             {"e5b_data_invalid", 1},
                             {"e1b_data_invalid", 1},
                             {"week_raw", 12},
                             {"tow_s", 20, 0, 't', 1}}));
            add("gal_ionosphere", iono(b, 6), true);
            break;
        case 6:
            r = utc(b, 6, time);
            r["tow_s"] = seconds(int64_t(b.u(105, 20)));
            add("gal_utc", utc(b, 6, time), true);
            break;
        case 7:
            r = fields(
                b, 6,
                {{"iod_a", 4}, {"week_raw", 2}, {"toa_s", 10, 0, 't', 600}});
            prefix(r, orbit_almanac(b, 22), "s1_");
            break;
        case 8:
            r["iod_a"] = int64_t(b.u(6, 4));
            prefix(r, tail_almanac(b, 10, false), "s1_");
            prefix(r,
                   fields(b, 43,
                          {{"subject_sv_id", 6},
                           {"delta_sqrt_a", 13, -9, 'f'},
                           {"eccentricity", 11, -16, 'U'},
                           {"omega_rad", 16, -15, 'f', pi},
                           {"delta_i_rad", 11, -14, 'f', pi},
                           {"omega0_rad", 16, -15, 'f', pi},
                           {"omega_dot_rad_s", 11, -33, 'f', pi}}),
                   "s2_");
            break;
        case 9:
            r = fields(
                b, 6,
                {{"iod_a", 4}, {"week_raw", 2}, {"toa_s", 10, 0, 't', 600}});
            r["s2_m0_rad"] = b.f(22, 16, -15, true, pi);
            prefix(r, tail_almanac(b, 38, false), "s2_");
            prefix(r,
                   fields(b, 71,
                          {{"subject_sv_id", 6},
                           {"delta_sqrt_a", 13, -9, 'f'},
                           {"eccentricity", 11, -16, 'U'},
                           {"omega_rad", 16, -15, 'f', pi},
                           {"delta_i_rad", 11, -14, 'f', pi}}),
                   "s3_");
            break;
        case 10:
            r["iod_a"] = int64_t(b.u(6, 4));
            prefix(r,
                   fields(b, 10,
                          {{"omega0_rad", 16, -15, 'f', pi},
                           {"omega_dot_rad_s", 11, -33, 'f', pi},
                           {"m0_rad", 16, -15, 'f', pi}}),
                   "s3_");
            prefix(r, tail_almanac(b, 53, false), "s3_");
            // GGTO's six-bit week is not the two-bit almanac reference week.
            add("gal_ggto", ggto(b, 86, false, time), true);
            break;
        case 16:
            r = fields(b, 6,
                       {{"delta_a_m", 5, 8, 'f'},
                        {"ex", 13, -22, 'f'},
                        {"ey", 13, -22, 'f'},
                        {"delta_i_rad", 17, -22, 'f', pi},
                        {"omega0_rad", 23, -22, 'f', pi},
                        {"lambda0_rad", 23, -22, 'f', pi},
                        {"af0_s", 22, -26, 'd'},
                        {"af1_s_s", 6, -35, 'f'}});
            name = "gal_reduced_ced";
            break;
        case 17:
        case 18:
        case 19:
        case 20: {
            r["iod_nav_lsb"] = int64_t(b.u(14, 2));
            Bytes p = b.bytes(6, 8);
            auto rest = b.bytes(16, 112);
            p.insert(p.end(), rest.begin(), rest.end());
            r["parity_octets"] = std::move(p);
            break;
        }
        case 0:
            r = fields(b, 96, {{"week_raw", 12}, {"tow_s", 20, 0, 't', 1}});
            r["time_valid"] = b.u(6, 2) == 2;
            break;
        case 22: {
            uint32_t crc = 0;
            constexpr uint32_t poly = 0x814141ab;
            for (int i = 0; i < 96; ++i) {
                bool x = ((crc >> 31) ^ b.u(i, 1)) & 1;
                crc <<= 1;
                if (x)
                    crc ^= poly;
            }
            r = {{"constellation_id", int64_t(b.u(6, 3))},
                 {"payload", b.bytes(9, 87)},
                 {"crc_valid", uint64_t(crc) == b.u(96, 32)}};
            if (b.u(6, 3) == 1 && b.u(9, 3) == 2) {
                auto sl = fields(b, 12,
                                 {{"week_raw", 12},
                                  {"t0_s", 9, 0, 't', 1800},
                                  {"mask_msb", 1},
                                  {"satellite_mask", 32},
                                  {"pconst_index", 4},
                                  {"psat_index", 4},
                                  {"ura_index", 4},
                                  {"ure_index", 4},
                                  {"bnom_index", 4},
                                  {"validity_index", 4}});
                sl["crc_valid"] = uint64_t(crc) == b.u(96, 32);
                sl["reference_gpst"] = gst_reference(
                    time, int64_t(b.u(12, 12)), int64_t(b.u(24, 9)) * 1800);
                const double pconst[] = {
                    1e-8, 1e-7, 1e-6, 3e-6, 6e-6,    8e-6,   1e-5,    2e-5,
                    4e-5, 6e-5, 8e-5, 1e-4, 1.25e-4, 1.5e-4, 1.75e-4, 2e-4};
                const double psat[] = {
                    1e-7, 3e-7,   6e-7,   1e-6,   2e-6, 3e-6,   5e-6,   7e-6,
                    1e-5, 1.2e-5, 1.4e-5, 1.7e-5, 2e-5, 2.4e-5, 2.8e-5, 3e-5};
                const double ura[] = {.75,  1,   1.5,  2, 2.25, 2.5, 2.75, 3,
                                      3.25, 3.5, 3.75, 4, 4.5,  5,   5.5,  6};
                const double bias[] = {0,   .1, .2,  .3,  .4,  .5,  .6, .75,
                                       .85, 1,  1.2, 1.4, 1.6, 1.8, 2,  2.4};
                const int hours[] = {1,  2,  3,  4,  6,   8,   12,  18,
                                     24, 36, 48, 72, 120, 168, 720, 1440};
                auto code = [&](const char *n) {
                    return std::get<int64_t>(sl.at(n));
                };
                sl["pconst"] = pconst[code("pconst_index")];
                sl["psat"] = psat[code("psat_index")];
                sl["ura_m"] = ura[code("ura_index")];
                sl["ure_m"] = (code("ure_index") + 1) * .25;
                sl["bnom_m"] = bias[code("bnom_index")];
                sl["validity_s"] =
                    seconds(hours[code("validity_index")] * 3600);
                add("gal_ism_sl3", std::move(sl), uint64_t(crc) == b.u(96, 32));
            }
            break;
        }
        case 63:
            add("gal_inav_dummy", {});
            ++counts["gal_dummy"];
            return out;
        default:
            ++counts["gal_unsupported_words"];
            return out;
        }
    } else {
        switch (type) {
        case 1:
            r = fields(b, 6,
                       {{"svid", 6},
                        {"iod_nav", 10},
                        {"toc_s", 14, 0, 't', 60},
                        {"af0_s", 31, -34, 'd'},
                        {"af1_s_s", 21, -46, 'f'},
                        {"af2_s_s2", 6, -59, 'f'},
                        {"sisa_index", 8}});
            merge(r, iono(b, 102));
            merge(r, fields(b, 143,
                            {{"bgd_e1_e5a_s", 10, -32, 'd'},
                             {"e5a_health", 2},
                             {"week_raw", 12},
                             {"tow_s", 20, 0, 't', 1},
                             {"e5a_data_invalid", 1}}));
            add("gal_ionosphere", iono(b, 102), true);
            break;
        case 2:
            r = fields(b, 6,
                       {{"iod_nav", 10},
                        {"m0_rad", 32, -31, 'f', pi},
                        {"omega_dot_rad_s", 24, -43, 'f', pi},
                        {"eccentricity", 32, -33, 'U'},
                        {"sqrt_a", 32, -19, 'U'},
                        {"omega0_rad", 32, -31, 'f', pi},
                        {"idot_rad_s", 14, -43, 'f', pi},
                        {"week_raw", 12},
                        {"tow_s", 20, 0, 't', 1}});
            break;
        case 3:
            r = fields(b, 6,
                       {{"iod_nav", 10},
                        {"i0_rad", 32, -31, 'f', pi},
                        {"omega_rad", 32, -31, 'f', pi},
                        {"delta_n_rad_s", 16, -43, 'f', pi},
                        {"cuc_rad", 16, -29, 'f'},
                        {"cus_rad", 16, -29, 'f'},
                        {"crc_m", 16, -5, 'f'},
                        {"crs_m", 16, -5, 'f'},
                        {"toe_s", 14, 0, 't', 60},
                        {"week_raw", 12},
                        {"tow_s", 20, 0, 't', 1}});
            break;
        case 4:
            r = fields(b, 6,
                       {{"iod_nav", 10},
                        {"cic_rad", 16, -29, 'f'},
                        {"cis_rad", 16, -29, 'f'}});
            add("gal_utc", utc(b, 48, time), true);
            add("gal_ggto", ggto(b, 147, true, time), true);
            merge(r, utc(b, 48, time));
            r["tow_s"] = seconds(int64_t(b.u(189, 20)));
            break;
        case 5:
            r = fields(
                b, 6,
                {{"iod_a", 4}, {"week_raw", 2}, {"toa_s", 10, 0, 't', 600}});
            prefix(r, orbit_almanac(b, 22), "s1_");
            prefix(r, tail_almanac(b, 122, true), "s1_");
            prefix(r,
                   fields(b, 153,
                          {{"subject_sv_id", 6},
                           {"delta_sqrt_a", 13, -9, 'f'},
                           {"eccentricity", 11, -16, 'U'},
                           {"omega_rad", 16, -15, 'f', pi},
                           {"delta_i_rad", 11, -14, 'f', pi},
                           {"omega0_msb", 4}}),
                   "s2_");
            break;
        case 6:
            r["iod_a"] = int64_t(b.u(6, 4));
            prefix(r,
                   fields(b, 10,
                          {{"omega0_lsb", 12},
                           {"omega_dot_rad_s", 11, -33, 'f', pi},
                           {"m0_rad", 16, -15, 'f', pi}}),
                   "s2_");
            prefix(r, tail_almanac(b, 49, true), "s2_");
            prefix(r, orbit_almanac(b, 80), "s3_");
            prefix(r, tail_almanac(b, 180, true), "s3_");
            break;
        case 63:
            add("gal_fnav_dummy", {});
            ++counts["gal_dummy"];
            return out;
        default:
            ++counts["gal_unsupported_words"];
            return out;
        }
    }
    if (r.contains("svid") && std::get<int64_t>(r.at("svid")) != satellite) {
        ++counts["gal_svid_mismatch"];
        return {};
    }
    Row occurrence = r;
    occurrence["message_type"] = int64_t(type);
    add(name, std::move(occurrence));
    ++counts["decoded_messages"];
    ++counts["gal_decoded"];
    if (!time)
        return out;
    if (type >= 1 && type <= (f ? 4 : 5)) {
        if (!state_->ephemerides.contains(source) &&
            state_->ephemerides.size() + state_->almanacs.size() >= 2048)
            throw std::runtime_error("Galileo assembly capacity exceeded");
        auto &a = state_->ephemerides[source];
        int timeout = f ? 150 : 90;
        if (a.start && *time - *a.start > seconds(timeout)) {
            a = {};
            ++counts["gal_assembly_timeout"];
        }
        Row part = r;
        // UTC from F/NAV page 4 is not an ephemeris issue or clock reference.
        if (f && type == 4)
            part = fields(b, 6,
                          {{"iod_nav", 10},
                           {"cic_rad", 16, -29, 'f'},
                           {"cis_rad", 16, -29, 'f'}});
        auto content = [](Row v) {
            v.erase("tow_s");
            return v;
        };
        for (const auto &[id, old] : a.parts) {
            bool conflict = part.contains("iod_nav") &&
                            old.contains("iod_nav") &&
                            part.at("iod_nav") != old.at("iod_nav");
            if (id == type && content(old) != content(part))
                conflict = true;
            if (conflict) {
                a = {};
                ++counts["gal_assembly_changed"];
                break;
            }
        }
        if (!a.start)
            a.start = time;
        a.parts[type] = part;
        if (a.parts.size() == size_t(f ? 4 : 5)) {
            Row eph;
            for (const auto &[id, p] : a.parts)
                for (const auto &[k, v] : p)
                    if (k != "tow_s" && k != "week_raw")
                        eph.insert({k, v});
            const auto &clock = a.parts.at(f ? 1 : 5);
            auto wn = std::get<int64_t>(clock.at("week_raw"));
            auto tow = std::get<Tick>(clock.at("tow_s"));
            auto tx = gst_reference(time, wn, int64_t(tow / ps));
            bool valid = std::holds_alternative<Tick>(tx);
            if (f && valid)
                for (int i : {2, 3}) {
                    const auto &p = a.parts.at(i);
                    auto x = gst_reference(
                        time, std::get<int64_t>(p.at("week_raw")),
                        int64_t(std::get<Tick>(p.at("tow_s")) / ps));
                    if (!std::holds_alternative<Tick>(x) ||
                        abs(std::get<Tick>(x) - std::get<Tick>(tx)) >
                            seconds(timeout))
                        valid = false;
                }
            if (valid) {
                eph["toe_gpst"] = nearest_week(std::get<Tick>(tx),
                                               std::get<Tick>(eph.at("toe_s")));
                eph["toc_gpst"] = nearest_week(std::get<Tick>(tx),
                                               std::get<Tick>(eph.at("toc_s")));
                eph["transmission_gpst"] = tx;
                eph["week_raw"] = wn;
                eph["first_received_gpst"] = *a.start;
                valid = std::holds_alternative<Tick>(eph.at("toe_gpst")) &&
                        std::holds_alternative<Tick>(eph.at("toc_gpst")) &&
                        std::get<double>(eph.at("sqrt_a")) > 0 &&
                        std::get<double>(eph.at("eccentricity")) < 1;
                if (valid) {
                    add(f ? "gal_fnav_ephemeris" : "gal_inav_ephemeris",
                        std::move(eph), true);
                    ++counts["gal_ephemerides"];
                }
            }
            if (!valid)
                ++counts["gal_invalid_ephemeris"];
            a = {};
        }
    }
    if ((!f && type >= 7 && type <= 10) || (f && (type == 5 || type == 6))) {
        if (!state_->almanacs.contains(source) &&
            state_->ephemerides.size() + state_->almanacs.size() >= 2048)
            throw std::runtime_error("Galileo assembly capacity exceeded");
        auto &a = state_->almanacs[source];
        if ((a.start && *time - *a.start > seconds(f ? 300 : 180)) ||
            type == (f ? 5 : 7))
            a = {};
        if (!a.parts.empty() &&
            a.parts.begin()->second.at("iod_a") != r.at("iod_a"))
            a = {};
        for (const auto &[id, old] : a.parts) {
            bool conflict = id == type && old != r;
            for (const auto *field : {"week_raw", "toa_s"})
                if (old.contains(field) && r.contains(field) &&
                    old.at(field) != r.at(field))
                    conflict = true;
            if (conflict) {
                a = {};
                ++counts["gal_almanac_conflict"];
                break;
            }
        }
        if (!a.start)
            a.start = time;
        a.parts[type] = r;
        a.times[type] = *time;
        for (int slot = 1; slot <= 3; ++slot) {
            int first = f ? 5 : slot + 6,
                last = f ? (slot == 1 ? 5 : 6) : slot + 7,
                ref = f ? 5 : (slot == 1 ? 7 : 9);
            if (a.emitted.contains(slot) || !a.parts.contains(first) ||
                !a.parts.contains(last) || !a.parts.contains(ref))
                continue;
            // IODa labels the whole almanac batch, not a three-subject group.
            // A lost group must not splice an older subject's head to a new
            // tail.
            auto gap = a.times.at(last) - a.times.at(first);
            if (gap < 0 || gap >= seconds(f ? 100 : slot == 2 ? 60 : 30))
                continue;
            auto x = strip(a.parts.at(first), "s" + std::to_string(slot) + "_");
            merge(x, strip(a.parts.at(last), "s" + std::to_string(slot) + "_"));
            if (!x.contains("subject_sv_id"))
                continue;
            auto id = std::get<int64_t>(x.at("subject_sv_id"));
            a.emitted.insert(slot);
            if (!id)
                continue;
            if (id > 36) {
                ++counts["gal_invalid_almanac"];
                continue;
            }
            if (f && slot == 2) {
                int64_t v = (std::get<int64_t>(x.at("omega0_msb")) << 12) |
                            std::get<int64_t>(x.at("omega0_lsb"));
                if (v & 32768)
                    v -= 65536;
                x.erase("omega0_msb");
                x.erase("omega0_lsb");
                x["omega0_rad"] = std::ldexp(double(v), -15) * pi;
            }
            const auto &rt = a.parts.at(ref);
            x["iod_a"] = rt.at("iod_a");
            x["week_raw"] = rt.at("week_raw");
            x["toa_s"] = rt.at("toa_s");
            x["reference_gpst"] =
                gst_reference(time, std::get<int64_t>(rt.at("week_raw")),
                              int64_t(std::get<Tick>(rt.at("toa_s")) / ps), 4);
            x["sqrt_a"] =
                std::get<double>(x.at("delta_sqrt_a")) + std::sqrt(29600000.0);
            x["i0_rad"] =
                std::get<double>(x.at("delta_i_rad")) + 56.0 * pi / 180;
            add(f ? "gal_fnav_almanac" : "gal_inav_almanac", std::move(x), true,
                std::to_string(id));
        }
    }
    return out;
}
bool galileo_snapshot(Row &r, const std::string &kind, Tick t) {
    if (kind == "gal_ism_sl3") {
        auto start = std::get_if<Tick>(&r.at("reference_gpst"));
        if (!start || t < *start ||
            t >= *start + std::get<Tick>(r.at("validity_s")))
            return false;
        r["applicability"] = std::string("TIME_WINDOW");
    }
    if (kind.starts_with("gal_has_")) {
        auto end = std::get_if<Tick>(&r.at("valid_until_gpst"));
        return end && std::get<Tick>(r.at("reference_gpst")) <= t && t < *end
                   ? (r["applicability"] = std::string("TIME_WINDOW"), true)
                   : false;
    }
    if (kind == "gal_inav_ephemeris" || kind == "gal_fnav_ephemeris") {
        // Backend selection remains separate. This is an explicit 4 h age
        // window, not an assertion that SISA/health/data validity permit a
        // given signal.
        if (!in_window(r, "toe_gpst", t, 0, 14400) ||
            !in_window(r, "toc_gpst", t, 0, 14400))
            return false;
        r["applicability"] = std::string("AGE_WINDOW");
    }
    if (kind == "gal_ggto" && !std::get<bool>(r.at("available")))
        return false;
    return true;
}
bool galileo_expired(const Row &r, const std::string &kind, Tick t) {
    if (kind == "gal_ism_sl3") {
        auto start = std::get_if<Tick>(&r.at("reference_gpst"));
        return start && t >= *start + std::get<Tick>(r.at("validity_s"));
    }
    if (kind.starts_with("gal_has_")) {
        auto end = std::get_if<Tick>(&r.at("valid_until_gpst"));
        return end && t >= *end;
    }
    if (kind == "gal_inav_ephemeris" || kind == "gal_fnav_ephemeris") {
        auto ref = std::get_if<Tick>(&r.at("toe_gpst"));
        return ref && t > *ref + seconds(14400);
    }
    return false;
}
} // namespace neognss_obs::broadcast_detail
