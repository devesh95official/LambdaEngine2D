// Small helpers shared by the rules that the engine does not provide (facing_for, clamp_input,
// standing_tile), plus the control bindings.
#include <cmath>
#include <lam/ecs/world.h>
#include <lam/physics/tile_world.h>

#include "sim/controls.h"
#include "sim/rules.h"

namespace lantern {

const Tuning &tuning(const lam::Ctx &ctx) { return ctx.res<Tuning>(); }

lam::Rng &rules_rng(const lam::Ctx &ctx) { return ctx.rng(lam::str_id("lantern.rules")); }

lam::Rect foot_box(lam::Vec2 size) { return lam::Rect{-size.x * 0.5f, -size.y, size.x, size.y}; }

lam::Rect body_at(lam::Vec2 feet, lam::Vec2 size) { return foot_box(size).translated(feet); }

lam::Vec2 facing_vector(Facing f) {
    switch (f) {
    case Facing::Down: return {0.0f, 1.0f};
    case Facing::Up: return {0.0f, -1.0f};
    case Facing::Left: return {-1.0f, 0.0f};
    case Facing::Right: return {1.0f, 0.0f};
    }
    return {0.0f, 1.0f};
}

Facing facing_for(lam::Vec2 dir, Facing current) {
    if (dir.x == 0.0f && dir.y == 0.0f) return current;
    const Facing horizontal = dir.x < 0.0f ? Facing::Left : Facing::Right;
    const Facing vertical = dir.y < 0.0f ? Facing::Up : Facing::Down;
    const float ax = std::fabs(dir.x);
    const float ay = std::fabs(dir.y);
    if (ax > ay) return horizontal;
    if (ay > ax) return vertical;
    return current == vertical ? vertical : horizontal;         // a keyboard diagonal never flickers
}

lam::Vec2 clamp_input(lam::Vec2 v) {
    if (!lam::is_finite(v)) return {};
    return lam::clamp_length(v, 1.0f);
}

lam::IVec2 standing_tile(const lam::TileWorld &tiles, lam::Vec2 feet, lam::Vec2 box) {
    return tiles.tile_at(body_at(feet, box).center());          // the tile holding the box centre
}

Controls bind_controls(lam::ActionMap &map) {
    using lam::ActionMap;
    using lam::Key;
    Controls c;
    c.move = map.axis2("move",
                       {{ActionMap::key(Key::W), ActionMap::key(Key::S), ActionMap::key(Key::A), ActionMap::key(Key::D)},
                        {ActionMap::key(Key::Up), ActionMap::key(Key::Down), ActionMap::key(Key::Left),
                         ActionMap::key(Key::Right)}},
                       {{lam::PadAxis::LeftX, lam::PadAxis::LeftY}});
    c.dash = map.button("dash", {ActionMap::key(Key::Space), ActionMap::key(Key::LShift),
                                 ActionMap::pad(lam::PadButton::South)});
    c.confirm = map.button("confirm", {ActionMap::key(Key::Enter), ActionMap::key(Key::Space),
                                       ActionMap::pad(lam::PadButton::South)});
    c.pause = map.button("pause", {ActionMap::key(Key::P), ActionMap::key(Key::Escape),
                                   ActionMap::pad(lam::PadButton::Start)});
    return c;
}

} // namespace lantern
