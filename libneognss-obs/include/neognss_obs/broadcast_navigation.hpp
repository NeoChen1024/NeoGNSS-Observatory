// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
struct ArrowSchema;
struct ArrowArray;
namespace neognss_obs {
// Processing state, not a CommonNEX catalog. Accepts canonical RawBits only.
class BroadcastNavigation {
  public:
    explicit BroadcastNavigation(std::string setup);
    ~BroadcastNavigation();
    void feed(ArrowSchema *, ArrowArray *);
    void clear();
    // RTKLIB satellite index is internal to native consumers. Position is ECEF
    // at transmit time; availability is evaluated at observation time.
    bool position(int satellite, int64_t observation_ns, double travel_seconds,
                  double *ecef) const;
    uint64_t decoded() const;
    std::map<std::string, uint64_t> decoded_by_family() const;
    bool ecef(char system, int number, int64_t gpst_ns, double *xyz) const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace neognss_obs
