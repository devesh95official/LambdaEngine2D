#include <lam/core/events.h>

namespace lam {

namespace detail {
std::uint64_t &event_grow_count() {
    static std::uint64_t count = 0;
    return count;
}
} // namespace detail

EventBus::EventBus() = default;
EventBus::~EventBus() = default;

std::uint32_t EventBus::sequence() const { return sequence_; }

void EventBus::trim(std::uint32_t before) {
    for (auto &c : channels_)
        if (c) c->trim(before);
}

std::size_t EventBus::retained() const {
    std::size_t n = 0;
    for (const auto &c : channels_)
        if (c) n += c->size();
    return n;
}

} // namespace lam
