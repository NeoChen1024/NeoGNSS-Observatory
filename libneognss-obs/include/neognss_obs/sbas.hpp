// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace neognss_obs::SBAS {
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
enum class L1Profile { standard, southpan_open };
struct TestMode {
    // Only populated for a profile explicitly defining the embedded MT2 body.
    std::optional<FastCorrections> fast;
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
        return delay_raw == 511 ? IgpStatus::do_not_use
               : givei == 15    ? IgpStatus::not_monitored
                                : IgpStatus::usable;
    }
    std::optional<double> delay_m() const {
        return delay_raw == 511 ? std::nullopt
                                : std::optional<double>{delay_raw * 0.125};
    }
};
struct IonosphericDelay {
    uint8_t band = 0, block = 0, iodi = 0;
    // These index the active MT18 mask, not the fixed geographic band grid.
    std::array<IonosphericCorrection, 15> corrections{};
};
struct Degradation {
    double brrc_m{}, cltc_lsb_m{}, cltc_v1_m_s{}, cltc_v0_m{};
    uint16_t iltc_v1_s{}, iltc_v0_s{}, igeo_s{}, iiono_s{};
    double cgeo_lsb_m{}, cgeo_v_m_s{}, cer_m{}, ciono_step_m{},
        ciono_ramp_m_s{};
    bool rss_udre{}, rss_iono{};
    double ccovariance{};
};
struct NetworkTime {
    int32_t a0_raw{}, a1_raw{};
    uint32_t reference_tow_s{}, gps_tow_s{};
    uint8_t reference_week_mod256{}, leap_week_mod256{}, leap_day{}, utc_id{};
    int8_t leap_seconds{}, future_leap_seconds{};
    uint16_t gps_week_mod1024{};
    // GLONASS timing parameters intentionally outside scientific scope.
};
struct GeoAlmanacEntry {
    uint8_t prn{}, health_status{};
    std::array<double, 3> position_m{}, velocity_m_s{};
};
struct GeoAlmanac {
    uint32_t reference_sod_s{};
    std::vector<GeoAlmanacEntry> entries;
};
struct LongTermEntry {
    uint8_t half{}, mask_position{}, issue{}, iodp{};
    bool velocity_code{};
    std::array<double, 3> delta_position_m{};
    int16_t clock_offset_raw{}; // 2^-31 seconds.
    std::optional<std::array<double, 3>> delta_velocity_m_s;
    std::optional<int8_t> clock_drift_raw; // 2^-39 seconds/second.
    std::optional<uint32_t> reference_sod_s;
};
struct LongTermCorrections {
    std::vector<LongTermEntry> entries;
};
struct MixedCorrections {
    uint8_t iodp{}, iodf{}, fast_block{};
    std::array<FastCorrection, 6> fast;
    LongTermCorrections long_term;
};
struct ServiceRegion {
    int16_t latitude1_deg{}, longitude1_deg{}, latitude2_deg{},
        longitude2_deg{};
    bool quadrangle{};
};
struct ServiceMessage {
    uint8_t iods{}, message_count{}, message_number{}, priority{};
    uint8_t delta_udre_inside_index{}, delta_udre_outside_index{};
    std::vector<ServiceRegion> regions;
};
struct CovarianceEntry {
    uint8_t mask_position{}, scale_exponent{};
    // Broadcast E11,E22,E33,E44,E12,E13,E14,E23,E24,E34, before scale.
    std::array<int16_t, 10> elements{};
};
struct Covariance {
    uint8_t iodp{};
    std::vector<CovarianceEntry> entries;
};
using Content =
    std::variant<std::monostate, TestMode, NullMessage, PrnMask,
                 FastCorrections, Integrity, FastDegradation, GeoNavigation,
                 IonosphericMask, IonosphericDelay, Degradation, NetworkTime,
                 GeoAlmanac, LongTermCorrections, MixedCorrections,
                 ServiceMessage, Covariance>;
struct Message {
    std::array<uint8_t, 32>
        bytes{}; // 250 over-air bits; bottom 6 bits cleared.
    uint8_t padding_bits = 0, preamble = 0, type = 0;
    uint32_t received_crc = 0, computed_crc = 0;
    bool preamble_valid = false, crc_valid = false;
    Content content;
};
enum class Status {
    decoded,
    invalid_word_count,
    invalid_preamble,
    invalid_crc,
    unsupported_message,
    invalid_content
};
struct Result {
    Status status;
    std::optional<Message> message;
};
// Receiver-independent 250-bit SBAS L1 message in 32 MSB-first bytes.
Result parse_l1(std::span<const uint8_t> bytes,
                L1Profile profile = L1Profile::standard);
std::optional<std::pair<int, int>> igp_coordinate(unsigned band,
                                                  unsigned mask_bit);
const char *status_name(Status status);
} // namespace neognss_obs::SBAS
