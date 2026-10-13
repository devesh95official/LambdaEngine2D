#pragma once
// The autoplay bot. Reads the focused world and presses the same actions a player would.
//  - play world (has TrailState): breadth-first search from the traveller to the nearest apple,
//    avoiding walls and the trail; if no apple is reachable, the free turn with the most room;
//  - any other world (title, game over): press confirm once that world has run `confirm_delay`
//    ticks (an attract-mode pause; the game uses 90 ticks after a bump).
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include <lam/input/driver.h>
#include <lam/math/grid.h>
#include "trail_rules.h"

namespace trail {

// Reusable buffers so the per-tick search does not allocate after warm-up.
struct BfsScratch {
    lam::Grid<std::int8_t> first;                               // first step of the path to a cell; -1 = unseen
    std::vector<lam::IVec2> queue;
};
// Pure search over a board. blocked(x, y) != 0 is a wall or a trail piece. Never returns the
// reversal of `heading`. nullopt when every turn is blocked.
std::optional<Dir> autoplay_choice(const lam::Grid<std::uint8_t> &blocked, lam::IVec2 head, Dir heading,
                                   std::span<const lam::IVec2> targets, BfsScratch &scratch);

class TrailBot final : public lam::InputDriver {
public:
    explicit TrailBot(Controls controls, int confirm_delay_ticks = 60);
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
    // The bot's turn for the run in `w` (nullopt: keep going / nothing to do). Used by fill and tests.
    std::optional<Dir> choose(lam::World &w);

private:
    Controls controls_;
    int confirm_delay_;
    lam::Grid<std::uint8_t> blocked_;
    std::vector<lam::IVec2> targets_;
    BfsScratch scratch_;
};

} // namespace trail
