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
SubframeResult parse_subframe(const cppgnss::FrameView &frame) {
    if (frame.protocol() != cppgnss::Protocol::ubx)
        return {SubframeStatus::invalid_frame, std::nullopt};
    if (frame.id() != static_cast<uint16_t>(cppgnss::UbxMessageId::RXM_SFRBX))
        return {SubframeStatus::not_sfrbx, std::nullopt};
    if (frame.payload.size() < 8)
        return {SubframeStatus::invalid_length, std::nullopt};
    if (frame.payload[6] != 2)
        return {SubframeStatus::unsupported_version, std::nullopt};
    const size_t n = frame.payload[4];
    if (n == 0 || n > 16 || frame.payload.size() != 8 + 4 * n)
        return {SubframeStatus::invalid_length, std::nullopt};
    auto parsed = cppgnss::parse<ubx_rxm_sfrbx>(frame);
    if (!parsed)
        return {SubframeStatus::invalid_frame, std::nullopt};
    const auto &decoded = parsed.value();
    NavigationSubframe result;
    result.signal = {decoded.gnssId, decoded.svId, decoded.sigId,
                     uint8_t(decoded.gnssId == 6 ? decoded.freqId : 0)};
    result.version = decoded.version;
    result.chn = decoded.chn;
    result.raw_freqId = decoded.freqId;
    result.reserved0 = decoded.reserved0;
    result.words.reserve(n);
    for (const auto &word : decoded.navdata_grp)
        result.words.push_back(word.dwrd);
    return {SubframeStatus::decoded, std::move(result)};
}
} // namespace UBX
