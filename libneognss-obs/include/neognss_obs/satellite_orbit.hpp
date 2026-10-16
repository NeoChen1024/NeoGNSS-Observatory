// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <boost/int128/int128.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
struct ArrowSchema;
struct ArrowArray;

// Satellite geometry outside positioning solvers. See
// docs/satellite-geometry.md for model, selection and product rules.
namespace neognss_obs {
// Exact GPST picoseconds since 1980-01-06T00:00:00.
using GpstTicks = boost::int128::int128;

enum class SatelliteSystem : char {
    Gps = 'G',
    Galileo = 'E',
    Beidou = 'C',
    Qzss = 'J',
};
struct SatelliteId {
    SatelliteSystem system;
    uint16_t number;
    bool operator==(const SatelliteId &) const = default;
};

enum class OrbitStatus : uint8_t {
    Available,
    NoCandidate,
    OutsideApplicability,
    Unhealthy,
    HealthUnknown,
    ProductGap,
    ProductEdge,
    MissingValue,
    ProductFlagged,
    Unsupported,
};
// Broadcast orbits refer to each family's ICD reference; SP3 to the centre of
// mass. Conversion between them is the caller's responsibility.
enum class ReferencePoint : uint8_t { Broadcast, CenterOfMass };
enum class ClockSource : uint8_t { None, Broadcast, ClockProduct, Sp3 };

struct OrbitQuery {
    SatelliteId satellite;
    GpstTicks time;
    // Broadcast candidates must have been received strictly before this time;
    // normally the reception time. Precise products ignore it.
    std::optional<GpstTicks> available_before;
};
struct SatelliteState {
    OrbitStatus status = OrbitStatus::NoCandidate;
    // Valid only when status is Available. ECEF in the system's own frame.
    std::array<double, 3> position_m{}, velocity_m_s{};
    // Against GPST, GST or BDT by system; excludes the relativistic term.
    double clock_s = 0, relativistic_s = 0;
    ReferencePoint reference_point = ReferencePoint::Broadcast;
    ClockSource clock_source = ClockSource::None;
    // Broadcast source identity; empty for precise products.
    std::string family;
    std::optional<int64_t> issue;
    std::optional<GpstTicks> toe;
};

class OrbitSource {
  public:
    virtual ~OrbitSource() = default;
    // states.size() must equal queries.size().
    virtual void evaluate(std::span<const OrbitQuery> queries,
                          std::span<SatelliteState> states) const = 0;
};

// Selects among decoder ephemeris occurrences; never decodes navigation bits.
class BroadcastEphemerides final : public OrbitSource {
  public:
    explicit BroadcastEphemerides(std::string setup);
    ~BroadcastEphemerides() override;
    // One decoder ephemeris batch; the output kind determines the model.
    void feed(const std::string &kind, ArrowSchema *, ArrowArray *);
    void evaluate(std::span<const OrbitQuery>,
                  std::span<SatelliteState>) const override;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

struct PreciseProductOptions {
    // Maximum spacing of the two bracketing clock-product samples.
    GpstTicks max_clock_spacing = GpstTicks(60) * 1000000000000;
    // Use an SP3 clock only when explicitly enabled and the clock product has
    // no bracketing value.
    bool sp3_clock_fallback = false;
};
class PreciseProducts final : public OrbitSource {
  public:
    explicit PreciseProducts(PreciseProductOptions options = {});
    ~PreciseProducts() override;
    // Replace loaded products; files are joined in time order.
    void load(const std::vector<std::string> &sp3,
              const std::vector<std::string> &clock);
    void evaluate(std::span<const OrbitQuery>,
                  std::span<SatelliteState>) const override;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

struct TransmitGeometry {
    SatelliteState state;
    GpstTicks transmit_time;
    // Satellite position rotated into the reception-time ECEF frame.
    std::array<double, 3> position_m{}, line_of_sight{};
    double range_m = 0;
};
// No receiver clock or atmospheric delay. state.status reports the source.
TransmitGeometry transmit_geometry(const OrbitSource &source,
                                   SatelliteId satellite, GpstTicks reception,
                                   const std::array<double, 3> &receiver_m);
} // namespace neognss_obs
