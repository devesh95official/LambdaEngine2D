// lam_render_cpu_bench: collect + plan of the bench scene at 10k and 50k sprites, no window.
// Release builds also check the budget: 50k sprites in <= 4 ms at p50.
#include <cstdio>
#include <vector>

#include <lam/core/alloc.h>
#include <lam/render/render_plugin.h>
#include <lam/test/support.h>

#include "bench_scene.h"

using namespace lam;

TEST_CASE("bench: collect + plan of the bench scene (10k and 50k sprites), headless") {
    for (const int sprites : {10000, 50000}) {
        test::HeadlessApp h;
        h.add<AssetsPlugin>();
        RenderConfig cfg;
        cfg.quad_capacity = static_cast<std::size_t>(sprites) + 4096;
        h.add<RenderPlugin>(cfg);
        bench::setup(h.app(), sprites);
        h.frames(30); // warm-up: pools, queue, plan buffers
        constexpr int kFrames = 300;
        std::vector<float> collect, plan, both;
        collect.reserve(kFrames);
        plan.reserve(kFrames);
        both.reserve(kFrames);
        std::uint64_t allocations = 0;
        for (int i = 0; i < kFrames; ++i) {
            const AllocationScope scope;
            h.frames(1);
            allocations += scope.count();
            const RenderStats &st = h.app().res<RenderStats>();
            collect.push_back(st.cpu_collect_ms);
            plan.push_back(st.cpu_plan_ms);
            both.push_back(st.cpu_collect_ms + st.cpu_plan_ms);
        }
        const RenderStats &st = h.app().res<RenderStats>();
        const float p50 = bench::percentile(both, 50), p99 = bench::percentile(both, 99);
        std::printf(
            "lam_render_cpu_bench sprites=%d quads=%u batches=%u | collect p50 %.3f p99 %.3f ms | "
            "plan p50 %.3f p99 %.3f ms | collect+plan p50 %.3f p99 %.3f ms | allocations/frame "
            "%.2f%s\n",
            sprites, st.quads, st.batches, static_cast<double>(bench::percentile(collect, 50)),
            static_cast<double>(bench::percentile(collect, 99)),
            static_cast<double>(bench::percentile(plan, 50)),
            static_cast<double>(bench::percentile(plan, 99)), static_cast<double>(p50),
            static_cast<double>(p99), static_cast<double>(allocations) / kFrames,
            allocation_counting_enabled() ? "" : " (counting off)");
        CHECK(st.batches <= 10u);
        CHECK(st.culled == 0u);
        if (allocation_counting_enabled()) CHECK(allocations == 0u);
#if !LAM_DEV
        if (sprites == 50000) CHECK(p50 <= 4.0f);
#endif
    }
}
