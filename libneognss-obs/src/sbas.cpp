// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cmath>
#include <neognss_obs/sbas.hpp>

namespace neognss_obs::SBAS {
BitView::BitView(std::span<const uint8_t> bytes, size_t count)
    : bytes_(bytes), bit_count_(count) {
    if (count > bytes.size() * 8)
        throw std::out_of_range("BitView exceeds storage");
}
uint64_t BitView::unsigned_at(size_t offset, size_t width) const {
    if (width > 64 || offset > bit_count_ || width > bit_count_ - offset)
        throw std::out_of_range("Bit field exceeds message");
    uint64_t value = 0;
    for (size_t i = offset; i < offset + width; ++i)
        value = (value << 1) | ((bytes_[i / 8] >> (7 - i % 8)) & 1);
    return value;
}
int64_t BitView::signed_at(size_t offset, size_t width) const {
    if (!width)
        throw std::out_of_range("Signed bit field is empty");
    uint64_t value = unsigned_at(offset, width);
    if (width < 64 && (value & (uint64_t{1} << (width - 1))))
        value |= ~uint64_t{0} << width;
    return std::bit_cast<int64_t>(value);
}
uint32_t crc24q(std::span<const uint8_t> bytes, size_t count) {
    BitView bits(bytes, count);
    uint32_t crc = 0;
    for (size_t i = 0; i < count; ++i) {
        const bool feedback = ((crc >> 23) ^ bits.unsigned_at(i, 1)) & 1;
        crc = (crc << 1) & 0xffffff;
        if (feedback)
            crc ^= 0x864cfb;
    }
    return crc;
}
namespace {
FastCorrections fast(const BitView &b, unsigned type) {
    FastCorrections v;
    v.iodf = b.unsigned_at(14, 2);
    v.iodp = b.unsigned_at(16, 2);
    v.first_mask_position = 1 + 13 * (type - 2);
    for (size_t i = 0; i < 13; ++i)
        v.satellites[i] = {int16_t(b.signed_at(18 + 12 * i, 12)),
                           uint8_t(b.unsigned_at(174 + 4 * i, 4))};
    return v;
}
bool long_half(const BitView &b, size_t start, uint8_t half,
               LongTermCorrections &out) {
    const bool velocity = b.unsigned_at(start, 1);
    const auto iodp = uint8_t(b.unsigned_at(start + (velocity ? 104 : 103), 2));
    for (unsigned i = 0; i < (velocity ? 1U : 2U); ++i) {
        size_t p = start + 1 + 51 * i;
        LongTermEntry e;
        e.half = half;
        e.iodp = iodp;
        e.velocity_code = velocity;
        e.mask_position = b.unsigned_at(p, 6);
        if (!e.mask_position)
            continue;
        if (e.mask_position > 51)
            return false;
        e.issue = b.unsigned_at(p + 6, 8);
        const unsigned width = velocity ? 11 : 9;
        for (unsigned j = 0; j < 3; ++j)
            e.delta_position_m[j] =
                b.signed_at(p + 14 + width * j, width) * 0.125;
        e.clock_offset_raw =
            b.signed_at(p + 14 + 3 * width, velocity ? 11 : 10);
        if (velocity) {
            std::array<double, 3> rates;
            for (unsigned j = 0; j < 3; ++j)
                rates[j] =
                    std::ldexp(double(b.signed_at(p + 58 + 8 * j, 8)), -11);
            e.delta_velocity_m_s = rates;
            e.clock_drift_raw = b.signed_at(p + 82, 8);
            e.reference_sod_s = b.unsigned_at(p + 90, 13) * 16;
            if (*e.reference_sod_s >= 86400)
                return false;
        }
        out.entries.push_back(e);
    }
    return true;
}
} // namespace
Result parse_l1(std::span<const uint8_t> bytes, L1Profile profile) {
    if (bytes.size() != 32)
        return {Status::invalid_word_count, std::nullopt};
    Message m;
    std::copy(bytes.begin(), bytes.end(), m.bytes.begin());
    m.padding_bits = m.bytes[31] & 0x3f;
    m.bytes[31] &= 0xc0;
    const BitView b(m.bytes, 250);
    m.preamble = b.unsigned_at(0, 8);
    m.type = b.unsigned_at(8, 6);
    m.received_crc = b.unsigned_at(226, 24);
    m.computed_crc = crc24q(m.bytes, 226);
    m.preamble_valid =
        m.preamble == 0x53 || m.preamble == 0x9a || m.preamble == 0xc6;
    m.crc_valid = m.received_crc == m.computed_crc;
    if (!m.preamble_valid)
        return {Status::invalid_preamble, m};
    if (!m.crc_valid)
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
        v.iodp = b.unsigned_at(224, 2);
        for (size_t i = 0; i < v.mask.size(); ++i)
            v.mask[i] = b.unsigned_at(14 + i, 1);
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
            v.iodf[i] = b.unsigned_at(14 + 2 * i, 2);
        for (size_t i = 0; i < 51; ++i)
            v.udrei[i] = b.unsigned_at(22 + 4 * i, 4);
        m.content = v;
        break;
    }
    case 7: {
        FastDegradation v;
        v.latency_s = b.unsigned_at(14, 4);
        v.iodp = b.unsigned_at(18, 2);
        for (size_t i = 0; i < 51; ++i)
            v.degradation_index[i] = b.unsigned_at(22 + 4 * i, 4);
        m.content = v;
        break;
    }
    case 9: {
        GeoNavigation v;
        v.iodn_raw = b.unsigned_at(14, 8);
        v.t0_raw = b.unsigned_at(22, 13);
        if (v.t0_raw * 16 >= 86400)
            return {Status::invalid_content, m};
        v.ura = b.unsigned_at(35, 4);
        v.position_raw = {int32_t(b.signed_at(39, 30)),
                          int32_t(b.signed_at(69, 30)),
                          int32_t(b.signed_at(99, 25))};
        v.velocity_raw = {int32_t(b.signed_at(124, 17)),
                          int32_t(b.signed_at(141, 17)),
                          int32_t(b.signed_at(158, 18))};
        v.acceleration_raw = {int32_t(b.signed_at(176, 10)),
                              int32_t(b.signed_at(186, 10)),
                              int32_t(b.signed_at(196, 10))};
        v.clock_offset_raw = b.signed_at(206, 12);
        v.clock_drift_raw = b.signed_at(218, 8);
        m.content = v;
        break;
    }
    case 10: {
        Degradation v;
        size_t p = 14;
        auto u = [&](unsigned n) {
            auto x = b.unsigned_at(p, n);
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
        v.a1_raw = b.signed_at(14, 24);
        v.a0_raw = b.signed_at(38, 32);
        v.reference_tow_s = b.unsigned_at(70, 8) * 4096;
        v.reference_week_mod256 = b.unsigned_at(78, 8);
        v.leap_seconds = b.signed_at(86, 8);
        v.leap_week_mod256 = b.unsigned_at(94, 8);
        v.leap_day = b.unsigned_at(102, 8);
        v.future_leap_seconds = b.signed_at(110, 8);
        v.utc_id = b.unsigned_at(118, 3);
        v.gps_tow_s = b.unsigned_at(121, 20);
        v.gps_week_mod1024 = b.unsigned_at(141, 10);
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
        v.reference_sod_s = b.unsigned_at(215, 11) * 64;
        if (v.reference_sod_s >= 86400)
            return {Status::invalid_content, m};
        for (unsigned i = 0; i < 3; ++i) {
            const size_t p = 14 + 67 * i;
            GeoAlmanacEntry e;
            e.prn = b.unsigned_at(p + 2, 8);
            if (!e.prn)
                continue;
            if (e.prn > 210)
                return {Status::invalid_content, m};
            e.health_status = b.unsigned_at(p + 10, 8);
            e.position_m = {b.signed_at(p + 18, 15) * 2600.0,
                            b.signed_at(p + 33, 15) * 2600.0,
                            b.signed_at(p + 48, 9) * 26000.0};
            e.velocity_m_s = {b.signed_at(p + 57, 3) * 10.0,
                              b.signed_at(p + 60, 3) * 10.0,
                              b.signed_at(p + 63, 4) * 60.0};
            v.entries.push_back(e);
        }
        m.content = std::move(v);
        break;
    }
    case 24: {
        MixedCorrections v;
        v.iodp = b.unsigned_at(110, 2);
        v.fast_block = b.unsigned_at(112, 2);
        v.iodf = b.unsigned_at(114, 2);
        for (size_t i = 0; i < 6; ++i)
            v.fast[i] = {int16_t(b.signed_at(14 + i * 12, 12)),
                         uint8_t(b.unsigned_at(86 + i * 4, 4))};
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
        v.iods = b.unsigned_at(14, 3);
        v.message_count = b.unsigned_at(17, 3) + 1;
        v.message_number = b.unsigned_at(20, 3) + 1;
        const auto n = b.unsigned_at(23, 3);
        v.priority = b.unsigned_at(26, 2);
        v.delta_udre_inside_index = b.unsigned_at(28, 4);
        v.delta_udre_outside_index = b.unsigned_at(32, 4);
        if (n > 5 || v.message_number > v.message_count)
            return {Status::invalid_content, m};
        for (size_t i = 0; i < n; ++i) {
            size_t p = 36 + i * 35;
            ServiceRegion r{int16_t(b.signed_at(p, 8)),
                            int16_t(b.signed_at(p + 8, 9)),
                            int16_t(b.signed_at(p + 17, 8)),
                            int16_t(b.signed_at(p + 25, 9)),
                            bool(b.unsigned_at(p + 34, 1))};
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
        v.iodp = b.unsigned_at(14, 2);
        for (size_t i = 0; i < 2; ++i) {
            const size_t p = 16 + 105 * i;
            CovarianceEntry e;
            e.mask_position = b.unsigned_at(p, 6);
            if (!e.mask_position)
                continue;
            if (e.mask_position > 51)
                return {Status::invalid_content, m};
            e.scale_exponent = b.unsigned_at(p + 6, 3);
            for (size_t j = 0; j < 4; ++j)
                e.elements[j] = b.unsigned_at(p + 9 + 9 * j, 9);
            for (size_t j = 0; j < 6; ++j)
                e.elements[4 + j] = b.signed_at(p + 45 + 10 * j, 10);
            v.entries.push_back(e);
        }
        m.content = std::move(v);
        break;
    }
    case 18: {
        IonosphericMask v;
        v.number_of_bands_raw = b.unsigned_at(14, 4);
        v.band = b.unsigned_at(18, 4);
        v.iodi = b.unsigned_at(22, 2);
        if (v.band > 10 || v.number_of_bands_raw > 11)
            return {Status::invalid_content, m};
        for (size_t i = 0; i < 201; ++i) {
            v.mask[i] = b.unsigned_at(24 + i, 1);
            if (v.mask[i] && !igp_coordinate(v.band, i + 1))
                return {Status::invalid_content, m};
        }
        m.content = v;
        break;
    }
    case 26: {
        IonosphericDelay v;
        v.band = b.unsigned_at(14, 4);
        v.block = b.unsigned_at(18, 4);
        v.iodi = b.unsigned_at(217, 2);
        if (v.band > 10 || v.block > 13)
            return {Status::invalid_content, m};
        for (size_t i = 0; i < 15; ++i)
            v.corrections[i] = {uint16_t(b.unsigned_at(22 + 13 * i, 9)),
                                uint8_t(b.unsigned_at(31 + 13 * i, 4))};
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
    case Status::invalid_word_count:
        return "invalid_word_count";
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
