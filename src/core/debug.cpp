#include <lam/core/debug.h>

namespace lam {

void DebugOverlay::stat(std::string_view label, double value) {
    if constexpr (!LAM_DEV) {
        (void)label;
        (void)value;
        return;
    } else {
        for (std::uint8_t i = 0; i < stat_count_; ++i) {
            if (stats_[i].label == label) {
                stats_[i].value = value;
                return;
            }
        }
        if (stat_count_ < stats_.size()) stats_[stat_count_++] = {label, value};
    }
}

void DebugOverlay::text(std::string_view line) {
    if constexpr (!LAM_DEV) {
        (void)line;
    } else {
        if (line_count_ == lines_.size()) {
            for (std::size_t i = 1; i < lines_.size(); ++i) lines_[i - 1] = lines_[i];
            --line_count_;
        }
        lines_[line_count_++] = line;
    }
}

std::span<const DebugOverlay::Stat> DebugOverlay::stats() const { return {stats_.data(), stat_count_}; }

std::span<const FixedString<96>> DebugOverlay::lines() const { return {lines_.data(), line_count_}; }

void DebugOverlay::clear_frame() {
    stat_count_ = 0;
    line_count_ = 0;
}

DebugDraw::DebugDraw(std::size_t capacity) : capacity_(LAM_DEV ? capacity : 0) { prims_.reserve(capacity_); }

void DebugDraw::push(const DebugPrim &p) {
    if constexpr (!LAM_DEV) {
        (void)p;
    } else {
        if (prims_.size() >= capacity_) {
            ++dropped_;
            return;
        }
        prims_.push_back(p);
    }
}

void DebugDraw::line(Vec2 a, Vec2 b, Color c, bool screen) {
    DebugPrim p;
    p.kind = DebugPrimKind::Line;
    p.a = a;
    p.b = b;
    p.color = c;
    p.screen = screen;
    push(p);
}

void DebugDraw::rect(Rect r, Color c, bool screen) {
    DebugPrim p;
    p.kind = DebugPrimKind::Rect;
    p.a = r.min();
    p.b = r.size();
    p.color = c;
    p.screen = screen;
    push(p);
}

void DebugDraw::circle(Vec2 center, float radius, Color c, bool screen) {
    DebugPrim p;
    p.kind = DebugPrimKind::Circle;
    p.a = center;
    p.radius = radius;
    p.color = c;
    p.screen = screen;
    push(p);
}

void DebugDraw::text(Vec2 pos, std::string_view s, Color c, bool screen) {
    DebugPrim p;
    p.kind = DebugPrimKind::Text;
    p.a = pos;
    p.text = s;
    p.color = c;
    p.screen = screen;
    push(p);
}

std::span<const DebugPrim> DebugDraw::prims() const { return prims_; }

std::size_t DebugDraw::dropped() const { return dropped_; }

void DebugDraw::clear() {
    prims_.clear();
    dropped_ = 0;
}

} // namespace lam
