// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <array>
#include <bit>
#include <cppgnss/sbf.hpp>
#include <cppgnss/sbf_navigation_page_gen.hpp>
#include <cppgnss/ubx_subframe.hpp>
#include <map>
#include <neognss_obs/raw_bits.hpp>
#include <stdexcept>
#include <string_view>

namespace neognss_obs {
namespace {
// Borrow receiver words; extract only the requested fields. No byte-per-bit
// expansion or temporary BCH codeword vectors are needed.
struct Bits {
    std::span<const uint8_t> bytes;
    std::span<const uint32_t> words;
    unsigned width = 32;
    size_t length = 0;
    bool inav_pair = false;
    size_t size() const { return length; }
    void resize(size_t n) {
        if (n > length)
            throw std::out_of_range("RawBits view outside receiver body");
        length = n;
    }
    uint32_t get(size_t begin, size_t end) const {
        if (begin > end || end > length || end - begin > 32)
            throw std::out_of_range("RawBits field outside receiver body");
        uint64_t result = 0;
        while (begin < end) {
            size_t physical = begin + (inav_pair && begin >= 114 ? 14 : 0);
            unsigned offset = physical % width;
            size_t count = std::min(end - begin, size_t(width - offset));
            if (inav_pair && begin < 114)
                count = std::min(count, 114 - begin);
            auto word =
                words.empty()
                    ? UBX::read_le<uint32_t>(bytes, (physical / width) * 4)
                    : words[physical / width];
            uint64_t mask = (uint64_t(1) << count) - 1;
            result =
                (result << count) | ((word >> (width - offset - count)) & mask);
            begin += count;
        }
        return uint32_t(result);
    }
};
Bits unpack(std::span<const uint8_t> bytes, unsigned width = 32) {
    return {bytes, {}, width, bytes.size() / 4 * width};
}
uint32_t value(const Bits &b, size_t begin, size_t end) {
    return b.get(begin, end);
}
std::string_view rinex_system(char system) {
    static constexpr std::string_view systems = "GECJS";
    return systems.substr(systems.find(system), 1);
}
void result(RawBits &r, std::string_view kind, std::string_view scope,
            bool pass) {
    r.checks.push_back(
        {"independent", kind, scope, pass ? "pass" : "fail", "computed", ""});
}
void receiver(RawBits &r, std::string_view kind, std::string_view scope,
              uint8_t v, std::string_view field = "CRCPassed") {
    r.checks.push_back({"receiver", kind, scope,
                        v == 1   ? "pass"
                        : v == 0 ? "fail"
                                 : "unknown",
                        "source_field", field});
}
// Over the packed body: integrity() runs after pack().
void crc(RawBits &r, size_t begin, size_t end, std::string_view scope) {
    result(r, "crc", scope, cppgnss::crc24q(r.body, begin, end) == 0);
}
void pack(RawBits &r, const Bits &b) {
    r.bit_length = b.size();
    r.body.assign((b.size() + 7) / 8, 0);
    for (size_t i = 0; i < b.size(); i += 8) {
        auto end = std::min(i + 8, b.size());
        r.body[i / 8] = value(b, i, end) << (8 - (end - i));
    }
}
void integrity(RawBits &r, const Bits &b) {
    if (r.format == "LNAV_300_V1") {
        const uint32_t masks[] = {0xBB1F3480, 0x5D8F9A40, 0xAEC7CD00,
                                  0x5763E680, 0x6BB1F340, 0x8B7A89C0};
        unsigned states = 15;
        for (unsigned i = 0; i < 10; ++i) {
            auto w = value(b, i * 30, (i + 1) * 30);
            unsigned next = 0;
            for (unsigned prev = 0; prev < 4; ++prev)
                if (states & (1u << prev)) {
                    auto parity = (w & 63) ^ ((prev & 1) ? 63 : 0);
                    unsigned calculated = 0;
                    for (auto mask : masks)
                        calculated =
                            (calculated << 1) |
                            (std::popcount(((prev << 30) | (w & 0x3fffffc0)) &
                                           mask) &
                             1);
                    if (calculated == parity)
                        next |= 1u << (parity & 3);
                }
            states = next;
        }
        result(r, "parity", "normalized_word_chain", states != 0);
    } else if (r.format == "D1D2_300_V1") {
        auto valid = [](uint32_t word) {
            for (int bit = 14; bit >= 4; --bit)
                if (word & (1u << bit))
                    word ^= 19u << (bit - 4);
            return word == 0;
        };
        bool pass = valid(value(b, 15, 30));
        for (size_t w = 30; w < 300; w += 30)
            for (size_t j = 0; j < 2; ++j) {
                auto cw = value(b, w + j * 11, w + j * 11 + 11);
                auto p = value(b, w + 22 + j * 4, w + 26 + j * 4);
                pass &= valid((cw << 4) | p);
            }
        result(r, "bch", "normalized_word_chain", pass);
    } else if (r.format == "BCNAV1_1800_V1") {
        crc(r, 72, 672, "sf2");
        crc(r, 1272, 1536, "sf3");
    } else if (r.format == "CNAV2_1800_V1") {
        crc(r, 52, 652, "sf2");
        crc(r, 1252, 1526, "sf3");
    } else if (r.format == "BCNAV2_576_V1")
        crc(r, 0, 288, "message");
    else if (r.format == "B2B_984_V1")
        crc(r, 12, 498, "message");
    else if (r.format == "INAV_228_V1")
        crc(r, 0, 220, "page");
    else if (r.format != "L6_2000_V1" && r.format != "QZS_L5S_250_V1")
        crc(r, 0, r.bit_length, r.unit);
}
// Normalize typed receiver headers; navigation-body decoding remains separate.
struct NavigationPage {
    uint32_t tow;
    uint16_t week;
    uint8_t satellite, source, channel, crc1, crc2 = 0;
    std::optional<uint8_t> viterbi, rs;
    std::vector<uint8_t> bits;
};
template <class T>
cppgnss::ParseResult<NavigationPage>
navigation_page(const cppgnss::FrameView &frame) {
    auto parsed = cppgnss::parse<T>(frame);
    if (!parsed)
        return parsed.error();
    auto &v = parsed.value();
    NavigationPage page{};
    page.tow = v.TOW;
    page.week = v.WNc;
    page.satellite = v.SVID;
    page.source = v.Source.SigIdx;
    page.channel = v.RxChannel;
    if constexpr (requires { v.Source.L1BCFlag; })
        page.source |= v.Source.L1BCFlag << 5;
    if constexpr (requires { v.CRCSF2; }) {
        page.crc1 = v.CRCSF2;
        page.crc2 = v.CRCSF3;
    } else if constexpr (requires { v.Parity; })
        page.crc1 = v.Parity;
    else
        page.crc1 = v.CRCPassed;
    if constexpr (requires { v.ViterbiCnt; })
        page.viterbi = v.ViterbiCnt;
    if constexpr (requires { v.RSCnt; })
        page.rs = v.RSCnt;
    if constexpr (requires { v.NavBits; })
        page.bits = std::move(v.NavBits);
    else
        page.bits = std::move(v.NAVBits);
    return cppgnss::ParsedMessage<NavigationPage>{std::move(page),
                                                  parsed.consumed()};
}
std::optional<cppgnss::ParseResult<NavigationPage>>
navigation_page(const cppgnss::FrameView &frame) {
    switch (frame.id()) {
    case uint16_t(cppgnss::SBF::GPSRawCA::message_id):
        return navigation_page<cppgnss::SBF::GPSRawCA>(frame);
    case uint16_t(cppgnss::SBF::GPSRawL2C::message_id):
        return navigation_page<cppgnss::SBF::GPSRawL2C>(frame);
    case uint16_t(cppgnss::SBF::GPSRawL5::message_id):
        return navigation_page<cppgnss::SBF::GPSRawL5>(frame);
    case uint16_t(cppgnss::SBF::GEORawL1::message_id):
        return navigation_page<cppgnss::SBF::GEORawL1>(frame);
    case uint16_t(cppgnss::SBF::GEORawL5::message_id):
        return navigation_page<cppgnss::SBF::GEORawL5>(frame);
    case uint16_t(cppgnss::SBF::GALRawFNAV::message_id):
        return navigation_page<cppgnss::SBF::GALRawFNAV>(frame);
    case uint16_t(cppgnss::SBF::GALRawINAV::message_id):
        return navigation_page<cppgnss::SBF::GALRawINAV>(frame);
    case uint16_t(cppgnss::SBF::GALRawCNAV::message_id):
        return navigation_page<cppgnss::SBF::GALRawCNAV>(frame);
    case uint16_t(cppgnss::SBF::BDSRaw::message_id):
        return navigation_page<cppgnss::SBF::BDSRaw>(frame);
    case uint16_t(cppgnss::SBF::QZSRawL1CA::message_id):
        return navigation_page<cppgnss::SBF::QZSRawL1CA>(frame);
    case uint16_t(cppgnss::SBF::QZSRawL2C::message_id):
        return navigation_page<cppgnss::SBF::QZSRawL2C>(frame);
    case uint16_t(cppgnss::SBF::QZSRawL5::message_id):
        return navigation_page<cppgnss::SBF::QZSRawL5>(frame);
    case uint16_t(cppgnss::SBF::QZSRawL6::message_id):
        return navigation_page<cppgnss::SBF::QZSRawL6>(frame);
    case uint16_t(cppgnss::SBF::BDSRawB1C::message_id):
        return navigation_page<cppgnss::SBF::BDSRawB1C>(frame);
    case uint16_t(cppgnss::SBF::BDSRawB2a::message_id):
        return navigation_page<cppgnss::SBF::BDSRawB2a>(frame);
    case uint16_t(cppgnss::SBF::GPSRawL1C::message_id):
        return navigation_page<cppgnss::SBF::GPSRawL1C>(frame);
    case uint16_t(cppgnss::SBF::QZSRawL1C::message_id):
        return navigation_page<cppgnss::SBF::QZSRawL1C>(frame);
    case uint16_t(cppgnss::SBF::QZSRawL1S::message_id):
        return navigation_page<cppgnss::SBF::QZSRawL1S>(frame);
    case uint16_t(cppgnss::SBF::BDSRawB2b::message_id):
        return navigation_page<cppgnss::SBF::BDSRawB2b>(frame);
    case uint16_t(cppgnss::SBF::QZSRawL5S::message_id):
        return navigation_page<cppgnss::SBF::QZSRawL5S>(frame);
    case uint16_t(cppgnss::SBF::QZSRawL6D::message_id):
        return navigation_page<cppgnss::SBF::QZSRawL6D>(frame);
    case uint16_t(cppgnss::SBF::QZSRawL6E::message_id):
        return navigation_page<cppgnss::SBF::QZSRawL6E>(frame);
    default:
        return {};
    }
}
RawBitsResult decode(const cppgnss::FrameView &f) {
    if (f.protocol() != cppgnss::Protocol::ubx &&
        f.protocol() != cppgnss::Protocol::sbf)
        return {};
    RawBits r;
    Bits b;
    unsigned width = 32;
    bool sbf = f.protocol() == cppgnss::Protocol::sbf;
    std::vector<uint8_t> navigation_bytes;
    std::vector<uint32_t> navigation_words;
    uint8_t source = 0, crc1 = 0, crc2 = 0;
    if (sbf) {
        if (f.id() == 4026 || f.id() == 4093)
            return {RawBitsStatus::excluded, {}};
        auto parsed = navigation_page(f);
        if (!parsed)
            return {};
        if (!*parsed)
            return {parsed->error().code ==
                            cppgnss::ParseErrorCode::UNSUPPORTED_REVISION
                        ? RawBitsStatus::unsupported
                        : RawBitsStatus::malformed,
                    {}};
        auto page = std::move(*parsed).value();
        source = page.source;
        crc1 = page.crc1;
        crc2 = page.crc2;
        r.receiver_channel = page.channel;
        switch (f.id()) {
        case 4018:
        case 4019:
        case 4020:
        case 4021:
        case 4022:
        case 4023:
        case 4024:
        case 4067:
        case 4068:
        case 4228:
        case 4246:
            r.viterbi_count = page.viterbi;
            break;
        case 4069:
        case 4270:
        case 4271:
            r.rs_corrected_symbols = page.rs;
            break;
        }
        // Source is a full byte in modern blocks, a five-bit signal plus
        // flags in legacy blocks, and a block-specific enum for L6.
        unsigned sig = f.id() <= 4024 || f.id() == 4067 || f.id() == 4068
                           ? source & 31
                           : source;
        static const std::map<uint16_t, std::vector<unsigned>> allowed{
            {4017, {0}},      {4018, {3}},  {4019, {4}},
            {4020, {24}},     {4021, {25}}, {4022, {20}},
            {4023, {17, 21}}, {4024, {19}}, {4047, {28, 29, 30}},
            {4066, {6, 38}},  {4067, {7}},  {4068, {26}},
            {4218, {13}},     {4219, {14}}, {4242, {34}},
            {4221, {5}},      {4227, {32}}, {4228, {33}},
            {4246, {39}},     {4270, {1}},  {4271, {2}},
            {4069, {0, 1, 2}}};
        if (auto it = allowed.find(f.id());
            it != allowed.end() &&
            std::find(it->second.begin(), it->second.end(), sig) ==
                it->second.end())
            return {RawBitsStatus::unsupported, {}};
        r.gpst_ms = cppgnss::SBF::gpst_ms(page.tow, page.week);
        const auto satellite = cppgnss::SBF::satellite(page.satellite);
        if (!satellite)
            return {RawBitsStatus::unsupported, {}};
        r.system = rinex_system(satellite->system);
        r.satellite = satellite->number;
        switch (f.id()) {
        case 4017:
            r.family = "GPS_LNAV";
            r.signals = {"GPS_L1_CA"};
            width = 30;
            break;
        case 4066:
            r.family = "QZS_LNAV";
            r.signals = {source == 38 ? "QZS_L1_CB" : "QZS_L1_CA"};
            width = 30;
            break;
        case 4018:
            r.family = "GPS_CNAV";
            r.signals = {"GPS_L2C"};
            break;
        case 4019:
            r.family = "GPS_CNAV";
            r.signals = {"GPS_L5_I"};
            break;
        case 4067:
            r.family = "QZS_CNAV";
            r.signals = {"QZS_L2C"};
            break;
        case 4068:
            r.family = "QZS_CNAV";
            r.signals = {"QZS_L5_I"};
            break;
        case 4020:
            r.family = "SBAS_L1";
            r.signals = {"SBAS_L1"};
            break;
        case 4021:
            r.family = "SBAS_L5";
            r.signals = {"SBAS_L5"};
            break;
        case 4022:
            r.family = "GAL_FNAV";
            r.signals = {"GAL_E5A_I"};
            break;
        case 4023:
            r.family = "GAL_INAV";
            r.signals = {(source & 31) == 21 ? "GAL_E5B_I" : "GAL_E1_B"};
            if (source & 32)
                r.signals = {"GAL_E1_B", "GAL_E5B_I"};
            break;
        case 4024:
            r.family = "GAL_CNAV";
            r.signals = {"GAL_E6_B"};
            break;
        case 4047:
            // B1I 3.0 / B3I 1.0 ranging-code tables assign 1-5 and
            // 59-63 to GEO (D2), and 6-58 to MEO/IGSO (D1).
            // Family identity does not imply that the BCH check passed.
            r.family = r.satellite >= 6 && r.satellite <= 58 ? "BDS_D1"
                       : (r.satellite >= 1 && r.satellite <= 5) ||
                               (r.satellite >= 59 && r.satellite <= 63)
                           ? "BDS_D2"
                           : "BDS_D1D2_UNCLASSIFIED";
            r.signals = {source == 28   ? "BDS_B1I"
                         : source == 29 ? "BDS_B2I"
                                        : "BDS_B3I"};
            break;
        case 4218:
            r.family = "BDS_BCNAV1";
            r.signals = {"BDS_B1C"};
            break;
        case 4219:
            r.family = "BDS_BCNAV2";
            r.signals = {"BDS_B2A"};
            break;
        case 4242:
            r.family = "BDS_B2B_UNCLASSIFIED";
            r.signals = {"BDS_B2B"};
            break;
        case 4221:
            r.family = "GPS_CNAV2";
            r.signals = {"GPS_L1C"};
            break;
        case 4227:
            r.family = "QZS_CNAV2";
            r.signals = {"QZS_L1C"};
            break;
        case 4228:
            r.family = "QZS_L1S";
            r.signals = {"QZS_L1S"};
            break;
        case 4246:
            r.family = "QZS_L5S";
            r.signals = {"QZS_L5S"};
            break;
        case 4069:
        case 4270:
        case 4271:
            r.family = "QZS_L6_UNCLASSIFIED";
            if (f.id() != 4069)
                r.signals = {f.id() == 4270 ? "QZS_L6D" : "QZS_L6E"};
            else if (source != 0)
                r.signals = {source == 1 ? "QZS_L6D" : "QZS_L6E"};
            break;
        }
        navigation_bytes = std::move(page.bits);
        b = unpack(navigation_bytes, width);
    } else {
        if (f.id() != 0x0213)
            return {};
        auto parsed = UBX::parse_subframe(f);
        if (!parsed)
            return {RawBitsStatus::malformed, {}};
        const auto &s = *parsed;
        r.receiver_channel = s.chn;
        auto g = s.signal.gnssId, sig = s.signal.sigId;
        if (g == 6 || g == 7)
            return {RawBitsStatus::excluded, {}};
        auto satellite = s.signal.satellite();
        if (!satellite)
            return {RawBitsStatus::unsupported, {}};
        r.satellite = *satellite;
        if (g == 0 || g == 5) {
            const bool gps = g == 0;
            r.system = gps ? "G" : "J";
            if (sig == 0) {
                r.family = gps ? "GPS_LNAV" : "QZS_LNAV";
                r.signals = {gps ? "GPS_L1_CA" : "QZS_L1_CA"};
                width = 30;
            } else if ((gps && (sig == 3 || sig == 4)) ||
                       (!gps && (sig == 4 || sig == 5))) {
                r.family = gps ? "GPS_CNAV" : "QZS_CNAV";
                r.signals = {gps ? "GPS_L2C" : "QZS_L2C"};
            } else if ((gps && sig == 6) || (!gps && sig == 8)) {
                r.family = gps ? "GPS_CNAV" : "QZS_CNAV";
                r.signals = {gps ? "GPS_L5_I" : "QZS_L5_I"};
            } else if (g == 5 && sig == 1) {
                r.family = "QZS_L1S";
                r.signals = {"QZS_L1S"};
            }
        } else if (g == 1 && sig == 0) {
            r.system = "S";
            r.family = "SBAS_L1";
            r.signals = {"SBAS_L1"};
        } else if (g == 2) {
            r.system = "E";
            if (sig == 1 || sig == 5) {
                r.family = "GAL_INAV";
                r.signals = {sig == 1 ? "GAL_E1_B" : "GAL_E5B_I"};
            } else if (sig == 3) {
                r.family = "GAL_FNAV";
                r.signals = {"GAL_E5A_I"};
            }
        } else if (g == 3) {
            r.system = "C";
            if (sig <= 4 || sig == 10) {
                r.family =
                    (sig == 1 || sig == 3 || sig == 10) ? "BDS_D2" : "BDS_D1";
                r.signals = {sig <= 1   ? "BDS_B1I"
                             : sig <= 3 ? "BDS_B2I"
                                        : "BDS_B3I"};
                width = 30;
            }
        }
        if (r.family.empty())
            return {RawBitsStatus::unsupported, {}};
        navigation_words = std::move(parsed->words);
        b = {{}, navigation_words, width, navigation_words.size() * width};
    }
    const auto *layout = raw_bits_layout(r.family);
    if (!layout)
        throw std::logic_error("RawBits family without format mapping");
    r.format = layout->format;
    r.unit = layout->unit;
    r.content = layout->content();
    const size_t length = layout->bit_length;
    const size_t words =
        r.format == "LNAV_300_V1" || (!sbf && r.format == "D1D2_300_V1") ? 10
        : !sbf && r.format == "INAV_228_V1" ? 8
                                            : (length + 31) / 32;
    const size_t actual_words =
        sbf ? navigation_bytes.size() / 4 : navigation_words.size();
    if (actual_words != words &&
        !(!sbf && (r.family == "SBAS_L1" || r.family == "QZS_L1S") &&
          actual_words == words + 1))
        return {RawBitsStatus::malformed, {}};
    if (layout->system != r.system)
        return {RawBitsStatus::unsupported, {}};
    // The word count above bounds every view below; Bits throws only for
    // internal offset errors. Galileo I/NAV packing is defined by the
    // receiver, not potentially corrupt page discriminator bits: preserve the
    // supplied pair even when checks fail.
    b.inav_pair = !sbf && r.format == "INAV_228_V1";
    b.resize(length);
    pack(r, b);
    integrity(r, b);
    if (sbf && r.family == "BDS_B2B_UNCLASSIFIED" && crc1 == 1 &&
        value(b, 0, 6) == r.satellite &&
        std::any_of(r.checks.begin(), r.checks.end(), [](const auto &c) {
            return c.origin == "independent" && c.kind == "crc" &&
                   c.scope == "message" && c.result == "pass";
        })) {
        // July 2020 B2b/PPP-B2b ICD type assignments. The unprotected
        // prefix is only a consistency gate, never a service discriminator.
        // Preserve reserved types and failed checks without guessing a family.
        const auto type = value(b, 12, 18);
        if (type == 10 || type == 30 || type == 40)
            r.family = "BDS_BCNAV3";
        else if (((type >= 1 && type <= 7) || type == 63) &&
                 bds_ppp_b2b_code_assigned(r.satellite))
            r.family = "BDS_PPP_B2B";
    }
    if (sbf) {
        if (length == 1800) {
            receiver(r, "crc", "sf2", crc1, "CRCSF2");
            receiver(r, "crc", "sf3", crc2, "CRCSF3");
        } else
            receiver(r,
                     length == 2000              ? "reed_solomon"
                     : r.format == "LNAV_300_V1" ? "parity"
                     : r.format == "D1D2_300_V1" ? "bch"
                                                 : "crc",
                     r.unit, crc1, length == 2000 ? "Parity" : "CRCPassed");
    }
    return {RawBitsStatus::decoded, std::move(r)};
}
} // namespace
RawBitsResult decode_raw_bits(const cppgnss::FrameView &frame) {
    return decode(frame);
}
} // namespace neognss_obs
