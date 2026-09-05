// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cppubx2/ubx_subframe.hpp>
#include <variant>

namespace UBX::SBAS {
// Offsets are zero-based, most-significant-bit first, in the over-air message.
class BitView {
public:
    BitView(std::span<const uint8_t> bytes, size_t bit_count);
    uint64_t unsigned_at(size_t offset, size_t width) const;
    int64_t signed_at(size_t offset, size_t width) const;
private:
    std::span<const uint8_t> bytes_;
    size_t bit_count_;
};
uint32_t crc24q(std::span<const uint8_t> bytes, size_t bit_count);

struct TestMode {}; // MT0: do not silently apply its optional fast corrections.
struct NullMessage {}; // MT63.
struct PrnMask {
    uint8_t iodp = 0;
    std::array<bool, 210> mask{}; // Index 0 is mask position 1, not PRN 0.
};
struct FastCorrection {
    int16_t correction_raw = 0;
    uint8_t udrei = 0;
    double correction_m() const { return correction_raw * 0.125; }
};
struct FastCorrections {
    uint8_t iodf = 0, iodp = 0, first_mask_position = 1;
    std::array<FastCorrection, 13> satellites{};
};
struct Integrity {
    std::array<uint8_t, 4> iodf{};
    std::array<uint8_t, 51> udrei{};
};
struct FastDegradation {
    uint8_t latency_s = 0, iodp = 0;
    std::array<uint8_t, 51> degradation_index{};
};
struct GeoNavigation {
    uint8_t iodn_raw = 0, ura = 0;
    uint16_t t0_raw = 0; // 16-second units in SBAS network time.
    std::array<int32_t, 3> position_raw{}, velocity_raw{}, acceleration_raw{};
    int16_t clock_offset_raw = 0;
    int8_t clock_drift_raw = 0;
};
struct IonosphericMask {
    uint8_t number_of_bands_raw = 0, band = 0, iodi = 0;
    std::array<bool, 201> mask{}; // Index 0 is IGP mask bit 1.
};
enum class IgpStatus { usable, not_monitored, do_not_use };
struct IonosphericCorrection {
    uint16_t delay_raw = 0;
    uint8_t givei = 0;
    IgpStatus status() const {
        return delay_raw == 511 ? IgpStatus::do_not_use :
               givei == 15 ? IgpStatus::not_monitored : IgpStatus::usable;
    }
    std::optional<double> delay_m() const {
        return delay_raw == 511 ? std::nullopt : std::optional<double>{delay_raw * 0.125};
    }
};
struct IonosphericDelay {
    uint8_t band = 0, block = 0, iodi = 0;
    // These index the active MT18 mask, not the fixed geographic band grid.
    std::array<IonosphericCorrection, 15> corrections{};
};
using Content = std::variant<std::monostate, TestMode, NullMessage, PrnMask,
    FastCorrections, Integrity, FastDegradation, GeoNavigation, IonosphericMask, IonosphericDelay>;
struct Message {
    std::optional<uint32_t> trailing_word; // Observed nine-word receiver variant.
    std::array<uint8_t, 32> bytes{}; // 250 over-air bits; bottom 6 bits cleared.
    uint8_t padding_bits = 0, preamble = 0, type = 0;
    uint32_t received_crc = 0, computed_crc = 0;
    bool preamble_valid = false, crc_valid = false;
    Content content;
};
enum class Status {
    decoded, unsupported_signal, invalid_word_count, invalid_preamble,
    invalid_crc, unsupported_message, invalid_content
};
struct Result {
    Status status;
    std::optional<Message> message;
};
// Only SBAS L1 C/A (gnssId=1, sigId=0), SFRBX v2. Never reinterpret QZSS L1S
// or SBAS L5 as legacy SBAS L1 based on word count or preamble alone.
Result parse(const NavigationSubframe &subframe);
const char *status_name(Status status);
}
