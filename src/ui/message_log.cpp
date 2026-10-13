// Message log: a ring buffer of fixed-capacity lines; no allocation after construction.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <lam/core/assert.h>
#include <lam/ui/message_log.h>

namespace lam {

MessageLog::MessageLog(std::size_t lines) : ring_(std::max<std::size_t>(1, lines)) {}

void MessageLog::add(std::string_view text, Color c) {
    const std::size_t cap = ring_.size();
    std::size_t slot;
    if (size_ < cap) {
        slot = (head_ + size_) % cap;
        ++size_;
    } else {
        slot = head_;                                           // overwrite the oldest
        head_ = (head_ + 1) % cap;
    }
    ring_[slot].text = text;                                    // truncates at a UTF-8 boundary
    ring_[slot].color = c;
    ++added_;
}

std::size_t MessageLog::size() const {
    return size_;
}

std::string_view MessageLog::line(std::size_t i) const {
    LAM_ASSERT(i < size_, "MessageLog::line: index out of range");
    if (i >= size_) return {};
    return ring_[(head_ + i) % ring_.size()].text.view();
}

Color MessageLog::color(std::size_t i) const {
    LAM_ASSERT(i < size_, "MessageLog::color: index out of range");
    if (i >= size_) return kWhite;
    return ring_[(head_ + i) % ring_.size()].color;
}

void MessageLog::clear() {
    head_ = 0;
    size_ = 0;
}

std::size_t MessageLog::capacity() const {
    return ring_.size();
}

std::uint64_t MessageLog::added() const {
    return added_;
}

} // namespace lam
