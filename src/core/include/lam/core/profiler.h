#pragma once
// Per-stage and per-system timings. App resource; active when LAM_DEV || LAM_PROFILE.
// The App calls end_frame() once per frame; timings() then reports that frame.
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
namespace lam {
struct StageTiming { std::string_view name; double ms = 0; double max_ms = 0; };
class Profiler {
public:
    Profiler();
    // name: static storage. Nested scopes are timed independently.
    void begin(std::string_view name);
    void end();
    // Last frame, per stage and per system, in first-seen order; max_ms is the max since start.
    std::span<const StageTiming> timings() const;
    // Engine: publishes this frame's totals and starts the next frame.
    void end_frame();
    void reset();
private:
    struct Open { std::uint32_t slot; std::uint64_t start_ns; };
    std::uint32_t slot_of(std::string_view name);
    std::vector<StageTiming> current_;
    std::vector<StageTiming> last_;
    std::vector<Open> stack_;
};
namespace detail {
Profiler *current_profiler();
void set_current_profiler(Profiler *p);
class ProfileScope {
public:
    explicit ProfileScope(std::string_view name) : p_(current_profiler()) {
        if (p_) p_->begin(name);
    }
    ~ProfileScope() {
        if (p_) p_->end();
    }
    ProfileScope(const ProfileScope &) = delete;
    ProfileScope &operator=(const ProfileScope &) = delete;
private:
    Profiler *p_;
};
}
}
#if (defined(LAM_DEV) && LAM_DEV) || (defined(LAM_PROFILE) && LAM_PROFILE)
#define LAM_PROFILE_SCOPE(name) ::lam::detail::ProfileScope LAM_PROFILE_CAT_(lam_prof_, __LINE__){name}
#define LAM_PROFILE_CAT2_(a, b) a##b
#define LAM_PROFILE_CAT_(a, b) LAM_PROFILE_CAT2_(a, b)
#else
#define LAM_PROFILE_SCOPE(name) ((void)0)
#endif
