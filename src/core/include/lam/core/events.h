#pragma once
// Typed event channels: one EventBus per world plus one for the App. Events carry the bus's
// sequence number, and Ctx::read<T>() returns what arrived since the stage last ran.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <lam/core/assert.h>
#include <lam/core/id.h>
namespace lam {
enum class EventScope : std::uint8_t { World, App };
namespace detail {
class EventChannelBase {
public:
    virtual ~EventChannelBase() = default;
    virtual void trim(std::uint32_t before) = 0;
    virtual std::size_t size() const = 0;
};
std::uint64_t &event_grow_count();                              // channels that grew past capacity
}
template <class T> class Events final : public detail::EventChannelBase {
public:
    Events(std::uint32_t *sequence, std::size_t capacity) : sequence_(sequence), capacity_(capacity) {
        items_.reserve(capacity);
        seqs_.reserve(capacity);
    }
    // Appends e with the next bus sequence number. No allocation below capacity().
    void emit(const T &e) {
        if (items_.size() == items_.capacity()) ++detail::event_grow_count();
        items_.push_back(e);
        seqs_.push_back((*sequence_)++);
    }
    // Events with from <= seq < to, in emission order.
    std::span<const T> window(std::uint32_t from_seq, std::uint32_t to_seq) const {
        const auto lo = std::lower_bound(seqs_.begin(), seqs_.end(), from_seq) - seqs_.begin();
        const auto hi = std::lower_bound(seqs_.begin(), seqs_.end(), to_seq) - seqs_.begin();
        if (hi <= lo) return {};
        return std::span<const T>(items_.data() + lo, static_cast<std::size_t>(hi - lo));
    }
    // Everything retained (tests, tools).
    std::span<const T> all() const { return items_; }
    std::size_t capacity() const { return capacity_; }
    // Raises capacity() to at least `capacity` and reserves that much (never shrinks). Cold path:
    // allocates when it grows. App::reserve_event calls it on every bus holding the channel.
    void reserve(std::size_t capacity) {
        if (capacity <= capacity_) return;
        capacity_ = capacity;
        items_.reserve(capacity);
        seqs_.reserve(capacity);
    }
    std::size_t size() const override { return items_.size(); }
    void trim(std::uint32_t before) override {
        const auto n = std::lower_bound(seqs_.begin(), seqs_.end(), before) - seqs_.begin();
        if (n <= 0) return;
        items_.erase(items_.begin(), items_.begin() + n);
        seqs_.erase(seqs_.begin(), seqs_.begin() + n);
    }
private:
    std::uint32_t *sequence_;
    std::size_t capacity_;
    std::vector<T> items_;
    std::vector<std::uint32_t> seqs_;
};
class EventBus {
public:
    EventBus();
    ~EventBus();
    EventBus(const EventBus &) = delete;
    EventBus &operator=(const EventBus &) = delete;
    // Registers channel T (idempotent; a second call keeps the first capacity).
    template <class T> Events<T> &add(std::size_t capacity = 256) {
        const TypeId id = type_id<T>();
        if (id >= channels_.size()) channels_.resize(id + 1);
        if (!channels_[id]) channels_[id] = std::make_unique<Events<T>>(&sequence_, capacity);
        return static_cast<Events<T> &>(*channels_[id]);
    }
    // LAM_ASSERT registered.
    template <class T> Events<T> &get() {
        Events<T> *e = try_get<T>();
        LAM_ASSERT(e != nullptr, "event type not registered: call App::add_event<T>() in a plugin build()");
        return *e;
    }
    template <class T> Events<T> *try_get() {
        const TypeId id = type_id<T>();
        if (id >= channels_.size() || !channels_[id]) return nullptr;
        return static_cast<Events<T> *>(channels_[id].get());
    }
    template <class T> const Events<T> *try_get() const {
        const TypeId id = type_id<T>();
        if (id >= channels_.size() || !channels_[id]) return nullptr;
        return static_cast<const Events<T> *>(channels_[id].get());
    }
    // The next sequence number.
    std::uint32_t sequence() const;
    // Engine: drops events with seq < before in every channel.
    void trim(std::uint32_t before);
    // Events currently retained in all channels.
    std::size_t retained() const;
private:
    std::uint32_t sequence_ = 1;
    std::vector<std::unique_ptr<detail::EventChannelBase>> channels_;
};
}
