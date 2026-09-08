// SPDX-License-Identifier: GPL-3.0-only
#include <cppgnss/sbf.hpp>
#include <neognss_obs/processing.hpp>
#include <neognss_obs/ubx_archive.hpp>
#include <set>

namespace neognss_obs {
struct DatasetScan::State {
  bool qa;
  cppgnss::Protocol protocol;
  cppgnss::StreamDecoder reader;
  EpochAssembler epochs;
  int64_t gap_ms;
  std::optional<int64_t> previous;
  std::set<uint16_t> sbf_epoch_blocks;
  std::map<uint16_t, int64_t> sbf_times;
  Json pending = Json::array(), output = Json::array(), counts = Json::object();
  State(cppgnss::Protocol p, bool q, int64_t gap)
      : qa(q), protocol(p), reader(p), gap_ms(gap) {
    if (!qa)
      return;
    for (const auto *key : {"epochs", "gaps", "time_reversals", "truncated_tail"})
      counts[key] = uint64_t(0);
    if (protocol == cppgnss::Protocol::ubx) {
      for (const auto *key : {"partial_epochs", "time_conflicts", "untimed_epochs",
                             "duplicate_epochs"})
        counts[key] = uint64_t(0);
    } else {
      for (const auto *key : {"invalid_payloads", "invalid_times",
                             "timestamped_blocks", "duplicate_epoch_blocks"})
        counts[key] = uint64_t(0);
    }
  }
  void count(const char *key) {
    counts[key] = counts.value(key, uint64_t(0)) + 1;
  }
  void time(int64_t now, bool repeated_is_epoch) {
    if (previous) {
      if (qa) {
        if (now < *previous)
          count("time_reversals");
        if (repeated_is_epoch && now == *previous)
          count("duplicate_epochs");
        if (now - *previous > gap_ms)
          count("gaps");
      } else if (now - *previous > gap_ms) {
        output.push_back({{"kind", "end"}, {"gpst_ms", *previous}});
      }
    }
    previous = now;
  }
  void epoch(const ArchiveEpoch &e) {
    if (qa) {
      count("epochs");
      if (e.flags & archive_partial)
        count("partial_epochs");
      if (e.flags & archive_time_conflict)
        count("time_conflicts");
      if (e.gpst_ms == unknown_gpst)
        count("untimed_epochs");
    }
    if (e.gpst_ms != unknown_gpst && (e.nav_frames || !pending.empty()))
      time(e.gpst_ms, true);
    if (!qa && !pending.empty()) {
      if (e.gpst_ms == unknown_gpst || (e.flags & archive_time_conflict))
        throw std::runtime_error(
            "Cannot assign an unambiguous GPST to SBAS epoch");
      for (auto &row : pending) {
        row["gpst_ms"] = e.gpst_ms;
        output.push_back(std::move(row));
      }
    }
    pending.clear();
  }
  void accept(const cppgnss::FrameView &f) {
    if (protocol == cppgnss::Protocol::ubx) {
      epochs.accept(
          f, [&](const ArchiveEpoch &e) { epoch(e); },
          [&] {
            if (qa || f.id != 0x0213)
              return;
            auto parsed =
                UBX::parse_subframe(UBX::ubx_frame(f.wire.subspan(2)));
            if (!parsed.subframe)
              return;
            const auto &s = *parsed.subframe;
            if (s.signal.gnssId != 1 || s.signal.sigId != 0)
              return;
            auto message = sbas_message(UBX::parse_sbas(s));
            if (!message.contains("hex"))
              return;
            if (pending.size() >= 65536)
              throw std::runtime_error(
                  "Unresolved SBAS epoch exceeds buffer limit");
            pending.push_back(
                {{"kind", "frame"}, {"prn", s.signal.svId}, {"sbas", message}});
          });
    } else if (qa) {
      // SBF schemas define whether a block actually carries TOW/WNc.
      const auto block = cppgnss::SBF::decode(f.id, f.revision, f.payload);
      if (block.status == cppgnss::SBF::Status::invalid_payload) {
        count("invalid_payloads");
        return;
      }
      if (!block.fields.contains("TOW") || !block.fields.contains("WNc"))
        return;
      const auto tow = std::get<uint64_t>(block.fields.at("TOW"));
      const auto week = std::get<uint64_t>(block.fields.at("WNc"));
      if (tow >= 604800000 || week == 65535) {
        count("invalid_times");
        return;
      }
      const int64_t now = int64_t(week) * 604800000 + tow;
      count("timestamped_blocks");
      if (sbf_times.contains(f.id) && now < sbf_times[f.id])
        count("time_reversals");
      sbf_times[f.id] = now;
      // Different block types can be emitted with different latency.
      // Only the navigation/measurement epoch blocks define cadence.
      if (f.id != 4027 && f.id != 4007)
        return;
      if (!previous || now != *previous) {
        sbf_epoch_blocks.clear();
        count("epochs");
      }
      // These are epoch-level blocks; repeated raw-navigation blocks at
      // one TOW can belong to different satellites and are not duplicates.
      if ((f.id == 4027 || f.id == 4007) &&
          !sbf_epoch_blocks.insert(f.id).second)
        count("duplicate_epoch_blocks");
      if (previous && now - *previous > gap_ms)
        count("gaps");
      previous = now;
    }
  }
  Json drain() {
    Json result = Json::array();
    result.swap(output);
    return result;
  }
};
DatasetScan::DatasetScan(const std::string &protocol, bool qa,
                         double gap_timeout) {
  if ((protocol != "ubx" && protocol != "sbf") || !std::isfinite(gap_timeout) ||
      gap_timeout <= 0)
    throw std::invalid_argument("Invalid dataset protocol or gap timeout");
  state_ = std::make_unique<State>(protocol == "ubx" ? cppgnss::Protocol::ubx
                                                     : cppgnss::Protocol::sbf,
                                   qa, std::llround(gap_timeout * 1000));
}
DatasetScan::~DatasetScan() = default;
Json DatasetScan::feed(std::span<const uint8_t> bytes) {
  state_->reader.feed(bytes,
                      [&](const cppgnss::FrameView &f) { state_->accept(f); });
  return state_->drain();
}
Json DatasetScan::finish() {
  try {
    state_->reader.finish();
  } catch (const std::runtime_error &) {
    if (!state_->qa)
      throw;
    state_->count("truncated_tail");
  }
  state_->epochs.finish(archive_partial,
                        [&](const ArchiveEpoch &e) { state_->epoch(e); });
  if (!state_->qa && state_->previous)
    state_->output.push_back({{"kind", "end"}, {"gpst_ms", *state_->previous}});
  return state_->drain();
}
Json DatasetScan::summary() const {
  const auto &s = *state_;
  return {{"source_bytes", s.reader.bytes},
          {"frames", s.reader.frames},
          {"invalid_frames", s.reader.invalid},
          {"noise_bytes", s.reader.noise},
          {"skipped_protocol_frames", s.reader.skipped_protocol_frames},
          {"skipped_protocol_bytes", s.reader.skipped_protocol_bytes},
          {"qa", s.counts}};
}
} // namespace neognss_obs
