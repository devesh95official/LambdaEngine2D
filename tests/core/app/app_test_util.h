#pragma once
// lam_app test helpers: a trace resource, a hand-driven clock and a fork-based death check.
#include <csignal>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#include <lam/core/app.h>
#include <lam/core/log.h>
#include <lam/core/platform.h>
namespace lam::apptest {

// App resource: what the test systems saw, in call order.
struct Trace {
    std::vector<std::string> lines;
    void add(std::string s) { lines.push_back(std::move(s)); }
    std::string joined() const {
        std::string out;
        for (const auto &l : lines) {
            if (!out.empty()) out += ' ';
            out += l;
        }
        return out;
    }
};

inline std::string world_label(const World &w) { return id_display(w.name()); }

// "label@world" into the App's Trace.
inline void trace(Ctx &ctx, std::string_view label) {
    ctx.res<Trace>().add(std::string(label) + "@" + world_label(ctx.world()));
}

// A platform whose clock the test moves by hand.
class ManualPlatform final : public Platform {
public:
    double now = 0;
    int begins = 0;
    int ends = 0;
    bool close = false;
    bool should_close() override { return close; }
    double now_seconds() override { return now; }
    void begin_frame() override { ++begins; }
    void end_frame() override { ++ends; }
    WindowInfo window() const override { return WindowInfo{}; }
    void set_fullscreen(bool) override {}
    bool has_gpu() const override { return false; }
};

// A non-headless App (settings not persisted) driven by a ManualPlatform; tick rate 64 so tick
// lengths are exact binary fractions.
struct ManualApp {
    explicit ManualApp(AppConfig c = make_config()) : app(std::move(c)) {
        auto p = std::make_unique<ManualPlatform>();
        platform = p.get();
        app.set_platform(std::move(p));
    }
    static AppConfig make_config() {
        AppConfig c;
        c.tick_rate = 64;
        c.settings_file = "";
        c.max_ticks_per_frame = 4;
        return c;
    }
    // One frame after advancing the clock by `ticks` tick lengths.
    void frame_after(double ticks) {
        platform->now += ticks / 64.0;
        app.run_frames(1);
    }
    App app;
    ManualPlatform *platform = nullptr;
};

// Runs fn in a forked child with stderr captured; true if the child aborted. `output` receives
// what it printed (LAM_CHECK/LAM_ASSERT messages).
inline bool dies(const std::function<void()> &fn, std::string *output = nullptr) {
    int fds[2];
    if (::pipe(fds) != 0) return false;
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // The child must die quietly: no doctest crash handler, no doctest output.
        std::signal(SIGABRT, SIG_DFL);
        ::close(fds[0]);
        ::dup2(fds[1], 2);
        if (FILE *null = std::fopen("/dev/null", "w")) ::dup2(::fileno(null), 1);
        set_log_sink(nullptr);
        fn();
        std::fflush(stderr);
        ::_exit(0);
    }
    ::close(fds[1]);
    std::string out;
    char buf[512];
    for (;;) {
        const ssize_t n = ::read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (output) *output = out;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

} // namespace lam::apptest
