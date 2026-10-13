#include <lam/core/id.h>

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace lam {
namespace {
struct NameTable {
    std::shared_mutex mutex;
    std::unordered_map<std::uint64_t, std::string_view> names;
    std::deque<std::string> storage;                            // stable addresses
};
NameTable &table() {
    static NameTable t;
    return t;
}
} // namespace

StrId intern(std::string_view s) {
    const StrId id = str_id(s);
    register_id_name(id, s);
    return id;
}

void register_id_name(StrId id, std::string_view name) {
    if (!id) return;
    NameTable &t = table();
    {
        std::shared_lock lock(t.mutex);
        if (t.names.find(id.value) != t.names.end()) return;
    }
    std::unique_lock lock(t.mutex);
    if (t.names.find(id.value) != t.names.end()) return;
    t.storage.emplace_back(name);
    t.names.emplace(id.value, std::string_view(t.storage.back()));
}

std::string_view id_name(StrId id) {
    NameTable &t = table();
    std::shared_lock lock(t.mutex);
    const auto it = t.names.find(id.value);
    return it == t.names.end() ? std::string_view{} : it->second;
}

std::string id_display(StrId id) {
    const std::string_view n = id_name(id);
    if (!n.empty()) return std::string(n);
    char buf[24];
    std::snprintf(buf, sizeof buf, "#%016llx", static_cast<unsigned long long>(id.value));
    return buf;
}

namespace detail {
TypeId next_type_id() {
    static std::atomic<TypeId> next{1};
    return next.fetch_add(1);
}
} // namespace detail

} // namespace lam
