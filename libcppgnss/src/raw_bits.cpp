// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <array>
#include <bit>
#include <cppgnss/raw_bits.hpp>
#include <cppgnss/sbas.hpp>
#include <cppgnss/ubx_subframe.hpp>
#include <map>

namespace cppgnss {
namespace {
using Bits = std::vector<uint8_t>;
Bits unpack(std::span<const uint8_t> bytes, unsigned width = 32) {
    Bits out;
    for (size_t i = 0; i + 4 <= bytes.size(); i += 4) {
        uint32_t w = UBX::read_le<uint32_t>(bytes, i);
        for (unsigned j = width; j > 0; --j)
            out.push_back((w >> (j - 1)) & 1);
    }
    return out;
}
Bits slice(const Bits &b, size_t begin, size_t end) {
    if (end > b.size() || begin > end)
        throw std::out_of_range("RawBits slice outside receiver body");
    return {b.begin() + begin, b.begin() + end};
}
uint32_t value(const Bits &b, size_t begin, size_t end) {
    uint32_t v = 0;
    for (size_t i = begin; i < end; ++i)
        v = (v << 1) | b.at(i);
    return v;
}
void result(RawBits &r, std::string kind, std::string scope, bool pass) {
    r.checks.push_back(
        {"independent", kind, scope, pass ? "pass" : "fail", "computed", ""});
}
void receiver(RawBits &r, std::string kind, std::string scope, uint8_t v,
              std::string field = "CRCPassed") {
    r.checks.push_back({"receiver", kind, scope,
                        v == 1   ? "pass"
                        : v == 0 ? "fail"
                                 : "unknown",
                        "source_field", field});
}
void crc(RawBits &r, const Bits &b, size_t begin, size_t end,
         std::string scope) {
    uint32_t c = 0;
    for (size_t i = begin; i < end; ++i) {
        auto top = (c >> 23) ^ b.at(i);
        c = (c << 1) & 0xffffff;
        if (top)
            c ^= 0x864cfb;
    }
    result(r, "crc", scope, c == 0);
}
void pack(RawBits &r, const Bits &b) {
    r.bit_length = b.size();
    r.body.assign((b.size() + 7) / 8, 0);
    for (size_t i = 0; i < b.size(); ++i)
        r.body[i / 8] |= b[i] << (7 - i % 8);
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
        auto valid = [](Bits word) {
            uint32_t rem = 0;
            for (auto bit : word) {
                rem = (rem << 1) | bit;
                if (rem & 16)
                    rem ^= 19;
            }
            return rem == 0;
        };
        bool pass = valid(slice(b, 15, 30));
        for (size_t w = 30; w < 300; w += 30)
            for (size_t j = 0; j < 2; ++j) {
                auto cw = slice(b, w + j * 11, w + j * 11 + 11);
                auto p = slice(b, w + 22 + j * 4, w + 26 + j * 4);
                cw.insert(cw.end(), p.begin(), p.end());
                pass &= valid(cw);
            }
        result(r, "bch", "normalized_word_chain", pass);
    } else if (r.format == "BCNAV1_1800_V1") {
        crc(r, b, 72, 672, "sf2");
        crc(r, b, 1272, 1536, "sf3");
    } else if (r.format == "CNAV2_1800_V1") {
        crc(r, b, 52, 652, "sf2");
        crc(r, b, 1252, 1526, "sf3");
    } else if (r.format == "BCNAV2_576_V1")
        crc(r, b, 0, 288, "message");
    else if (r.format == "B2B_984_V1")
        crc(r, b, 12, 498, "message");
    else if (r.format == "INAV_228_V1")
        crc(r, b, 0, 220, "page");
    else if (r.format != "L6_2000_V1" && r.format != "QZS_L5S_250_V1")
        crc(r, b, 0, b.size(), r.unit);
}
RawBitsResult decode(const FrameView &f) {
    RawBits r;
    Bits b;
    size_t length = 0;
    unsigned width = 32;
    bool sbf = f.protocol == Protocol::sbf;
    auto p = f.payload;
    uint8_t source = 0;
    if (sbf) {
        if (f.id == 4026 || f.id == 4093)
            return {RawBitsStatus::excluded, {}};
        const std::array<uint16_t, 22> ids{
            4017, 4018, 4019, 4020, 4021, 4022, 4023, 4024, 4047, 4066, 4067,
            4068, 4069, 4218, 4219, 4221, 4227, 4228, 4242, 4246, 4270, 4271};
        if (std::find(ids.begin(), ids.end(), f.id) == ids.end())
            return {};
        if (p.size() < 12)
            return {RawBitsStatus::malformed, {}};
        if (f.revision != 0)
            return {RawBitsStatus::unsupported, {}};
        source = p[9];
        r.receiver_channel = p[11];
        switch (f.id) {
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
            r.viterbi_count = p[8];
            break;
        case 4069:
        case 4270:
        case 4271:
            r.rs_corrected_symbols = p[8];
            break;
        }
        // Source is a full byte in modern blocks, a five-bit signal plus
        // flags in legacy blocks, and a block-specific enum for L6.
        unsigned sig =
            f.id <= 4024 || f.id == 4067 || f.id == 4068 ? source & 31 : source;
        static const std::map<uint16_t, std::vector<unsigned>> allowed{
            {4017, {0}},      {4018, {3}},  {4019, {4}},
            {4020, {24}},     {4021, {25}}, {4022, {20}},
            {4023, {17, 21}}, {4024, {19}}, {4047, {28, 29, 30}},
            {4066, {6, 38}},  {4067, {7}},  {4068, {26}},
            {4218, {13}},     {4219, {14}}, {4242, {34}},
            {4221, {5}},      {4227, {32}}, {4228, {33}},
            {4246, {39}},     {4270, {1}},  {4271, {2}},
            {4069, {0, 1, 2}}};
        if (auto it = allowed.find(f.id);
            it != allowed.end() &&
            std::find(it->second.begin(), it->second.end(), sig) ==
                it->second.end())
            return {RawBitsStatus::unsupported, {}};
        auto sv = p[6];
        auto tow = UBX::read_le<uint32_t>(p, 0);
        auto week = UBX::read_le<uint16_t>(p, 4);
        if (tow < 604800000 && week != 65535)
            r.gpst_ms = int64_t(week) * 604800000 + tow;
        if (sv >= 1 && sv <= 37) {
            r.system = "GPS";
            r.satellite = sv;
        } else if (sv >= 71 && sv <= 106) {
            r.system = "GAL";
            r.satellite = sv - 70;
        } else if ((sv >= 120 && sv <= 140) || (sv >= 198 && sv <= 215)) {
            r.system = "SBAS";
            r.satellite = sv <= 140 ? sv : sv - 57;
        } else if (sv >= 141 && sv <= 180) {
            r.system = "BDS";
            r.satellite = sv - 140;
        } else if (sv >= 223 && sv <= 245) {
            r.system = "BDS";
            r.satellite = sv - 182;
        } else if (sv >= 181 && sv <= 190) {
            r.system = "QZS";
            r.satellite = sv + 12;
        } else
            return {RawBitsStatus::unsupported, {}};
        switch (f.id) {
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
            r.family = "BDS_D1D2_UNCLASSIFIED";
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
            if (f.id != 4069)
                r.signals = {f.id == 4270 ? "QZS_L6D" : "QZS_L6E"};
            else if (source != 0)
                r.signals = {source == 1 ? "QZS_L6D" : "QZS_L6E"};
            break;
        }
        b = unpack(p.subspan(12), width);
    } else {
        if (f.id != 0x0213)
            return {};
        auto parsed = UBX::parse_subframe(UBX::ubx_frame(f.wire.subspan(2)));
        if (!parsed.subframe)
            return {RawBitsStatus::malformed, {}};
        const auto &s = *parsed.subframe;
        r.receiver_channel = s.chn;
        auto g = s.signal.gnssId, sig = s.signal.sigId;
        if (g == 6 || g == 7)
            return {RawBitsStatus::excluded, {}};
        auto satellite = s.signal.prn();
        if (!satellite)
            return {RawBitsStatus::unsupported, {}};
        r.satellite = *satellite;
        if (g == 0 || g == 5) {
            r.system = g == 0 ? "GPS" : "QZS";
            if (sig == 0) {
                r.family = r.system + "_LNAV";
                r.signals = {r.system + "_L1_CA"};
                width = 30;
            } else if ((g == 0 && (sig == 3 || sig == 4)) ||
                       (g == 5 && (sig == 4 || sig == 5))) {
                r.family = r.system + "_CNAV";
                r.signals = {r.system + "_L2C"};
            } else if ((g == 0 && sig == 6) || (g == 5 && sig == 8)) {
                r.family = r.system + "_CNAV";
                r.signals = {r.system + "_L5_I"};
            } else if (g == 5 && sig == 1) {
                r.family = "QZS_L1S";
                r.signals = {"QZS_L1S"};
            }
        } else if (g == 1 && sig == 0) {
            r.system = "SBAS";
            r.family = "SBAS_L1";
            r.signals = {"SBAS_L1"};
        } else if (g == 2) {
            r.system = "GAL";
            if (sig == 1 || sig == 5) {
                r.family = "GAL_INAV";
                r.signals = {sig == 1 ? "GAL_E1_B" : "GAL_E5B_I"};
            } else if (sig == 3) {
                r.family = "GAL_FNAV";
                r.signals = {"GAL_E5A_I"};
            }
        } else if (g == 3) {
            r.system = "BDS";
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
        b = unpack(p.subspan(8), width);
    }
    if (r.family.ends_with("_LNAV")) {
        r.format = "LNAV_300_V1";
        length = 300;
        r.unit = "subframe";
    } else if (r.family == "GPS_CNAV" || r.family == "QZS_CNAV") {
        r.format = "CNAV_300_V1";
        length = 300;
    } else if (r.family == "GAL_INAV") {
        r.format = "INAV_228_V1";
        length = 228;
        r.unit = "page";
        if (!sbf) {
            auto second = slice(b, 128, 242);
            b = slice(b, 0, 114);
            b.insert(b.end(), second.begin(), second.end());
        }
        // Packing is defined by the receiver, not potentially corrupt page
        // discriminator bits. Preserve the supplied pair even when checks fail.
    } else if (r.family == "GAL_FNAV") {
        r.format = "FNAV_238_V1";
        length = 238;
        r.unit = "page";
    } else if (r.family == "GAL_CNAV") {
        r.format = "CNAV_PAGE_486_V1";
        length = 486;
        r.unit = "page";
    } else if (r.family == "BDS_D1" || r.family == "BDS_D2" ||
               r.family == "BDS_D1D2_UNCLASSIFIED") {
        r.format = "D1D2_300_V1";
        length = 300;
        r.unit = "subframe";
    } else if (r.family == "BDS_BCNAV1") {
        r.format = "BCNAV1_1800_V1";
        length = 1800;
    } else if (r.family == "BDS_BCNAV2") {
        r.format = "BCNAV2_576_V1";
        length = 576;
    } else if (r.family == "BDS_B2B_UNCLASSIFIED") {
        r.format = "B2B_984_V1";
        length = 984;
    } else if (r.family.ends_with("_CNAV2")) {
        r.format = "CNAV2_1800_V1";
        length = 1800;
    } else if (r.family == "QZS_L6_UNCLASSIFIED") {
        r.format = "L6_2000_V1";
        length = 2000;
    } else {
        r.format = r.family + "_250_V1";
        length = 250;
    }
    if (length >= 576)
        r.content = "binary_symbols";
    const size_t words =
        r.format == "LNAV_300_V1" || (!sbf && r.format == "D1D2_300_V1") ? 10
        : !sbf && r.format == "INAV_228_V1"                              ? 8
                                            : (length + 31) / 32;
    const size_t expected = (sbf ? 12 : 8) + words * 4;
    if (p.size() != expected &&
        !(!sbf && (r.family == "SBAS_L1" || r.family == "QZS_L1S") &&
          p.size() == expected + 4))
        return {RawBitsStatus::malformed, {}};
    if (r.family.substr(0, r.family.find('_')) != r.system)
        return {RawBitsStatus::unsupported, {}};
    if (b.size() < length)
        return {RawBitsStatus::malformed, {}};
    b.resize(length);
    pack(r, b);
    integrity(r, b);
    if (sbf) {
        if (length == 1800) {
            receiver(r, "crc", "sf2", p[7], "CRCSF2");
            receiver(r, "crc", "sf3", p[8], "CRCSF3");
        } else
            receiver(r,
                     length == 2000              ? "reed_solomon"
                     : r.format == "LNAV_300_V1" ? "parity"
                     : r.format == "D1D2_300_V1" ? "bch"
                                                 : "crc",
                     r.unit, p[7], length == 2000 ? "Parity" : "CRCPassed");
    }
    if (r.system == "GPS")
        r.system = "G";
    else if (r.system == "GAL")
        r.system = "E";
    else if (r.system == "BDS")
        r.system = "C";
    else if (r.system == "QZS") {
        r.system = "J";
        r.satellite -= 192;
    } else if (r.system == "SBAS") {
        r.system = "S";
        r.satellite -= 100;
    }
    return {RawBitsStatus::decoded, std::move(r)};
}
} // namespace
RawBitsResult decode_raw_bits(const FrameView &frame) {
    try {
        return decode(frame);
    } catch (const std::out_of_range &) {
        return {RawBitsStatus::malformed, {}};
    }
}
} // namespace cppgnss
