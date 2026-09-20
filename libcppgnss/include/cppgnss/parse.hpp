// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppgnss/dump.hpp>
#include <cppgnss/stream.hpp>
#include <optional>
#include <stdexcept>
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
    std::variant<ParsedMessage<T>, ParseError> result_;
    void require_value() const {
        if (!*this)
            throw std::logic_error("ParseResult has no value");
    }
    void require_error() const {
        if (*this)
            throw std::logic_error("ParseResult has no error");
    }

  public:
    ParseResult(ParsedMessage<T> value) : result_(std::move(value)) {}
    ParseResult(ParseError error) : result_(std::move(error)) {}
    explicit operator bool() const noexcept {
        return std::holds_alternative<ParsedMessage<T>>(result_);
    }
    T &value() & {
        require_value();
        return std::get<ParsedMessage<T>>(result_).message;
    }
    const T &value() const & {
        return std::get<ParsedMessage<T>>(result_).message;
    }
    T &&value() && {
        require_value();
        return std::move(std::get<ParsedMessage<T>>(result_).message);
    }
    const ParseError &error() const & {
        require_error();
        return std::get<ParseError>(result_);
    }
    ParseError error() && {
        require_error();
        return std::move(std::get<ParseError>(result_));
    }
    size_t consumed() const {
        require_value();
        return std::get<ParsedMessage<T>>(result_).consumed;
    }
};
template <class T> ParseResult<T> parse(const FrameView &frame) {
    if (frame.protocol != T::protocol)
        return ParseError{ParseErrorCode::WRONG_PROTOCOL, {}, "Wrong protocol"};
    if (frame.id != static_cast<uint16_t>(T::message_id))
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
    if (frame.protocol == Protocol::sbf ||
        result.consumed() < frame.payload.size()) {
        text.resize(text.size() - 2);
        if (frame.protocol == Protocol::sbf)
            text += ", revision=" + std::to_string(frame.revision);
        if (result.consumed() < frame.payload.size()) {
            text += ", trailing=hex:";
            constexpr char digits[] = "0123456789abcdef";
            for (auto b : frame.payload.subspan(result.consumed())) {
                text += digits[b >> 4];
                text += digits[b & 15];
            }
        }
        text += ")\n";
    }
    return text;
}
std::string dump_ubx(const FrameView &);
std::string dump_sbf(const FrameView &);
} // namespace detail
} // namespace cppgnss
