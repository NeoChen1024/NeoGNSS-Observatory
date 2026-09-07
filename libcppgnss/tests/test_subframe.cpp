// SPDX-License-Identifier: GPL-3.0-only
#include <cppgnss/ubx_subframe.hpp>
#include <cassert>

using namespace UBX;
namespace SBAS = cppgnss::SBAS;
template<class F> static void out_of_range(F f) {
    bool caught = false;
    try { f(); } catch(const std::out_of_range &) { caught = true; }
    assert(caught);
}
int main() {
    std::array<uint8_t, 8> bytes{};
    bytes.fill(255);
    SBAS::BitView bits(bytes, 64);
    assert(bits.signed_at(0, 64) == -1);
    assert(bits.signed_at(63, 1) == -1);
    assert(bits.unsigned_at(64, 0) == 0);
    out_of_range([&] { bits.unsigned_at(63, 2); });
    out_of_range([&] { bits.unsigned_at(SIZE_MAX, 1); });
    out_of_range([&] { bits.signed_at(0, 0); });
    out_of_range([&] { SBAS::BitView invalid(bytes, 65); });
    bytes[0] = 0x80;
    assert(bits.signed_at(0, 8) == -128);
    NavigationSubframe subframe;
    subframe.signal = {1, 137, 0, 0}; subframe.version = 2;
    // Real MT18 vector retained for core field/CRC decoding checks.
    subframe.words = {0x5348a300, 0x03ffc001, 0xffc000ff, 0xf0003ff0,
                     0x000ffc00, 0x03fc0000, 0x18000000, 0x2b2e7000, 0xdeadbeef};
    auto result = UBX::parse_sbas(subframe);
    assert(result.status == SBAS::Status::decoded);
    assert(result.message->trailing_word == 0xdeadbeef);
    assert(std::get<SBAS::IonosphericMask>(result.message->content).band == 8);
    subframe.words.resize(10);
    assert(UBX::parse_sbas(subframe).status == SBAS::Status::invalid_word_count);
    assert((SignalKey{5, 10, 0, 0}.prn() == 202));
    assert((!SignalKey{6, 2, 0, 0}.prn()));
    assert((!SignalKey{99, 2, 0, 0}.prn()));
    SubframeDemultiplexer demux;
    const ubx_frame invalid(std::span<const uint8_t>{});
    bool called = false;
    assert(demux.dispatch(invalid, [&](const auto &) { called = true; }).status == SubframeStatus::invalid_frame);
    assert(!called && demux.counts().empty());
    demux.clear();
}
