#pragma once
// Run-length grid encoding for saves and hashing: {"w": W, "h": H, "runs": [value, count, ...]},
// row major. Reading checks that the runs add up to W * H and every value fits T.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include <lam/core/data.h>
#include <lam/core/hash.h>
#include <lam/math/grid.h>

namespace lam::detail {

// Largest grid a save may hold (cells); larger sizes are rejected before allocating.
inline constexpr std::int64_t kMaxSavedGridCells = 1 << 24;

template <class T>
void write_grid(const Grid<T> &g, DataWriter &out) {
    out.begin_object();
    out.key("w").value(g.width());
    out.key("h").value(g.height());
    out.key("runs").begin_array();
    const auto cells = g.cells();
    std::size_t i = 0;
    while (i < cells.size()) {
        std::size_t j = i + 1;
        while (j < cells.size() && cells[j] == cells[i])
            ++j;
        out.value(static_cast<std::uint64_t>(cells[i]));
        out.value(static_cast<std::uint64_t>(j - i));
        i = j;
    }
    out.end_array();
    out.end_object();
}

// Reads what write_grid wrote into `g` (resized). Errors are recorded in the reader's document.
template <class T>
void read_grid(DataReader in, Grid<T> &g) {
    const int w = in.integer("w", 0, 1 << 16);
    const int h = in.integer("h", 0, 1 << 16);
    DataArray runs = in.array("runs");
    if (in.failed()) return;
    if (static_cast<std::int64_t>(w) * h > kMaxSavedGridCells) {
        in.fail("grid too large");
        return;
    }
    g.resize(w, h);
    if (runs.size() % 2 != 0) {
        runs.fail("runs must hold value, count pairs");
        return;
    }
    const auto cells = g.cells();
    std::size_t at = 0;
    const auto max_value = static_cast<std::int64_t>(std::numeric_limits<T>::max());
    for (std::size_t i = 0; i + 1 < runs.size(); i += 2) {
        const std::int64_t v = runs.integer64(i, 0, max_value);
        const std::int64_t n = runs.integer64(i + 1, 1, kMaxSavedGridCells);
        if (at + static_cast<std::size_t>(n) > cells.size()) {
            runs.fail("runs cover more than w * h cells");
            return;
        }
        for (std::int64_t k = 0; k < n; ++k)
            cells[at++] = static_cast<T>(v);
    }
    if (at != cells.size())
        runs.fail("runs cover " + std::to_string(at) + " of " + std::to_string(cells.size()) +
                  " cells");
}

template <class T>
void hash_grid(const Grid<T> &g, Hasher &h) {
    h.u64(static_cast<std::uint64_t>(g.width()));
    h.u64(static_cast<std::uint64_t>(g.height()));
    const auto cells = g.cells();
    if (!cells.empty()) h.bytes(cells.data(), cells.size() * sizeof(T));
}

} // namespace lam::detail
