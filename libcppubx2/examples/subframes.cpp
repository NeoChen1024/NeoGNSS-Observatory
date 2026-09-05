// SPDX-License-Identifier: GPL-3.0-only
#include <cppubx2/sbas.hpp>
#include <cppubx2/ubx_reader.hpp>
#include <openssl/evp.h>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <sstream>

namespace fs = std::filesystem;
using namespace UBX;
static void close_file(FILE *file) { fclose(file); }
using File = std::unique_ptr<FILE, decltype(&close_file)>;

static std::string quote(std::string_view s) {
    std::string out = "\"";
    for(unsigned char c : s) {
        if(c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if(c < 32) out += std::format("\\u{:04x}", c);
        else out += char(c);
    }
    return out + '"';
}
static std::string hex(std::span<const uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size()*2);
    for(auto b : bytes) { out += digits[b >> 4]; out += digits[b & 15]; }
    return out;
}
template<class Range> static void array(std::ostream &out, const Range &items) {
    out << '[';
    bool first = true;
    for(auto x : items) { if(!first) out << ','; first = false; out << +x; }
    out << ']';
}
static void content(std::ostream &out, const SBAS::Content &c) {
    out << '{';
    std::visit([&](const auto &v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T, std::monostate>) out << "\"kind\":\"unparsed\"";
        else if constexpr(std::is_same_v<T, SBAS::TestMode>) out << "\"kind\":\"test_mode\"";
        else if constexpr(std::is_same_v<T, SBAS::NullMessage>) out << "\"kind\":\"null\"";
        else if constexpr(std::is_same_v<T, SBAS::PrnMask>) {
            out << "\"kind\":\"prn_mask\",\"iodp\":" << +v.iodp << ",\"active_mask_positions\":";
            std::vector<unsigned> active;
            for(size_t i = 0; i < v.mask.size(); ++i) if(v.mask[i]) active.push_back(i+1);
            array(out, active);
        } else if constexpr(std::is_same_v<T, SBAS::FastCorrections>) {
            out << "\"kind\":\"fast_corrections\",\"iodp\":" << +v.iodp << ",\"iodf\":" << +v.iodf
                << ",\"first_mask_position\":" << +v.first_mask_position << ",\"satellites\":[";
            for(size_t i = 0; i < v.satellites.size(); ++i) {
                if(i) out << ',';
                out << "{\"correction_raw\":" << v.satellites[i].correction_raw
                    << ",\"correction_m\":" << v.satellites[i].correction_m()
                    << ",\"udrei\":" << +v.satellites[i].udrei << '}';
            }
            out << ']';
        } else if constexpr(std::is_same_v<T, SBAS::Integrity>) {
            out << "\"kind\":\"integrity\",\"iodf\":"; array(out, v.iodf);
            out << ",\"udrei\":"; array(out, v.udrei);
        } else if constexpr(std::is_same_v<T, SBAS::FastDegradation>) {
            out << "\"kind\":\"fast_degradation\",\"iodp\":" << +v.iodp << ",\"latency_s\":" << +v.latency_s;
            out << ",\"degradation_index\":"; array(out, v.degradation_index);
        } else if constexpr(std::is_same_v<T, SBAS::GeoNavigation>) {
            out << "\"kind\":\"geo_navigation\",\"iodn_raw\":" << +v.iodn_raw
                << ",\"t0_raw\":" << v.t0_raw << ",\"ura\":" << +v.ura;
            out << ",\"position_raw\":"; array(out, v.position_raw);
            out << ",\"velocity_raw\":"; array(out, v.velocity_raw);
            out << ",\"acceleration_raw\":"; array(out, v.acceleration_raw);
            out << ",\"clock_offset_raw\":" << v.clock_offset_raw << ",\"clock_drift_raw\":" << +v.clock_drift_raw;
        } else if constexpr(std::is_same_v<T, SBAS::IonosphericMask>) {
            out << "\"kind\":\"ionospheric_mask\",\"number_of_bands_raw\":" << +v.number_of_bands_raw
                << ",\"band\":" << +v.band << ",\"iodi\":" << +v.iodi << ",\"active_mask_positions\":";
            std::vector<unsigned> active;
            for(size_t i = 0; i < v.mask.size(); ++i) if(v.mask[i]) active.push_back(i+1);
            array(out, active);
        } else if constexpr(std::is_same_v<T, SBAS::IonosphericDelay>) {
            out << "\"kind\":\"ionospheric_delay\",\"band\":" << +v.band
                << ",\"block\":" << +v.block << ",\"iodi\":" << +v.iodi << ",\"corrections\":[";
            for(size_t i = 0; i < v.corrections.size(); ++i) {
                if(i) out << ',';
                const auto &e = v.corrections[i];
                out << "{\"active_mask_ordinal\":" << 15*v.block+i+1 << ",\"delay_raw\":" << e.delay_raw
                    << ",\"givei\":" << +e.givei << ",\"delay_m\":";
                if(e.delay_m()) out << *e.delay_m(); else out << "null";
                out << ",\"status\":" << quote(e.status() == SBAS::IgpStatus::usable ? "usable" :
                    e.status() == SBAS::IgpStatus::not_monitored ? "not_monitored" : "do_not_use") << '}';
            }
            out << ']';
        }
    }, c);
    out << '}';
}
static void write(FILE *f, const std::string &line) {
    if(fwrite(line.data(), 1, line.size(), f) != line.size() || fputc('\n', f) == EOF)
        throw std::runtime_error("Output write failed");
}
struct Reader {
    FILE *file;
    EVP_MD_CTX *digest;
    std::array<uint8_t, 65536> buffer{};
    size_t cursor = 0, size = 0;
    uint64_t offset = 0;
    static ByteReadResult next(void *context) {
        auto &r = *static_cast<Reader *>(context);
        if(r.cursor == r.size) {
            r.size = fread(r.buffer.data(), 1, r.buffer.size(), r.file);
            r.cursor = 0;
            if(!r.size) return {ferror(r.file) ? ReadResult::error : ReadResult::end};
            if(EVP_DigestUpdate(r.digest, r.buffer.data(), r.size) != 1)
                throw std::runtime_error("Input hash failed");
        }
        ++r.offset;
        return {ReadResult::ok, r.buffer[r.cursor++]};
    }
};
int main(int argc, char **argv) {
    const bool sbas_only = argc == 4 && std::string_view(argv[3]) == "--sbas-only";
    if(argc != 3 && !sbas_only) {
        std::cerr << "Usage: cppubx2_subframes INPUT.ubx NEW_OUTPUT_DIRECTORY [--sbas-only]\n";
        return argc == 2 && std::string_view(argv[1]) == "--help" ? 0 : 2;
    }
    try {
        File input(fopen(argv[1], "rb"), close_file);
        if(!input) throw std::runtime_error("Cannot open input");
        fs::path output(argv[2]);
        if(!fs::create_directory(output)) throw std::runtime_error("Output directory must not already exist");
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if(!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
            throw std::runtime_error("Cannot initialize input hash");
        Reader reader{input.get(), digest.get()};
        SubframeDemultiplexer demux;
        std::map<SignalKey, File> streams;
        std::map<SBAS::Status, uint64_t> sbas_status;
        std::map<unsigned, uint64_t> message_types;
        File errors(fopen((output / "errors.jsonl").c_str(), "wx"), close_file);
        if(!errors) throw std::runtime_error("Cannot create error stream");
        uint64_t frames = 0, malformed = 0, discarded = 0;
        ubx_buf_t body;
        while(true) {
            size_t noise = 0;
            const auto start_offset = reader.offset;
            auto result = read_ubx_frame(&reader, Reader::next, body, &noise);
            if(result == ReadResult::end) noise = reader.offset - start_offset;
            discarded += noise;
            if(noise) write(errors.get(), std::format(
                "{{\"offset\":{},\"kind\":\"noise\",\"length\":{}}}", start_offset, noise));
            if(result == ReadResult::end) break;
            if(result != ReadResult::ok) throw std::runtime_error("Truncated frame or input I/O error");
            ++frames;
            const auto offset = reader.offset - body.size() - 2;
            const ubx_frame frame(body);
            // Validate framing for every message, but avoid decoding/exporting
            // other constellations in a dedicated SBAS extraction run.
            if(sbas_only && frame.valid && frame.class_id == UBX_CLASS_RXM &&
               frame.msg_id == UBX_RXM_SFRBX && frame.payload.size() >= 8 &&
               frame.payload[6] == 2 && frame.payload[0] != 1) continue;
            auto routed = demux.dispatch(frame, [&](const NavigationSubframe &s) {
                if(!streams.contains(s.signal)) {
                    auto name = std::format("gnss-{}_sv-{}_sig-{}_freq-{}.jsonl",
                        s.signal.gnssId, s.signal.svId, s.signal.sigId, s.signal.freqId);
                    File f(fopen((output / name).c_str(), "wx"), close_file);
                    if(!f) throw std::runtime_error("Cannot create signal stream");
                    streams.emplace(s.signal, std::move(f));
                }
                std::ostringstream out;
                out << "{\"offset\":" << offset << ",\"gnssId\":" << +s.signal.gnssId
                    << ",\"svId\":" << +s.signal.svId << ",\"sigId\":" << +s.signal.sigId
                    << ",\"freqId\":" << +s.raw_freqId << ",\"chn\":" << +s.chn
                    << ",\"version\":" << +s.version << ",\"reserved0\":" << +s.reserved0 << ",\"prn\":";
                if(s.signal.prn()) out << *s.signal.prn(); else out << "null";
                out << ",\"words\":"; array(out, s.words);
                if(s.signal.gnssId == 1) {
                    const auto decoded = SBAS::parse(s);
                    ++sbas_status[decoded.status];
                    out << ",\"sbas\":{\"status\":" << quote(SBAS::status_name(decoded.status));
                    if(decoded.message) {
                        const auto &m = *decoded.message;
                        if(m.crc_valid && m.preamble_valid) ++message_types[m.type];
                        out << ",\"type\":" << +m.type << ",\"preamble\":" << +m.preamble
                            << ",\"crc_valid\":" << (m.crc_valid ? "true" : "false")
                            << ",\"received_crc\":" << m.received_crc << ",\"computed_crc\":" << m.computed_crc
                            << ",\"padding_bits\":" << +m.padding_bits << ",\"hex\":" << quote(hex(m.bytes))
                            << ",\"content\":";
                        content(out, m.content);
                    }
                    out << '}';
                }
                out << '}';
                write(streams.at(s.signal).get(), out.str());
            });
            if(routed.status != SubframeStatus::decoded && routed.status != SubframeStatus::not_sfrbx) {
                ++malformed;
                write(errors.get(), std::format("{{\"offset\":{},\"status_code\":{},\"body_hex\":{}}}",
                    offset, int(routed.status), quote(hex(body))));
            }
        }
        for(auto &[key, file] : streams)
            if(fclose(file.release()) != 0) throw std::runtime_error("Signal output close failed");
        if(fclose(errors.release()) != 0) throw std::runtime_error("Error output close failed");
        std::array<uint8_t, 32> hash{};
        unsigned length = 0;
        if(EVP_DigestFinal_ex(digest.get(), hash.data(), &length) != 1 || length != hash.size())
            throw std::runtime_error("Input hash finalization failed");
        std::ostringstream summary;
        summary << "{\"schema\":1,\"status\":\"complete\",\"source\":" << quote(fs::absolute(argv[1]).string())
                << ",\"sbas_only\":" << (sbas_only ? "true" : "false")
                << ",\"source_sha256\":" << quote(hex(hash)) << ",\"source_bytes\":" << reader.offset
                << ",\"ubx_frames\":" << frames << ",\"malformed\":" << malformed
                << ",\"discarded_noise_bytes\":" << discarded << ",\"streams\":[";
        bool first = true;
        for(const auto &[key, count] : demux.counts()) {
            if(!first) summary << ',';
            first = false;
            summary << std::format("{{\"gnssId\":{},\"svId\":{},\"sigId\":{},\"freqId\":{},\"frames\":{}}}",
                key.gnssId, key.svId, key.sigId, key.freqId, count);
        }
        summary << "],\"sbas_status\":{";
        first = true;
        for(const auto &[status, count] : sbas_status) {
            if(!first) summary << ',';
            first = false;
            summary << quote(SBAS::status_name(status)) << ':' << count;
        }
        summary << "},\"sbas_message_types\":{";
        first = true;
        for(const auto &[type, count] : message_types) {
            if(!first) summary << ',';
            first = false;
            summary << quote(std::to_string(type)) << ':' << count;
        }
        summary << "}}";
        File metadata(fopen((output / "summary.json").c_str(), "wx"), close_file);
        if(!metadata) throw std::runtime_error("Cannot create summary");
        write(metadata.get(), summary.str());
        if(fclose(metadata.release()) != 0) throw std::runtime_error("Summary close failed");
        std::cout << summary.str() << '\n';
    } catch(const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
