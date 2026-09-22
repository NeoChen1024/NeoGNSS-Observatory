// SPDX-License-Identifier: GPL-3.0-only
#include <neognss_obs/processing.hpp>
#include <set>

namespace neognss_obs {
namespace {
struct GridUpdate {
    struct Correction {
        int delay, givei;
        std::string_view status;
    };
    int type = -1, count = 0, band = 0, iodi = 0, block = 0;
    std::vector<int> positions;
    std::vector<Correction> corrections;
};
GridUpdate update(const SBAS::Result &r, bool accepted) {
    GridUpdate u;
    if (!accepted || r.status != SBAS::Status::decoded)
        return u;
    const auto &m = *r.message;
    u.type = m.type;
    if (auto p = std::get_if<SBAS::IonosphericMask>(&m.content)) {
        u.count = p->number_of_bands_raw;
        u.band = p->band;
        u.iodi = p->iodi;
        for (size_t i = 0; i < p->mask.size(); ++i)
            if (p->mask[i])
                u.positions.push_back(int(i + 1));
    } else if (auto p = std::get_if<SBAS::IonosphericDelay>(&m.content)) {
        u.band = p->band;
        u.block = p->block;
        u.iodi = p->iodi;
        for (const auto &c : p->corrections) {
            auto status = c.status();
            u.corrections.push_back(
                {c.delay_raw, c.givei,
                 status == SBAS::IgpStatus::usable       ? "usable"
                 : status == SBAS::IgpStatus::do_not_use ? "do_not_use"
                                                         : "not_monitored"});
        }
    }
    return u;
}
GridUpdate update(const Json &m) {
    GridUpdate u;
    if (m.value("status", "") != "decoded")
        return u;
    u.type = m.at("type");
    const auto &c = m.at("content");
    if (u.type == 18) {
        u.count = c.at("number_of_bands_raw");
        u.band = c.at("band");
        u.iodi = c.at("iodi");
        u.positions = c.at("active_mask_positions").get<std::vector<int>>();
    } else if (u.type == 26) {
        u.band = c.at("band");
        u.block = c.at("block");
        u.iodi = c.at("iodi");
        for (const auto &v : c.at("corrections"))
            u.corrections.push_back(
                {v.at("delay_raw"), v.at("givei"),
                 v.at("status").get_ref<const std::string &>()});
    }
    return u;
}
Json json_intervals(const std::vector<GridInterval> &rows) {
    Json out = Json::array();
    for (const auto &r : rows)
        out.push_back({{"start_gpst_ms", r.start_gpst_ms},
                       {"end_gpst_ms", r.end_gpst_ms},
                       {"band", r.band},
                       {"mask_bit", r.mask_bit},
                       {"iodi", r.iodi},
                       {"givei", r.givei},
                       {"frame_offset", r.frame_id},
                       {"latitude", r.latitude},
                       {"longitude", r.longitude},
                       {"delay_m", r.delay_m},
                       {"vtec_tecu", r.vtec_tecu}});
    return out;
}
} // namespace
struct GridProcessor::State {
    struct Mask {
        std::vector<int> positions;
        int64_t time;
    };
    struct Cell {
        int64_t start, expiry;
        int delay, givei, iodi;
        uint64_t offset;
    };
    using Key = std::pair<int, int>;
    int64_t correction_age, mask_age, gap_timeout;
    std::map<int, Mask> masks;
    std::map<Key, Cell> cells;
    int iodi = -1, count = -1;
    std::optional<int64_t> last;
    std::map<std::string, uint64_t> diagnostics;
    std::vector<GridInterval> rows;
    int64_t deadline() const {
        if (masks.empty() || int(masks.size()) != count)
            return -1;
        auto end = std::numeric_limits<int64_t>::max();
        for (auto &[b, m] : masks)
            end = std::min(end, m.time + mask_age);
        return end;
    }
    void flush_cell(Key key, Cell &c, int64_t time) {
        const auto end = std::min({time, c.expiry, deadline()});
        if (end > c.start) {
            auto coordinate =
                neognss_obs::SBAS::igp_coordinate(key.first, key.second);
            if (!coordinate)
                throw std::runtime_error("Invalid IGP coordinate");
            const double delay = c.delay * 0.125;
            rows.push_back({c.start, end, 0, key.first, key.second, c.iodi,
                            c.givei, int64_t(c.offset), 0,
                            double(coordinate->first),
                            double(coordinate->second), delay,
                            delay * (1575.42e6 * 1575.42e6 / (40.3 * 1e16))});
        }
        c.start = time;
    }
    void flush(int64_t time) {
        for (auto &[key, c] : cells)
            flush_cell(key, c, time);
    }
    void reset(int64_t time) {
        flush(time);
        cells.clear();
        masks.clear();
        iodi = count = -1;
        last.reset();
    }
    void accept(int64_t time, uint64_t offset, const GridUpdate &content) {
        if (last && time < *last)
            throw std::runtime_error("SBAS reception-context time reversed");
        if (last && gap_timeout > 0 && time - *last > gap_timeout) {
            reset(*last);
            ++diagnostics["signal_gap_reset"];
        }
        last = time;
        if (content.type < 0) {
            ++diagnostics["unparsed_or_invalid"];
            return;
        }
        const int type = content.type;
        if (type == 0) {
            reset(time);
            last = time;
            ++diagnostics["test_mode_reset"];
        } else if (type == 18) {
            flush(time);
            const int new_count = content.count, band = content.band,
                      new_iodi = content.iodi;
            auto positions = content.positions;
            bool valid =
                new_count >= 1 && new_count <= 11 &&
                std::set<int>(positions.begin(), positions.end()).size() ==
                    positions.size();
            for (auto p : positions)
                valid = valid &&
                        neognss_obs::SBAS::igp_coordinate(band, p).has_value();
            if (!valid) {
                reset(time);
                ++diagnostics["invalid_mask"];
                return;
            }
            if (iodi != new_iodi || count != new_count) {
                cells.clear();
                masks.clear();
                iodi = new_iodi;
                count = new_count;
            } else if (deadline() <= time) {
                for (auto it = masks.begin(); it != masks.end();) {
                    if (it->second.time + mask_age <= time) {
                        cells.clear();
                        it = masks.erase(it);
                    } else
                        ++it;
                }
            }
            if (masks.contains(band) && masks.at(band).positions != positions) {
                cells.clear();
                masks.clear();
                ++diagnostics["same_iodi_mask_changed"];
            }
            masks[band] = {std::move(positions), time};
        } else if (type == 26) {
            const int band = content.band, block = content.block;
            if (content.iodi != iodi || !masks.contains(band) ||
                deadline() <= time) {
                ++diagnostics["correction_without_current_complete_mask"];
                return;
            }
            const auto &positions = masks.at(band).positions;
            size_t i = 0;
            for (auto &correction : content.corrections) {
                const size_t ordinal = block * 15 + i++;
                if (ordinal >= positions.size())
                    continue;
                Key key{band, positions[ordinal]};
                if (cells.contains(key)) {
                    flush_cell(key, cells.at(key), time);
                    cells.erase(key);
                }
                const int delay = correction.delay, givei = correction.givei;
                const std::string status(correction.status);
                if (status != "usable" || delay == 511 || givei == 15) {
                    ++diagnostics[status];
                    continue;
                }
                cells[key] = {time,  time + correction_age, delay, givei, iodi,
                              offset};
            }
        }
    }
    std::vector<GridInterval> drain() {
        std::vector<GridInterval> out;
        out.swap(rows);
        return out;
    }
};
GridProcessor::GridProcessor(double correction_age, double mask_age,
                             double gap_timeout)
    : state_(std::make_unique<State>()) {
    if (!std::isfinite(correction_age) || !std::isfinite(mask_age) ||
        !std::isfinite(gap_timeout) || correction_age <= 0 || mask_age <= 0 ||
        gap_timeout < 0)
        throw std::invalid_argument("Invalid SBAS aging policy");
    state_->correction_age = std::llround(correction_age * 1000);
    state_->mask_age = std::llround(mask_age * 1000);
    state_->gap_timeout = std::llround(gap_timeout * 1000);
}
GridProcessor::~GridProcessor() = default;
Json GridProcessor::process(const Json &rows) {
    for (auto &row : rows)
        state_->accept(row.at("gpst_ms"), row.at("offset"),
                       update(row.at("sbas")));
    return json_intervals(state_->drain());
}
Json GridProcessor::finish(int64_t time) {
    return json_intervals(finish_intervals(time));
}
std::vector<GridInterval> GridProcessor::finish_intervals(int64_t time) {
    if (state_->last && time < *state_->last)
        throw std::runtime_error("SBAS group end precedes its last message");
    state_->flush(time);
    state_->cells.clear();
    return state_->drain();
}
Json GridProcessor::process_frames(const Json &rows) {
    std::vector<GridFrame> batch;
    for (const auto &r : rows) {
        const auto &bytes = r.at("frame").get_binary();
        if (bytes.size() != 32)
            throw std::runtime_error("Expected canonical SBAS body");
        GridFrame f{r.at("gpst_ms"),
                    r.at("frame_id"),
                    {},
                    r.at("crc_valid"),
                    r.at("accepted").is_null() || r.at("accepted").get<bool>()};
        std::copy(bytes.begin(), bytes.end(), f.bytes.begin());
        batch.push_back(f);
    }
    return json_intervals(process_frames(std::span<const GridFrame>(batch)));
}
std::vector<GridInterval>
GridProcessor::process_frames(std::span<const GridFrame> frames) {
    for (const auto &f : frames) {
        if (f.bytes.back() & 63)
            throw std::runtime_error(
                "Expected canonical 250-bit SBAS frame with zero padding");
        auto parsed = SBAS::parse_l1(f.bytes);
        if ((parsed.message && parsed.message->crc_valid) != f.crc_valid)
            throw std::runtime_error("SBAS frame CRC metadata mismatch");
        state_->accept(f.gpst_ms, f.frame_id, update(parsed, f.accepted));
    }
    return state_->drain();
}
Json GridProcessor::diagnostics() const { return state_->diagnostics; }
} // namespace neognss_obs
