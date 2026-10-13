// Asset budgets: the per-frame finalize budget, and the idle cost of asset_update. Margins are
// wide so sanitizer and Debug builds don't flake.
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/test/support.h>

#include "asset_test_util.h"

using namespace lam;
using namespace assets_test;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// Microseconds of one server.update(): the best mean of 10 batches of `n` calls, so a preemption
// on a busy machine does not count as the server's cost.
double mean_update_us(AssetServer &s, int n) {
    double best = 1e30;
    for (int batch = 0; batch < 10; ++batch) {
        const Clock::time_point t = Clock::now();
        for (int i = 0; i < n; ++i)
            s.update();
        best = std::min(best, ms_since(t) * 1000.0 / n);
    }
    return best;
}

} // namespace

TEST_CASE("budget: slow finalizes are spread over frames within finalize_budget_ms") {
    constexpr float kBudgetMs = 5.0f;
    constexpr int kFinalizeMs = 2;
    constexpr int kAssets = 10;
    AsyncFixture f(async_config(kBudgetMs));
    std::vector<AssetId<Node>> ids;
    for (int i = 0; i < kAssets; ++i) {
        const std::string path = "s" + std::to_string(i) + ".node";
        write(f.dir, path, "value: x\nslowfin: " + std::to_string(kFinalizeMs));
        ids.push_back(f.s().load<Node>(path));
    }
    REQUIRE(wait_decodes(kAssets));
    int frames = 0;
    int most = 0;
    double longest_ms = 0.0;
    std::size_t ready_before = 0;
    while (f.s().pending() > 0 && frames < 100) {
        const Clock::time_point t = Clock::now();
        f.s().update();
        longest_ms = std::max(longest_ms, ms_since(t));
        ++frames;
        const auto ready = static_cast<std::size_t>(std::count_if(
            ids.begin(), ids.end(), [&](AssetId<Node> id) { return f.s().state(id) == LoadState::Ready; }));
        CHECK(ready > ready_before); // at least one finalize per update
        most = std::max(most, static_cast<int>(ready - ready_before));
        ready_before = ready;
    }
    CHECK(f.s().pending() == 0);
    // A finalize starts only while the budget is not used up: at most ceil(5 / 2) = 3 per frame.
    CHECK(most <= 3);
    CHECK(frames >= 4);
    // One update overruns the budget by at most the finalize that was running (plus slack).
    CHECK(longest_ms < kBudgetMs + kFinalizeMs + 20.0);
    MESSAGE("finalize budget " << kBudgetMs << " ms, " << kAssets << " finalizes of " << kFinalizeMs
                               << " ms: " << frames << " frames, at most " << most
                               << " per frame, longest update " << longest_ms << " ms");
}

TEST_CASE("budget: steady-state asset_update cost with 1000 Ready assets") {
    test::CaptureLog log; // 1000 Loaded events grow the event channel (a LAM_DEV warning)
    constexpr int kAssets = 1000;
    {
        SyncFixture f;
        for (int i = 0; i < kAssets; ++i) {
            const std::string path = "a" + std::to_string(i) + ".node";
            write_fast(f.dir, path, "value: x");
            f.s().load<Node>(path);
        }
        const double us = mean_update_us(f.s(), 1000);
        CHECK(us < 100.0);
        MESSAGE("synchronous update(): " << us << " us with " << kAssets << " assets");
    }
    {
        AsyncFixture f(async_config());
        for (int i = 0; i < kAssets; ++i)
            write_fast(f.dir, "a" + std::to_string(i) + ".node", "value: x");
        const Clock::time_point t = Clock::now();
        for (int i = 0; i < kAssets; ++i)
            f.s().load<Node>("a" + std::to_string(i) + ".node");
        f.s().wait_all();
        const double load_ms = ms_since(t);
        const double us = mean_update_us(f.s(), 1000);
        CHECK(us < 100.0);
        MESSAGE("asynchronous update(): " << us << " us with " << kAssets << " assets; load() of "
                                          << kAssets << " small files to all Ready: " << load_ms
                                          << " ms");
    }
}

#if LAM_DEV
TEST_CASE("budget: hot reload polling cost per watched file") {
    test::CaptureLog log;
    constexpr int kAssets = 1000;
    AssetsConfig c = async_config();
    c.hot_reload = true;
    c.watch_interval_seconds = 0.0f; // every watched file on every update: the worst case
    AsyncFixture f(c);
    for (int i = 0; i < kAssets; ++i) {
        const std::string path = "w" + std::to_string(i) + ".node";
        write_fast(f.dir, path, "value: x");
        f.s().load<Node>(path);
    }
    f.s().wait_all();
    f.s().update();
    const double us = mean_update_us(f.s(), 5);
    const double per_file = us / kAssets;
    // With the default 0.5 s interval at 60 fps a frame stats 1000 / 30 ~ 34 files.
    MESSAGE("hot reload poll: " << per_file << " us per watched file; " << kAssets
                                << " files every frame = " << us << " us; at 0.5 s / 60 fps ~ "
                                << per_file * kAssets / 30.0 << " us per frame");
    CHECK(per_file < 200.0);
    CHECK(g_journal.count('D') == kAssets); // nothing changed: nothing reloaded
}
#endif
