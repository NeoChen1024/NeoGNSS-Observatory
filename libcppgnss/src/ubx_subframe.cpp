// SPDX-License-Identifier: GPL-3.0-only
#include <cppgnss/ubx_subframe.hpp>
#include <cppgnss/ubx_rxm_gen.hpp>

namespace UBX {
cppgnss::SBAS::Result parse_sbas(const NavigationSubframe &s) {
    using namespace cppgnss::SBAS;
    if(s.version != 2 || s.signal.gnssId != 1 || s.signal.sigId != 0)
        return {Status::unsupported_signal, std::nullopt};
    // Nine-word reports occur in the archive. Only the first eight words
    // carry the standard frame; retain the unexplained ninth word verbatim.
    if(s.words.size() != 8 && s.words.size() != 9)
        return {Status::invalid_word_count, std::nullopt};
    std::array<uint8_t,32> bytes{};
    // UBX transports little-endian U4 values. Their decoded values contain
    // consecutive MSB-first SBAS bits, unlike the legacy RXM-SFRB last word.
    for(size_t i = 0; i < 8; ++i)
        for(size_t j = 0; j < 4; ++j) bytes[4*i+j] = uint8_t(s.words[i] >> (24-8*j));
    auto result = parse_l1(bytes);
    if(result.message && s.words.size()==9) result.message->trailing_word=s.words[8];
    return result;
}

std::optional<uint16_t> SignalKey::prn() const {
    switch(gnssId) {
    case 0: case 2: case 3: case 7:
        return svId == 0 ? std::nullopt : std::optional<uint16_t>{svId};
    case 1:
        return svId >= 120 && svId <= 158 ? std::optional<uint16_t>{svId} : std::nullopt;
    case 5:
        return svId >= 1 && svId <= 10 ? std::optional<uint16_t>{uint16_t(svId + 192)} : std::nullopt;
    default: return std::nullopt;
    }
}
SubframeResult parse_subframe(const ubx_frame &frame) {
    if(!frame.valid || frame.payload.size() != frame.length)
        return {SubframeStatus::invalid_frame, std::nullopt};
    if(frame.class_id != UBX_CLASS_RXM || frame.msg_id != UBX_RXM_SFRBX)
        return {SubframeStatus::not_sfrbx, std::nullopt};
    if(frame.payload.size() < 8)
        return {SubframeStatus::invalid_length, std::nullopt};
    if(frame.payload[6] != 2)
        return {SubframeStatus::unsupported_version, std::nullopt};
    const size_t n = frame.payload[4];
    if(n == 0 || n > 16 || frame.payload.size() != 8 + 4*n)
        return {SubframeStatus::invalid_length, std::nullopt};
    const ubx_rxm_sfrbx decoded(frame);
    if(!decoded.valid) return {SubframeStatus::invalid_frame, std::nullopt};
    NavigationSubframe result;
    result.signal = {decoded.gnssId, decoded.svId, decoded.sigId,
                     uint8_t(decoded.gnssId == 6 ? decoded.freqId : 0)};
    result.version = decoded.version;
    result.chn = decoded.chn;
    result.raw_freqId = decoded.freqId;
    result.reserved0 = decoded.reserved0;
    result.words.reserve(n);
    for(const auto &word : decoded.navdata_grp) result.words.push_back(word.dwrd);
    return {SubframeStatus::decoded, std::move(result)};
}
SubframeResult SubframeDemultiplexer::dispatch(const ubx_frame &frame, const Sink &sink) {
    auto result = parse_subframe(frame);
    if(result.subframe) {
        ++counts_[result.subframe->signal];
        sink(*result.subframe);
    }
    return result;
}
}
