// The solver (pure) and SolverBot, the InputDriver behind --bot (see rules.h).
#include <algorithm>
#include <string>
#include <unordered_set>
#include <lam/ecs/world.h>
#include "grid_puzzle/rules.h"

namespace grid_puzzle {

namespace {

// One string per position: the player, then the crates in a fixed order (crates are interchangeable).
std::string key_of(State s) {
    std::sort(s.crates.begin(), s.crates.end(), [](lam::IVec2 a, lam::IVec2 b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    std::string k;
    k += static_cast<char>(s.player.x);
    k += static_cast<char>(s.player.y);
    for (lam::IVec2 c : s.crates) {
        k += static_cast<char>(c.x);
        k += static_cast<char>(c.y);
    }
    return k;
}

// Sets the bits a held-down key would set this tick (pressed, held and repeated).
void press(lam::InputFrame &f, lam::ButtonAction a) {
    if (!a) return;
    const std::uint64_t bit = std::uint64_t{1} << (a.id & 63u);
    for (lam::ActionBits *bits : {&f.pressed, &f.held, &f.repeated}) (*bits)[a.id >> 6u] |= bit;
}

}  // namespace

std::vector<lam::IVec2> solve(const Level &level, const State &from, std::size_t max_states) {
    struct Node { State s; std::size_t parent; lam::IVec2 dir; };
    std::vector<Node> nodes{{from, 0, {}}};                     // the BFS queue, kept for the path back
    std::unordered_set<std::string> seen{key_of(from)};
    for (std::size_t i = 0; i < nodes.size() && nodes.size() < max_states; ++i) {
        if (solved(level, nodes[i].s)) {
            std::vector<lam::IVec2> path;
            for (std::size_t n = i; n != 0; n = nodes[n].parent) path.push_back(nodes[n].dir);
            std::reverse(path.begin(), path.end());
            return path;
        }
        for (lam::IVec2 d : kDirs) {
            State next = nodes[i].s;                            // copy: push_back may move nodes
            if (try_move(level, next, d) != Move::Blocked && seen.insert(key_of(next)).second)
                nodes.push_back({std::move(next), i, d});
        }
    }
    return {};
}

// Called every tick at TickBegin -1400, after live input is latched and before any rule runs.
void SolverBot::fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) {
    lam::World &w = ctx.world();                                // the focused world
    const Run *run = w.resources().try_get<Run>();
    const Levels *levels = ctx.try_res<Levels>();
    if (run == nullptr || levels == nullptr || busy(w)) return; // no puzzle here, or a glide is running
    if (run->solved) {
        press(frames[0], controls_.next);
        return;
    }
    const Level &level = levels->list[static_cast<std::size_t>(run->level)];
    const State now = read_state(w);
    // Replan on a new level or whenever the position is not the one the last press should have
    // made (the player took over, an undo, a restart). Otherwise keep following the plan.
    if (run->level != level_ || !(now == expect_)) {
        plan_ = solve(level, now);
        next_ = 0;
        level_ = run->level;
        expect_ = now;
    }
    if (next_ >= plan_.size()) return;                          // no solution from here: wait for undo
    const lam::IVec2 dir = plan_[next_++];
    try_move(level, expect_, dir);
    const auto i = static_cast<std::size_t>(std::find(kDirs.begin(), kDirs.end(), dir) - kDirs.begin());
    press(frames[0], controls_.dirs()[i]);
}

}  // namespace grid_puzzle
