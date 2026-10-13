#include <lam/core/profiler.h>

#include <chrono>

namespace lam {
namespace {
Profiler *g_current = nullptr;
std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
} // namespace

namespace detail {
Profiler *current_profiler() { return g_current; }
void set_current_profiler(Profiler *p) { g_current = p; }
} // namespace detail

Profiler::Profiler() {
    current_.reserve(256);
    last_.reserve(256);
    stack_.reserve(64);
}

std::uint32_t Profiler::slot_of(std::string_view name) {
    for (std::uint32_t i = 0; i < current_.size(); ++i)
        if (current_[i].name.data() == name.data() || current_[i].name == name) return i;
    current_.push_back({name, 0, 0});
    return static_cast<std::uint32_t>(current_.size() - 1);
}

void Profiler::begin(std::string_view name) { stack_.push_back({slot_of(name), now_ns()}); }

void Profiler::end() {
    if (stack_.empty()) return;
    const Open o = stack_.back();
    stack_.pop_back();
    current_[o.slot].ms += static_cast<double>(now_ns() - o.start_ns) / 1e6;
}

std::span<const StageTiming> Profiler::timings() const { return last_; }

void Profiler::end_frame() {
    last_.resize(current_.size());
    for (std::size_t i = 0; i < current_.size(); ++i) {
        const double max_before = last_[i].name == current_[i].name ? last_[i].max_ms : 0.0;
        last_[i] = current_[i];
        last_[i].max_ms = current_[i].ms > max_before ? current_[i].ms : max_before;
        current_[i].ms = 0;
    }
}

void Profiler::reset() {
    current_.clear();
    last_.clear();
    stack_.clear();
}

} // namespace lam
