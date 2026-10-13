#pragma once
// Four-way facing for top-down games (+y down). Enumerator order matches the row order of
// four-direction sprite sheets.
//   hero.facing = dir4_of(input.move, hero.facing);
//   IVec2 ahead = cell + dir4_offset(hero.facing);
#include <cstdint>
#include <lam/math/vec2.h>
namespace lam {
enum class Dir4 : std::uint8_t { Down, Left, Right, Up };
// The dominant axis of v. Ties, tiny and non-finite vectors keep `current`, so facing doesn't
// flicker on diagonals or when the stick is released.
constexpr Dir4 dir4_of(Vec2 v, Dir4 current) {
    const float ax = v.x < 0 ? -v.x : v.x;
    const float ay = v.y < 0 ? -v.y : v.y;
    if (!(length_sq(v) >= 1e-6f) || !(ax != ay)) return current;   // short, NaN or a tie
    if (ax > ay) return v.x < 0 ? Dir4::Left : Dir4::Right;
    return v.y < 0 ? Dir4::Up : Dir4::Down;
}
// Down {0, 1}, Left {-1, 0}, Right {1, 0}, Up {0, -1} (+y is down).
constexpr IVec2 dir4_offset(Dir4 d) {
    switch (d) {
    case Dir4::Down: return {0, 1};
    case Dir4::Left: return {-1, 0};
    case Dir4::Right: return {1, 0};
    case Dir4::Up: break;
    }
    return {0, -1};
}
// dir4_offset as a unit Vec2.
constexpr Vec2 dir4_vector(Dir4 d) { return to_vec2(dir4_offset(d)); }
// The opposite direction (Down <-> Up, Left <-> Right).
constexpr Dir4 dir4_opposite(Dir4 d) {
    switch (d) {
    case Dir4::Down: return Dir4::Up;
    case Dir4::Left: return Dir4::Right;
    case Dir4::Right: return Dir4::Left;
    case Dir4::Up: break;
    }
    return Dir4::Down;
}
}
