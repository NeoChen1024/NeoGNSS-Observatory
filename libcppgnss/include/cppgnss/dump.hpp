// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace cppgnss::detail {
struct TextDump {
    std::string text;
    std::vector<char> closing;
    std::vector<bool> first{false};
    void separator() {
        if (!first.back())
            text += ", ";
        first.back() = false;
    }
    void begin(std::string_view name, std::optional<size_t> = {},
               bool array = false) {
        separator();
        if (!name.empty()) {
            text += name;
            text += '=';
        }
        text += array ? '[' : '{';
        closing.push_back(array ? ']' : '}');
        first.push_back(true);
    }
    void end() {
        text += closing.back();
        closing.pop_back();
        first.pop_back();
    }
    void field(std::string_view name, std::string_view value) {
        separator();
        text += name;
        text += '=';
        text += value;
    }
    std::string finish() && {
        text += ")\n";
        return std::move(text);
    }
};
} // namespace cppgnss::detail
