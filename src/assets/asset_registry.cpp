// Registry, slots, refcounts and scopes. The registry is shared with the worker under
// Impl::mutex; slots, refcounts and scopes are main-thread only.
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lam/core/assert.h>

#include "detail/asset_internal.h"

namespace lam {

using detail::AssetRef;
using detail::Lookup;
using detail::RegEntry;
using detail::Slot;
using detail::TypeTable;

AssetServer::Impl::Impl(App &a, AssetServer &s, AssetsConfig c)
    : app(a), self(s), config(std::move(c)) {}

TypeTable &AssetServer::Impl::table(TypeId type) {
    if (TypeTable *t = table_if(type)) return *t;
    // Main thread only (register_type, loads); the worker reads `types` under the mutex.
    const std::lock_guard<std::mutex> lock(mutex);
    if (type >= types.size()) types.resize(type + 1);
    types[type] = std::make_unique<TypeTable>();
    types[type]->slots.emplace_back(); // index 0 = none
    types[type]->reg.emplace_back();
    return *types[type];
}

TypeTable *AssetServer::Impl::table_if(TypeId type) const {
    return type < types.size() ? types[type].get() : nullptr;
}

Slot *AssetServer::Impl::find(TypeId type, std::uint64_t bits) {
    TypeTable *t = table_if(type);
    if (t == nullptr) return nullptr;
    const std::uint32_t index = detail::asset_bits_index(bits);
    if (index == 0 || index >= t->slots.size()) return nullptr;
    Slot &s = t->slots[index];
    if (s.state == LoadState::None || s.generation != detail::asset_bits_generation(bits))
        return nullptr;
    return &s;
}

const Slot *AssetServer::Impl::find(TypeId type, std::uint64_t bits) const {
    return const_cast<Impl *>(this)->find(type, bits);
}

bool AssetServer::Impl::materialised(AssetRef r, std::uint32_t generation) const {
    const TypeTable *t = table_if(r.type);
    if (t == nullptr || r.index == 0 || r.index >= t->slots.size()) return false;
    const Slot &s = t->slots[r.index];
    return s.state != LoadState::None && s.generation == generation;
}

Slot &AssetServer::Impl::slot_storage(AssetRef r) {
    TypeTable &t = *types[r.type];
    while (t.slots.size() <= r.index)
        t.slots.emplace_back();
    return t.slots[r.index];
}

AssetLoaderBase *AssetServer::Impl::find_loader_locked(const TypeTable &t,
                                                       std::string_view path) const {
    std::string_view file = detail::asset_strip_fragment(path);
    const std::size_t slash = file.find_last_of('/');
    if (slash != std::string_view::npos) file = file.substr(slash + 1);
    const std::string lower = detail::asset_to_lower(file);
    for (const detail::LoaderEntry &e : t.loaders) { // longest extension first
        if (lower.size() >= e.extension.size() &&
            std::string_view(lower).substr(lower.size() - e.extension.size()) == e.extension) {
            return e.loader;
        }
    }
    return nullptr;
}

std::uint32_t AssetServer::Impl::alloc_index_locked(TypeTable &t) {
    std::uint32_t index = 0;
    if (!t.free.empty()) {
        index = t.free.back();
        t.free.pop_back();
    } else {
        LAM_CHECK(t.reg.size() < 0xFFFFFFFFu, "AssetServer: too many assets of one type");
        t.reg.emplace_back();
        index = static_cast<std::uint32_t>(t.reg.size() - 1);
    }
    RegEntry &e = t.reg[index];
    e.live = true;
    e.pins = 0;
    return index;
}

Lookup AssetServer::Impl::lookup_locked(TypeId type, const std::string &key, bool allow_create,
                                        bool pin) {
    Lookup out;
    TypeTable *t = table_if(type);
    if (t == nullptr) return out;
    if (const auto it = t->by_path.find(key); it != t->by_path.end()) {
        RegEntry &e = t->reg[it->second];
        if (pin) ++e.pins;
        out.ok = true;
        out.ref = AssetRef{type, it->second};
        out.generation = e.generation;
        out.loader = find_loader_locked(*t, key);
        return out;
    }
    if (!allow_create) return out;
    AssetLoaderBase *loader = find_loader_locked(*t, key);
    if (loader == nullptr) return out;
    const std::uint32_t index = alloc_index_locked(*t);
    RegEntry &e = t->reg[index];
    if (pin) ++e.pins;
    t->by_path.emplace(key, index);
    out.ok = true;
    out.created = true;
    out.ref = AssetRef{type, index};
    out.generation = e.generation;
    out.loader = loader;
    return out;
}

Slot &AssetServer::Impl::materialise(AssetRef r, std::uint32_t generation, const std::string &path,
                                     AssetLoaderBase *loader) {
    Slot &s = slot_storage(r);
    LAM_ASSERT(s.state == LoadState::None, "AssetServer: materialising a live slot");
    s.generation = generation;
    s.state = LoadState::Queued;
    s.runtime = false;
    s.loader = loader;
    s.path = path;
    s.refs = 0;
    s.version = 0;
    s.error = {};
    s.reload_requested = false;
    return s;
}

void AssetServer::Impl::release_index(AssetRef r) {
    Slot &s = at(r);
    const std::lock_guard<std::mutex> lock(mutex);
    TypeTable &t = *types[r.type];
    if (const auto it = t.by_path.find(s.path); it != t.by_path.end() && it->second == r.index)
        t.by_path.erase(it);
    RegEntry &e = t.reg[r.index];
    e.live = false;
    e.pins = 0;
    if (detail::advance_generation(e.generation)) t.free.push_back(r.index);
    s.generation = e.generation;
}

void AssetServer::Impl::emit(TypeId type, std::uint8_t kind, std::uint64_t bits) {
    TypeTable *t = table_if(type);
    if (t != nullptr && t->emit != nullptr) t->emit(app, kind, bits);
}

// ---- refcounts and scopes ----------------------------------------------------------------------

detail::ScopeRecord *AssetServer::Impl::scope_record(AssetScope scope) {
    for (detail::ScopeRecord &r : scopes) {
        if (r.id == scope) return &r;
    }
    return nullptr;
}

void AssetServer::Impl::release(AssetRef r) {
    Slot &s = at(r);
    LAM_ASSERT(s.refs > 0, "AssetServer: asset released more often than retained");
    if (s.refs > 0) --s.refs;
    if (s.refs == 0) gc_pending = true;
}

// One reference per (asset, scope), so loading a path twice in a scope is free. During a loader's
// finalize() the scope is kFinalizeScope: whatever it adds or loads becomes a dependency of the
// asset being finalized and dies with it.
void AssetServer::Impl::retain_in_scope(AssetRef r, AssetScope scope) {
    if (scope == detail::kFinalizeScope) {
        LAM_ASSERT(finalizing != nullptr, "AssetServer: kFinalizeScope used outside a finalize()");
        if (finalizing != nullptr) {
            if (r == finalizing->ref) return; // the asset itself: retained by whoever loads it
            std::vector<AssetRef> &deps = finalizing->deps;
            if (std::find(deps.begin(), deps.end(), r) != deps.end()) return;
            retain(r);
            finalizing->deps.push_back(r);
            return;
        }
        scope = kRootScope;
    }
    detail::ScopeRecord *rec = scope_record(scope);
    LAM_ASSERT(rec != nullptr,
               "AssetServer: load into a scope that was never begun or already ended");
    if (rec == nullptr) rec = &scopes.front(); // Release: the root scope
    Slot &s = at(r);
    if (std::find(s.scopes.begin(), s.scopes.end(), rec->id) != s.scopes.end()) return;
    s.scopes.push_back(rec->id);
    rec->entries.push_back(r);
    retain(r);
}

void AssetServer::Impl::end_scope(AssetScope scope) {
    auto it = std::find_if(scopes.begin(), scopes.end(),
                           [&](const detail::ScopeRecord &r) { return r.id == scope; });
    LAM_ASSERT(it != scopes.end(), "AssetServer::end_scope: scope was never begun or already ended");
    if (it == scopes.end()) return;
    std::vector<AssetRef> entries = std::move(it->entries);
    it->entries.clear();
    if (scope != kRootScope) scopes.erase(it);
    if (current == scope) current = kRootScope;
    for (const AssetRef &r : entries) {
        Slot &s = at(r);
        s.scopes.erase(std::remove(s.scopes.begin(), s.scopes.end(), scope), s.scopes.end());
        release(r);
    }
    gc_pending = true;
}

} // namespace lam
