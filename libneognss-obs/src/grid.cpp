// SPDX-License-Identifier: GPL-3.0-only
#include <neognss_obs/processing.hpp>
#include <set>

namespace neognss_obs {
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
    Json rows = Json::array();
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
            auto coordinate = cppgnss::SBAS::igp_coordinate(key.first, key.second);
            if (!coordinate)
                throw std::runtime_error("Invalid IGP coordinate");
            const double delay = c.delay * 0.125;
            rows.push_back({{"start_gpst_ms", c.start},
                            {"end_gpst_ms", end},
                            {"band", key.first},
                            {"mask_bit", key.second},
                            {"latitude", coordinate->first},
                            {"longitude", coordinate->second},
                            {"delay_m", delay},
                            {"vtec_tecu", delay * (1575.42e6 * 1575.42e6 / (40.3 * 1e16))},
                            {"iodi", c.iodi},
                            {"givei", c.givei},
                            {"frame_offset", c.offset}});
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
    void accept(const Json &row) {
        const int64_t time = row.at("gpst_ms");
        if (last && time < *last)
            throw std::runtime_error("SBAS reception-context time reversed");
        if (last && gap_timeout > 0 && time - *last > gap_timeout) {
            reset(*last);
            ++diagnostics["signal_gap_reset"];
        }
        last = time;
        const auto &message = row.at("sbas");
        if (message.value("status", "") != "decoded") {
            ++diagnostics["unparsed_or_invalid"];
            return;
        }
        const int type = message.at("type");
        const auto &content = message.at("content");
        if (type == 0) {
            reset(time);
            last = time;
            ++diagnostics["test_mode_reset"];
        } else if (type == 18) {
            flush(time);
            const int new_count = content.at("number_of_bands_raw"), band = content.at("band"),
                      new_iodi = content.at("iodi");
            auto positions = content.at("active_mask_positions").get<std::vector<int>>();
            bool valid = new_count >= 1 && new_count <= 11 &&
                         std::set<int>(positions.begin(), positions.end()).size() == positions.size();
            for (auto p : positions)
                valid = valid && cppgnss::SBAS::igp_coordinate(band, p).has_value();
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
            const int band = content.at("band"), block = content.at("block");
            if (content.at("iodi") != iodi || !masks.contains(band) || deadline() <= time) {
                ++diagnostics["correction_without_current_complete_mask"];
                return;
            }
            const auto &positions = masks.at(band).positions;
            size_t i = 0;
            for (auto &correction : content.at("corrections")) {
                const size_t ordinal = block * 15 + i++;
                if (ordinal >= positions.size())
                    continue;
                Key key{band, positions[ordinal]};
                if (cells.contains(key)) {
                    flush_cell(key, cells.at(key), time);
                    cells.erase(key);
                }
                const int delay = correction.at("delay_raw"), givei = correction.at("givei");
                const std::string status = correction.at("status");
                if (status != "usable" || delay == 511 || givei == 15) {
                    ++diagnostics[status];
                    continue;
                }
                cells[key] = {time, time + correction_age, delay, givei, iodi, row.at("offset").get<uint64_t>()};
            }
        }
    }
    Json drain() {
        Json out = Json::array();
        out.swap(rows);
        return out;
    }
};
GridProcessor::GridProcessor(double correction_age, double mask_age, double gap_timeout) : state_(std::make_unique<State>()) {
    if (!std::isfinite(correction_age) || !std::isfinite(mask_age) || !std::isfinite(gap_timeout) ||
        correction_age <= 0 || mask_age <= 0 || gap_timeout < 0)
        throw std::invalid_argument("Invalid SBAS aging policy");
    state_->correction_age = std::llround(correction_age * 1000);
    state_->mask_age = std::llround(mask_age * 1000);
    state_->gap_timeout = std::llround(gap_timeout * 1000);
}
GridProcessor::~GridProcessor() = default;
Json GridProcessor::process(const Json &rows) {
    for (auto &row : rows)
        state_->accept(row);
    return state_->drain();
}
Json GridProcessor::finish(int64_t time) {
    if (state_->last && time < *state_->last)
        throw std::runtime_error("SBAS group end precedes its last message");
    state_->flush(time);
    state_->cells.clear();
    return state_->drain();
}
Json GridProcessor::process_frames(const Json &rows) {
    for (const auto &row : rows) {
        const auto &bytes = row.at("frame").get_binary();
        if (bytes.size() != 32 || (bytes.back() & 63))
            throw std::runtime_error("Expected canonical 250-bit SBAS frame with zero padding");
        auto message = sbas_message(cppgnss::SBAS::parse_l1(bytes));
        if (message.value("crc_valid", false) != row.at("crc_valid").get<bool>())
            throw std::runtime_error("SBAS frame CRC metadata mismatch");
        if (!row.at("accepted").is_null() && !row.at("accepted").get<bool>())
            message["status"] = "receiver_rejected";
        state_->accept({{"gpst_ms", row.at("gpst_ms")}, {"offset", row.at("frame_id")}, {"sbas", message}});
    }
    return state_->drain();
}
Json GridProcessor::diagnostics() const { return state_->diagnostics; }
} // namespace neognss_obs
