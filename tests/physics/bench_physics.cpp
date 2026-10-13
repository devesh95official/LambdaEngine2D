// lam_physics_bench: N bodies (half dynamic, half static) in a walled tile world, spread evenly
// ("uniform") or 90% packed into 10% of the area ("clustered"). Prints ms per whole App tick and
// allocations. Budget at 10k bodies in Release: 4 ms uniform, 6 ms clustered (--strict enforces).
// Cells default to 32 px since bodies are 4..12 px; --cell 64 measures PhysicsConfig's default.
//   lam_physics_bench [--bodies N] [--ticks T] [--scenario uniform|clustered|both] [--cell PX] [--strict]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <lam/core/alloc.h>
#include <lam/core/app.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/rng.h>
#include <lam/physics/components.h>
#include <lam/physics/physics_plugin.h>
#include <lam/physics/queries.h>
#include <lam/physics/tile_world.h>

using namespace lam;

namespace {

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct BenchResult {
    double mean = 0, median = 0, p99 = 0, worst = 0;
    PhysicsStats stats;
    double allocs_per_tick = 0;
    std::uint64_t hash = 0;
};

BenchResult run(int bodies, int ticks, bool clustered, float cell) {
    AppConfig ac;
    ac.settings_file = "";
    ac.entity_capacity = static_cast<std::size_t>(bodies) + 1024;
    App app(ac);
    PhysicsConfig pc;
    pc.cell_size = cell;
    pc.body_capacity = static_cast<std::size_t>(bodies) + 64;
    app.add_plugin<PhysicsPlugin>(pc);
    World &w = app.main_world();
    // Area: about one body per 40 x 40 px; walls all round (outside_solid) and a few pillars.
    const float side = std::sqrt(static_cast<float>(bodies)) * 40.0f;
    const int tiles = static_cast<int>(std::ceil(side / 32.0f));
    TileWorld &tw = w.resources().get<TileWorld>();
    tw.resize(tiles, tiles, 32, {});
    for (int i = 4; i < tiles - 4; i += 9) {
        for (int j = 4; j < tiles - 4; j += 13) tw.set({i, j}, TileShape::Solid, 0);
    }
    w.reserve<Transform>(static_cast<std::size_t>(bodies));
    w.reserve<PrevTransform>(static_cast<std::size_t>(bodies));
    w.reserve<Collider>(static_cast<std::size_t>(bodies));
    w.reserve<Velocity>(static_cast<std::size_t>(bodies));
    w.reserve<RigidBody>(static_cast<std::size_t>(bodies));
    Rng rng(2026);
    const float cluster = side * std::sqrt(0.1f);
    for (int i = 0; i < bodies; ++i) {
        const bool in_cluster = clustered && rng.unit() < 0.9f;
        const float lo = in_cluster ? (side - cluster) * 0.5f : 40.0f;
        const float hi = in_cluster ? (side + cluster) * 0.5f : side - 40.0f;
        const Vec2 at{rng.range(lo, hi), rng.range(lo, hi)};
        const Entity e = w.create();
        w.add<Transform>(e, Transform{at});
        Collider c;
        if (i % 2 == 0) {
            c.shape = ShapeType::Box;
            c.half = {rng.range(2.0f, 6.0f), rng.range(2.0f, 6.0f)};
        } else {
            c.shape = ShapeType::Circle;
            c.radius = rng.range(2.0f, 6.0f);
        }
        w.add<Collider>(e, c);
        if (i % 4 < 2) {
            RigidBody rb;
            rb.restitution = 0.3f;
            rb.friction = 0.2f;
            w.add<RigidBody>(e, rb);
            w.get<Velocity>(e).linear = {rng.range(-60.0f, 60.0f), rng.range(-60.0f, 60.0f)};
        }
    }
    app.step_ticks(60);                                          // warm-up: buffers reach size
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(ticks));
    const AllocationScope allocs;
    for (int t = 0; t < ticks; ++t) {
        const double t0 = now_ms();
        app.step_ticks(1);
        times.push_back(now_ms() - t0);
    }
    BenchResult r;
    r.allocs_per_tick = static_cast<double>(allocs.count()) / static_cast<double>(ticks > 0 ? ticks : 1);
    std::vector<double> sorted = times;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (const double v : times) sum += v;
    if (!sorted.empty()) {
        r.mean = sum / static_cast<double>(sorted.size());
        r.median = sorted[sorted.size() / 2];
        r.p99 = sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];
        r.worst = sorted.back();
    }
    r.stats = w.resources().get<PhysicsStats>();
    r.hash = w.state_hash();
    return r;
}

} // namespace

int main(int argc, char **argv) {
    int bodies = 10000;
    int ticks = 600;
    bool strict = false;
    float cell = 32.0f;
    std::string scenario = "both";
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const bool has_value = i + 1 < argc;
        if (std::strcmp(a, "--bodies") == 0 && has_value) {
            bodies = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(a, "--ticks") == 0 && has_value) {
            ticks = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(a, "--scenario") == 0 && has_value) {
            scenario = argv[++i];
        } else if (std::strcmp(a, "--cell") == 0 && has_value) {
            cell = static_cast<float>(std::atof(argv[++i]));
            if (!(cell > 0.0f)) cell = 32.0f;
        } else if (std::strcmp(a, "--strict") == 0) {
            strict = true;
        } else {
            std::fprintf(stderr, "usage: lam_physics_bench [--bodies N] [--ticks T] [--scenario uniform|clustered|both] [--cell PX] [--strict]\n");
            return 2;
        }
    }
#if defined(NDEBUG)
    const char *build = "release";
#else
    const char *build = "debug (budgets apply to Release)";
#endif
    std::printf("lam_physics_bench: %d bodies (half dynamic, boxes and circles), %d ticks, cell %.0f px, %s build\n",
                bodies, ticks, static_cast<double>(cell), build);
    bool over = false;
    for (const bool clustered : {false, true}) {
        if (scenario == "uniform" && clustered) continue;
        if (scenario == "clustered" && !clustered) continue;
        const BenchResult r = run(bodies, ticks, clustered, cell);
        const double budget = clustered ? 6.0 : 4.0;
        const bool ok = r.median <= budget;
        over = over || !ok;
        std::printf("  %-9s mean %.3f ms  median %.3f ms  p99 %.3f ms  worst %.3f ms  (budget %.0f ms: %s)\n",
                    clustered ? "clustered" : "uniform", r.mean, r.median, r.p99, r.worst, budget, ok ? "ok" : "OVER");
        std::printf("            PhysicsStats bodies %u pairs %u dropped %u cells %u contacts %u; allocations/tick %.2f%s; hash %016llx\n",
                    r.stats.bodies, r.stats.pairs, r.stats.dropped_pairs, r.stats.cells, r.stats.contacts,
                    r.allocs_per_tick, allocation_counting_enabled() ? "" : " (not counted)",
                    static_cast<unsigned long long>(r.hash));
    }
    return strict && over ? 1 : 0;
}
