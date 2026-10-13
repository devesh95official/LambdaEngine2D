// Grid-step movement (puzzles, roguelikes): the cell occupancy index, try_step with tile and
// entity blocking, grid teleports and the tick-time step tween that keeps TurnScheduler busy.
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <lam/core/assert.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/turns.h>
#include <lam/ecs/world.h>
#include <lam/tilemap/grid_mover.h>

#include "detail/tilemap_state.h"

namespace lam {

namespace {

int clamp_unit(int v) {
    return (v > 0) - (v < 0);
}

bool tile_free(const TileWorld &tw, IVec2 c) {
    return tw.shapes.inside(c) && tw.shapes(c.x, c.y) == TileShape::Empty &&
           tw.cost.get_or(c, 0) != 0;
}

} // namespace

// ---- GridIndex ---------------------------------------------------------------------------------

void GridIndex::rebuild(World &w, const TileWorld &tiles) {
    width_ = tiles.shapes.width();
    height_ = tiles.shapes.height();
    const std::size_t n = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    cells_.assign(n, Cell{});
    std::size_t entities = 0;
    for (auto [e, gp] : w.view<GridPosition>()) {
        (void)e;
        if (gp.cell.x < 0 || gp.cell.y < 0 || gp.cell.x >= width_ || gp.cell.y >= height_) continue;
        ++cells_[static_cast<std::size_t>(gp.cell.y) * static_cast<std::size_t>(width_) +
                 static_cast<std::size_t>(gp.cell.x)]
              .count;
        ++entities;
    }
    // One slot of slack per occupied cell, so a step onto a cell with one entity rarely relocates.
    std::uint32_t total = 0;
    for (Cell &c : cells_) {
        c.begin = total;
        c.capacity = c.count == 0 ? 0 : c.count + 1;
        total += c.capacity;
        c.count = 0;
    }
    // Room for relocated runs between rebuilds; the capacity is kept across rebuilds.
    const std::size_t want = total + 4 * entities + 64;
    if (entries_.capacity() < want) entries_.reserve(want);
    entries_.assign(total, Entity{});
    for (auto [e, gp] : w.view<GridPosition>()) {
        if (gp.cell.x < 0 || gp.cell.y < 0 || gp.cell.x >= width_ || gp.cell.y >= height_) continue;
        Cell &c = cells_[static_cast<std::size_t>(gp.cell.y) * static_cast<std::size_t>(width_) +
                         static_cast<std::size_t>(gp.cell.x)];
        // Insertion keeps the run sorted by entity index (runs are short).
        Entity *run = entries_.data() + c.begin;
        std::uint32_t k = c.count;
        while (k > 0 && run[k - 1].index > e.index) {
            run[k] = run[k - 1];
            --k;
        }
        run[k] = e;
        ++c.count;
    }
}

std::span<const Entity> GridIndex::at(IVec2 cell) const {
    if (cell.x < 0 || cell.y < 0 || cell.x >= width_ || cell.y >= height_) return {};
    const Cell &c = cells_[static_cast<std::size_t>(cell.y) * static_cast<std::size_t>(width_) +
                           static_cast<std::size_t>(cell.x)];
    return {entries_.data() + c.begin, c.count};
}

void GridIndex::erase(Entity e, IVec2 cell) {
    if (cell.x < 0 || cell.y < 0 || cell.x >= width_ || cell.y >= height_) return;
    Cell &c = cells_[static_cast<std::size_t>(cell.y) * static_cast<std::size_t>(width_) +
                     static_cast<std::size_t>(cell.x)];
    Entity *run = entries_.data() + c.begin;
    for (std::uint32_t i = 0; i < c.count; ++i) {
        if (run[i] != e) continue;
        for (std::uint32_t j = i + 1; j < c.count; ++j)
            run[j - 1] = run[j];
        --c.count;
        return;
    }
}

void GridIndex::insert(Entity e, IVec2 cell) {
    if (cell.x < 0 || cell.y < 0 || cell.x >= width_ || cell.y >= height_) return;
    Cell &c = cells_[static_cast<std::size_t>(cell.y) * static_cast<std::size_t>(width_) +
                     static_cast<std::size_t>(cell.x)];
    if (c.count == c.capacity) {
        const std::uint32_t capacity = std::max<std::uint32_t>(4u, c.capacity * 2u);
        const auto begin = static_cast<std::uint32_t>(entries_.size());
        entries_.resize(entries_.size() + capacity);
        std::copy(entries_.begin() + c.begin, entries_.begin() + c.begin + c.count,
                  entries_.begin() + begin);
        c.begin = begin;
        c.capacity = capacity;
    }
    Entity *run = entries_.data() + c.begin;
    std::uint32_t k = c.count;
    while (k > 0 && run[k - 1].index > e.index) {
        run[k] = run[k - 1];
        --k;
    }
    run[k] = e;
    ++c.count;
}

void GridIndex::update(Entity e, IVec2 from, IVec2 to) {
    erase(e, from);
    insert(e, to);
}

// ---- stepping ----------------------------------------------------------------------------------

StepResult can_step(const World &w, Entity e, IVec2 dir, const StepRules &rules, Entity *blocker) {
    const GridPosition *gp = w.try_get<GridPosition>(e);
    LAM_ASSERT(gp != nullptr, "try_step/can_step: the entity has no GridPosition");
    if (gp == nullptr) return StepResult::Busy;
    dir = {clamp_unit(dir.x), clamp_unit(dir.y)};
    const bool diagonal = dir.x != 0 && dir.y != 0;
    if ((dir.x == 0 && dir.y == 0) || (diagonal && !rules.diagonal)) return StepResult::Busy;
    const GridMover *mv = w.try_get<GridMover>(e);
    if (mv != nullptr && mv->moving) return StepResult::Busy;
    const TileWorld *tw = w.resources().try_get<TileWorld>();
    LAM_ASSERT(tw != nullptr, "try_step/can_step: the world has no TileWorld");
    if (tw == nullptr) return StepResult::BlockedTile;
    const IVec2 from = gp->cell;
    const IVec2 to = from + dir;
    if (!tile_free(*tw, to)) return StepResult::BlockedTile;
    if (diagonal && (!tile_free(*tw, {to.x, from.y}) || !tile_free(*tw, {from.x, to.y}))) {
        return StepResult::BlockedTile;
    }
    if (const GridIndex *index = w.resources().try_get<GridIndex>()) {
        for (const Entity o : index->at(to)) {
            if (o == e || !w.alive(o)) continue;
            const GridPosition *og = w.try_get<GridPosition>(o);
            if (og == nullptr || !og->blocks || (og->layer & rules.blocking) == 0 || og->cell != to)
                continue;
            if (blocker != nullptr) *blocker = o;
            return StepResult::BlockedEntity;
        }
    }
    return StepResult::Moved;
}

StepResult try_step(World &w, Entity e, IVec2 dir, const StepRules &rules, Entity *blocker) {
    const StepResult r = can_step(w, e, dir, rules, blocker);
    if (r != StepResult::Moved) return r;
    GridPosition *gp = w.try_get<GridPosition>(e);
    GridMover *mv = w.try_get<GridMover>(e);
    const TileWorld *tw = w.resources().try_get<TileWorld>();
    GridIndex *index = w.resources().try_get<GridIndex>();
    dir = {clamp_unit(dir.x), clamp_unit(dir.y)};
    const IVec2 from = gp->cell;
    const IVec2 to = from + dir;
    gp->cell = to;
    if (index != nullptr) index->update(e, from, to);
    if (mv != nullptr) {
        mv->from = from;
        mv->to = to;
        mv->elapsed = 0;
        mv->moving = true;
    } else if (Transform *t = w.try_get<Transform>(e)) {
        t->position = tw->tile_center(to);
    }
    return StepResult::Moved;
}

void set_grid_position(World &w, Entity e, IVec2 cell, bool snap) {
    GridPosition *gp = w.try_get<GridPosition>(e);
    if (gp == nullptr) return;
    const IVec2 from = gp->cell;
    gp->cell = cell;
    if (GridIndex *index = w.resources().try_get<GridIndex>()) index->update(e, from, cell);
    const TileWorld *tw = w.resources().try_get<TileWorld>();
    GridMover *mv = w.try_get<GridMover>(e);
    if (snap) {
        if (mv != nullptr) {
            mv->from = cell;
            mv->to = cell;
            mv->elapsed = 0;
            mv->moving = false;
        }
        if (tw != nullptr && w.has<Transform>(e)) w.teleport(e, tw->tile_center(cell));
        return;
    }
    if (mv != nullptr) {
        mv->from = from;
        mv->to = cell;
        mv->elapsed = 0;
        mv->moving = true;
    } else if (Transform *t = w.try_get<Transform>(e); t != nullptr && tw != nullptr) {
        t->position = tw->tile_center(cell);
    }
}

void install_grid_resources(World &w) {
    Resources &r = w.resources();
    if (!r.has<TileWorld>()) r.insert<TileWorld>();
    if (!r.has<GridIndex>()) r.insert<GridIndex>();
}

void grid_index_rebuild(World &w) {
    GridIndex *index = w.resources().try_get<GridIndex>();
    const TileWorld *tw = w.resources().try_get<TileWorld>();
    if (index != nullptr && tw != nullptr) index->rebuild(w, *tw);
}

void grid_movers(World &w) {
    const TileWorld *tw = w.resources().try_get<TileWorld>();
    if (tw == nullptr) return;
    bool any = false;
    for (auto [e, mv, t] : w.view<GridMover, Transform>()) {
        (void)e;
        if (!mv.moving) continue;
        if (mv.elapsed < mv.ticks) ++mv.elapsed;
        const Vec2 a = tw->tile_center(mv.from);
        const Vec2 b = tw->tile_center(mv.to);
        if (mv.elapsed >= mv.ticks) {
            mv.moving = false;
            t.position = b;
            continue;
        }
        const float k = static_cast<float>(mv.elapsed) / static_cast<float>(mv.ticks);
        t.position = lerp(a, b, ease(mv.ease, k));
        any = true;
    }
    TurnScheduler *turns = w.resources().try_get<TurnScheduler>();
    detail::TilemapWorldState *st = w.resources().try_get<detail::TilemapWorldState>();
    if (turns == nullptr) return;
    if (st == nullptr) { // no TilemapPlugin state: the movers own the flag
        turns->busy = any;
        return;
    }
    if (any) {
        turns->busy = true;
        st->movers_set_busy = true;
    } else if (st != nullptr && st->movers_set_busy) {
        turns->busy = false; // only a busy flag this system raised is lowered
        st->movers_set_busy = false;
    }
}

} // namespace lam
