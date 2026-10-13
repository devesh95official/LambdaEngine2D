#pragma once
// Autopilot. Dodges a wolf winding up close by (sideways, dashing late in the wind-up), else walks
// to the nearest lantern (straight when clear, A* round the trees). Presses confirm on the title
// and 120 ticks after a death so headless runs go on.
#include <cstdint>
#include <span>
#include <lam/input/driver.h>
#include <lam/tilemap/grid_path.h>

#include "sim/controls.h"

namespace lantern {

class Autopilot final : public lam::InputDriver {
public:
    explicit Autopilot(Controls controls) : controls_(controls) {}
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;

private:
    Controls controls_;
    lam::PathFinder paths_;                                     // the bot's own A* (not the wolves' budget)
    lam::PathRoute route_;
    std::uint64_t route_tick_ = 0;
    int sized_cells_ = 0;
};

} // namespace lantern
