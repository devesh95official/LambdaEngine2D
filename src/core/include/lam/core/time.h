#pragma once
// Time as seen by a system. Ctx::time() is per world: `tick` is that world's tick and `alpha` is
// 1 for worlds that do not tick this frame. App::time() has tick == app_tick.
#include <cstdint>
namespace lam {
struct Time {
    std::uint64_t tick = 0;                                     // ticks run by this world
    std::uint64_t app_tick = 0;                                 // fixed steps run by the App (replay key)
    std::uint64_t frame = 0;                                    // frames since start
    float tick_dt = 1.0f / 60;                                  // seconds per tick (constant)
    float frame_dt = 0;                                         // this frame's real seconds (tick_dt under --step)
    float alpha = 1;                                            // [0, 1]
    double real_seconds = 0;
    int tick_rate = 60;
    int ticks_this_frame = 0;
    bool paused = false;
};
}
