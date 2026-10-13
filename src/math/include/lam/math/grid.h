#pragma once
// Row-major 2D grid for collision, costs, FOV and tiles. (0, 0) is the top-left; negative sizes
// clamp to 0.
#include <cstddef>
#include <span>
#include <vector>
#include <lam/math/detail/math_assert.h>
#include <lam/math/vec2.h>
namespace lam {
template <class T> class Grid {
public:
    Grid() = default;
    Grid(int width, int height, T fill = T{}) { resize(width, height, fill); }
    // Allocates; load time only.
    void resize(int width, int height, T fill = T{}) {
        width_ = width > 0 ? width : 0;
        height_ = height > 0 ? height : 0;
        cells_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), fill);
    }
    int width() const { return width_; }
    int height() const { return height_; }
    std::size_t size() const { return cells_.size(); }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < width_ && y < height_; }
    bool inside(IVec2 p) const { return inside(p.x, p.y); }
    // LAM_ASSERT(inside).
    std::size_t index(int x, int y) const {
        LAM_MATH_ASSERT(inside(x, y), "Grid::index outside the grid");
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x);
    }
    IVec2 coord(std::size_t i) const {
        if (width_ == 0) return {};
        return {static_cast<int>(i % static_cast<std::size_t>(width_)),
                static_cast<int>(i / static_cast<std::size_t>(width_))};
    }
    T &operator()(int x, int y) { return cells_[index(x, y)]; }
    const T &operator()(int x, int y) const { return cells_[index(x, y)]; }
    // The cell, or `outside` beyond the edges.
    T get_or(int x, int y, T outside) const { return inside(x, y) ? cells_[index(x, y)] : outside; }
    T get_or(IVec2 p, T outside) const { return get_or(p.x, p.y, outside); }
    // Ignored outside.
    void set(int x, int y, T v) {
        if (inside(x, y)) cells_[index(x, y)] = v;
    }
    void fill(T v) {
        for (T &c : cells_) c = v;
    }
    std::span<T> cells() { return cells_; }
    std::span<const T> cells() const { return cells_; }
    bool operator==(const Grid &) const = default;

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<T> cells_;
};
}
