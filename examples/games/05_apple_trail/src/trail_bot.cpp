#include "trail_bot.h"

#include <lam/physics/tile_world.h>
#include <lam/tilemap/grid_mover.h>

namespace trail {
namespace {
constexpr Dir kDirs[] = {Dir::Up, Dir::Down, Dir::Left, Dir::Right};
}

std::optional<Dir> autoplay_choice(const lam::Grid<std::uint8_t> &blocked, lam::IVec2 head, Dir heading,
                                   std::span<const lam::IVec2> targets, BfsScratch &scratch) {
    const auto open_cell = [&](lam::IVec2 c) { return blocked.get_or(c, 1) == 0; };
    const auto is_target = [&](lam::IVec2 c) {
        for (const lam::IVec2 t : targets)
            if (t == c) return true;
        return false;
    };
    scratch.first.resize(blocked.width(), blocked.height(), -1);   // same size: no reallocation
    scratch.queue.clear();
    scratch.queue.reserve(static_cast<std::size_t>(blocked.width() * blocked.height()));   // once: a cell is queued at most once
    // Breadth-first search, remembering the first step of each path.
    for (int d = 0; d < 4; ++d) {
        const lam::IVec2 n = step(head, kDirs[d]);
        if (!open_cell(n) || opposite(kDirs[d], heading)) continue;
        scratch.first(n.x, n.y) = static_cast<std::int8_t>(d);
        scratch.queue.push_back(n);
    }
    for (std::size_t i = 0; i < scratch.queue.size(); ++i) {
        const lam::IVec2 t = scratch.queue[i];
        const std::int8_t first = scratch.first(t.x, t.y);
        if (is_target(t)) return kDirs[first];
        for (const Dir d : kDirs) {
            const lam::IVec2 n = step(t, d);
            if (!open_cell(n) || scratch.first(n.x, n.y) >= 0) continue;
            scratch.first(n.x, n.y) = first;
            scratch.queue.push_back(n);
        }
    }
    // Nothing reachable: the free neighbour with the most free neighbours of its own.
    std::optional<Dir> best;
    int best_room = -1;
    for (const Dir d : kDirs) {
        const lam::IVec2 n = step(head, d);
        if (!open_cell(n) || opposite(d, heading)) continue;
        int room = 0;
        for (const Dir e : kDirs)
            if (open_cell(step(n, e))) ++room;
        if (room > best_room) {
            best_room = room;
            best = d;
        }
    }
    return best;
}

TrailBot::TrailBot(Controls controls, int confirm_delay_ticks)
    : controls_(controls), confirm_delay_(confirm_delay_ticks) {
    targets_.reserve(4);                                        // a red and a golden apple at most
}

std::optional<Dir> TrailBot::choose(lam::World &w) {
    const TrailState *s = w.resources().try_get<TrailState>();
    const lam::TileWorld *tiles = w.resources().try_get<lam::TileWorld>();
    if (s == nullptr || tiles == nullptr || !s->alive || !w.alive(s->head)) return std::nullopt;

    // The board as the bot sees it: walls, and every trail piece except the tail (it moves away
    // on the next step).
    blocked_.resize(tiles->shapes.width(), tiles->shapes.height(), 0);
    for (int y = 0; y < blocked_.height(); ++y)
        for (int x = 0; x < blocked_.width(); ++x)
            blocked_(x, y) = tiles->at({x, y}) == lam::TileShape::Solid ? 1 : 0;
    targets_.clear();
    for (auto [e, at] : w.view<lam::GridPosition>()) {
        if (w.has<Apple>(e))
            targets_.push_back(at.cell);
        else if (w.has<Segment>(e) && e != s->tail)
            blocked_.set(at.cell.x, at.cell.y, 1);
    }
    return autoplay_choice(blocked_, w.get<lam::GridPosition>(s->head).cell, s->heading, targets_, scratch_);
}

void TrailBot::fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) {
    lam::InputFrame &f = frames[0];
    f.clear_devices();                                          // the bot replaces live presses; UI intents stay

    lam::World &w = ctx.world();
    if (w.resources().has<TrailState>()) {
        if (const std::optional<Dir> d = choose(w)) {
            const lam::ButtonAction a = *d == Dir::Up     ? controls_.up
                                        : *d == Dir::Down ? controls_.down
                                        : *d == Dir::Left ? controls_.left
                                                          : controls_.right;
            f.press(a);                                         // pressed + held + repeated
        }
        return;
    }
    if (w.tick() >= static_cast<std::uint64_t>(confirm_delay_)) f.press(controls_.confirm);
}

} // namespace trail
