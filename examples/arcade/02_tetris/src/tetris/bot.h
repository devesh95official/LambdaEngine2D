#pragma once
// --bot: scores every rotation and column for the current piece and the hold (or next) piece,
// by height, holes, bumpiness and lines, then holds, rotates, moves and hard-drops.
#include <cstdint>
#include <span>
#include <lam/input/driver.h>
#include "tetris/rules.h"

namespace tetris {

struct Plan { int rot = 0, x = 0; double score = -1e18; };
// Best placement for `kind` on `b`.
Plan best_placement(const Board &b, int kind);

class Bot final : public lam::InputDriver {
public:
    explicit Bot(Controls c, bool reckless = false) : c_(c), reckless_(reckless) {}
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;

private:
    Controls c_;
    bool reckless_;                                             // --bot-reckless: hard-drop right away
    std::uint32_t key_ = 0xFFFFFFFFu;
    Plan plan_;
    bool use_hold_ = false;
    int actions_ = 0;
};

} // namespace tetris
