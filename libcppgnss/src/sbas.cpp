// SPDX-License-Identifier: GPL-3.0-only
#include <cppgnss/sbas.hpp>
#include <algorithm>

namespace cppgnss::SBAS {
BitView::BitView(std::span<const uint8_t> bytes, size_t count) : bytes_(bytes), bit_count_(count) {
    if(count > bytes.size()*8) throw std::out_of_range("BitView exceeds storage");
}
uint64_t BitView::unsigned_at(size_t offset, size_t width) const {
    if(width > 64 || offset > bit_count_ || width > bit_count_-offset)
        throw std::out_of_range("Bit field exceeds message");
    uint64_t value = 0;
    for(size_t i = offset; i < offset+width; ++i)
        value = (value << 1) | ((bytes_[i/8] >> (7-i%8)) & 1);
    return value;
}
int64_t BitView::signed_at(size_t offset, size_t width) const {
    if(!width) throw std::out_of_range("Signed bit field is empty");
    uint64_t value = unsigned_at(offset, width);
    if(width < 64 && (value & (uint64_t{1} << (width-1))))
        value |= ~uint64_t{0} << width;
    return std::bit_cast<int64_t>(value);
}
uint32_t crc24q(std::span<const uint8_t> bytes, size_t count) {
    BitView bits(bytes, count);
    uint32_t crc = 0;
    for(size_t i = 0; i < count; ++i) {
        const bool feedback = ((crc >> 23) ^ bits.unsigned_at(i, 1)) & 1;
        crc = (crc << 1) & 0xffffff;
        if(feedback) crc ^= 0x864cfb;
    }
    return crc;
}
Result parse_l1(std::span<const uint8_t> bytes) {
    if(bytes.size()!=32) return {Status::invalid_word_count,std::nullopt};
    Message m;
    std::copy(bytes.begin(),bytes.end(),m.bytes.begin());
    m.padding_bits = m.bytes[31] & 0x3f;
    m.bytes[31] &= 0xc0;
    const BitView b(m.bytes, 250);
    m.preamble = b.unsigned_at(0, 8);
    m.type = b.unsigned_at(8, 6);
    m.received_crc = b.unsigned_at(226, 24);
    m.computed_crc = crc24q(m.bytes, 226);
    m.preamble_valid = m.preamble == 0x53 || m.preamble == 0x9a || m.preamble == 0xc6;
    m.crc_valid = m.received_crc == m.computed_crc;
    if(!m.preamble_valid) return {Status::invalid_preamble, m};
    if(!m.crc_valid) return {Status::invalid_crc, m};
    switch(m.type) {
    case 0: m.content = TestMode{}; break;
    case 63: m.content = NullMessage{}; break;
    case 1: {
        PrnMask v;
        v.iodp = b.unsigned_at(224, 2);
        for(size_t i = 0; i < v.mask.size(); ++i) v.mask[i] = b.unsigned_at(14+i, 1);
        m.content = v; break;
    }
    case 2: case 3: case 4: case 5: {
        FastCorrections v;
        v.iodf = b.unsigned_at(14, 2); v.iodp = b.unsigned_at(16, 2);
        v.first_mask_position = 1 + 13*(m.type-2);
        for(size_t i = 0; i < 13; ++i)
            v.satellites[i] = {int16_t(b.signed_at(18+12*i, 12)), uint8_t(b.unsigned_at(174+4*i, 4))};
        m.content = v; break;
    }
    case 6: {
        Integrity v;
        for(size_t i = 0; i < 4; ++i) v.iodf[i] = b.unsigned_at(14+2*i, 2);
        for(size_t i = 0; i < 51; ++i) v.udrei[i] = b.unsigned_at(22+4*i, 4);
        m.content = v; break;
    }
    case 7: {
        FastDegradation v;
        v.latency_s = b.unsigned_at(14, 4); v.iodp = b.unsigned_at(18, 2);
        for(size_t i = 0; i < 51; ++i) v.degradation_index[i] = b.unsigned_at(22+4*i, 4);
        m.content = v; break;
    }
    case 9: {
        GeoNavigation v;
        v.iodn_raw = b.unsigned_at(14, 8); v.t0_raw = b.unsigned_at(22, 13); v.ura = b.unsigned_at(35, 4);
        v.position_raw = {int32_t(b.signed_at(39,30)), int32_t(b.signed_at(69,30)), int32_t(b.signed_at(99,25))};
        v.velocity_raw = {int32_t(b.signed_at(124,17)), int32_t(b.signed_at(141,17)), int32_t(b.signed_at(158,18))};
        v.acceleration_raw = {int32_t(b.signed_at(176,10)), int32_t(b.signed_at(186,10)), int32_t(b.signed_at(196,10))};
        v.clock_offset_raw = b.signed_at(206,12); v.clock_drift_raw = b.signed_at(218,8);
        m.content = v; break;
    }
    case 18: {
        IonosphericMask v;
        v.number_of_bands_raw = b.unsigned_at(14,4); v.band = b.unsigned_at(18,4); v.iodi = b.unsigned_at(22,2);
        if(v.band > 10) return {Status::invalid_content, m};
        for(size_t i = 0; i < 201; ++i) v.mask[i] = b.unsigned_at(24+i,1);
        m.content = v; break;
    }
    case 26: {
        IonosphericDelay v;
        v.band = b.unsigned_at(14,4); v.block = b.unsigned_at(18,4); v.iodi = b.unsigned_at(217,2);
        if(v.band > 10 || v.block > 13) return {Status::invalid_content, m};
        for(size_t i = 0; i < 15; ++i)
            v.corrections[i] = {uint16_t(b.unsigned_at(22+13*i,9)), uint8_t(b.unsigned_at(31+13*i,4))};
        m.content = v; break;
    }
    default: return {Status::unsupported_message, m};
    }
    return {Status::decoded, m};
}
const char *status_name(Status s) {
    switch(s) {
    case Status::decoded: return "decoded";
    case Status::unsupported_signal: return "unsupported_signal";
    case Status::invalid_word_count: return "invalid_word_count";
    case Status::invalid_preamble: return "invalid_preamble";
    case Status::invalid_crc: return "invalid_crc";
    case Status::unsupported_message: return "unsupported_message";
    case Status::invalid_content: return "invalid_content";
    }
    return "unknown";
}
std::optional<std::pair<int,int>> igp_coordinate(unsigned band,unsigned mask_bit) {
    if(band>10 || !mask_bit) return std::nullopt;
    unsigned bit=0;
    if(band<9) {
        for(int column=0;column<8;++column) {
            const int lon=-180+int(band)*40+column*5;
            std::vector<int> latitudes;
            if(column%2==0) {
                if(lon==-140 || lon==-50 || lon==40 || lon==130) latitudes.push_back(-85);
                latitudes.push_back(-75); latitudes.push_back(-65);
            }
            for(int lat=-55;lat<=55;lat+=5) latitudes.push_back(lat);
            if(column%2==0) {
                latitudes.push_back(65); latitudes.push_back(75);
                if(lon==-180 || lon==-90 || lon==0 || lon==90) latitudes.push_back(85);
            }
            for(auto lat:latitudes) if(++bit==mask_bit) return {{lat,lon}};
        }
    } else {
        for(auto [lat,step]:std::array<std::pair<int,int>,5>{{{60,5},{65,10},{70,10},{75,10},{85,30}}})
            for(int lon=(band==10 && lat==85)?-170:-180;lon<180;lon+=step)
                if(++bit==mask_bit) return {{band==9?lat:-lat,lon}};
    }
    return std::nullopt;
}
}
