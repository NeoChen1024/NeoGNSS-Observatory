// SPDX-License-Identifier: GPL-3.0-only
#include <cppgnss/parse.hpp>
#include <format>
namespace cppgnss {
std::string dump_raw(const FrameView &frame, const ParseError *error) {
    auto text = std::format("({} id=0x{:04x}, revision={}, length={}",
                            frame.protocol == Protocol::ubx ? "UBX" : "SBF",
                            frame.id, frame.revision, frame.wire.size());
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
    return frame.protocol == Protocol::ubx ? detail::dump_ubx(frame)
                                           : detail::dump_sbf(frame);
}
} // namespace cppgnss
