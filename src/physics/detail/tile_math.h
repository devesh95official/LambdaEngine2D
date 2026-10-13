#pragma once
// Private tile helpers. Everything goes through tile_span so sweeps and overlap tests can't
// disagree by a rounding step. Indices are clamped to two tiles past the grid, which also tames NaN.
#include <cmath>
#include <cstdint>
#include <optional>
#include <lam/math/rect.h>
#include <lam/math/vec2.h>
#include <lam/physics/tile_world.h>
namespace lam::detail {

// A run of tile indices along one axis, both ends included. Empty if last < first.
struct TileSpan {
    int first = 0;
    int last = -1;
};

// A tile index held to [-2, count + 1]. NaN and -inf land at -2, +inf at count + 1.
inline int limit_tile(float tile, int count) {
    if (!(tile > -2.0f)) return -2;
    if (tile > static_cast<float>(count + 1)) return count + 1;
    return static_cast<int>(tile);
}

// The tiles the half-open interval [low, high) overlaps on an axis whose tile 0 starts at
// `origin`. Touching a tile's edge is not overlapping it.
inline TileSpan tile_span(float low, float high, float origin, float size, int count) {
    return TileSpan{limit_tile(std::floor((low - origin) / size), count),
                    limit_tile(std::ceil((high - origin) / size) - 1.0f, count)};
}

// The tile index holding coordinate v (clamped like limit_tile).
inline int tile_index(float v, float origin, float size, int count) {
    return limit_tile(std::floor((v - origin) / size), count);
}

// Coordinate of the boundary that starts tile `line`.
inline float tile_edge(int line, float origin, float size) {
    return origin + static_cast<float>(line) * size;
}

// An empty (0 x 0) TileWorld collides with nothing.
inline bool has_tiles(const TileWorld &w) { return w.has_cells(); }
constexpr std::uint8_t kAllSides = kSideLeft | kSideRight | kSideTop | kSideBottom;
// Sides past which cells are Empty.
inline std::uint8_t open_sides(const TileWorld &w) {
    if (!w.outside_solid || !has_tiles(w)) return kAllSides;
    return static_cast<std::uint8_t>(w.open_sides & kAllSides);
}
inline std::uint8_t closed_sides(const TileWorld &w) {
    return static_cast<std::uint8_t>(kAllSides & ~open_sides(w));
}
inline TileShape shape_at(const TileWorld &w, IVec2 c) { return w.at(c); }
inline std::uint8_t flags_at(const TileWorld &w, IVec2 c) { return w.flags_at(c); }

inline bool is_slope(TileShape s) { return s >= TileShape::SlopeUpRight; }

// Top-down semantics: everything but Empty and OneWay is a full block.
inline bool blocks_full(TileShape s) { return s != TileShape::Empty && s != TileShape::OneWay; }

// y of a slope's surface at x (clamped to the tile's columns). `tile` is the tile's rect.
inline float slope_surface(TileShape s, const Rect &tile, float x) {
    const float size = tile.w;
    const float left = tile.x;
    const float right = tile.x + size;
    const float bottom = tile.y + size;
    const float from_left = clamp(x - left, 0.0f, size);
    const float from_right = clamp(right - x, 0.0f, size);
    switch (s) {
    case TileShape::SlopeUpRight: return bottom - from_left;
    case TileShape::SlopeUpLeft: return bottom - from_right;
    case TileShape::HalfSlopeUpRightLow: return bottom - from_left * 0.5f;
    case TileShape::HalfSlopeUpRightHigh: return bottom - size * 0.5f - from_left * 0.5f;
    case TileShape::HalfSlopeUpLeftLow: return bottom - from_right * 0.5f;
    case TileShape::HalfSlopeUpLeftHigh: return bottom - size * 0.5f - from_right * 0.5f;
    case TileShape::Solid:
    case TileShape::OneWay: return tile.y;
    case TileShape::Empty: return bottom;
    }
    return bottom;
}

// Outward unit normal of a slope's surface; (0, -1) for flat shapes.
inline Vec2 slope_normal(TileShape s) {
    constexpr float k45 = 0.70710678f;
    constexpr float kHalfX = 0.44721360f; // (1, 2) / sqrt(5)
    constexpr float kHalfY = 0.89442719f;
    switch (s) {
    case TileShape::SlopeUpRight: return {-k45, -k45};
    case TileShape::SlopeUpLeft: return {k45, -k45};
    case TileShape::HalfSlopeUpRightLow:
    case TileShape::HalfSlopeUpRightHigh: return {-kHalfX, -kHalfY};
    case TileShape::HalfSlopeUpLeftLow:
    case TileShape::HalfSlopeUpLeftHigh: return {kHalfX, -kHalfY};
    default: return {0.0f, -1.0f};
    }
}

// The solid profile of a tile's left (or right) edge: the surface height there. A tile whose
// edge surface equals its bottom has no face on that side.
inline float slope_edge_surface(TileShape s, const Rect &tile, bool left_edge) {
    return slope_surface(s, tile, left_edge ? tile.x : tile.x + tile.w);
}

// Up to four vertices of a tile's solid part (Solid: the rect; slopes: triangle or quad).
// Returns the count (0 for Empty/OneWay).
inline int tile_polygon(TileShape s, const Rect &t, Vec2 *out) {
    const float l = t.x, r = t.x + t.w, top = t.y, b = t.y + t.h, mid = t.y + t.h * 0.5f;
    switch (s) {
    case TileShape::Solid:
        out[0] = {l, top}; out[1] = {r, top}; out[2] = {r, b}; out[3] = {l, b};
        return 4;
    case TileShape::SlopeUpRight:
        out[0] = {r, top}; out[1] = {r, b}; out[2] = {l, b};
        return 3;
    case TileShape::SlopeUpLeft:
        out[0] = {l, top}; out[1] = {r, b}; out[2] = {l, b};
        return 3;
    case TileShape::HalfSlopeUpRightLow:
        out[0] = {r, mid}; out[1] = {r, b}; out[2] = {l, b};
        return 3;
    case TileShape::HalfSlopeUpRightHigh:
        out[0] = {l, mid}; out[1] = {r, top}; out[2] = {r, b}; out[3] = {l, b};
        return 4;
    case TileShape::HalfSlopeUpLeftLow:
        out[0] = {l, mid}; out[1] = {r, b}; out[2] = {l, b};
        return 3;
    case TileShape::HalfSlopeUpLeftHigh:
        out[0] = {l, top}; out[1] = {r, mid}; out[2] = {r, b}; out[3] = {l, b};
        return 4;
    default: return 0;
    }
}

// A swept-circle (or ray, radius 0) hit against the tile grid.
struct TileSweepHit {
    float t = 0;                                                // [0, 1] along from -> to
    Vec2 point;                                                 // centre of the circle at contact
    Vec2 normal;                                                // surface normal; zero if starting inside
    IVec2 cell;
};
// First blocking tile hit by a moving circle; no tunnelling at any speed. skip_start_inside
// ignores tiles already overlapped at `from`, for projectiles resting against a wall.
std::optional<TileSweepHit> sweep_circle_tiles(const TileWorld &w, Vec2 from, Vec2 to, float radius,
                                               bool skip_start_inside = false);

}
