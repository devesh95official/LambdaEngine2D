#pragma once
// Autoplay. Writes only what a player could press. Drinks a potion when low, fights adjacent
// monsters, rests when hurt and alone, picks up known items, explores the nearest frontier and
// takes the stairs once the floor is explored (or it lingered). No state beyond scratch buffers.
#include <cstdint>
#include <lam/input/driver.h>
#include <lam/math/grid.h>
#include <lam/tilemap/grid_maps.h>
#include <span>
#include <vector>

namespace rl {
class BotDriver final : public lam::InputDriver {
public:
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
    std::uint32_t decisions() const { return decisions_; } // inputs produced so far
    // Permadeath: after the hero dies the focused world is the game-over menu (no RunState) above
    // the dead run's world. The bot then sends `intent` every tick (the executable passes the
    // menu's "New run" selection) until the menu closes. Without it the bot stops at the menu.
    void set_restart_intent(const lam::Intent &intent) { restart_ = intent; }

private:
    // Distance field over explored, walkable cells toward `goals_` (scratch, rebuilt per decision).
    bool route(const lam::World &w, lam::IVec2 from);
    lam::DijkstraMap map_;
    lam::Grid<std::uint16_t> cost_;
    std::vector<lam::IVec2> goals_;
    std::uint32_t decisions_ = 0;
    lam::Intent restart_{};
};
} // namespace rl
