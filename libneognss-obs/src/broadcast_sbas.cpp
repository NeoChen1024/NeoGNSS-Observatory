// SPDX-License-Identifier: GPL-3.0-only
#include "broadcast_fields.hpp"
#include <cmath>
#include <stdexcept>

namespace neognss_obs::broadcast_detail {
namespace {
using namespace SBAS;
Tick binary_seconds(int64_t raw, unsigned fractional_bits) {
    // Integer-to-picosecond round-half-to-even without an intermediate double.
    Tick scaled = Tick(raw) * ps;
    const bool negative = scaled < 0;
    if (negative)
        scaled = -scaled;
    const Tick divisor = Tick(1) << fractional_bits;
    Tick rounded = scaled / divisor;
    const Tick remainder = scaled % divisor;
    if (remainder * 2 > divisor ||
        (remainder * 2 == divisor && (rounded & 1) != 0))
        ++rounded;
    return negative ? -rounded : rounded;
}
const char *udre_status(uint8_t index) {
    return index < 14 ? "USABLE" : index == 14 ? "NOT_MONITORED" : "DO_NOT_USE";
}
Records fast_fields(const FastCorrection *entries, size_t count,
                    unsigned first) {
    Records out{{{"mask_position", int64_t{}},
                 {"correction_m", 0.0},
                 {"udrei", int64_t{}},
                 {"status", std::string{}}},
                {}};
    for (size_t i = 0; i < count && first + i <= 51; ++i)
        out.rows.push_back(
            {{"mask_position", int64_t(first + i)},
             {"correction_m", entries[i].correction_m()},
             {"udrei", int64_t(entries[i].udrei)},
             {"status", std::string(udre_status(entries[i].udrei))}});
    return out;
}
Records long_fields(const LongTermCorrections &v) {
    Records out{{{"half", int64_t{}},
                 {"mask_position", int64_t{}},
                 {"issue", int64_t{}},
                 {"iodp", int64_t{}},
                 {"velocity_code", false},
                 {"delta_x_m", 0.0},
                 {"delta_y_m", 0.0},
                 {"delta_z_m", 0.0},
                 {"clock_offset_s", Tick{}},
                 {"clock_drift_s_s", 0.0},
                 {"delta_vx_m_s", 0.0},
                 {"delta_vy_m_s", 0.0},
                 {"delta_vz_m_s", 0.0},
                 {"reference_sod_s", Tick{}}},
                {}};
    for (const auto &e : v.entries) {
        Fields r{{"half", int64_t(e.half)},
                 {"mask_position", int64_t(e.mask_position)},
                 {"issue", int64_t(e.issue)},
                 {"iodp", int64_t(e.iodp)},
                 {"velocity_code", e.velocity_code},
                 {"delta_x_m", e.delta_position_m[0]},
                 {"delta_y_m", e.delta_position_m[1]},
                 {"delta_z_m", e.delta_position_m[2]},
                 {"clock_offset_s", binary_seconds(e.clock_offset_raw, 31)},
                 {"clock_drift_s_s", std::monostate{}},
                 {"delta_vx_m_s", std::monostate{}},
                 {"delta_vy_m_s", std::monostate{}},
                 {"delta_vz_m_s", std::monostate{}},
                 {"reference_sod_s", std::monostate{}}};
        if (e.delta_velocity_m_s) {
            r["delta_vx_m_s"] = (*e.delta_velocity_m_s)[0];
            r["delta_vy_m_s"] = (*e.delta_velocity_m_s)[1];
            r["delta_vz_m_s"] = (*e.delta_velocity_m_s)[2];
            r["clock_drift_s_s"] = std::ldexp(double(*e.clock_drift_raw), -39);
            r["reference_sod_s"] = seconds(*e.reference_sod_s);
        }
        out.rows.push_back(std::move(r));
    }
    return out;
}
} // namespace

std::pair<std::string, Row> sbas_fields(const SBAS::Message &m) {
    std::string kind;
    Row r{{"message_type", int64_t(m.type)}};
    std::visit(
        [&](const auto &v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, TestMode>) {
                kind = "sbas_service_status";
                r["restriction"] = std::string("DO_NOT_USE_FOR_SAFETY");
                r["payload_interpretation"] =
                    std::string(v.fast ? "SOUTHPAN_OPEN_MT2" : "UNINTERPRETED");
                // Keep the absent issue group null, not an invented IOD=0.
                Record info{{{"iodp", int64_t{}}, {"iodf", int64_t{}}}, {}};
                if (v.fast)
                    info.row = Fields{{"iodp", int64_t(v.fast->iodp)},
                                      {"iodf", int64_t(v.fast->iodf)}};
                r["embedded_fast_issue"] = std::move(info);
                r["embedded_fast_corrections"] =
                    v.fast ? fast_fields(v.fast->satellites.data(), 13, 1)
                           : fast_fields(nullptr, 0, 1);
            } else if constexpr (std::is_same_v<T, NullMessage>) {
                kind = "sbas_null";
            } else if constexpr (std::is_same_v<T, PrnMask>) {
                kind = "sbas_prn_mask";
                r["iodp"] = int64_t(v.iodp);
                Records entries{
                    {{"mask_position", int64_t{}}, {"mask_bit", int64_t{}}},
                    {}};
                int64_t ordinal = 0;
                for (size_t i = 0; i < v.mask.size(); ++i)
                    if (v.mask[i])
                        entries.rows.push_back({{"mask_position", ++ordinal},
                                                {"mask_bit", int64_t(i + 1)}});
                r["entries"] = std::move(entries);
            } else if constexpr (std::is_same_v<T, FastCorrections>) {
                kind = "sbas_fast_corrections";
                r["iodp"] = int64_t(v.iodp);
                r["iodf"] = int64_t(v.iodf);
                r["fast_block"] = int64_t((v.first_mask_position - 1) / 13);
                r["entries"] =
                    fast_fields(v.satellites.data(), 13, v.first_mask_position);
            } else if constexpr (std::is_same_v<T, Integrity>) {
                kind = "sbas_integrity";
                r["iodf_by_block"] =
                    std::vector<int64_t>(v.iodf.begin(), v.iodf.end());
                Records entries{{{"mask_position", int64_t{}},
                                 {"udrei", int64_t{}},
                                 {"status", std::string{}}},
                                {}};
                for (size_t i = 0; i < v.udrei.size(); ++i)
                    entries.rows.push_back(
                        {{"mask_position", int64_t(i + 1)},
                         {"udrei", int64_t(v.udrei[i])},
                         {"status", std::string(udre_status(v.udrei[i]))}});
                r["entries"] = std::move(entries);
            } else if constexpr (std::is_same_v<T, FastDegradation>) {
                kind = "sbas_fast_degradation";
                r["iodp"] = int64_t(v.iodp);
                r["latency_s"] = seconds(v.latency_s);
                constexpr double factors[] = {0,      .00005, .00009, .00012,
                                              .00015, .00020, .00030, .00045,
                                              .00060, .00090, .00150, .00210,
                                              .00270, .00330, .00460, .00580};
                Records entries{{{"mask_position", int64_t{}},
                                 {"degradation_index", int64_t{}},
                                 {"degradation_m_s2", 0.0}},
                                {}};
                for (size_t i = 0; i < v.degradation_index.size(); ++i)
                    entries.rows.push_back(
                        {{"mask_position", int64_t(i + 1)},
                         {"degradation_index", int64_t(v.degradation_index[i])},
                         {"degradation_m_s2",
                          factors[v.degradation_index[i]]}});
                r["entries"] = std::move(entries);
            } else if constexpr (std::is_same_v<T, GeoNavigation>) {
                kind = "sbas_geo_ephemeris";
                r["reference_sod_s"] = seconds(v.t0_raw * 16);
                r["time_system"] = std::string("SNT");
                r["ura_index"] = int64_t(v.ura);
                r["broadcast_status"] =
                    std::string(v.ura == 15 ? "DO_NOT_USE" : "USABLE");
                r["position_m"] = std::vector<double>{v.position_raw[0] * .08,
                                                      v.position_raw[1] * .08,
                                                      v.position_raw[2] * .4};
                r["velocity_m_s"] = std::vector<double>{
                    v.velocity_raw[0] * .000625, v.velocity_raw[1] * .000625,
                    v.velocity_raw[2] * .004};
                r["acceleration_m_s2"] =
                    std::vector<double>{v.acceleration_raw[0] * .0000125,
                                        v.acceleration_raw[1] * .0000125,
                                        v.acceleration_raw[2] * .0000625};
                r["clock_offset_s"] = binary_seconds(v.clock_offset_raw, 31);
                r["clock_drift_s_s"] =
                    std::ldexp(double(v.clock_drift_raw), -40);
                // First eight payload bits are reserved in the selected ICAO
                // table.
            } else if constexpr (std::is_same_v<T, Degradation>) {
                kind = "sbas_degradation";
                r.insert({{"brrc_m", v.brrc_m},
                          {"cltc_lsb_m", v.cltc_lsb_m},
                          {"cltc_v1_m_s", v.cltc_v1_m_s},
                          {"cltc_v0_m", v.cltc_v0_m},
                          {"iltc_v1_s", seconds(v.iltc_v1_s)},
                          {"iltc_v0_s", seconds(v.iltc_v0_s)},
                          {"cgeo_lsb_m", v.cgeo_lsb_m},
                          {"cgeo_v_m_s", v.cgeo_v_m_s},
                          {"igeo_s", seconds(v.igeo_s)},
                          {"cer_m", v.cer_m},
                          {"ciono_step_m", v.ciono_step_m},
                          {"iiono_s", seconds(v.iiono_s)},
                          {"ciono_ramp_m_s", v.ciono_ramp_m_s},
                          {"rss_udre", v.rss_udre},
                          {"rss_iono", v.rss_iono},
                          {"ccovariance", v.ccovariance}});
            } else if constexpr (std::is_same_v<T, NetworkTime>) {
                kind = "sbas_network_time";
                r.insert({{"time_system", std::string("SNT")},
                          {"utc_id", int64_t(v.utc_id)},
                          {"utc_available", v.utc_id != 7},
                          {"gps_tow_s", seconds(v.gps_tow_s)},
                          {"gps_week_mod1024", int64_t(v.gps_week_mod1024)}});
                Record utc{{{"a0_s", Tick{}},
                            {"a1_s_s", 0.0},
                            {"reference_tow_s", Tick{}},
                            {"reference_week_mod256", int64_t{}},
                            {"leap_week_mod256", int64_t{}},
                            {"leap_day", int64_t{}},
                            {"leap_seconds", int64_t{}},
                            {"future_leap_seconds", int64_t{}}},
                           {}};
                if (v.utc_id != 7)
                    utc.row = Fields(
                        {{"a0_s", binary_seconds(v.a0_raw, 30)},
                         {"a1_s_s", std::ldexp(double(v.a1_raw), -50)},
                         {"reference_tow_s", seconds(v.reference_tow_s)},
                         {"reference_week_mod256",
                          int64_t(v.reference_week_mod256)},
                         {"leap_week_mod256", int64_t(v.leap_week_mod256)},
                         {"leap_day", int64_t(v.leap_day)},
                         {"leap_seconds", int64_t(v.leap_seconds)},
                         {"future_leap_seconds",
                          int64_t(v.future_leap_seconds)}});
                r["utc_parameters"] = std::move(utc);
            } else if constexpr (std::is_same_v<T, GeoAlmanac>) {
                kind = "sbas_geo_almanac";
                r["reference_sod_s"] = seconds(v.reference_sod_s);
                r["time_system"] = std::string("SNT");
                Records entries{{{"prn", int64_t{}},
                                 {"subject_number", int64_t{}},
                                 {"health_status", int64_t{}},
                                 {"service_provider_id", int64_t{}},
                                 {"ranging_on", false},
                                 {"precision_corrections_on", false},
                                 {"basic_corrections_on", false},
                                 {"position_m", std::vector<double>{}},
                                 {"velocity_m_s", std::vector<double>{}}},
                                {}};
                for (const auto &e : v.entries)
                    entries.rows.push_back(
                        {{"prn", int64_t(e.prn)},
                         {"subject_number", e.prn >= 120 && e.prn <= 158
                                                ? Scalar(int64_t(e.prn - 100))
                                                : Scalar{}},
                         {"health_status", int64_t(e.health_status)},
                         {"service_provider_id", int64_t(e.health_status >> 4)},
                         {"ranging_on", !(e.health_status & 1)},
                         {"precision_corrections_on", !(e.health_status & 2)},
                         {"basic_corrections_on", !(e.health_status & 4)},
                         {"position_m",
                          std::vector<double>(e.position_m.begin(),
                                              e.position_m.end())},
                         {"velocity_m_s",
                          std::vector<double>(e.velocity_m_s.begin(),
                                              e.velocity_m_s.end())}});
                r["entries"] = std::move(entries);
            } else if constexpr (std::is_same_v<T, IonosphericMask>) {
                kind = "sbas_ionospheric_mask";
                r["band"] = int64_t(v.band);
                r["iodi"] = int64_t(v.iodi);
                r["band_count"] = int64_t(v.number_of_bands_raw);
                Records entries{{{"mask_position", int64_t{}},
                                 {"mask_bit", int64_t{}},
                                 {"latitude_deg", 0.0},
                                 {"longitude_deg", 0.0}},
                                {}};
                int64_t ordinal = 0;
                for (size_t i = 0; i < v.mask.size(); ++i)
                    if (v.mask[i]) {
                        auto coordinate = igp_coordinate(v.band, i + 1);
                        if (!coordinate)
                            throw std::runtime_error(
                                "SBAS mask contains nonexistent grid point");
                        entries.rows.push_back(
                            {{"mask_position", ++ordinal},
                             {"mask_bit", int64_t(i + 1)},
                             {"latitude_deg", double(coordinate->first)},
                             {"longitude_deg", double(coordinate->second)}});
                    }
                r["entries"] = std::move(entries);
            } else if constexpr (std::is_same_v<T, IonosphericDelay>) {
                kind = "sbas_ionospheric_delays";
                r["band"] = int64_t(v.band);
                r["block"] = int64_t(v.block);
                r["iodi"] = int64_t(v.iodi);
                Records entries{{{"mask_position", int64_t{}},
                                 {"vertical_delay_m", 0.0},
                                 {"givei", int64_t{}},
                                 {"status", std::string{}}},
                                {}};
                for (size_t i = 0; i < v.corrections.size(); ++i) {
                    const auto &e = v.corrections[i];
                    auto status = e.status();
                    entries.rows.push_back(
                        {{"mask_position", int64_t(15 * v.block + i + 1)},
                         {"vertical_delay_m",
                          e.delay_m() ? Scalar(*e.delay_m()) : Scalar{}},
                         {"givei", int64_t(e.givei)},
                         {"status",
                          std::string(status == IgpStatus::usable ? "USABLE"
                                      : status == IgpStatus::not_monitored
                                          ? "NOT_MONITORED"
                                          : "DO_NOT_USE")}});
                }
                r["entries"] = std::move(entries);
            } else if constexpr (std::is_same_v<T, LongTermCorrections>) {
                kind = "sbas_long_term_corrections";
                r["entries"] = long_fields(v);
            } else if constexpr (std::is_same_v<T, MixedCorrections>) {
                kind = "sbas_mixed_corrections";
                r["iodp"] = int64_t(v.iodp);
                r["iodf"] = int64_t(v.iodf);
                r["fast_block"] = int64_t(v.fast_block);
                r["fast_corrections"] =
                    fast_fields(v.fast.data(), 6, 1 + 13 * v.fast_block);
                r["long_term_corrections"] = long_fields(v.long_term);
            } else if constexpr (std::is_same_v<T, ServiceMessage>) {
                kind = "sbas_service_region";
                r.insert({{"iods", int64_t(v.iods)},
                          {"message_count", int64_t(v.message_count)},
                          {"message_number", int64_t(v.message_number)},
                          {"priority", int64_t(v.priority)},
                          {"delta_udre_inside_index",
                           int64_t(v.delta_udre_inside_index)},
                          {"delta_udre_outside_index",
                           int64_t(v.delta_udre_outside_index)}});
                constexpr double delta[] = {1, 1.1, 1.25, 1.5, 2,  3,  4,  5,
                                            6, 8,   10,   20,  30, 40, 50, 100};
                r["delta_udre_inside"] = delta[v.delta_udre_inside_index];
                r["delta_udre_outside"] = delta[v.delta_udre_outside_index];
                Records entries{{{"latitude1_deg", 0.0},
                                 {"longitude1_deg", 0.0},
                                 {"latitude2_deg", 0.0},
                                 {"longitude2_deg", 0.0},
                                 {"quadrangle", false}},
                                {}};
                for (const auto &e : v.regions)
                    entries.rows.push_back(
                        {{"latitude1_deg", double(e.latitude1_deg)},
                         {"longitude1_deg", double(e.longitude1_deg)},
                         {"latitude2_deg", double(e.latitude2_deg)},
                         {"longitude2_deg", double(e.longitude2_deg)},
                         {"quadrangle", e.quadrangle}});
                r["regions"] = std::move(entries);
            } else if constexpr (std::is_same_v<T, Covariance>) {
                kind = "sbas_covariance";
                r["iodp"] = int64_t(v.iodp);
                Records entries{{{"mask_position", int64_t{}},
                                 {"scale_exponent", int64_t{}},
                                 {"e_elements", std::vector<int64_t>{}},
                                 {"r_upper", std::vector<double>{}}},
                                {}};
                for (const auto &e : v.entries) {
                    std::vector<double> scaled;
                    for (auto element : e.elements)
                        scaled.push_back(std::ldexp(double(element),
                                                    int(e.scale_exponent) - 5));
                    entries.rows.push_back(
                        {{"mask_position", int64_t(e.mask_position)},
                         {"scale_exponent", int64_t(e.scale_exponent)},
                         {"e_elements", std::vector<int64_t>(e.elements.begin(),
                                                             e.elements.end())},
                         {"r_upper", std::move(scaled)}});
                }
                r["entries"] = std::move(entries);
            } else {
                throw std::runtime_error(
                    "Unparsed SBAS content reached output");
            }
        },
        m.content);
    return {std::move(kind), std::move(r)};
}
} // namespace neognss_obs::broadcast_detail
