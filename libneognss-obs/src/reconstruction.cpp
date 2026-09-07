// SPDX-License-Identifier: GPL-3.0-only
#include <chrono>
#include <filesystem>
#include <format>
#include <neognss_obs/processing.hpp>
#include <neognss_obs/ubx_archive.hpp>
#include <set>

namespace neognss_obs {
struct SegmentPlanner::State {
    int64_t timeout;
    std::map<std::pair<std::string, uint64_t>, int64_t> overrides;
    std::vector<std::pair<std::string, ArchiveEpoch>> group;
    Json artifacts = Json::array(), events = Json::array();
    std::map<std::string, Json> unassigned;
    std::set<std::string> names;
    std::optional<size_t> segment;
    std::optional<int64_t> last;
    std::string pending = "first_available";
    static void append(Json &spans, const std::string &path, uint64_t begin, uint64_t end) {
        if (!spans.empty() && spans.back().at("source") == path && spans.back().at("end") == begin)
            spans.back()["end"] = end;
        else
            spans.push_back({{"source", path}, {"begin", begin}, {"end", end}});
    }
    void quarantine(const std::string &path, const ArchiveEpoch &e, const std::string &reason) {
        if (!unassigned.contains(path))
            unassigned[path] = Json::array();
        append(unassigned.at(path), path, e.begin, e.end);
        events.push_back({{"type", reason},
                          {"source", path},
                          {"begin", e.begin},
                          {"end", e.end},
                          {"gpst_ms", e.gpst_ms == unknown_gpst ? Json() : Json(e.gpst_ms)}});
    }
    void flush() {
        if (group.empty())
            return;
        const auto time = group.front().second.gpst_ms;
        uint64_t nav = 0;
        for (auto &[p, e] : group)
            nav += e.nav_frames;
        if (!nav) {
            for (auto &[p, e] : group)
                quarantine(p, e, "no_nav_interval");
            group.clear();
            return;
        }
        for (auto &[p, e] : group)
            if (e.flags & archive_time_conflict)
                throw std::runtime_error("Conflicting GPST epoch: " + std::to_string(time));
        if (last && time < *last)
            throw std::runtime_error("Reconstructed time runs backwards");
        auto reason = pending;
        if (last && time > *last + timeout) {
            reason = "missing_interval";
            segment.reset();
            events.push_back({{"type", reason},
                              {"after_gpst_ms", *last},
                              {"next_gpst_ms", time},
                              {"interval_ms", time - *last},
                              {"gap_timeout_ms", timeout}});
        }
        if (last && time / 86400000 != *last / 86400000) {
            if (segment)
                reason = "gpst_midnight";
            segment.reset();
        }
        if (!segment) {
            using namespace std::chrono;
            // Calendar arithmetic in GPST; sys_time is only a formatting carrier.
            const auto date = sys_days(year(1980) / January / 6) + milliseconds(time);
            const auto name = std::format("GPST-{:%Y-%m-%d--%H-%M-%S}-{:03}.ubx", floor<seconds>(date), time % 1000);
            if (!names.insert(name).second)
                throw std::runtime_error("Segment filename collision: " + name);
            artifacts.push_back({{"name", name},
                                 {"kind", "gpst_segment"},
                                 {"start_gpst_ms", time},
                                 {"end_gpst_ms", time},
                                 {"start_gpst", time / 1000.0},
                                 {"end_gpst", time / 1000.0},
                                 {"gap_timeout_ms", timeout},
                                 {"max_observed_interval_ms", 0},
                                 {"reason", reason},
                                 {"frames", 0},
                                 {"nav_epochs", 0},
                                 {"spans", Json::array()}});
            segment = artifacts.size() - 1;
        }
        auto &a = artifacts.at(*segment);
        for (auto &[path, e] : group) {
            append(a["spans"], path, e.begin, e.end);
            a["frames"] = a["frames"].get<uint64_t>() + e.frames;
        }
        const auto n = a["nav_epochs"].get<uint64_t>();
        if (n)
            a["max_observed_interval_ms"] =
                std::max(a["max_observed_interval_ms"].get<int64_t>(), time - a["end_gpst_ms"].get<int64_t>());
        a["nav_epochs"] = n + 1;
        a["end_gpst_ms"] = time;
        a["end_gpst"] = time / 1000.0;
        last = time;
        pending = "continuation";
        group.clear();
    }
};
SegmentPlanner::SegmentPlanner(const Json &joins, int64_t timeout) : state_(std::make_unique<State>()) {
    if (timeout < 1)
        throw std::invalid_argument("Gap timeout must be positive milliseconds");
    state_->timeout = timeout;
    for (const auto &j : joins)
        if (j.value("kind", "") == "split_epoch_continuation")
            state_->overrides[{j.at("previous").get<std::string>(), j.at("previous_epoch_begin").get<uint64_t>()}] =
                j.at("anchor_gpst_ms");
}
SegmentPlanner::~SegmentPlanner() = default;
void SegmentPlanner::feed(const Json &source, std::span<const uint8_t> index) {
    if (index.size() < 48 || std::string_view(reinterpret_cast<const char *>(index.data()), 8) != "UBXIDX04" ||
        (index.size() - 48) % 56)
        throw std::runtime_error("Expected UBXIDX04 epoch index");
    const std::string path = source.at("path");
    for (size_t pos = 48; pos < index.size(); pos += 56) {
        ArchiveEpoch e{UBX::read_le<uint64_t>(index, pos),      UBX::read_le<uint64_t>(index, pos + 8),
                       UBX::read_le<int64_t>(index, pos + 16),  UBX::read_le<int64_t>(index, pos + 24),
                       UBX::read_le<uint64_t>(index, pos + 32), UBX::read_le<uint32_t>(index, pos + 40),
                       UBX::read_le<uint32_t>(index, pos + 44), UBX::read_le<uint32_t>(index, pos + 48),
                       UBX::read_le<int32_t>(index, pos + 52)};
        if (auto it = state_->overrides.find({path, e.begin}); it != state_->overrides.end())
            e.gpst_ms = it->second;
        if (e.flags & archive_noise) {
            state_->events.push_back(
                {{"type", "non_ubx_or_corrupt_bytes"}, {"source", path}, {"begin", e.begin}, {"end", e.end}});
            continue;
        }
        if (e.begin < source.at("begin").get<uint64_t>() || e.end > source.at("end").get<uint64_t>())
            continue;
        if (e.gpst_ms == unknown_gpst) {
            state_->quarantine(path, e, "unknown_time");
            continue;
        }
        if (!state_->group.empty() && e.gpst_ms != state_->group.front().second.gpst_ms)
            state_->flush();
        state_->group.emplace_back(path, e);
    }
}
Json SegmentPlanner::finish() {
    state_->flush();
    for (auto &[path, spans] : state_->unassigned)
        state_->artifacts.push_back({{"name", "unassigned/" + std::filesystem::path(path).filename().string()},
                                     {"kind", "unassigned_frames"},
                                     {"spans", spans}});
    state_->unassigned.clear();
    for (auto &a : state_->artifacts) {
        uint64_t size = 0;
        for (auto &s : a["spans"])
            size += s["end"].get<uint64_t>() - s["begin"].get<uint64_t>();
        a["size"] = size;
    }
    return {{"artifacts", state_->artifacts}, {"events", state_->events}};
}
} // namespace neognss_obs
