// SPDX-License-Identifier: GPL-3.0-only
#include <neognss_obs/processing.hpp>
#include <neognss_obs/ubx_archive.hpp>
#include <openssl/evp.h>

namespace neognss_obs {
std::vector<uint8_t> archive_index(std::span<const uint8_t> bytes) {
    std::vector<uint8_t> out{'U', 'B', 'X', 'I', 'D', 'X', '0', '4'};
    unsigned char digest[32];
    unsigned length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(),
                   nullptr) != 1 ||
        length != 32)
        throw std::runtime_error("Archive SHA256 failed");
    out.insert(out.end(), digest, digest + 32);
    const auto append = [&]<class T>(T value) {
        value = UBX::little_to_native(value);
        const auto *p = reinterpret_cast<const uint8_t *>(&value);
        out.insert(out.end(), p, p + sizeof value);
    };
    append(uint64_t(bytes.size()));
    scan_archive(bytes, [&](const ArchiveEpoch &e) {
        append(e.begin);
        append(e.end);
        append(e.gpst_ms);
        append(e.tow_ms);
        append(e.fingerprint);
        append(e.flags);
        append(e.frames);
        append(e.nav_frames);
        append(e.gps_week);
    });
    return out;
}
static std::string hex(std::span<const uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}
Json sbas_message(const neognss_obs::SBAS::Result &result) {
    namespace S = neognss_obs::SBAS;
    Json out = {{"status", S::status_name(result.status)}};
    if (!result.message)
        return out;
    const auto &m = *result.message;
    out.update({{"type", m.type},
                {"preamble", m.preamble},
                {"crc_valid", m.crc_valid},
                {"received_crc", m.received_crc},
                {"computed_crc", m.computed_crc},
                {"padding_bits", m.padding_bits},
                {"hex", hex(m.bytes)}});
    Json c = {{"kind", "unparsed"}};
    std::visit(
        [&](const auto &v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, S::TestMode>)
                c = {{"kind", "test_mode"}};
            else if constexpr (std::is_same_v<T, S::NullMessage>)
                c = {{"kind", "null"}};
            else if constexpr (std::is_same_v<T, S::PrnMask> ||
                               std::is_same_v<T, S::IonosphericMask>) {
                c["active_mask_positions"] = Json::array();
                for (size_t i = 0; i < v.mask.size(); ++i)
                    if (v.mask[i])
                        c["active_mask_positions"].push_back(i + 1);
                if constexpr (std::is_same_v<T, S::PrnMask>)
                    c.update({{"kind", "prn_mask"}, {"iodp", v.iodp}});
                else
                    c.update({{"kind", "ionospheric_mask"},
                              {"number_of_bands_raw", v.number_of_bands_raw},
                              {"band", v.band},
                              {"iodi", v.iodi}});
            } else if constexpr (std::is_same_v<T, S::IonosphericDelay>) {
                c = {{"kind", "ionospheric_delay"},
                     {"band", v.band},
                     {"block", v.block},
                     {"iodi", v.iodi},
                     {"corrections", Json::array()}};
                for (size_t i = 0; i < v.corrections.size(); ++i) {
                    const auto &e = v.corrections[i];
                    c["corrections"].push_back(
                        {{"active_mask_ordinal", 15 * v.block + i + 1},
                         {"delay_raw", e.delay_raw},
                         {"givei", e.givei},
                         {"delay_m", e.delay_m() ? Json(*e.delay_m()) : Json()},
                         {"status", e.status() == S::IgpStatus::usable
                                        ? "usable"
                                    : e.status() == S::IgpStatus::not_monitored
                                        ? "not_monitored"
                                        : "do_not_use"}});
                }
            } else if constexpr (std::is_same_v<T, S::FastCorrections>) {
                c = {{"kind", "fast_corrections"},
                     {"iodp", v.iodp},
                     {"iodf", v.iodf},
                     {"first_mask_position", v.first_mask_position},
                     {"satellites", Json::array()}};
                for (const auto &e : v.satellites)
                    c["satellites"].push_back(
                        {{"correction_raw", e.correction_raw},
                         {"correction_m", e.correction_m()},
                         {"udrei", e.udrei}});
            } else if constexpr (std::is_same_v<T, S::Integrity>)
                c = {{"kind", "integrity"},
                     {"iodf", v.iodf},
                     {"udrei", v.udrei}};
            else if constexpr (std::is_same_v<T, S::FastDegradation>)
                c = {{"kind", "fast_degradation"},
                     {"iodp", v.iodp},
                     {"latency_s", v.latency_s},
                     {"degradation_index", v.degradation_index}};
            else if constexpr (std::is_same_v<T, S::GeoNavigation>)
                c = {{"kind", "geo_navigation"},
                     {"iodn_raw", v.iodn_raw},
                     {"t0_raw", v.t0_raw},
                     {"ura", v.ura},
                     {"position_raw", v.position_raw},
                     {"velocity_raw", v.velocity_raw},
                     {"acceleration_raw", v.acceleration_raw},
                     {"clock_offset_raw", v.clock_offset_raw},
                     {"clock_drift_raw", v.clock_drift_raw}};
        },
        m.content);
    out["content"] = std::move(c);
    return out;
}
} // namespace neognss_obs
