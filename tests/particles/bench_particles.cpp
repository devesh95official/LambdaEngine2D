// lam_particles_bench: update and draw cost for a full budget (65536 particles in 16 pools), no
// window. Build with -DLAM_BUILD_BENCH=ON and run in Release. Prints p50/p99 ms and allocations.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <doctest/doctest.h>

#include <lam/core/alloc.h>
#include <lam/math/rng.h>
#include <lam/particles/particles.h>
#include <lam/render/render_queue.h>

using namespace lam;

namespace {

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    const auto i = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
    return v[i];
}

} // namespace

TEST_CASE("bench: 65536 particles, update and draw") {
    const std::uint32_t total = 65536;
    const int frames = 600;
    ParticlePools pools(total, 64);
    Rng rng(1);
    RenderQueue queue(total + 1024);
    // 16 pools of equal size, spread over a 2000 px wide area; the view sees about half of it.
    const std::uint32_t per_pool = std::min<std::uint32_t>(total / 16, kMaxParticlesPerPool);
    std::vector<ParticlePoolId> ids;
    for (int p = 0; p < 16; ++p) {
        EmitterConfig c;
        c.capacity = per_pool;
        c.life_min = 1e3f;
        c.life_max = 1e3f;
        c.speed_min = 5;
        c.speed_max = 20; // with drag they drift at most 40 px, so the view keeps seeing half
        c.drag = 0.5f;
        c.jitter = p % 2 == 0 ? 8.0f : 0.0f;
        c.rotation_max = p % 4 == 0 ? 3.0f : 0.0f;
        c.spawn_area = {-60, -60, 120, 120};
        c.size_start = 3;
        c.size_end = 1;
        ids.push_back(pools.create(c));
        for (std::uint32_t done = 0; done < per_pool; done += 65535)
            pools.emit(ids.back(), {static_cast<float>(p) * 125.0f, 500.0f},
                       static_cast<std::uint16_t>(std::min<std::uint32_t>(65535, per_pool - done)), rng);
    }
    const Rect view{0, 0, 1000, 1000};
    std::vector<double> update_ms, draw_ms;
    update_ms.reserve(static_cast<std::size_t>(frames));
    draw_ms.reserve(static_cast<std::size_t>(frames));
    for (int i = 0; i < 60; ++i) { // warm-up
        pools.update(1.0f / 60.0f, rng);
        pools.draw(queue, view);
        queue.clear();
    }
    const AllocationScope allocs;
    for (int i = 0; i < frames; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        pools.update(1.0f / 60.0f, rng);
        const auto t1 = std::chrono::steady_clock::now();
        pools.draw(queue, view);
        const auto t2 = std::chrono::steady_clock::now();
        queue.clear();
        update_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        draw_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
    }
    const std::uint64_t allocations = allocs.count(); // before the report allocates
    const ParticlePoolStats st = pools.stats();
    std::printf("lam_particles_bench: %u particles in %u pools, %d frames\n", st.alive, st.pools, frames);
    std::printf("  update  p50 %.3f ms  p99 %.3f ms\n", percentile(update_ms, 0.5), percentile(update_ms, 0.99));
    std::printf("  draw    p50 %.3f ms  p99 %.3f ms  (%u drawn, %u culled)\n", percentile(draw_ms, 0.5),
                percentile(draw_ms, 0.99), st.drawn, st.culled);
    if (allocation_counting_enabled()) {
        std::printf("  allocations per frame: %.3f\n",
                    static_cast<double>(allocations) / static_cast<double>(frames));
        CHECK(allocations == 0);
    } else {
        std::printf("  allocations per frame: not counted (no lam_alloc_hook)\n");
    }
    CHECK(st.alive == total);
}
