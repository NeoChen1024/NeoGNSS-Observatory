// SPDX-License-Identifier: GPL-3.0-only
#include "broadcast_bits.hpp"
#include <algorithm>
#include <cmath>
#include <cppgnss/stream.hpp>
#include <neognss_obs/sbas.hpp>

namespace neognss_obs::SBAS {
namespace {
using Bits = broadcast_detail::BroadcastBits;
FastCorrections fast(const Bits &b, unsigned type) {
    FastCorrections v;
    v.iodf = b.u(14, 2);
    v.iodp = b.u(16, 2);
    v.first_mask_position = 1 + 13 * (type - 2);
    for (size_t i = 0; i < 13; ++i)
        v.satellites[i] = {int16_t(b.s(18 + 12 * i, 12)),
                           uint8_t(b.u(174 + 4 * i, 4))};
    return v;
}
bool long_half(const Bits &b, size_t start, uint8_t half,
               LongTermCorrections &out) {
    const bool velocity = b.u(start, 1);
    const auto iodp = uint8_t(b.u(start + (velocity ? 104 : 103), 2));
    for (unsigned i = 0; i < (velocity ? 1U : 2U); ++i) {
        size_t p = start + 1 + 51 * i;
        LongTermEntry e;
        e.half = half;
        e.iodp = iodp;
        e.velocity_code = velocity;
        e.mask_position = b.u(p, 6);
        if (!e.mask_position)
            continue;
        if (e.mask_position > 51)
            return false;
        e.issue = b.u(p + 6, 8);
        const unsigned width = velocity ? 11 : 9;
        for (unsigned j = 0; j < 3; ++j)
            e.delta_position_m[j] = b.s(p + 14 + width * j, width) * 0.125;
        e.clock_offset_raw = b.s(p + 14 + 3 * width, velocity ? 11 : 10);
        if (velocity) {
            std::array<double, 3> rates;
            for (unsigned j = 0; j < 3; ++j)
                rates[j] = std::ldexp(double(b.s(p + 58 + 8 * j, 8)), -11);
            e.delta_velocity_m_s = rates;
            e.clock_drift_raw = b.s(p + 82, 8);
            e.reference_sod_s = b.u(p + 90, 13) * 16;
            if (*e.reference_sod_s >= 86400)
                return false;
        }
        out.entries.push_back(e);
    }
    return true;
}
} // namespace
Result parse_l1(std::span<const uint8_t> bytes, L1Profile profile) {
    if (bytes.size() != 32 || (bytes[31] & 0x3f))
        throw std::runtime_error("Invalid SBAS L1 body size/padding");
    const Bits b(bytes, 250);
    const auto preamble = b.u(0, 8);
    Message m;
    m.type = b.u(8, 6);
    if (preamble != 0x53 && preamble != 0x9a && preamble != 0xc6)
        return {Status::invalid_preamble, m};
    if (cppgnss::crc24q(bytes, 0, 250))
        return {Status::invalid_crc, m};
    switch (m.type) {
    case 0:
        m.content = TestMode{profile == L1Profile::southpan_open
                                 ? std::optional{fast(b, 2)}
                                 : std::nullopt};
        break;
    case 63:
        m.content = NullMessage{};
        break;
    case 1: {
        PrnMask v;
        v.iodp = b.u(224, 2);
        for (size_t i = 0; i < v.mask.size(); ++i)
            v.mask[i] = b.u(14 + i, 1);
        if (std::count(v.mask.begin(), v.mask.end(), true) > 51)
            return {Status::invalid_content, m};
        m.content = v;
        break;
    }
    case 2:
    case 3:
    case 4:
    case 5: {
        m.content = fast(b, m.type);
        break;
    }
    case 6: {
        Integrity v;
        for (size_t i = 0; i < 4; ++i)
            v.iodf[i] = b.u(14 + 2 * i, 2);
        for (size_t i = 0; i < 51; ++i)
            v.udrei[i] = b.u(22 + 4 * i, 4);
        m.content = v;
        break;
    }
    case 7: {
        FastDegradation v;
        v.latency_s = b.u(14, 4);
        v.iodp = b.u(18, 2);
        for (size_t i = 0; i < 51; ++i)
            v.degradation_index[i] = b.u(22 + 4 * i, 4);
        m.content = v;
        break;
    }
    case 9: {
        GeoNavigation v;
        v.iodn_raw = b.u(14, 8);
        v.t0_raw = b.u(22, 13);
        if (v.t0_raw * 16 >= 86400)
            return {Status::invalid_content, m};
        v.ura = b.u(35, 4);
        v.position_raw = {int32_t(b.s(39, 30)), int32_t(b.s(69, 30)),
                          int32_t(b.s(99, 25))};
        v.velocity_raw = {int32_t(b.s(124, 17)), int32_t(b.s(141, 17)),
                          int32_t(b.s(158, 18))};
        v.acceleration_raw = {int32_t(b.s(176, 10)), int32_t(b.s(186, 10)),
                              int32_t(b.s(196, 10))};
        v.clock_offset_raw = b.s(206, 12);
        v.clock_drift_raw = b.s(218, 8);
        m.content = v;
        break;
    }
    case 10: {
        Degradation v;
        size_t p = 14;
        auto u = [&](unsigned n) {
            auto x = b.u(p, n);
            p += n;
            return x;
        };
        v.brrc_m = u(10) * 0.002;
        v.cltc_lsb_m = u(10) * 0.002;
        v.cltc_v1_m_s = u(10) * 0.00005;
        v.iltc_v1_s = u(9);
        v.cltc_v0_m = u(10) * 0.002;
        v.iltc_v0_s = u(9);
        v.cgeo_lsb_m = u(10) * 0.0005;
        v.cgeo_v_m_s = u(10) * 0.00005;
        v.igeo_s = u(9);
        v.cer_m = u(6) * 0.5;
        v.ciono_step_m = u(10) * 0.001;
        v.iiono_s = u(9);
        v.ciono_ramp_m_s = u(10) * 0.000005;
        v.rss_udre = u(1);
        v.rss_iono = u(1);
        v.ccovariance = u(7) * 0.1;
        m.content = v;
        break;
    }
    case 12: {
        NetworkTime v;
        v.a1_raw = b.s(14, 24);
        v.a0_raw = b.s(38, 32);
        v.reference_tow_s = b.u(70, 8) * 4096;
        v.reference_week_mod256 = b.u(78, 8);
        v.leap_seconds = b.s(86, 8);
        v.leap_week_mod256 = b.u(94, 8);
        v.leap_day = b.u(102, 8);
        v.future_leap_seconds = b.s(110, 8);
        v.utc_id = b.u(118, 3);
        v.gps_tow_s = b.u(121, 20);
        v.gps_week_mod1024 = b.u(141, 10);
        // UTC ID 7 explicitly denotes no UTC parameters. Do not validate
        // inapplicable UTC fields, which may contain arbitrary fill values.
        if (v.gps_tow_s >= 604800 ||
            (v.utc_id != 7 &&
             (v.reference_tow_s >= 604800 || v.leap_day < 1 || v.leap_day > 7)))
            return {Status::invalid_content, m};
        m.content = v;
        break;
    }
    case 17: {
        GeoAlmanac v;
        v.reference_sod_s = b.u(215, 11) * 64;
        if (v.reference_sod_s >= 86400)
            return {Status::invalid_content, m};
        for (unsigned i = 0; i < 3; ++i) {
            const size_t p = 14 + 67 * i;
            GeoAlmanacEntry e;
            e.prn = b.u(p + 2, 8);
            if (!e.prn)
                continue;
            if (e.prn > 210)
                return {Status::invalid_content, m};
            e.health_status = b.u(p + 10, 8);
            e.position_m = {b.s(p + 18, 15) * 2600.0, b.s(p + 33, 15) * 2600.0,
                            b.s(p + 48, 9) * 26000.0};
            e.velocity_m_s = {b.s(p + 57, 3) * 10.0, b.s(p + 60, 3) * 10.0,
                              b.s(p + 63, 4) * 60.0};
            v.entries.push_back(e);
        }
        m.content = std::move(v);
        break;
    }
    case 24: {
        MixedCorrections v;
        v.iodp = b.u(110, 2);
        v.fast_block = b.u(112, 2);
        v.iodf = b.u(114, 2);
        for (size_t i = 0; i < 6; ++i)
            v.fast[i] = {int16_t(b.s(14 + i * 12, 12)),
                         uint8_t(b.u(86 + i * 4, 4))};
        if (!long_half(b, 120, 1, v.long_term))
            return {Status::invalid_content, m};
        m.content = std::move(v);
        break;
    }
    case 25: {
        LongTermCorrections v;
        if (!long_half(b, 14, 0, v) || !long_half(b, 120, 1, v))
            return {Status::invalid_content, m};
        m.content = std::move(v);
        break;
    }
    case 27: {
        ServiceMessage v;
        v.iods = b.u(14, 3);
        v.message_count = b.u(17, 3) + 1;
        v.message_number = b.u(20, 3) + 1;
        const auto n = b.u(23, 3);
        v.priority = b.u(26, 2);
        v.delta_udre_inside_index = b.u(28, 4);
        v.delta_udre_outside_index = b.u(32, 4);
        if (n > 5 || v.message_number > v.message_count)
            return {Status::invalid_content, m};
        for (size_t i = 0; i < n; ++i) {
            size_t p = 36 + i * 35;
            ServiceRegion r{int16_t(b.s(p, 8)), int16_t(b.s(p + 8, 9)),
                            int16_t(b.s(p + 17, 8)), int16_t(b.s(p + 25, 9)),
                            bool(b.u(p + 34, 1))};
            if (std::abs(r.latitude1_deg) > 90 ||
                std::abs(r.latitude2_deg) > 90 ||
                std::abs(r.longitude1_deg) > 180 ||
                std::abs(r.longitude2_deg) > 180)
                return {Status::invalid_content, m};
            v.regions.push_back(r);
        }
        m.content = std::move(v);
        break;
    }
    case 28: {
        Covariance v;
        v.iodp = b.u(14, 2);
        for (size_t i = 0; i < 2; ++i) {
            const size_t p = 16 + 105 * i;
            CovarianceEntry e;
            e.mask_position = b.u(p, 6);
            if (!e.mask_position)
                continue;
            if (e.mask_position > 51)
                return {Status::invalid_content, m};
            e.scale_exponent = b.u(p + 6, 3);
            for (size_t j = 0; j < 4; ++j)
                e.elements[j] = b.u(p + 9 + 9 * j, 9);
            for (size_t j = 0; j < 6; ++j)
                e.elements[4 + j] = b.s(p + 45 + 10 * j, 10);
            v.entries.push_back(e);
        }
        m.content = std::move(v);
        break;
    }
    case 18: {
        IonosphericMask v;
        v.number_of_bands_raw = b.u(14, 4);
        v.band = b.u(18, 4);
        v.iodi = b.u(22, 2);
        if (v.band > 10 || v.number_of_bands_raw > 11)
            return {Status::invalid_content, m};
        for (size_t i = 0; i < 201; ++i) {
            v.mask[i] = b.u(24 + i, 1);
            if (v.mask[i] && !igp_coordinate(v.band, i + 1))
                return {Status::invalid_content, m};
        }
        m.content = v;
        break;
    }
    case 26: {
        IonosphericDelay v;
        v.band = b.u(14, 4);
        v.block = b.u(18, 4);
        v.iodi = b.u(217, 2);
        if (v.band > 10 || v.block > 13)
            return {Status::invalid_content, m};
        for (size_t i = 0; i < 15; ++i)
            v.corrections[i] = {uint16_t(b.u(22 + 13 * i, 9)),
                                uint8_t(b.u(31 + 13 * i, 4))};
        m.content = v;
        break;
    }
    default:
        return {Status::unsupported_message, m};
    }
    return {Status::decoded, m};
}
const char *status_name(Status s) {
    switch (s) {
    case Status::decoded:
        return "decoded";
    case Status::invalid_preamble:
        return "invalid_preamble";
    case Status::invalid_crc:
        return "invalid_crc";
    case Status::unsupported_message:
        return "unsupported_message";
    case Status::invalid_content:
        return "invalid_content";
    }
    return "unknown";
}
std::optional<std::pair<int, int>> igp_coordinate(unsigned band,
                                                  unsigned mask_bit) {
    if (band > 10 || !mask_bit)
        return std::nullopt;
    unsigned bit = 0;
    if (band < 9) {
        for (int column = 0; column < 8; ++column) {
            const int lon = -180 + int(band) * 40 + column * 5;
            std::vector<int> latitudes;
            if (column % 2 == 0) {
                if (lon == -140 || lon == -50 || lon == 40 || lon == 130)
                    latitudes.push_back(-85);
                latitudes.push_back(-75);
                latitudes.push_back(-65);
            }
            for (int lat = -55; lat <= 55; lat += 5)
                latitudes.push_back(lat);
            if (column % 2 == 0) {
                latitudes.push_back(65);
                latitudes.push_back(75);
                if (lon == -180 || lon == -90 || lon == 0 || lon == 90)
                    latitudes.push_back(85);
            }
            for (auto lat : latitudes)
                if (++bit == mask_bit)
                    return {{lat, lon}};
        }
    } else {
        for (auto [lat, step] : std::array<std::pair<int, int>, 5>{
                 {{60, 5}, {65, 10}, {70, 10}, {75, 10}, {85, 30}}})
            for (int lon = (band == 10 && lat == 85) ? -170 : -180; lon < 180;
                 lon += step)
                if (++bit == mask_bit)
                    return {{band == 9 ? lat : -lat, lon}};
    }
    return std::nullopt;
}
} // namespace neognss_obs::SBAS
