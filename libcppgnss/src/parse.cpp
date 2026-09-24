// SPDX-License-Identifier: GPL-3.0-only
#include <cppgnss/parse.hpp>
#include <format>
namespace cppgnss {
std::string dump_raw(const FrameView &frame, const ParseError *error) {
    auto protocol = frame.protocol();
    auto text = std::format("({}", protocol == Protocol::ubx     ? "UBX"
                                   : protocol == Protocol::sbf   ? "SBF"
                                   : protocol == Protocol::rtcm3 ? "RTCM3"
                                                                 : "NMEA");
    if (protocol == Protocol::nmea)
        text += ", address=" +
                std::string(std::get<NmeaHeader>(frame.header).address);
    else
        text += std::format(" id=0x{:04x}", frame.id());
    if (protocol == Protocol::sbf)
        text += std::format(", revision={}", frame.revision());
    text += std::format(", length={}", frame.wire.size());
    if (error) {
        text += ", decode_error=" + error->detail;
        if (error->offset)
            text += std::format(" at payload byte {}", *error->offset);
    } else
        text += ", status=unknown_block";
    text += ", payload=hex:";
    constexpr char digits[] = "0123456789abcdef";
    for (auto b : frame.payload) {
        text += digits[b >> 4];
        text += digits[b & 15];
    }
    return text + ")\n";
}
std::string dump(const FrameView &frame) {
    if (frame.protocol() == Protocol::rtcm3)
        return detail::dump_rtcm3(frame);
    if (frame.protocol() == Protocol::nmea)
        return detail::dump_nmea(frame);
    return frame.protocol() == Protocol::ubx ? detail::dump_ubx(frame)
                                             : detail::dump_sbf(frame);
}
} // namespace cppgnss
