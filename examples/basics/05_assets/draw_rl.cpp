// assets_demo drawing. Written like an engine backend: raylib types stay global, no
// `using namespace lam`, colours convert through <lam/rl/convert.h>.
#include "draw_rl.h"

#include <raylib.h>

#include <lam/rl/convert.h>

namespace {

// Black or white, whichever reads better on `c`.
::Color ink_for(lam::Color c) {
    const int luma = (c.r * 299 + c.g * 587 + c.b * 114) / 1000;
    return luma > 140 ? ::Color{20, 20, 20, 255} : ::Color{240, 240, 240, 255};
}

} // namespace

void demo_draw(const DemoView &view) {
    ClearBackground(lam::rl::to_rl(view.background));
    const ::Color ink = ink_for(view.background);
    constexpr int kMargin = 24;
    constexpr int kSwatch = 72;
    constexpr int kGap = 12;
    int y = kMargin;
    for (const DemoRow &row : view.rows) {
        DrawText(row.caption.c_str(), kMargin, y, 20, ink);
        y += 28;
        int x = kMargin;
        for (const DemoSwatch &s : row.swatches) {
            DrawRectangle(x, y, kSwatch, kSwatch, lam::rl::to_rl(s.color));
            DrawRectangleLines(x, y, kSwatch, kSwatch, ink);
            DrawText(s.name.c_str(), x, y + kSwatch + 4, 16, ink);
            x += kSwatch + kGap;
        }
        y += kSwatch + 32;
    }
    y += 8;
    for (const std::string &line : view.lines) {
        DrawText(line.c_str(), kMargin, y, 18, ink);
        y += 24;
    }
}
