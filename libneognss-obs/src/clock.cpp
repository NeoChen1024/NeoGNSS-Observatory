// SPDX-License-Identifier: GPL-3.0-only
#include <cmath>
#include <cstring>
#include <neognss_obs/processing.hpp>
#include <set>

namespace neognss_obs {
namespace {
constexpr int64_t week_ns = 604800000000000LL;
template <class T> T read(std::span<const uint8_t> p, size_t offset = 0) {
    if (offset + sizeof(T) > p.size())
        throw std::runtime_error("Short clock telemetry payload");
    T v;
    std::memcpy(&v, p.data() + offset, sizeof v);
    return UBX::little_to_native(v);
}
Json optional_time(const std::optional<int64_t> &v) { return v ? Json(*v) : Json(); }
} // namespace
struct ClockProcessor::State {
    struct Record {
        std::string source;
        uint64_t offset;
        uint16_t id;
        std::vector<uint8_t> payload;
    };
    cppgnss::StreamDecoder reader{cppgnss::Protocol::ubx};
    double max_gap, tolerance, temp_age;
    std::string source;
    std::vector<Record> epoch;
    std::optional<int64_t> tow, uptime, last_reset;
    Json temperature, previous;
    int64_t session = 0, arc = 0, adjustment = 0;
    std::map<std::string, uint64_t> counts;
    std::map<std::string, std::pair<Json, Json>> ranges;
    struct Bin {
        uint64_t n = 0;
        double mean = 0, m2 = 0;
    };
    std::map<int, Bin> bins;
    Json samples = Json::array(), events = Json::array(), telemetry = Json::array();
    Json drain() {
        Json out = {{"samples", Json::array()}, {"events", Json::array()}, {"telemetry", Json::array()}};
        out["samples"].swap(samples);
        out["events"].swap(events);
        out["telemetry"].swap(telemetry);
        out["skipped_protocol_frames"] = reader.skipped_protocol_frames;
        out["skipped_protocol_bytes"] = reader.skipped_protocol_bytes;
        return out;
    }
    void accept(const cppgnss::FrameView &f) {
        if (f.id != 0x0120 && f.id != 0x0122 && f.id != 0x0161 && f.id != 0x0a39 && f.id != 0x0215)
            return;
        auto p = f.payload;
        size_t expected = f.id == 0x0120 ? 16 : f.id == 0x0122 ? 20 : f.id == 0x0161 ? 4 : f.id == 0x0a39 ? 24 : 0;
        if (expected && p.size() != expected)
            throw std::runtime_error("Invalid telemetry payload length at " + source + ":" + std::to_string(f.offset));
        std::optional<int64_t> next_tow;
        if ((f.id >> 8) == 1) {
            const auto t = read<uint32_t>(p);
            if (t > 604800000)
                throw std::runtime_error("Invalid iTOW=" + std::to_string(t) + " at " + source + ":" +
                                         std::to_string(f.offset));
            next_tow = ((int64_t(t) + 500) / 1000 * 1000) % 604800000;
        } else if (f.id == 0x0215) {
            if (p.size() < 16 || p.size() != 16u + p[11] * 32u || p[13] != 1)
                throw std::runtime_error("Invalid or unsupported RAWX payload");
            const double seconds = read<double>(p);
            if (p[11]) {
                if (!std::isfinite(seconds) || seconds < 0 || seconds >= 604800)
                    throw std::runtime_error("Invalid RAWX rcvTow");
                next_tow = (int64_t(std::floor(seconds + 0.5)) * 1000) % 604800000;
            }
            p = p.first(16);
        } else if (p[0] != 1)
            throw std::runtime_error("Unsupported MON-SYS msgVer");
        if (next_tow && tow && *next_tow != *tow)
            finish_epoch();
        if (next_tow)
            tow = next_tow;
        epoch.push_back({source, f.offset, f.id, {p.begin(), p.end()}});
        if (epoch.size() > 4096)
            throw std::runtime_error("Too many telemetry messages without an epoch boundary");
        if (f.id == 0x0161)
            finish_epoch();
    }
    void finish_epoch() {
        if (epoch.empty())
            return;
        auto records = std::move(epoch);
        epoch.clear();
        tow.reset();
        std::set<int64_t> anchors;
        std::vector<const Record *> clocks, monitors;
        std::optional<int64_t> ftow;
        std::optional<bool> rawx_reset;
        std::string basis;
        for (const auto &r : records) {
            const auto &p = r.payload;
            if (r.id == 0x0120 && (p[11] & 3) == 3) {
                const auto week = read<int16_t>(p, 8);
                if (week >= 0) {
                    anchors.insert(week * week_ns + ((int64_t(read<uint32_t>(p)) + 500) / 1000) * 1000000000);
                    ftow = read<int32_t>(p, 4);
                    basis = "NAV-TIMEGPS_nominal_iTOW";
                }
            } else if (r.id == 0x0215) {
                if (!p[11]) {
                    ++counts["empty_rawx_ignored_as_time_anchor"];
                    continue;
                }
                anchors.insert(read<uint16_t>(p, 8) * week_ns +
                               int64_t(std::floor(read<double>(p) + 0.5)) * 1000000000);
                rawx_reset = bool(p[12] & 2) || rawx_reset.value_or(false);
                if (basis.empty())
                    basis = "RXM-RAWX_nearest_second";
            } else if (r.id == 0x0122)
                clocks.push_back(&r);
            else if (r.id == 0x0a39)
                monitors.push_back(&r);
        }
        if (anchors.size() > 1)
            throw std::runtime_error("Conflicting GPST anchors in telemetry epoch");
        if (clocks.size() > 1)
            throw std::runtime_error("Multiple NAV-CLOCK messages in one epoch");
        std::optional<int64_t> gpst;
        if (!anchors.empty())
            gpst = *anchors.begin();
        if (gpst && !clocks.empty()) {
            const int64_t itow = read<uint32_t>(clocks[0]->payload);
            *gpst += (itow - ((itow + 500) / 1000) * 1000) * 1000000;
            basis = "NAV-CLOCK_iTOW_with_" + basis;
        }
        for (auto r : monitors) {
            const auto runtime = read<uint32_t>(r->payload, 8);
            if (uptime && runtime < *uptime) {
                ++session;
                previous = nullptr;
                temperature = nullptr;
                last_reset.reset();
                ++counts["restarts"];
                events.push_back({{"kind", "receiver_restart"},
                                  {"gpst_ns", optional_time(gpst)},
                                  {"source", r->source},
                                  {"source_offset", r->offset},
                                  {"previous_runtime_s", *uptime},
                                  {"runtime_s", runtime},
                                  {"receiver_session_id", session}});
            }
            uptime = runtime;
            temperature = {{"temperature_c", double(read<int8_t>(r->payload, 18))},
                           {"temperature_reference_gpst_ns", optional_time(gpst)},
                           {"temperature_source", r->source},
                           {"temperature_source_offset", r->offset}};
            std::string hex;
            constexpr char digits[] = "0123456789abcdef";
            for (auto b : r->payload) {
                hex += digits[b >> 4];
                hex += digits[b & 15];
            }
            Json t = {{"kind", "MON-SYS"},
                      {"association", "stream_epoch_not_measurement_time"},
                      {"gpst_ns", optional_time(gpst)},
                      {"source", r->source},
                      {"source_offset", r->offset},
                      {"runtime_s", runtime},
                      {"receiver_session_id", session},
                      {"payload_hex", hex}};
            t.update(temperature);
            telemetry.push_back(std::move(t));
            ++counts["mon_sys_messages"];
        }
        if (clocks.empty())
            return;
        const auto &r = *clocks[0];
        const auto &p = r.payload;
        const auto itow = read<uint32_t>(p);
        const auto bias = read<int32_t>(p, 4), drift = read<int32_t>(p, 8);
        Json row = {{"gpst_ns", optional_time(gpst)},
                    {"iTOW_ms", itow},
                    {"clock_bias_ns", bias},
                    {"clock_drift_ns_s", drift},
                    {"time_accuracy_ns", read<uint32_t>(p, 12)},
                    {"frequency_accuracy_ps_s", read<uint32_t>(p, 16)},
                    {"source", r.source},
                    {"source_offset", r.offset},
                    {"receiver_session_id", session},
                    {"runtime_s", optional_time(uptime)},
                    {"rawx_clock_reset", rawx_reset ? Json(*rawx_reset) : Json()},
                    {"timegps_fTOW_ns", optional_time(ftow)},
                    {"time_basis", basis.empty() ? Json() : Json(basis)},
                    {"temperature_association", "unavailable"}};
        if (!temperature.is_null() && gpst && !temperature.at("temperature_reference_gpst_ns").is_null()) {
            const double age = (*gpst - temperature.at("temperature_reference_gpst_ns").get<int64_t>()) / 1e9;
            if (age >= 0 && age <= temp_age) {
                row.update(temperature);
                row["temperature_age_s"] = age;
                row["temperature_association"] = "stream_epoch_not_measurement_time";
            }
        }
        std::string reason = "arc_start";
        if (!gpst) {
            previous = nullptr;
            temperature = nullptr;
            row["unwrap_quality"] = "unassigned_time";
            ++counts["unassigned_samples"];
        } else {
            bool valid = !previous.is_null();
            double dt = valid ? (*gpst - previous.at("gpst_ns").get<int64_t>()) / 1e9 : 0;
            if (valid && dt <= 0)
                throw std::runtime_error("Non-increasing NAV-CLOCK GPST at " + r.source + ":" +
                                         std::to_string(r.offset));
            if (valid && dt > max_gap) {
                reason = "gap";
                valid = false;
                if (monitors.empty()) {
                    temperature = nullptr;
                    for (auto it = row.begin(); it != row.end();) {
                        if (it.key().starts_with("temperature_"))
                            it = row.erase(it);
                        else
                            ++it;
                    }
                    row["temperature_association"] = "unavailable";
                }
            }
            if (valid) {
                const double jump = double(bias) - previous.at("clock_bias_ns").get<int64_t>() -
                                    previous.at("clock_drift_ns_s").get<int64_t>() * dt;
                const auto milliseconds = int64_t(std::nearbyint(jump / 1e6));
                const int64_t correction = milliseconds * 1000000;
                if (milliseconds && std::abs(jump - correction) <= tolerance) {
                    adjustment += correction;
                    reason = rawx_reset.value_or(false) ? "rawx_confirmed_adjustment" : "bias_inferred_adjustment";
                    ++counts[reason];
                    events.push_back({{"kind", "clock_adjustment"},
                                      {"gpst_ns", *gpst},
                                      {"receiver_session_id", session},
                                      {"clock_arc_id", arc},
                                      {"source", r.source},
                                      {"source_offset", r.offset},
                                      {"previous_gpst_ns", previous.at("gpst_ns")},
                                      {"bias_before_ns", previous.at("clock_bias_ns")},
                                      {"bias_after_ns", bias},
                                      {"drift_ns_s", drift},
                                      {"jump_residual_ns", jump},
                                      {"adjustment_ns", correction},
                                      {"evidence", reason},
                                      {"temperature_c", row.value("temperature_c", Json())},
                                      {"interval_since_previous_adjustment_s",
                                       last_reset ? Json((*gpst - *last_reset) / 1e9) : Json()}});
                    last_reset = *gpst;
                } else if (std::abs(jump) > tolerance || rawx_reset.value_or(false)) {
                    reason = "unresolved_adjustment";
                    valid = false;
                } else
                    reason = "continuous";
            }
            if (!valid) {
                ++arc;
                adjustment = 0;
                last_reset.reset();
                ++counts["clock_arcs"];
                events.push_back({{"kind", "clock_arc_start"},
                                  {"reason", reason},
                                  {"gpst_ns", *gpst},
                                  {"clock_arc_id", arc},
                                  {"receiver_session_id", session}});
            }
            row.update({{"clock_arc_id", arc},
                        {"clock_adjustment_total_ns", adjustment},
                        {"clock_bias_unwrapped_ns", bias - adjustment},
                        {"unwrap_quality", reason}});
            previous = row;
        }
        ++counts["samples"];
        const bool has_temp = row.contains("temperature_c") && !row.at("temperature_c").is_null();
        ++counts[has_temp ? "samples_with_temperature" : "samples_without_temperature"];
        for (auto field : {"gpst_ns", "clock_bias_ns", "clock_drift_ns_s", "temperature_c"}) {
            if (row.contains(field) && !row.at(field).is_null()) {
                const Json value = row.at(field);
                auto [it, inserted] = ranges.try_emplace(field, value, value);
                if (!inserted) {
                    it->second.first = std::min(it->second.first, value);
                    it->second.second = std::max(it->second.second, value);
                }
            }
        }
        if (has_temp) {
            auto &b = bins[int(row.at("temperature_c").get<double>())];
            ++b.n;
            const double delta = drift - b.mean;
            b.mean += delta / b.n;
            b.m2 += delta * (drift - b.mean);
        }
        samples.push_back(std::move(row));
    }
};
ClockProcessor::ClockProcessor(double gap, double tolerance, double age) : state_(std::make_unique<State>()) {
    if (!std::isfinite(gap) || !std::isfinite(tolerance) || !std::isfinite(age) || gap <= 0 || tolerance <= 0 ||
        tolerance >= 500000 || age < 0)
        throw std::invalid_argument("Invalid clock processing policy");
    state_->max_gap = gap;
    state_->tolerance = tolerance;
    state_->temp_age = age;
}
ClockProcessor::~ClockProcessor() = default;
Json ClockProcessor::feed(std::span<const uint8_t> data, const std::string &source) {
    if (!state_->source.empty() && source != state_->source)
        throw std::runtime_error("Call end_file before changing clock source");
    state_->source = source;
    state_->reader.feed(data, [&](const cppgnss::FrameView &frame) { state_->accept(frame); });
    if (state_->reader.invalid || state_->reader.noise)
        throw std::runtime_error("Invalid framing in reconstructed clock input: " + source);
    return state_->drain();
}
Json ClockProcessor::end_file() {
    state_->reader.finish();
    auto out = state_->drain();
    out["size"] = state_->reader.bytes;
    out["frames"] = state_->reader.frames;
    state_->reader = cppgnss::StreamDecoder(cppgnss::Protocol::ubx);
    state_->source.clear();
    return out;
}
Json ClockProcessor::finish() {
    state_->reader.finish();
    state_->finish_epoch();
    return state_->drain();
}
Json ClockProcessor::summary() const {
    const auto &s = *state_;
    Json out = {{"counts", s.counts},
                {"receiver_sessions", s.counts.empty() ? 0 : s.session + 1},
                {"ranges", Json::object()},
                {"temperature_drift_pearson_r", nullptr},
                {"temperature_drift_bins", Json::array()}};
    for (auto &[key, range] : s.ranges)
        out["ranges"][key] = {{"min", range.first}, {"max", range.second}};
    double n = 0, mt = 0, md = 0;
    for (auto &[t, b] : s.bins) {
        n += b.n;
        mt += double(t) * b.n;
        md += b.n * b.mean;
    }
    if (n) {
        mt /= n;
        md /= n;
        double vt = 0, vd = 0, cov = 0;
        for (auto &[t, b] : s.bins) {
            vt += b.n * (t - mt) * (t - mt);
            vd += b.m2 + b.n * (b.mean - md) * (b.mean - md);
            cov += b.n * (t - mt) * (b.mean - md);
            out["temperature_drift_bins"].push_back({{"temperature_c", t},
                                                     {"samples", b.n},
                                                     {"mean_drift_ns_s", b.mean},
                                                     {"stddev_drift_ns_s", std::sqrt(b.m2 / b.n)}});
        }
        if (vt > 0 && vd > 0)
            out["temperature_drift_pearson_r"] = std::clamp(cov / std::sqrt(vt * vd), -1.0, 1.0);
    }
    return out;
}
} // namespace neognss_obs
