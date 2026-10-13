// ECS bench: a 100k-entity view pass, fresh and with 10% churn per tick. Release builds also
// check the budget, with a 2x margin.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/test/support.h>
#include <vector>

using namespace lam;

namespace {
struct BenchVel {
    Vec2 v;
}; // bench-local (no physics dependency)

constexpr int kEntities = 100000;
constexpr int kRuns = 50;

struct Timing {
    double best = 0;
    double median = 0;
};

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

Timing time_view(World &w) {
    std::vector<double> runs;
    runs.reserve(kRuns);
    float sink = 0;
    for (int r = 0; r < 5; ++r) // warm caches and CPU clocks; untimed
        w.view<Transform, BenchVel>().each(
            [](Entity, Transform &t, BenchVel &v) { t.position += v.v; });
    for (int r = 0; r < kRuns; ++r) {
        const double t0 = now_ms();
        w.view<Transform, BenchVel>().each(
            [](Entity, Transform &t, BenchVel &v) { t.position += v.v; });
        runs.push_back(now_ms() - t0);
        sink += w.view<Transform>().size_hint() > 0 ? 1.0f : 0.0f;
    }
    std::sort(runs.begin(), runs.end());
    (void)sink;
    return {runs.front(), runs[runs.size() / 2]};
}

void populate(World &w) {
    w.reserve<Transform>(kEntities);
    w.reserve<PrevTransform>(kEntities);
    w.reserve<BenchVel>(kEntities);
    for (int i = 0; i < kEntities; ++i) {
        const Entity e = w.create();
        w.add<Transform>(e, Transform{{float(i % 1000), float(i / 1000)}});
        w.add<BenchVel>(e, BenchVel{{1, 0.5f}});
    }
}

// Destroys every 10th entity of the view (by a rotating phase) and respawns as many.
void churn(World &w, int phase) {
    int k = 0;
    w.view<Transform, BenchVel>().each([&](Entity e, Transform &, BenchVel &) {
        if ((k++ + phase) % 10 == 0) w.commands().destroy(e);
    });
    const int n = k / 10;
    for (int i = 0; i < n; ++i) {
        const Entity e = w.commands().spawn();
        w.commands().add(e, Transform{{float(i), 0}});
        w.commands().add(e, BenchVel{{0, 1}});
    }
    w.commands().apply();
    settle_spawned(w);
}
} // namespace

TEST_CASE("bench: 100k view<Transform, BenchVel> fresh and churned") {
    WorldConfig cfg;
    cfg.entity_capacity = kEntities + 1024;
    cfg.commands_arena_bytes = 16u << 20;
    World w(cfg);
    populate(w);
    const Timing fresh = time_view(w);
    for (int t = 0; t < 60; ++t)
        churn(w, t);
    const Timing churned = time_view(w);
    AllocationScope allocs;
    for (int t = 0; t < 10; ++t)
        churn(w, t);
    const auto churn_allocs = allocs.count();
    std::printf(
        "lam_ecs_bench: %d entities view<Transform, BenchVel>: fresh best %.3f ms median %.3f ms "
        "(budget 0.5) | churned best %.3f ms median %.3f ms (budget 1.0) | churn allocations/tick "
        "%.1f\n",
        kEntities, fresh.best, fresh.median, churned.best, churned.median,
        double(churn_allocs) / 10.0);
    CHECK(w.entity_count() == std::size_t(kEntities));
#if defined(NDEBUG)
    CHECK(fresh.median <= 0.5 * 2);
    CHECK(churned.median <= 1.0 * 2);
#endif
}
