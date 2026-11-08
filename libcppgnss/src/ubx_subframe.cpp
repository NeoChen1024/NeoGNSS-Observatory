// SPDX-License-Identifier: GPL-3.0-only
#include <cppgnss/ubx_rxm_gen.hpp>
#include <cppgnss/ubx_subframe.hpp>

namespace UBX {
std::optional<uint16_t> SignalKey::prn() const {
    switch (gnssId) {
    case 0:
    case 2:
    case 3:
    case 7:
        return svId == 0 ? std::nullopt : std::optional<uint16_t>{svId};
    case 1:
        return svId >= 120 && svId <= 158 ? std::optional<uint16_t>{svId}
                                          : std::nullopt;
    case 5:
        return svId >= 1 && svId <= 10
                   ? std::optional<uint16_t>{uint16_t(svId + 192)}
                   : std::nullopt;
    default:
        return std::nullopt;
    }
}
std::optional<NavigationSubframe>
parse_subframe(const cppgnss::FrameView &frame) {
    // The generated decoder checks identity and the numWords-derived length.
    auto parsed = cppgnss::parse<ubx_rxm_sfrbx>(frame);
    if (!parsed)
        return std::nullopt;
    const auto &decoded = parsed.value();
    if (decoded.version != 2 || decoded.numWords == 0 || decoded.numWords > 16)
        return std::nullopt;
    NavigationSubframe result;
    result.signal = {decoded.gnssId, decoded.svId, decoded.sigId,
                     uint8_t(decoded.gnssId == 6 ? decoded.freqId : 0)};
    result.chn = decoded.chn;
    result.words.reserve(decoded.numWords);
    for (const auto &word : decoded.navdata_grp)
        result.words.push_back(word.dwrd);
    return result;
}
} // namespace UBX
