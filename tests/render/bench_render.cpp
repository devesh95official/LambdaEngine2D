// lam_render_bench: the bench scene in a real window.
//   lam_render_bench [--sprites N] [--frames N] [--no-vsync] [--hidden] [--rlgl-batch] [--help]
// Prints p50/p99 CPU time of collect_sprites, render_plan and render_submit, the swap time, draw
// calls and operator-new calls per frame (raylib's malloc isn't counted). 60 warm-up frames.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/alloc.h>
#include <lam/core/app.h>
#include <lam/core/platform.h>
#include <lam/core/raylib_platform.h>
#include <lam/render/render_plan.h>
#include <lam/render/render_plugin.h>

#include "bench_scene.h"

namespace {

using namespace lam;

struct Samples { // App resource
    std::vector<float> collect, plan, upload, swap, total;
    std::vector<std::uint32_t> draws;
    std::uint64_t allocations = 0;        // engine (outside GL/window calls)
    std::uint64_t driver_allocations = 0; // inside GL/window-system calls
    std::uint64_t alloc_mark = 0;
    std::uint64_t driver_mark = 0;
    std::uint64_t bracket_mark = 0; // platform begin/end_frame bracket
    std::uint64_t bracket = 0;
    int frame = 0;
    int warmup = 60;
    double last = 0;
};

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Brackets platform_begin_frame and platform_end_frame to separate driver allocations from ours.
void bracket_mark(Ctx &ctx) {
    ctx.res<Samples>().bracket_mark = allocation_count();
}
void bracket_count(Ctx &ctx) {
    Samples &s = ctx.res<Samples>();
    s.bracket += allocation_count() - s.bracket_mark;
}

// FrameEnd Once +10002: after the swap timer, so RenderStats holds this frame's numbers.
void record(Ctx &ctx) {
    Samples &s = ctx.res<Samples>();
    const RenderStats &st = ctx.res<RenderStats>();
    const double t = now_ms();
    const std::uint64_t allocs = allocation_count();
    const std::uint64_t driver = ctx.res<RenderBackendStats>().driver_allocations + s.bracket;
    if (s.frame >= s.warmup) {
        s.collect.push_back(st.cpu_collect_ms);
        s.plan.push_back(st.cpu_plan_ms);
        s.upload.push_back(st.cpu_upload_ms);
        s.swap.push_back(st.gpu_swap_ms);
        s.total.push_back(static_cast<float>(t - s.last));
        s.draws.push_back(st.draw_calls);
        s.allocations += (allocs - s.alloc_mark) - (driver - s.driver_mark);
        s.driver_allocations += driver - s.driver_mark;
    }
    s.alloc_mark = allocs;
    s.driver_mark = driver;
    s.last = t;
    ++s.frame;
}

int int_arg(const LaunchOptions &o, std::string_view flag, int fallback) {
    for (std::size_t i = 0; i + 1 < o.extra.size(); ++i)
        if (o.extra[i] == flag) return std::atoi(o.extra[i + 1].c_str());
    return fallback;
}

constexpr const char *kUsage =
    "usage: lam_render_bench [--sprites N] [--frames N] [--no-vsync] [--hidden] [--rlgl-batch]\n"
    "  --sprites N    sprites in the bench scene (10000)\n"
    "  --frames N     measured frames after 60 warm-up frames (600)\n"
    "  --no-vsync     do not wait for the display (vsync is on by default)\n"
    "  --hidden       hidden window\n"
    "  --rlgl-batch   draw through rlgl's own batch instead of the engine's\n"
    "Prints p50/p99 ms of collect, plan, upload and swap, draw calls and allocations per frame.\n";

} // namespace

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        }
    }
    AppConfig cfg;
    cfg.title = "lam_render_bench";
    cfg.window_size = {960, 540};
    cfg.virtual_size = {480, 270};
    cfg.audio = false;
    cfg.settings_file = "";
    App app(cfg, argc, argv);
    const int sprites = int_arg(app.options(), "--sprites", 10000);
    const int frames = app.options().frames > 0 ? app.options().frames : 600;
    app.add_plugin<PlatformPlugin>();
    app.add_plugin<AssetsPlugin>();
    RenderConfig rc;
    rc.quad_capacity = static_cast<std::size_t>(sprites) + 4096;
    app.add_plugin<RenderPlugin>(rc);
    RaylibRenderConfig rl;
    rl.rlgl_batch = app.options().has("--rlgl-batch");
    app.add_plugin<RaylibRenderPlugin>(rl);
    if (!app.platform().has_gpu()) {
        std::fprintf(stderr, "lam_render_bench: no window/GPU\n");
        return 1;
    }
    bench::setup(app, sprites);
    Samples &s = app.insert_resource<Samples>();
    const std::size_t n = static_cast<std::size_t>(frames);
    for (auto *v : {&s.collect, &s.plan, &s.upload, &s.swap, &s.total})
        v->reserve(n);
    s.draws.reserve(n);
    s.last = now_ms();
    const auto once = [&](Stage stage, SystemFn fn, std::string_view name, int order) {
        SystemDesc d;
        d.stage = stage;
        d.fn = fn;
        d.name = name;
        d.order = order;
        d.scope = SystemScope::Once;
        app.add_system(d);
    };
    once(Stage::FrameBegin, bracket_mark, "bench.begin_frame.mark", -10001);
    once(Stage::FrameBegin, bracket_count, "bench.begin_frame.count", -9999);
    once(Stage::FrameEnd, bracket_mark, "bench.end_frame.mark", 9999);
    once(Stage::FrameEnd, bracket_count, "bench.end_frame.count", 10001);
    once(Stage::FrameEnd, record, "bench.record", 10002);
    app.run_frames(frames + s.warmup);

    const auto p = [](const std::vector<float> &v, float q) {
        return static_cast<double>(bench::percentile(v, q));
    };
    std::uint32_t max_draws = 0;
    for (std::uint32_t x : s.draws)
        max_draws = std::max(max_draws, x);
    const RenderStats &st = app.res<RenderStats>();
    std::printf(
        "lam_render_bench sprites=%d frames=%d%s%s quads=%u batches=%u draw_calls=%u (max %u)\n",
        sprites, frames, app.options().no_vsync ? " no-vsync" : "",
        rl.rlgl_batch ? " rlgl-batch" : "", st.quads, st.batches, st.draw_calls, max_draws);
    std::printf("  collect  p50 %7.3f  p99 %7.3f ms\n", p(s.collect, 50), p(s.collect, 99));
    std::printf("  plan     p50 %7.3f  p99 %7.3f ms\n", p(s.plan, 50), p(s.plan, 99));
    std::printf(
        "  upload   p50 %7.3f  p99 %7.3f ms   (render_submit: upload + draw calls + present)\n",
        p(s.upload, 50), p(s.upload, 99));
    std::printf("  swap     p50 %7.3f  p99 %7.3f ms\n", p(s.swap, 50), p(s.swap, 99));
    std::printf("  frame    p50 %7.3f  p99 %7.3f ms   (%.0f fps at p50)\n", p(s.total, 50),
                p(s.total, 99), p(s.total, 50) > 0.0 ? 1000.0 / p(s.total, 50) : 0.0);
    const double measured = s.total.empty() ? 1.0 : static_cast<double>(s.total.size());
    std::printf("  allocations/frame %.3f engine, %.1f inside GL/window-system calls%s\n",
                static_cast<double>(s.allocations) / measured,
                static_cast<double>(s.driver_allocations) / measured,
                allocation_counting_enabled() ? "" : " (counting off)");
    return max_draws <= 10 ? 0 : 2;
}
