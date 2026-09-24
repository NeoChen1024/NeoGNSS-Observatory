// SPDX-License-Identifier: GPL-3.0-only
#include <array>
#include <cppgnss/rtcm3.hpp>

namespace cppgnss::RTCM3 {
namespace {
constexpr double range_ms = 299792.458;
struct Truncated {
    size_t bit;
};
class Bits {
    std::span<const uint8_t> bytes_;
    size_t bit_ = 0;

  public:
    explicit Bits(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    size_t position() const { return bit_; }
    size_t remaining() const { return bytes_.size() * 8 - bit_; }
    uint32_t u(unsigned width) {
        if (width > remaining())
            throw Truncated{bit_};
        uint32_t value = 0;
        for (unsigned i = 0; i < width; ++i, ++bit_)
            value = (value << 1) | ((bytes_[bit_ / 8] >> (7 - bit_ % 8)) & 1);
        return value;
    }
    int32_t s(unsigned width) {
        const auto value = u(width);
        return value & (uint32_t{1} << (width - 1))
                   ? int32_t(int64_t(value) - (int64_t{1} << width))
                   : int32_t(value);
    }
};

char msm_system(uint16_t id) {
    switch (id / 10) {
    case 107:
        return 'G';
    case 108:
        return 'R';
    case 109:
        return 'E';
    case 110:
        return 'S';
    case 111:
        return 'J';
    case 112:
        return 'C';
    case 113:
        return 'I';
    default:
        return '\0';
    }
}

MsmHeader read_header(Bits &bits, uint16_t id) {
    MsmHeader header;
    header.message_id = bits.u(12);
    if (header.message_id != id)
        throw ParseError{ParseErrorCode::WRONG_MESSAGE, size_t{0},
                         "RTCM3 payload and frame message IDs differ"};
    header.station_id = bits.u(12);
    header.system = msm_system(id);
    header.epoch_ms = bits.u(30);
    header.multiple_message = bits.u(1);
    header.issue_of_data_station = bits.u(3);
    bits.u(7); // Reserved session-transmission field.
    header.clock_steering = bits.u(2);
    header.external_clock = bits.u(2);
    header.divergence_free_smoothing = bits.u(1);
    header.smoothing_interval = bits.u(3);
    return header;
}
struct Masks {
    std::vector<uint8_t> satellites;
    std::vector<std::pair<size_t, uint8_t>> cells;
};
Masks read_masks(Bits &bits) {
    Masks masks;
    std::vector<uint8_t> signals;
    for (uint8_t id = 1; id <= 64; ++id)
        if (bits.u(1))
            masks.satellites.push_back(id);
    for (uint8_t id = 1; id <= 32; ++id)
        if (bits.u(1))
            signals.push_back(id);
    // This is the MSM wire-format matrix limit, independent of any numerical
    // engine's limits on observation count or frequencies per satellite.
    if (masks.satellites.size() * signals.size() > 64)
        throw ParseError{ParseErrorCode::INVALID_PAYLOAD, bits.position() / 8,
                         "RTCM3 MSM satellite/signal matrix exceeds 64 cells"};
    for (size_t satellite = 0; satellite < masks.satellites.size(); ++satellite)
        for (const auto signal : signals)
            if (bits.u(1))
                masks.cells.emplace_back(satellite, signal);
    return masks;
}

// Standard signal identities cross-checked against the pinned RTKLIB MSM
// tables. Its explicitly tentative receiver/PocketSDR assignments are omitted.
std::string_view signal_code(char system, uint8_t signal) {
    using Codes = std::array<std::string_view, 32>;
    static constexpr Codes gps{"",   "1C", "1P", "1W", "", "",   "",   "2C",
                               "2P", "2W", "",   "",   "", "",   "2S", "2L",
                               "2X", "",   "",   "",   "", "5I", "5Q", "5X",
                               "",   "",   "",   "",   "", "1S", "1L", "1X"};
    static constexpr Codes galileo{
        "",   "1C", "1A", "1B", "1X", "1Z", "",   "6C", "6A", "6B", "6X",
        "6Z", "",   "7I", "7Q", "7X", "",   "8I", "8Q", "8X", "",   "5I",
        "5Q", "5X", "",   "",   "",   "",   "",   "",   "",   ""};
    static constexpr Codes sbas{
        "", "1C", "", "", "", "",   "",   "",   "", "", "", "", "", "", "", "",
        "", "",   "", "", "", "5I", "5Q", "5X", "", "", "", "", "", "", "", ""};
    static constexpr Codes qzss{"",   "1C", "",   "", "", "",   "",   "",
                                "6S", "6L", "6X", "", "", "",   "2S", "2L",
                                "2X", "",   "",   "", "", "5I", "5Q", "5X",
                                "",   "",   "",   "", "", "1S", "1L", "1X"};
    static constexpr Codes beidou{"",   "2I", "2Q", "2X", "", "",   "",   "6I",
                                  "6Q", "6X", "",   "",   "", "7I", "7Q", "7X",
                                  "",   "",   "",   "",   "", "5D", "5P", "5X",
                                  "7D", "",   "",   "",   "", "1D", "1P", "1X"};
    switch (system) {
    case 'G':
        return gps[signal - 1];
    case 'E':
        return galileo[signal - 1];
    case 'S':
        return sbas[signal - 1];
    case 'J':
        return qzss[signal - 1];
    case 'C':
        return beidou[signal - 1];
    default:
        return {};
    }
}
} // namespace

bool is_observation_message(uint16_t id) {
    return (id >= 1001 && id <= 1004) || (id >= 1009 && id <= 1012) ||
           (id / 10 >= 107 && id / 10 <= 113 && id % 10 >= 1 && id % 10 <= 7);
}

ParseResult<MsmHeader> parse_msm_header(const FrameView &frame) {
    if (frame.protocol != Protocol::rtcm3)
        return ParseError{ParseErrorCode::WRONG_PROTOCOL,
                          {},
                          "RTCM3 MSM header requires an RTCM3 frame"};
    if (!msm_system(frame.id) || frame.id % 10 < 1 || frame.id % 10 > 7)
        return ParseError{ParseErrorCode::UNSUPPORTED_LAYOUT,
                          {},
                          "RTCM3 message has no supported common MSM header"};
    if (frame.payload.size() > 1023)
        return ParseError{ParseErrorCode::INVALID_PAYLOAD,
                          {},
                          "RTCM3 payload exceeds the 10-bit length limit"};
    Bits bits(frame.payload);
    try {
        auto header = read_header(bits, frame.id);
        read_masks(bits);
        return ParsedMessage<MsmHeader>{std::move(header),
                                        (bits.position() + 7) / 8};
    } catch (const ParseError &error) {
        return error;
    } catch (const Truncated &error) {
        return ParseError{ParseErrorCode::INVALID_PAYLOAD, error.bit / 8,
                          "Truncated RTCM3 MSM common header"};
    }
}

ParseResult<ObservationMessage> parse_observation(const FrameView &frame) {
    if (frame.protocol != Protocol::rtcm3)
        return ParseError{ParseErrorCode::WRONG_PROTOCOL,
                          {},
                          "RTCM3 observation requires an RTCM3 frame"};
    const char system = msm_system(frame.id);
    const auto subtype = frame.id % 10;
    if (!system || system == 'R' || system == 'I' || subtype < 4 || subtype > 7)
        return ParseError{ParseErrorCode::UNSUPPORTED_LAYOUT,
                          {},
                          "Supported RTCM3 observations are GPS/Galileo/SBAS/"
                          "QZSS/BeiDou MSM4-7"};
    if (frame.payload.size() > 1023)
        return ParseError{ParseErrorCode::INVALID_PAYLOAD,
                          {},
                          "RTCM3 payload exceeds the 10-bit length limit"};
    Bits bits(frame.payload);
    try {
        ObservationMessage message;
        static_cast<MsmHeader &>(message) = read_header(bits, frame.id);
        if (message.epoch_ms >= 604800000)
            return ParseError{ParseErrorCode::INVALID_PAYLOAD, size_t{3},
                              "RTCM3 MSM epoch is outside its native week"};
        const auto masks = read_masks(bits);
        const auto &satellites = masks.satellites;
        std::vector<size_t> cell_satellites;
        for (const auto &[satellite, signal] : masks.cells) {
            Cell cell;
            cell.satellite_id = satellites[satellite];
            cell.signal_id = signal;
            cell.observation_code = signal_code(system, signal);
            message.cells.push_back(cell);
            cell_satellites.push_back(satellite);
        }

        const bool high_resolution = subtype >= 6;
        const bool rate_present = subtype == 5 || subtype == 7;
        const size_t satellite_bits = rate_present ? 36 : 18;
        const size_t cell_bits = high_resolution ? (rate_present ? 80 : 65)
                                                 : (rate_present ? 63 : 48);
        const size_t required = satellites.size() * satellite_bits +
                                message.cells.size() * cell_bits;
        if (required > bits.remaining())
            throw Truncated{bits.position()};
        std::vector<std::optional<double>> ranges(satellites.size());
        std::vector<std::optional<double>> rates(satellites.size());
        std::vector<uint8_t> extended(satellites.size());
        for (auto &range : ranges) {
            const auto value = bits.u(8);
            if (value != 255)
                range = value * range_ms;
        }
        if (rate_present)
            for (auto &value : extended)
                value = bits.u(4);
        for (auto &range : ranges) {
            const auto value = bits.u(10);
            if (range)
                *range += value * 0x1p-10 * range_ms;
        }
        if (rate_present)
            for (auto &rate : rates) {
                const auto value = bits.s(14);
                if (value != -8192)
                    rate = value;
            }
        for (size_t i = 0; i < message.cells.size(); ++i) {
            const auto value = bits.s(high_resolution ? 20 : 15);
            const auto &range = ranges[cell_satellites[i]];
            if (range && value != (high_resolution ? -524288 : -16384))
                message.cells[i].pseudorange_m =
                    *range +
                    value * (high_resolution ? 0x1p-29 : 0x1p-24) * range_ms;
        }
        for (size_t i = 0; i < message.cells.size(); ++i) {
            const auto value = bits.s(high_resolution ? 24 : 22);
            const auto &range = ranges[cell_satellites[i]];
            if (range && value != (high_resolution ? -8388608 : -2097152))
                message.cells[i].phase_range_m =
                    *range +
                    value * (high_resolution ? 0x1p-31 : 0x1p-29) * range_ms;
        }
        for (auto &cell : message.cells) {
            cell.lock_bits = high_resolution ? 10 : 4;
            cell.lock_indicator = bits.u(cell.lock_bits);
        }
        for (auto &cell : message.cells)
            cell.half_cycle_ambiguity = bits.u(1);
        for (auto &cell : message.cells) {
            const auto value = bits.u(high_resolution ? 10 : 6);
            if (value)
                cell.cn0_dbhz = value * (high_resolution ? 0.0625 : 1.0);
        }
        if (rate_present)
            for (size_t i = 0; i < message.cells.size(); ++i) {
                const auto value = bits.s(15);
                auto &cell = message.cells[i];
                const auto &rate = rates[cell_satellites[i]];
                cell.extended_satellite_info = extended[cell_satellites[i]];
                if (rate && value != -16384)
                    cell.phase_range_rate_m_s = *rate + value * 0.0001;
            }
        const size_t consumed = (bits.position() + 7) / 8;
        // MSM decoders ignore unexpected trailing data for forward
        // compatibility. The enclosing frame CRC still covers every byte.
        return ParsedMessage<ObservationMessage>{std::move(message), consumed};
    } catch (const ParseError &error) {
        return error;
    } catch (const Truncated &error) {
        return ParseError{ParseErrorCode::INVALID_PAYLOAD, error.bit / 8,
                          "Truncated RTCM3 MSM observation payload"};
    }
}
} // namespace cppgnss::RTCM3
