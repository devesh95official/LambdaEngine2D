#pragma once
// assets_demo: what the window shows, as plain data, so main.cpp never sees raylib. draw_rl.cpp
// draws it in raylib immediate mode inside the platform's frame.
#include <span>
#include <string>
#include <lam/math/color.h>

// One colour of a palette, with its name.
struct DemoSwatch {
    std::string name;
    lam::Color color;
};

// One row of swatches under a caption ("day v2").
struct DemoRow {
    std::string caption;
    std::span<const DemoSwatch> swatches;
};

// Everything one frame draws. Spans point into the demo's state and are read during the call.
struct DemoView {
    lam::Color background = lam::kBlack;
    std::span<const DemoRow> rows;
    std::span<const std::string> lines;                         // status and event log, top to bottom
};

// Clears to view.background, then draws the rows and the text. Main thread, inside the frame.
void demo_draw(const DemoView &view);
