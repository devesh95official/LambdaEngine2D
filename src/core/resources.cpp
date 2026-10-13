#include <lam/core/log.h>
#include <lam/core/resources.h>

#include <cstdio>

namespace lam {

namespace detail {
void resource_missing(const char *type_name, const char *where) {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "%s: resource %s is missing (insert it in a plugin build()/init_world(); App resources "
                  "use ctx.res<T>(), World resources use ctx.local<T>())",
                  where, type_name);
    assert_fail(__FILE__, __LINE__, "resource present", buf);
}
} // namespace detail

Resources::Resources() = default;

Resources::~Resources() { clear(); }

void Resources::clear() {
    while (!order_.empty()) {
        const Entry e = order_.back();
        order_.pop_back();
        slots_[e.id] = nullptr;
        e.destroy(e.ptr);
    }
}

void *Resources::get_raw(TypeId id) const { return id < slots_.size() ? slots_[id] : nullptr; }

void Resources::remove_id(TypeId id) {
    if (id >= slots_.size() || !slots_[id]) return;
    for (std::size_t i = order_.size(); i-- > 0;) {
        if (order_[i].id != id) continue;
        const Entry e = order_[i];
        order_.erase(order_.begin() + static_cast<std::ptrdiff_t>(i));
        slots_[id] = nullptr;
        e.destroy(e.ptr);
        return;
    }
}

} // namespace lam
