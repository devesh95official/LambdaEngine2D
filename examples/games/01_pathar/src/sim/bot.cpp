// SolverBot (see bot.h).
#include "sim/bot.h"

#include <lam/ecs/world.h>
#include "sim/board.h"

namespace pathar {

void SolverBot::fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) {
    lam::World &w = ctx.world();  // the focused world (a menu's world has no Board: no-op)
    const Board *b = w.resources().try_get<Board>();
    const PuzzleState *ps = w.resources().try_get<PuzzleState>();
    if (b == nullptr || ps == nullptr || b->index < 0 || ps->solved) return;
    if (ps->queued != 0 || ps->path_pos < ps->path_len || busy(w)) return;  // a step is under way
    const State now = read_state(w);
    // Replan on a new level, or when the position is not the expected one (the player
    // interfered, an undo, a restart); a failed search is not retried until something changes.
    const bool stale = serial_ != b->serial || !(now == expect_);
    if (stale || (next_ >= plan_.size() && !failed_)) {
        const Solution sol = solve(b->level, now, 1000000);
        plan_ = sol.found ? sol.moves : std::vector<Dir>{};
        next_ = 0;
        serial_ = b->serial;
        failed_ = !sol.found;
        expect_ = now;
        ++plans_;
    }
    if (next_ >= plan_.size()) return;
    const Dir d = plan_[next_++];
    apply_move(b->level, expect_, d);  // where the step will leave us
    frames[0].press(controls_.dir(d));  // pressed + held + repeated, as a key going down
    ++presses_;
}

}  // namespace pathar
