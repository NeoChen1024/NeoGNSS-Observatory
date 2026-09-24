// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <neognss_obs/ppp.hpp>

namespace neognss_obs {
class BroadcastNavigation;
struct StecSample {
    int64_t gpst_ns, arc_id, receiver_segment_start_ns;
    int32_t prn, system, pair_id, product_issues;
    double raw_phase_gf_m, relative_stec_tecu;
    double phase_gf_m, code_gf_corrected_m, elevation_deg, azimuth_deg;
    double ipp_latitude_deg, ipp_longitude_deg, mapping, gim_stec_tecu,
        gim_rms_tecu;
};
struct StecArc {
    int64_t gpst_ns, end_ns, arc_id, samples, leveling_samples,
        receiver_segment_start_ns;
    int32_t prn, system, pair_id, valid, start_reason, end_reason, provisional;
    double level_offset_m, scatter_m;
};
struct StecResult {
    std::vector<StecSample> samples;
    std::vector<StecArc> arcs;
};
class StecProcessor {
  public:
    explicit StecProcessor(const Json &settings);
    ~StecProcessor();
    void products(const Json &products);
    void navigation(std::shared_ptr<BroadcastNavigation> navigation);
    StecResult process(const ObservationBatch &batch);
    void restarts(std::span<const int64_t> boundaries);
    std::vector<StecArc> finish();
    std::vector<StecArc> preview() const;
    Json checkpoint() const;
    void restore(const Json &state);
    Json summary() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
// Inputs are already leveled, valid arc samples. Fit one receiver/signal-pair
// window, never an independent zero point for each satellite or arc.
struct DcbSample {
    int64_t gpst_ns, arc_id;
    int32_t prn;
    double residual_tecu, elevation_deg, azimuth_deg, gim_rms_tecu;
};
Json fit_receiver_dcb(std::span<const DcbSample>, const Json &settings);
} // namespace neognss_obs
