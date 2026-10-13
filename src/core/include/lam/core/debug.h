#pragma once
// DebugOverlay (App resource: stats and text lines) and DebugDraw (World resource, drawn in that
// world's view at RenderUI +2000). Fixed capacity; no-ops when LAM_DEV is 0.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <lam/core/fixed_string.h>
#include <lam/math/color.h>
#include <lam/math/rect.h>
namespace lam {
class DebugOverlay {
public:
    struct Stat { std::string_view label; double value = 0; };
    // label: static storage. Replaces the value of an existing label this frame. At most 64.
    void stat(std::string_view label, double value);
    // Copied into a fixed ring of 32 lines.
    void text(std::string_view line);
    std::span<const Stat> stats() const;
    std::span<const FixedString<96>> lines() const;
    void clear_frame();
    bool visible = false;                                       // F3 (LAM_DEV)
    bool show_colliders = false;                                // F4 (LAM_DEV)
private:
    std::array<Stat, 64> stats_{};
    std::array<FixedString<96>, 32> lines_{};
    std::uint8_t stat_count_ = 0;
    std::uint8_t line_count_ = 0;
};
enum class DebugPrimKind : std::uint8_t { Line, Rect, Circle, Text };
struct DebugPrim {
    DebugPrimKind kind = DebugPrimKind::Line;
    bool screen = false;                                        // virtual pixels instead of world
    Vec2 a, b;                                                  // line: a-b; rect: a = min, b = size; circle/text: a
    float radius = 0;
    Color color = kWhite;
    FixedString<48> text;
};
class DebugDraw {
public:
    explicit DebugDraw(std::size_t capacity = 4096);
    void line(Vec2 a, Vec2 b, Color c, bool screen = false);
    void rect(Rect r, Color c, bool screen = false);
    void circle(Vec2 center, float radius, Color c, bool screen = false);
    void text(Vec2 p, std::string_view s, Color c, bool screen = false);
    std::span<const DebugPrim> prims() const;
    // Primitives dropped over capacity this frame.
    std::size_t dropped() const;
    void clear();
private:
    void push(const DebugPrim &p);
    std::vector<DebugPrim> prims_;
    std::size_t capacity_;
    std::size_t dropped_ = 0;
};
}
