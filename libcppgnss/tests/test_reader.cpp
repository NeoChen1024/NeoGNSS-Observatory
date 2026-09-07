// SPDX-License-Identifier: GPL-3.0-only
#include <cppgnss/ubx_reader.hpp>
#include <cassert>

using namespace UBX;

struct Input {
    ubx_buf_t bytes;
    size_t offset = 0;
    ReadResult terminal = ReadResult::end;
};

static ByteReadResult next(void *context)
{
    auto &input = *static_cast<Input *>(context);
    if(input.offset == input.bytes.size()) return {input.terminal};
    return {ReadResult::ok, input.bytes[input.offset++]};
}

static unsigned diagnostics = 0;
static void on_error(std::string_view detail, std::source_location)
{
    assert(!detail.empty());
    ++diagnostics;
}

int main()
{
    const ubx_buf_t packet{0xb5, 0x62, 1, 2, 0, 0, 3, 10};
    ubx_buf_t raw;
    Input input{{0x00, 0xb5}};
    input.bytes.insert(input.bytes.end(), packet.begin(), packet.end());
    size_t discarded = 0;
    assert(read_ubx_frame(&input, next, raw, &discarded) == ReadResult::ok);
    assert(discarded == 2);
    assert(ubx_frame(raw).valid);
    assert(read_ubx_frame(&input, next, raw) == ReadResult::end);
    for(size_t length = 1; length < packet.size(); ++length) {
        Input partial{{packet.begin(), packet.begin() + length}};
        assert(read_ubx_frame(&partial, next, raw) == ReadResult::truncated);
        for(auto terminal : {ReadResult::timeout, ReadResult::error}) {
            partial.offset = 0;
            partial.terminal = terminal;
            assert(read_ubx_frame(&partial, next, raw) == terminal);
        }
    }
    input = {packet};
    assert(read_ubx_frame(&input, next, raw) == ReadResult::ok);
    raw.back() ^= 1;
    assert(!ubx_frame(raw).valid); // Silent by default.
    assert(diagnostics == 0);
    auto previous = set_parse_error_handler(on_error);
    assert(previous == nullptr);
    assert(!ubx_frame(raw).valid);
    assert(diagnostics == 1);
    assert(set_parse_error_handler(previous) == on_error);
    assert(!ubx_frame(raw).valid);
    assert(diagnostics == 1);
}
