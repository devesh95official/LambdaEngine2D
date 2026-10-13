#pragma once
// Shared test harness, linked into every test executable by lam_add_test. LAM_DATA_DIR points at
// a per-process temp folder so tests never touch the user's real data.
//   lam::test::HeadlessApp h;  h.add<MyPlugin>();  h.ticks(120);
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <doctest/doctest.h>
#include <lam/core/alloc.h>
#include <lam/core/app.h>
#include <lam/core/log.h>
namespace lam::test {
// Unique temp folder, removed with its contents by the destructor.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
    const std::string &path() const;                            // absolute
    std::string file(std::string_view name) const;              // path()/name (not created)
private:
    std::string path_;
};
// An App built with --headless: NullPlatform, one tick per frame, settings not persisted.
class HeadlessApp {
public:
    explicit HeadlessApp(AppConfig config = {});
    App &app();                                                 // valid while this object lives
    template <class P, class... A> P &add(A &&...args) { return app().add_plugin<P>(std::forward<A>(args)...); }
    void frames(int n);                                         // App::run_frames
    void ticks(int n);                                          // App::step_ticks (one tick per frame)
private:
    std::unique_ptr<App> app_;
};
// app.step_ticks(n): n frames with exactly one tick each (unless ticks are held).
void run_ticks(App &app, int n);
// app.run_frames(n): n frames; under --headless each runs one tick.
void run_frames(App &app, int n);
// Captures log lines while alive, then restores the previous sink. Thread-safe.
class CaptureLog final : public LogSink {
public:
    CaptureLog();
    ~CaptureLog() override;
    CaptureLog(const CaptureLog &) = delete;
    CaptureLog &operator=(const CaptureLog &) = delete;
    void write(LogLevel level, std::string_view msg) override;
    bool contains(std::string_view text) const;                 // any captured line contains text
    std::size_t count(LogLevel level) const;                    // lines captured at exactly `level`
    std::string text() const;                                   // every line, '\n'-terminated
    void clear();
private:
    struct Line {
        LogLevel level;
        std::string text;
    };
    mutable std::mutex mutex_;
    std::vector<Line> lines_;
    LogSink *previous_ = nullptr;
};
// Compares `actual` with a golden file (relative to LAM_TEST_SOURCE_DIR when set) and prints the
// first differing line on mismatch. LAM_UPDATE_GOLDEN=1 rewrites the file instead.
bool golden_equal(std::string_view actual, const std::string &golden_path);
namespace detail {
// Routes assertion results to a private reporter while alive; end() drops their failures.
class MustFailScope {
public:
    MustFailScope();
    ~MustFailScope();
    MustFailScope(const MustFailScope &) = delete;
    MustFailScope &operator=(const MustFailScope &) = delete;
    // Restores reporting; true if an assertion failed inside. Idempotent.
    bool end();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
}
// AllocationScope around stmt, then CHECK(count == 0); reports "skipped" when counting is off (asan).
// Use after warm-up: run_ticks(app, 120); CHECK_NO_ALLOC(run_ticks(app, 600));
#define CHECK_NO_ALLOC(...)                                                                        \
    do {                                                                                           \
        if (!::lam::allocation_counting_enabled()) {                                               \
            __VA_ARGS__;                                                                           \
            MESSAGE("CHECK_NO_ALLOC skipped: allocation counting is off (no lam_alloc_hook)");     \
        } else {                                                                                   \
            const ::lam::AllocationScope lam_no_alloc_scope_;                                      \
            __VA_ARGS__;                                                                           \
            const auto lam_no_alloc_count_ = lam_no_alloc_scope_.count();                          \
            CHECK_MESSAGE(lam_no_alloc_count_ == 0u,                                               \
                          "CHECK_NO_ALLOC(" #__VA_ARGS__ "): " << lam_no_alloc_count_              \
                                                               << " allocation(s)");               \
        }                                                                                          \
    } while (false)
// Canary: passes only if the CHECK-family `check` inside fails. A REQUIRE would end the test.
//   MUST_FAIL(CHECK_NO_ALLOC(std::vector<int> v(100); escape = v.data()));
// (escape is volatile so Release can't optimise the allocation away.)
#define MUST_FAIL(...)                                                                             \
    do {                                                                                           \
        ::lam::test::detail::MustFailScope lam_must_fail_;                                         \
        __VA_ARGS__;                                                                               \
        const bool lam_must_fail_ok_ = lam_must_fail_.end();                                       \
        CHECK_MESSAGE(lam_must_fail_ok_, "MUST_FAIL canary: `" #__VA_ARGS__ "` did not fail");     \
    } while (false)
