// SPDX-License-Identifier: GPL-3.0-only
#include <cppgnss/sbf.hpp>
#include <cppgnss/sbf_navigation_page_gen.hpp>

namespace cppgnss::SBF {
std::optional<SbasFrame> extract_sbas_l1(const FrameView &frame) {
    if (frame.protocol != Protocol::sbf || frame.id != 4020)
        return std::nullopt;
    auto parsed = cppgnss::parse<GEORawL1>(frame);
    if (!parsed)
        return std::nullopt;
    const auto &block = parsed.value();
    const auto svid = block.SVID;
    const auto signal = block.Source.SigIdx;
    if (signal != 24 ||
        !((svid >= 120 && svid <= 140) || (svid >= 198 && svid <= 215)))
        return std::nullopt;
    const auto &wire = block.NavBits;
    if (wire.size() != 32)
        return std::nullopt;
    std::array<uint8_t, 32> bits;
    // SBF stores eight little-endian U4 words containing MSB-first air bits.
    for (size_t i = 0; i < 8; ++i)
        for (size_t j = 0; j < 4; ++j)
            bits[4 * i + j] = wire[4 * i + 3 - j];
    return SbasFrame{block.TOW,
                     block.WNc,
                     uint16_t(svid <= 140 ? svid : svid - 57),
                     uint8_t(svid),
                     uint8_t(signal),
                     block.FreqNr,
                     block.RxChannel,
                     block.ViterbiCnt,
                     block.CRCPassed != 0,
                     cppgnss::SBAS::parse_l1(bits)};
}
} // namespace cppgnss::SBF
