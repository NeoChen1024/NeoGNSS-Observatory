// SPDX-License-Identifier: GPL-3.0-only
#include "broadcast_cnav.hpp"
#include "broadcast_bits.hpp"
#include <cmath>
#include <numbers>

namespace neognss_obs::broadcast_detail {
namespace {
constexpr double pi = std::numbers::pi;
// One 300-bit CNAV message. Unsigned fields are int64 Row values.
struct CnavBits : BroadcastBits {
    explicit CnavBits(std::span<const uint8_t> data)
        : BroadcastBits(data, 300) {}
    int64_t u(size_t p, size_t n) const {
        return int64_t(BroadcastBits::u(p, n));
    }
    Tick tow(size_t p, size_t n, int scale) const {
        return seconds(u(p, n) * scale);
    }
};
Value week_time(std::optional<Tick> now, int64_t week, Tick tow,
                int modulo = 8192) {
    if (!now || tow >= seconds(604800))
        return {};
    return reference(*now, week, modulo, int64_t(tow / ps));
}
Value near_time(std::optional<Tick> now, Tick tow) {
    if (!now || tow >= seconds(604800))
        return {};
    Tick t = (*now / seconds(604800)) * seconds(604800) + tow;
    if (t - *now > seconds(302400))
        t -= seconds(604800);
    if (*now - t > seconds(302400))
        t += seconds(604800);
    return t;
}
// IS-GPS-200N 30.3.3.1.3 and Table 30-VI; QPNT-006 section 5.4.2.
double a_reference(bool gps) { return gps ? 26559710.0 : 42164200.0; }
std::string orbit(bool gps, int64_t id) {
    if (gps)
        return "GPS";
    if (id >= 2 && id <= 5)
        return "QZO";
    if (id >= 7 && id <= 9)
        return "GEO_QGEO";
    return "UNKNOWN";
}
void health(Row &r, const CnavBits &b, int p) {
    r["l1_unhealthy"] = bool(b.u(p, 1));
    r["l2_unhealthy"] = bool(b.u(p + 1, 1));
    r["l5_unhealthy"] = bool(b.u(p + 2, 1));
}
bool gps_row(const Row &r) {
    return std::get<std::string>(r.at("satellite_system")) == "G";
}
bool window(const Row &r, const char *field, Tick time, int64_t half) {
    const auto *ref = std::get_if<Tick>(&r.at(field));
    return ref && time >= *ref - seconds(half) && time <= *ref + seconds(half);
}
// GPS toe/toc lie 1.5 hours into a three-hour fit (IS-GPS-200N Table
// 30-XIII); QPNT-006 Table 4.3.1-2 gives one hour for QZSS.
int64_t fit_half(const Row &r) { return gps_row(r) ? 5400 : 3600; }
} // namespace

std::vector<DecodedMessage>
CnavDecoder::decode(std::span<const uint8_t> data, const std::string &source,
                    bool gps, int64_t satellite, bool l2, bool l5,
                    std::optional<Tick> time,
                    std::map<std::string, uint64_t> &counts) {
    if (data.size() != 38 || (data[37] & 15))
        throw std::invalid_argument("Invalid CNAV size/padding");
    CnavBits b{data};
    std::vector<DecodedMessage> out;
    const int mt = int(b.u(14, 6));
    if (b.u(0, 8) != 0x8b || satellite < 1 || satellite > (gps ? 63 : 10) ||
        b.u(8, 6) != satellite || b.u(20, 17) >= 100800 || (!l2 && !l5)) {
        ++counts["cnav_invalid_header"];
        return out;
    }
    Row header{{"message_type", int64_t(mt)},
               {"prn_id_raw", b.u(8, 6)},
               {"tow_count", b.u(20, 17)},
               {"alert", bool(b.u(37, 1))}};
    auto add = [&](std::string kind, Row fields, bool candidate = false,
                   std::string discriminator = "") {
        fields.insert(header.begin(), header.end());
        out.push_back({std::move(kind), std::move(fields),
                       candidate && !bool(b.u(37, 1)),
                       std::move(discriminator)});
    };
    Row part;
    int part_id = 0;
    switch (mt) {
    case 0:
        add("cnav_test_mode", {});
        ++counts["cnav_test_mode"];
        break;
    case 10:
        part = {{"week_raw", b.u(38, 13)},
                {"top_s", b.tow(54, 11, 300)},
                {"ura_ed_index", b.s(65, 5)},
                {"toe_s", b.tow(70, 11, 300)},
                {"delta_a_m", b.f(81, 26, -9)},
                {"a_reference_m", a_reference(gps)},
                {"a_dot_m_s", b.f(107, 25, -21)},
                {"delta_n_rad_s", b.f(132, 17, -44, true, pi)},
                {"delta_n_dot_rad_s2", b.f(149, 23, -57, true, pi)},
                {"m0_rad", b.f(172, 33, -32, true, pi)},
                {"eccentricity", b.f(205, 33, -34, false)},
                {"omega_rad", b.f(238, 33, -32, true, pi)},
                {"integrity_status_flag", bool(b.u(271, 1))},
                // GPS: L2C phasing (IS-GPS-200N 30.3.3.1.1); QZSS: ephemeris
                // status (QPNT-006 5.4.1).
                {"l2c_phasing", gps ? Value(bool(b.u(272, 1))) : Value{}},
                {"ephemeris_status_flag",
                 gps ? Value{} : Value(bool(b.u(272, 1)))}};
        health(part, b, 51);
        add("cnav_ephemeris_1", part);
        part_id = 10;
        break;
    case 11:
        part = {{"toe_s", b.tow(38, 11, 300)},
                {"omega0_rad", b.f(49, 33, -32, true, pi)},
                {"i0_rad", b.f(82, 33, -32, true, pi)},
                {"delta_omega_dot_rad_s", b.f(115, 17, -44, true, pi)},
                {"omega_dot_reference_rad_s", -2.6e-9 * pi},
                {"idot_rad_s", b.f(132, 15, -44, true, pi)},
                {"cis_rad", b.f(147, 16, -30)},
                {"cic_rad", b.f(163, 16, -30)},
                {"crs_m", b.f(179, 24, -8)},
                {"crc_m", b.f(203, 24, -8)},
                {"cus_rad", b.f(227, 21, -30)},
                {"cuc_rad", b.f(248, 21, -30)}};
        add("cnav_ephemeris_2", part);
        part_id = 11;
        break;
    case 12:
    case 15:
    case 30:
    case 31:
    case 32:
    case 33:
    case 35:
    case 37:
        break;
    case 34:
    case 36:
        if (!gps) {
            ++counts["cnav_unsupported_types"];
            return out;
        }
        if (mt == 34)
            ++counts["cnav_differential_corrections_not_decoded"];
        break;
    case 60:
    case 61:
        if (gps) {
            ++counts["cnav_unsupported_types"];
            return out;
        }
        break;
    default:
        ++counts["cnav_unsupported_types"];
        return out;
    }
    const bool clock = (mt >= 30 && mt <= 37) || mt == 61;
    if (clock) {
        part = {
            {"top_s", b.tow(38, 11, 300)},  {"toc_s", b.tow(60, 11, 300)},
            {"ura_ned0_index", b.s(49, 5)}, {"ura_ned1_index", b.u(54, 3)},
            {"ura_ned2_index", b.u(57, 3)}, {"af0_s", b.offset(71, 26, -35)},
            {"af1_s_s", b.f(97, 20, -48)},  {"af2_s_s2", b.f(117, 10, -60)}};
        Row fields = part;
        fields["toc_gpst"] = near_time(time, std::get<Tick>(part.at("toc_s")));
        fields["top_gpst"] = near_time(time, std::get<Tick>(part.at("top_s")));
        add("cnav_clock", std::move(fields), true);
        part_id = 30;
    }
    if (mt == 30 || mt == 61) {
        // GPS TGD/ISC are relative to L1 P(Y) (IS-GPS-200N 30.3.3.3.1.1.1).
        Row delays{{"reference_signal",
                    std::string(gps ? "GPS_L1_PY" : "QZSS_L1_CA_OR_CB")}};
        const char *names[] = {"tgd_s", "isc_l1_ca_cb_s", "isc_l2c_s",
                               "isc_l5_i_s", "isc_l5_q_s"};
        for (int i = 0; i < 5; ++i)
            delays[names[i]] = b.offset(127 + 13 * i, 13, -35);
        add("cnav_group_delay", std::move(delays), true);
        Row iono{{"region", std::string(gps        ? "GLOBAL"
                                        : mt == 30 ? "WIDE_AREA"
                                                   : "JAPAN")}};
        const int exps[] = {-30, -27, -24, -24, 11, 14, 16, 16};
        for (int i = 0; i < 8; ++i)
            iono[std::string(i < 4 ? "alpha" : "beta") +
                 std::to_string(i % 4)] = b.f(192 + 8 * i, 8, exps[i]);
        add("cnav_ionosphere", std::move(iono), true,
            mt == 30 ? "wide" : "japan");
        if (mt == 30)
            add("cnav_prediction_week",
                {{"week_raw", b.u(256, 8)},
                 {"top_s", b.tow(38, 11, 300)},
                 {"top_gpst",
                  week_time(time, b.u(256, 8), b.tow(38, 11, 300), 256)}});
    }
    if (mt == 12 || mt == 31 || mt == 37) {
        const int p = mt == 12 ? 38 : 127;
        const auto week = b.u(p, 13);
        const auto toa = b.tow(p + 13, 8, 4096);
        const int start = p + 21;
        const int n = mt == 12 ? 7 : mt == 31 ? 4 : 1;
        for (int i = 0; i < n; ++i) {
            int pos = start + i * 31;
            auto id = b.u(pos, 6);
            if (id == 0) {
                ++counts["cnav_dummy_almanacs"];
                // GPS fills every later packet with alternating bits
                // (IS-GPS-200N 30.3.3.4.6.1).
                if (gps)
                    break;
                continue;
            }
            if (!gps && id > 10) {
                ++counts["cnav_invalid_almanacs"];
                continue;
            }
            Row r{{"subject_sv_id", id},
                  {"week_raw", week},
                  {"toa_s", toa},
                  {"reference_gpst", week_time(time, week, toa)},
                  {"orbit_reference", orbit(gps, id)}};
            if (mt != 37) {
                r["delta_a_m"] = b.f(pos + 6, 8, 9);
                r["a_reference_m"] = a_reference(gps);
                r["omega0_rad"] = b.f(pos + 14, 7, -6, true, pi);
                r["phi0_rad"] = b.f(pos + 21, 7, -6, true, pi);
                health(r, b, pos + 28);
                add("cnav_reduced_almanac", std::move(r), true,
                    std::to_string(id));
            } else {
                health(r, b, 154);
                // GPS transmits e directly and delta i relative to 0.30
                // semicircles (IS-GPS-200N Table 30-V).
                const auto ref = orbit(gps, id);
                const double e = b.f(157, 11, -16, false);
                const double di = b.f(168, 11, -14, true, pi);
                r["delta_eccentricity"] = gps ? Value{} : Value(e);
                r["delta_i_rad"] = di;
                r["eccentricity"] = ref == "UNKNOWN"
                                        ? Value{}
                                        : Value(e + (ref == "QZO" ? 0.06 : 0));
                r["i0_rad"] = ref == "UNKNOWN" ? Value{}
                              : ref == "GPS"
                                  ? Value(di + 0.30 * pi)
                                  : Value(di + (ref == "QZO" ? pi * 0.25 : 0));
                r["omega_dot_rad_s"] = b.f(179, 11, -33, true, pi);
                r["sqrt_a"] = b.f(190, 17, -4, false);
                r["omega0_rad"] = b.f(207, 16, -15, true, pi);
                r["omega_rad"] = b.f(223, 16, -15, true, pi);
                r["m0_rad"] = b.f(239, 16, -15, true, pi);
                r["af0_s"] = b.offset(255, 11, -20);
                r["af1_s_s"] = b.f(266, 10, -37);
                add("cnav_midi_almanac", std::move(r), true,
                    std::to_string(id));
            }
        }
    }
    if (mt == 32) {
        auto ref = b.tow(127, 16, 16);
        add("cnav_eop",
            {{"teop_s", ref},
             {"reference_gpst", Value{}},
             {"top_s", b.tow(38, 11, 300)},
             {"pm_x_arcsec", b.f(143, 21, -20)},
             {"pm_x_rate_arcsec_day", b.f(164, 15, -21)},
             {"pm_y_arcsec", b.f(179, 21, -20)},
             {"pm_y_rate_arcsec_day", b.f(200, 15, -21)},
             // IS-GPS-200N Table 30-VII labels this UT1-GPST with a 2^-23
             // LSB, but GPS broadcasts match IERS UT1-UTC at QZSS's 2^-24.
             {"ut1_minus_utc_s", b.offset(215, 31, -24)},
             {"ut1_minus_utc_rate_s_day", b.f(246, 19, -25)}},
            true);
    }
    if (mt == 33) {
        auto ref = b.tow(171, 16, 16);
        add("cnav_utc",
            {{"a0_s", b.offset(127, 16, -35)},
             {"a1_s_s", b.f(143, 13, -51)},
             {"a2_s_s2", b.f(156, 7, -68)},
             {"delta_tls_s", b.s(163, 8)},
             {"tot_s", ref},
             {"week_raw", b.u(187, 13)},
             {"reference_gpst", week_time(time, b.u(187, 13), ref)},
             {"top_s", b.tow(38, 11, 300)},
             {"wn_lsf_raw", b.u(200, 13)},
             {"dn", b.u(213, 4)},
             {"delta_tlsf_s", b.s(217, 8)},
             {"utc_reference", std::string(gps ? "UTC_USNO" : "UTC_NICT")}},
            true);
    }
    if (mt == 35) {
        auto ref = b.tow(127, 16, 16);
        auto id = b.u(156, 3);
        // IS-GPS-200N 30.3.3.8.1 defines only Galileo and GLONASS targets.
        const char *systems[] = {
            "UNAVAILABLE", "GST",      "GLOT",     gps ? "RESERVED" : "GPST",
            "RESERVED",    "RESERVED", "RESERVED", "RESERVED"};
        add("cnav_time_offset",
            {{"tggto_s", ref},
             {"week_raw", b.u(143, 13)},
             {"reference_gpst", week_time(time, b.u(143, 13), ref)},
             {"gnss_id", id},
             {"source_time_system", std::string(gps ? "GPST" : "QZSST")},
             {"target_time_system", std::string(systems[id])},
             {"a0_s", b.offset(159, 16, -35)},
             {"a1_s_s", b.f(175, 13, -51)},
             {"a2_s_s2", b.f(188, 7, -68)}},
            id >= 1 && id <= (gps ? 2 : 3), std::to_string(id));
    }
    if (mt == 15 || mt == 36) {
        // Type 36 carries 18 characters after its clock fields.
        auto payload = mt == 15 ? b.bytes(38, 232) : b.bytes(127, 144);
        std::string display;
        const char *hex = "0123456789ABCDEF";
        bool printable = true;
        for (auto ch : payload) {
            if (ch >= 32 && ch <= 126)
                display += char(ch);
            else {
                printable = false;
                display += "\\x";
                display += hex[ch >> 4];
                display += hex[ch & 15];
            }
        }
        add("cnav_text", {{"text_page", b.u(mt == 15 ? 270 : 271, 4)},
                          {"payload", payload},
                          {"display_text", display},
                          {"printable_ascii", printable}});
    }
    if (mt == 60) {
        if (!l5 || l2) {
            ++counts["cnav_invalid_qznma_source"];
            return out;
        }
        add("cnav_qznma_payload", {{"payload", b.bytes(38, 238)},
                                   {"payload_bit_length", int64_t(238)}});
    }
    ++counts["decoded_messages"];
    ++counts["cnav_decoded"];
    if (!part_id || !time || b.u(37, 1))
        return out;
    if (!assemblies_.contains(source) && assemblies_.size() >= 1024)
        throw std::runtime_error("CNAV assembly capacity exceeded");
    auto &a = assemblies_[source];
    // Three maximum broadcast intervals, distinct from parameter validity.
    if (a.start && *time - *a.start > seconds(l2 ? 144 : 72)) {
        a = {};
        ++counts["cnav_assembly_timeout"];
    }
    for (const auto &[id, old] : a.parts) {
        bool conflict = id == part_id && old != part;
        for (const auto *name : {"toe_s", "top_s"})
            if (old.contains(name) && part.contains(name) &&
                old.at(name) != part.at(name))
                conflict = true;
        // GPS toe equals toc within one CEI data set (IS-GPS-200N 30.3.4.4).
        auto differs = [](const Row &x, const Row &y) {
            return x.contains("toe_s") && y.contains("toc_s") &&
                   x.at("toe_s") != y.at("toc_s");
        };
        if (gps && (differs(old, part) || differs(part, old)))
            conflict = true;
        if (conflict) {
            a = {};
            ++counts["cnav_assembly_changed"];
            break;
        }
    }
    if (!a.start)
        a.start = time;
    a.parts[part_id] = part;
    if (a.parts.size() != 3)
        return out;
    Row eph;
    for (const auto &[id, fields] : a.parts)
        eph.insert(fields.begin(), fields.end());
    auto toe = std::get<Tick>(eph.at("toe_s"));
    auto toc = std::get<Tick>(eph.at("toc_s"));
    auto top = std::get<Tick>(eph.at("top_s"));
    auto week = std::get<int64_t>(eph.at("week_raw"));
    // WN denotes transmission week, not necessarily the week containing toe.
    // Resolve the reference week across Sunday using the transmitted TOW.
    auto transmitted = reference(*time, week, 8192, b.u(20, 17) * 6);
    auto ref = near_time(transmitted, toe);
    const auto *toe_gpst = std::get_if<Tick>(&ref);
    if (!toe_gpst || toc >= seconds(604800) || top >= seconds(604800) ||
        std::get<double>(eph.at("eccentricity")) >= 1 ||
        a_reference(gps) + std::get<double>(eph.at("delta_a_m")) <= 0) {
        ++counts["cnav_invalid_ephemeris"];
    } else {
        eph["toe_gpst"] = ref;
        eph["toc_gpst"] = near_time(*toe_gpst, toc);
        eph["top_gpst"] = near_time(*toe_gpst, top);
        eph["first_received_gpst"] = *a.start;
        // IS-GPS-200N 30.3.3.2.3: the CNAV clock refers to L1/L2 P(Y).
        eph["orbit_clock_reference"] =
            std::string(gps ? "GPS_L1_PY_GPS_L2_PY" : "QZS_L1_CP_QZS_L5_Q");
        add("cnav_ephemeris", std::move(eph), true);
        ++counts["cnav_ephemerides"];
    }
    a = {};
    return out;
}

bool cnav_snapshot(Row &r, const std::string &kind, Tick t) {
    if (kind == "cnav_ephemeris") {
        if (!window(r, "toe_gpst", t, fit_half(r)) ||
            !window(r, "toc_gpst", t, fit_half(r)))
            return false;
        r["applicability"] = std::string("ORBIT_CLOCK_FIT");
    } else if (kind == "cnav_clock") {
        if (!window(r, "toc_gpst", t, fit_half(r)))
            return false;
        r["applicability"] = std::string("CLOCK_FIT");
    } else if (kind == "cnav_eop" && gps_row(r)) {
        // IS-GPS-200 defines no EOP fit interval; the week still requires UTC.
        if (std::holds_alternative<std::monostate>(r.at("reference_gpst")))
            return false;
        r["within_fit_interval"] = Value{};
    } else if (kind == "cnav_eop") {
        if (!window(r, "reference_gpst", t, 21600))
            return false;
        r["applicability"] = std::string("REFERENCE_WINDOW");
        r["within_fit_interval"] = window(r, "reference_gpst", t, 3600);
    } else if (kind == "cnav_reduced_almanac" || kind == "cnav_midi_almanac" ||
               kind == "cnav_utc" || kind == "cnav_time_offset") {
        if (!window(r, "reference_gpst", t, 72 * 3600))
            return false;
        r["applicability"] = std::string("REFERENCE_WINDOW");
    }
    return true;
}
bool cnav_expired(const Row &r, const std::string &kind, Tick t) {
    const char *field = "reference_gpst";
    int64_t half = 72 * 3600;
    if (kind == "cnav_clock") {
        field = "toc_gpst";
        half = fit_half(r);
    } else if (kind == "cnav_ephemeris") {
        field = "toe_gpst";
        half = fit_half(r);
    } else if (kind == "cnav_eop") {
        // The week is established by a matching UTC candidate at snapshot time.
        // Retain at most a week of unresolved occurrences like other models.
        return false;
    }
    auto i = r.find(field);
    if (i == r.end())
        return false;
    auto ref = std::get_if<Tick>(&i->second);
    return ref && t > *ref + seconds(half);
}
} // namespace neognss_obs::broadcast_detail
