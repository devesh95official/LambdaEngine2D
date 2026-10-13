#pragma once
// Type-keyed singleton store. Values never move after insertion, so references stay valid
// until remove<T>(). Destroyed in reverse insertion order.
#include <cstdint>
#include <memory>
#include <typeinfo>
#include <utility>
#include <vector>
#include <lam/core/assert.h>
#include <lam/core/id.h>
namespace lam {
namespace detail {
[[noreturn]] void resource_missing(const char *type_name, const char *where);
}
class Resources {
public:
    Resources();
    ~Resources();
    Resources(const Resources &) = delete;
    Resources &operator=(const Resources &) = delete;
    // Constructs T in place. LAM_ASSERT(!has<T>()).
    template <class T, class... A> T &insert(A &&...args) {
        const TypeId id = type_id<T>();
        if (id >= slots_.size()) slots_.resize(id + 1, nullptr);
        LAM_ASSERT(slots_[id] == nullptr, "Resources::insert: resource already present");
        if (slots_[id]) remove_id(id);
        T *p = new T(std::forward<A>(args)...);
        slots_[id] = p;
        order_.push_back({id, p, [](void *q) { delete static_cast<T *>(q); }});
        return *p;
    }
    // LAM_ASSERT present.
    template <class T> T &get() {
        T *p = try_get<T>();
        if (!p) detail::resource_missing(typeid(T).name(), "Resources::get");
        return *p;
    }
    template <class T> const T &get() const {
        const T *p = try_get<T>();
        if (!p) detail::resource_missing(typeid(T).name(), "Resources::get");
        return *p;
    }
    template <class T> T *try_get() {
        const TypeId id = type_id<T>();
        return id < slots_.size() ? static_cast<T *>(slots_[id]) : nullptr;
    }
    template <class T> const T *try_get() const {
        const TypeId id = type_id<T>();
        return id < slots_.size() ? static_cast<const T *>(slots_[id]) : nullptr;
    }
    template <class T> bool has() const { return try_get<T>() != nullptr; }
    template <class T> void remove() { remove_id(type_id<T>()); }
    // Destroys everything in reverse insertion order.
    void clear();
    // Type-erased access by TypeId (snapshots of world resources).
    void *get_raw(TypeId id) const;
private:
    struct Entry {
        TypeId id;
        void *ptr;
        void (*destroy)(void *);
    };
    void remove_id(TypeId id);
    std::vector<void *> slots_;
    std::vector<Entry> order_;
};
}
