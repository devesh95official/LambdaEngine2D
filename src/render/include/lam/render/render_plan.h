#pragma once
// The planned frame. Headless runs stop here, so tests can check exactly what the GPU would draw.
#include <cstdint>
#include <vector>
#include <lam/render/render_queue.h>
#include <lam/render/texture.h>
namespace lam {
struct RenderPlan {                                             // App resource
    std::vector<Batch> batches;                                 // composites have quad_count 0
    std::vector<QuadVertex> staging;                            // 4 vertices per quad
    SortScratch scratch;
    std::uint64_t frame = 0;
};
// Set `request` to grab the virtual screen (before upscaling) after the next submit. The image is
// RGBA8, top row first. Never becomes ready headless.
struct ScreenCapture {                                          // App resource
    bool request = false;
    bool ready = false;
    std::uint64_t frame = 0;
    ImageData image;
};
// raylib backend counters. driver_allocations counts allocations inside GL calls (Apple's GL
// allocates per call) so they can be left out of the engine's own budget.
struct RenderBackendStats {
    std::uint64_t frames = 0;
    std::uint64_t texture_uploads = 0;
    std::uint64_t driver_allocations = 0;
    bool rlgl_batch = false;
};
}
