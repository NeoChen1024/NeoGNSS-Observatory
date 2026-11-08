// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppgnss/dump.hpp>
#include <cppgnss/stream.hpp>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace cppgnss {
enum class ParseErrorCode {
    WRONG_PROTOCOL,
    WRONG_MESSAGE,
    UNSUPPORTED_REVISION,
    UNSUPPORTED_LAYOUT,
    INVALID_PAYLOAD
};
struct ParseError {
    ParseErrorCode code;
    std::optional<size_t> offset;
    std::string detail;
};
template <class T> struct ParsedMessage {
    T message;
    size_t consumed;
};
template <class T> class ParseResult {
    // Accessing the wrong branch throws std::bad_variant_access.
    std::variant<ParsedMessage<T>, ParseError> result_;

  public:
    ParseResult(ParsedMessage<T> value) : result_(std::move(value)) {}
    ParseResult(ParseError error) : result_(std::move(error)) {}
    explicit operator bool() const noexcept {
        return std::holds_alternative<ParsedMessage<T>>(result_);
    }
    T &value() & { return std::get<ParsedMessage<T>>(result_).message; }
    const T &value() const & {
        return std::get<ParsedMessage<T>>(result_).message;
    }
    T &&value() && {
        return std::move(std::get<ParsedMessage<T>>(result_).message);
    }
    const ParseError &error() const & { return std::get<ParseError>(result_); }
    ParseError error() && { return std::move(std::get<ParseError>(result_)); }
    size_t consumed() const {
        return std::get<ParsedMessage<T>>(result_).consumed;
    }
};
template <class T> ParseResult<T> parse(const FrameView &frame) {
    if (frame.protocol() != T::protocol)
        return ParseError{ParseErrorCode::WRONG_PROTOCOL, {}, "Wrong protocol"};
    if (!T::matches(frame))
        return ParseError{
            ParseErrorCode::WRONG_MESSAGE, {}, "Wrong message ID"};
    return T::decode_payload(frame);
}
std::string dump_raw(const FrameView &, const ParseError *error = nullptr);
std::string dump(const FrameView &);
namespace detail {
template <class T>
std::string dump_parsed(const FrameView &frame, const ParseResult<T> &result) {
    if (!result)
        return dump_raw(frame, &result.error());
    auto text = result.value().dump();
    if (frame.protocol() == Protocol::sbf ||
        result.consumed() < frame.payload.size()) {
        text.resize(text.size() - 2);
        if (frame.protocol() == Protocol::sbf)
            text += ", revision=" + std::to_string(frame.revision());
        if (result.consumed() < frame.payload.size()) {
            text += ", trailing=hex:" +
                    hex(frame.payload.subspan(result.consumed()));
        }
        text += ")\n";
    }
    return text;
}
std::string dump_ubx(const FrameView &);
std::string dump_sbf(const FrameView &);
std::string dump_rtcm3(const FrameView &);
std::string dump_nmea(const FrameView &);
} // namespace detail
} // namespace cppgnss
