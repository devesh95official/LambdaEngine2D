#pragma once
// raylib <-> lam conversions for backend .cpp files only (target lam_rl_common, INTERFACE; include
// as <lam/rl/convert.h>). Backends never write `using namespace lam`.
#include <raylib.h>
#include <lam/math/color.h>
#include <lam/math/rect.h>
#include <lam/math/vec2.h>
namespace lam::rl {
inline ::Color to_rl(lam::Color c) { return ::Color{c.r, c.g, c.b, c.a}; }
inline ::Vector2 to_rl(lam::Vec2 v) { return ::Vector2{v.x, v.y}; }
inline ::Rectangle to_rl(const lam::Rect &r) { return ::Rectangle{r.x, r.y, r.w, r.h}; }
inline lam::Color from_rl(::Color c) { return lam::Color{c.r, c.g, c.b, c.a}; }
inline lam::Vec2 from_rl(::Vector2 v) { return lam::Vec2{v.x, v.y}; }
inline lam::Rect from_rl(const ::Rectangle &r) { return lam::Rect{r.x, r.y, r.width, r.height}; }
}
