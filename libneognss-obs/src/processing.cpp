// SPDX-License-Identifier: GPL-3.0-only
#include <neognss_obs/processing.hpp>
#include <neognss_obs/ubx_archive.hpp>
#include <openssl/evp.h>

namespace neognss_obs {
std::vector<uint8_t> archive_index(std::span<const uint8_t> bytes) {
    std::vector<uint8_t> out{'U', 'B', 'X', 'I', 'D', 'X', '0', '4'};
    unsigned char digest[32];
    unsigned length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr) != 1 || length != 32)
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
Json sbas_message(const cppgnss::SBAS::Result &result) {
    namespace S = cppgnss::SBAS;
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
    if (m.trailing_word)
        out["trailing_word"] = *m.trailing_word;
    Json c = {{"kind", "unparsed"}};
    std::visit(
        [&](const auto &v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, S::TestMode>)
                c = {{"kind", "test_mode"}};
            else if constexpr (std::is_same_v<T, S::NullMessage>)
                c = {{"kind", "null"}};
            else if constexpr (std::is_same_v<T, S::PrnMask> || std::is_same_v<T, S::IonosphericMask>) {
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
                         {"status", e.status() == S::IgpStatus::usable          ? "usable"
                                    : e.status() == S::IgpStatus::not_monitored ? "not_monitored"
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
                        {{"correction_raw", e.correction_raw}, {"correction_m", e.correction_m()}, {"udrei", e.udrei}});
            } else if constexpr (std::is_same_v<T, S::Integrity>)
                c = {{"kind", "integrity"}, {"iodf", v.iodf}, {"udrei", v.udrei}};
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
Json SubframeProcessor::feed(std::span<const uint8_t> data) {
    Json rows = Json::array();
    reader_.feed(data, [&](const cppgnss::FrameView &f) {
        if (f.id != 0x0213)
            return;
        if (sbas_only_ && f.payload.size() >= 8 && f.payload[6] == 2 && f.payload[0] != 1)
            return;
        auto parsed = UBX::parse_subframe(UBX::ubx_frame(f.wire.subspan(2)));
        if (!parsed.subframe) {
            ++malformed_;
            return;
        }
        const auto &s = *parsed.subframe;
        ++counts_[s.signal];
        Json row = {{"offset", f.offset},     {"gnssId", s.signal.gnssId},
                    {"svId", s.signal.svId},  {"sigId", s.signal.sigId},
                    {"freqId", s.raw_freqId}, {"chn", s.chn},
                    {"version", s.version},   {"reserved0", s.reserved0},
                    {"words", s.words},       {"prn", s.signal.prn() ? Json(*s.signal.prn()) : Json()}};
        if (s.signal.gnssId == 1) {
            auto result = UBX::parse_sbas(s);
            ++statuses_[cppgnss::SBAS::status_name(result.status)];
            if (result.message && result.message->crc_valid && result.message->preamble_valid)
                ++types_[std::to_string(result.message->type)];
            row["sbas"] = sbas_message(result);
        }
        rows.push_back(std::move(row));
    });
    return rows;
}
Json SubframeProcessor::finish() {
    reader_.finish();
    return Json::array();
}
Json SubframeProcessor::summary() const {
    Json streams = Json::array();
    for (auto &[k, n] : counts_)
        streams.push_back(
            {{"gnssId", k.gnssId}, {"svId", k.svId}, {"sigId", k.sigId}, {"freqId", k.freqId}, {"frames", n}});
    return {{"status", "complete"},
            {"source_bytes", reader_.bytes},
            {"ubx_frames", reader_.frames},
            {"malformed", malformed_ + reader_.invalid},
            {"discarded_noise_bytes", reader_.noise},
            {"skipped_protocol_frames", reader_.skipped_protocol_frames},
            {"skipped_protocol_bytes", reader_.skipped_protocol_bytes},
            {"streams", streams},
            {"sbas_status", statuses_},
            {"sbas_message_types", types_}};
}
} // namespace neognss_obs
