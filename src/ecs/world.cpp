// World and component pools. Snapshot/restore are in snapshot.cpp, teleport in transform.cpp.
#include <lam/ecs/world.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>

#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>

namespace lam {

// ------------------------------------------------------------------------------------------------
// Pool

namespace detail {

namespace {
std::byte *allocate_aligned(std::size_t bytes, std::size_t align) {
    return static_cast<std::byte *>(::operator new(bytes, std::align_val_t(align)));
}
void free_aligned(std::byte *p, std::size_t align) {
    if (p) ::operator delete(p, std::align_val_t(align));
}
} // namespace

Pool::Pool(ComponentId id, const ComponentInfo &info, bool trivial)
    : id_(id), info_(&info), tag_(info.size == 0), trivial_(trivial), stride_(info.size),
      align_(info.align > 16 ? info.align : 16) {}

Pool::~Pool() {
    if (!tag_ && !trivial_) {
        for (std::uint32_t d = 0; d < dense_.size(); ++d)
            info_->destroy(at(d));
    }
    free_aligned(data_, align_);
}

void *Pool::tag_dummy() {
    alignas(16) static std::byte storage[16] = {};
    return storage;
}

void Pool::check_unlocked() const {
    LAM_CHECK(locks_ == 0, "structural change to a component pool while a View iterates it; "
                           "record it with world.commands() instead");
}

void Pool::ensure_data(std::size_t n) {
    if (tag_ || n <= capacity_) return;
    std::size_t cap = capacity_ * 2;
    if (cap < 16) cap = 16;
    if (cap < n) cap = n;
    std::byte *mem = allocate_aligned((cap + 1) * stride_, align_); // +1: scratch slot for swaps
    if (trivial_) {
        if (!dense_.empty()) std::memcpy(mem, data_, dense_.size() * stride_);
    } else {
        for (std::size_t d = 0; d < dense_.size(); ++d) {
            info_->move(mem + d * stride_, data_ + d * stride_);
            info_->destroy(data_ + d * stride_);
        }
    }
    free_aligned(data_, align_);
    data_ = mem;
    capacity_ = cap;
}

void Pool::set_sparse(std::uint32_t index, std::uint32_t slot) {
    const std::uint32_t page = index >> kPageBits;
    if (page >= pages_.size()) pages_.resize(page + 1);
    if (!pages_[page]) {
        pages_[page] = std::make_unique<std::uint32_t[]>(kPageSize);
        std::fill_n(pages_[page].get(), kPageSize, kNoSlot);
    }
    pages_[page][index & (kPageSize - 1)] = slot;
}

void Pool::relocate(void *dst, void *src) {
    if (trivial_) {
        std::memcpy(dst, src, stride_);
    } else {
        info_->move(dst, src);
        info_->destroy(src);
    }
}

void *Pool::insert(Entity e) {
    check_unlocked();
    const auto d = static_cast<std::uint32_t>(dense_.size());
    LAM_CHECK(d != kNoSlot, "component pool full");
    ensure_data(std::size_t(d) + 1);
    dense_.push_back(e);
    set_sparse(e.index, d);
    ++version_;
    return at(d);
}

void Pool::erase(std::uint32_t d) {
    check_unlocked();
    LAM_CHECK(d < dense_.size(), "component pool index out of range");
    const Entity removed = dense_[d];
    const auto last = static_cast<std::uint32_t>(dense_.size() - 1);
    if (!tag_ && !trivial_) info_->destroy(at(d));
    if (d != last) {
        if (!tag_) {
            if (trivial_)
                std::memcpy(at(d), at(last), stride_);
            else
                relocate(at(d), at(last));
        }
        dense_[d] = dense_[last];
        set_sparse(dense_[d].index, d);
    }
    dense_.pop_back();
    set_sparse(removed.index, kNoSlot);
    ++version_;
}

void Pool::clear() {
    check_unlocked();
    if (dense_.empty()) return;
    if (!tag_ && !trivial_) {
        for (std::uint32_t d = 0; d < dense_.size(); ++d)
            info_->destroy(at(d));
    }
    for (const Entity &e : dense_)
        set_sparse(e.index, kNoSlot);
    dense_.clear();
    ++version_;
}

void Pool::reserve(std::size_t n, std::size_t index_space) {
    ensure_data(n);
    dense_.reserve(n);
    if (index_space == 0) return;
    const auto last_page = static_cast<std::uint32_t>((index_space - 1) >> kPageBits);
    if (last_page >= pages_.size()) pages_.resize(last_page + 1);
    for (std::uint32_t page = 0; page <= last_page; ++page) {
        if (pages_[page]) continue;
        pages_[page] = std::make_unique<std::uint32_t[]>(kPageSize);
        std::fill_n(pages_[page].get(), kPageSize, kNoSlot);
    }
}

void Pool::swap_slots(std::uint32_t a, std::uint32_t b) {
    if (a == b) return;
    check_unlocked();
    if (!tag_) {
        void *scratch = data_ + capacity_ * stride_;
        relocate(scratch, at(a));
        relocate(at(a), at(b));
        relocate(at(b), scratch);
    }
    std::swap(dense_[a], dense_[b]);
    set_sparse(dense_[a].index, a);
    set_sparse(dense_[b].index, b);
    ++version_;
}

void Pool::permute(std::span<std::uint32_t> order) {
    check_unlocked();
    const auto n = static_cast<std::uint32_t>(dense_.size());
    LAM_CHECK(order.size() == n, "Pool::permute: order size mismatch");
    constexpr std::uint32_t kDone = kNoSlot;
    void *scratch = tag_ ? nullptr : data_ + capacity_ * stride_;
    for (std::uint32_t k = 0; k < n; ++k) {
        if (order[k] == kDone || order[k] == k) continue;
        const Entity held = dense_[k];
        if (!tag_) relocate(scratch, at(k));
        std::uint32_t j = k;
        for (;;) {
            const std::uint32_t src = order[j];
            order[j] = kDone;
            if (src == k) {
                if (!tag_) relocate(at(j), scratch);
                dense_[j] = held;
                break;
            }
            if (!tag_) relocate(at(j), at(src));
            dense_[j] = dense_[src];
            j = src;
        }
    }
    for (std::uint32_t k = 0; k < n; ++k)
        set_sparse(dense_[k].index, k);
    ++version_;
}

void Pool::assign(std::span<const Entity> ents, const void *src) {
    check_unlocked();
    for (const Entity &e : dense_)
        set_sparse(e.index, kNoSlot);
    const std::size_t n = ents.size(), old = dense_.size();
    if (!tag_) {
        const auto *s = static_cast<const std::byte *>(src);
        if (trivial_) {
            dense_.clear(); // nothing worth moving on growth
            ensure_data(n);
            if (n) std::memcpy(data_, s, n * stride_);
        } else {
            ensure_data(n); // keeps the old values constructed
            const std::size_t keep = old < n ? old : n;
            for (std::size_t i = 0; i < keep; ++i)
                info_->copy(data_ + i * stride_, s + i * stride_);
            for (std::size_t i = keep; i < n; ++i) {
                info_->construct(data_ + i * stride_);
                info_->copy(data_ + i * stride_, s + i * stride_);
            }
            for (std::size_t i = n; i < old; ++i)
                info_->destroy(data_ + i * stride_);
        }
    }
    dense_.assign(ents.begin(), ents.end());
    for (std::uint32_t k = 0; k < n; ++k)
        set_sparse(dense_[k].index, k);
    ++version_;
}

std::span<const Entity> pool_entities(const World &w, ComponentId id) {
    const Pool *p = w.pool_ptr(id);
    return p ? p->entities() : std::span<const Entity>{};
}

} // namespace detail

// ------------------------------------------------------------------------------------------------
// World

namespace {
std::atomic<std::uint32_t> g_next_world_id{1};
constexpr std::uint32_t kLastGeneration = 0xFFFFFFFEu; // UINT32_MAX is never issued
} // namespace

World::World(WorldConfig config)
    : id_(g_next_world_id.fetch_add(1)), config_(config), seed_(config.seed) {
    register_engine_components();
    generations_.reserve(config.entity_capacity);
    states_.reserve(config.entity_capacity);
    free_.reserve(config.entity_capacity);
    restore_hooks_.reserve(8);
    snapshot_resources_.reserve(16);
    restore_hooks_.push_back(&restore_engine_state);
    commands_ = std::make_unique<Commands>(*this, config.commands_arena_bytes);
}

World::~World() {
    if (commands_) commands_->discard();
    pools_.clear();
}

std::uint32_t World::id() const {
    return id_;
}
StrId World::name() const {
    return config_.name;
}

Entity World::allocate(SlotState state) {
    std::uint32_t index = 0;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
        ++generations_[index];
    } else {
        LAM_CHECK(generations_.size() < 0xFFFFFFFFu, "entity table full");
        index = static_cast<std::uint32_t>(generations_.size());
        generations_.push_back(1);
        states_.push_back(kFree);
    }
    states_[index] = state;
    if (state == kAlive) ++alive_count_;
    return {index, generations_[index]};
}

Entity World::create() {
    return allocate(kAlive);
}
Entity World::reserve_entity() {
    return allocate(kReserved);
}

bool World::commit_reserved(Entity e) {
    if (e.index >= states_.size() || states_[e.index] != kReserved ||
        generations_[e.index] != e.generation)
        return false;
    states_[e.index] = kAlive;
    ++alive_count_;
    return true;
}

void World::release_slot(std::uint32_t index) {
    if (states_[index] == kAlive) --alive_count_;
    if (generations_[index] >= kLastGeneration) {
        states_[index] = kRetired; // retired, never reused
    } else {
        states_[index] = kFree;
        free_.push_back(index);
    }
}

bool World::alive(Entity e) const {
    return e.generation != 0 && e.index < states_.size() && states_[e.index] == kAlive &&
           generations_[e.index] == e.generation;
}

std::size_t World::entity_count() const {
    return alive_count_;
}

void World::destroy(Entity e) {
    LAM_CHECK(!iterating(), "World::destroy while a View iterates; use commands().destroy()");
    if (!alive(e)) return;
    const ComponentId children_id = ComponentRegistry::global().id<Children>();
    for (;;) { // children first (each unlinks itself)
        detail::Pool *cp = pool_ptr(children_id);
        const std::uint32_t d = cp ? cp->find(e) : detail::kNoSlot;
        if (d == detail::kNoSlot) break;
        const Entity child = static_cast<const Children *>(cp->at(d))->first;
        if (!alive(child) || child == e) {
            cp->erase(d); // corrupt list: drop it
            break;
        }
        destroy(child);
    }
    for (std::size_t id = 0; id < pools_.size(); ++id) {
        detail::Pool *p = pools_[id].get();
        if (p && p->contains(e)) fire_remove_and_erase(*p, e);
    }
    for (std::size_t id = 0; id < pools_.size(); ++id) { // anything a hook re-added
        detail::Pool *p = pools_[id].get();
        if (!p) continue;
        const std::uint32_t d = p->find(e);
        if (d != detail::kNoSlot) p->erase(d);
    }
    release_slot(e.index);
}

detail::Pool &World::pool_for(ComponentId id) {
    if (id >= pools_.size()) pools_.resize(std::size_t(id) + 1);
    if (!pools_[id]) {
        ComponentRegistry &reg = ComponentRegistry::global();
        pools_[id] = std::make_unique<detail::Pool>(id, reg.at(id), reg.trivially_copyable(id));
    }
    return *pools_[id];
}

void World::fire_remove_and_erase(detail::Pool &p, Entity e) {
    std::uint32_t d = p.find(e);
    if (d == detail::kNoSlot) return;
    LAM_CHECK(p.locks() == 0, "remove/destroy of a component while a View iterates its pool; "
                              "record it with world.commands() instead");
    if (p.info().on_remove) {
        p.info().on_remove(*this, e, p.at(d));
        d = p.find(e);
        if (d == detail::kNoSlot) return;
    }
    p.erase(d);
}

void *World::get_raw(Entity e, ComponentId id) {
    detail::Pool *p = pool_ptr(id);
    if (!p) return nullptr;
    const std::uint32_t d = p->find(e);
    return d == detail::kNoSlot ? nullptr : p->at(d);
}

void *World::add_raw(Entity e, ComponentId id, bool fire_hooks) {
    LAM_CHECK(alive(e), "World::add_raw: entity is not alive");
    detail::Pool &p = pool_for(id);
    const ComponentInfo &info = p.info();
    LAM_CHECK(p.tag() || info.construct != nullptr,
              "World::add_raw: type is not default-constructible");
    const std::uint32_t d = p.find(e);
    if (d != detail::kNoSlot) {
        if (!p.tag()) {
            info.destroy(p.at(d));
            info.construct(p.at(d));
        }
        if (fire_hooks && info.on_replace) info.on_replace(*this, e, p.at(d));
        return get_raw(e, id);
    }
    void *slot = p.insert(e);
    if (!p.tag()) info.construct(slot);
    if (fire_hooks && info.on_add) info.on_add(*this, e, slot);
    return get_raw(e, id);
}

void *World::add_from(Entity e, ComponentId id, void *src, bool move, bool fire_hooks) {
    LAM_CHECK(alive(e), "World: add to an entity that is not alive");
    detail::Pool &p = pool_for(id);
    const ComponentInfo &info = p.info();
    const std::uint32_t d = p.find(e);
    if (d != detail::kNoSlot) {
        if (!p.tag()) {
            if (p.trivial())
                std::memcpy(p.at(d), src, info.size);
            else {
                LAM_CHECK(info.copy != nullptr, "component is not copy-assignable");
                info.copy(p.at(d), src);
            }
        }
        if (fire_hooks && info.on_replace) info.on_replace(*this, e, p.at(d));
        return get_raw(e, id);
    }
    if (!p.tag() && !p.trivial()) {
        LAM_CHECK(move ? info.move != nullptr : (info.construct != nullptr && info.copy != nullptr),
                  "component cannot be copied into a pool");
    }
    void *slot = p.insert(e);
    if (!p.tag()) {
        if (p.trivial())
            std::memcpy(slot, src, info.size);
        else if (move)
            info.move(slot, src);
        else {
            info.construct(slot);
            info.copy(slot, src);
        }
    }
    if (fire_hooks && info.on_add) info.on_add(*this, e, slot);
    return get_raw(e, id);
}

void World::remove_raw(Entity e, ComponentId id) {
    detail::Pool *p = pool_ptr(id);
    if (p && p->contains(e)) fire_remove_and_erase(*p, e);
}

std::size_t World::components_of(Entity e, std::span<ComponentId> out) const {
    std::size_t n = 0;
    for (std::size_t id = 0; id < pools_.size(); ++id) {
        const detail::Pool *p = pools_[id].get();
        if (!p || !p->contains(e)) continue;
        if (n < out.size()) out[n] = static_cast<ComponentId>(id);
        ++n;
    }
    return n;
}

Resources &World::resources() {
    return resources_;
}
const Resources &World::resources() const {
    return resources_;
}
EventBus &World::events() {
    return events_;
}
Commands &World::commands() {
    return *commands_;
}

void World::add_snapshot_resource(TypeId type, ComponentId info, std::uint32_t flags) {
    [[maybe_unused]] const ComponentInfo &ci = ComponentRegistry::global().at(info);
    LAM_ASSERT(
        (flags & (kSerialize | kHash)) == 0 || ci.read != nullptr,
        "snapshot_resource: kSerialize/kHash need LAM_REFLECT or LAM_REFLECT_CUSTOM on the type");
    LAM_ASSERT((flags & (kSnapshot | kSerialize)) == 0 || ci.copy != nullptr,
               "snapshot_resource: the type must be copy-assignable");
    for (SnapshotResource &r : snapshot_resources_) {
        if (r.type == type) {
            r.flags = flags;
            return;
        }
    }
    snapshot_resources_.push_back({type, info, flags});
}

void World::add_restore_hook(void (*hook)(World &)) {
    restore_hooks_.push_back(hook);
}
bool World::has_restore_hook(void (*hook)(World &)) const {
    return std::find(restore_hooks_.begin(), restore_hooks_.end(), hook) != restore_hooks_.end();
}
void World::set_prefabs(const PrefabLibrary *prefabs) {
    prefabs_ = prefabs;
}
const PrefabLibrary *World::prefabs() const {
    return prefabs_;
}

Rng &World::rng(StrId stream) {
    for (std::uint32_t i = 0; i < rng_count_; ++i)
        if (rngs_[i].id == stream) return rngs_[i].rng;
    LAM_CHECK(rng_count_ < kMaxRngStreams, "World::rng: more than 32 RNG streams");
    RngSlot &s = rngs_[rng_count_++];
    s.id = stream;
    s.rng.reseed(derive_seed(seed_, stream.value));
    return s.rng;
}

void World::reseed(std::uint64_t seed) {
    seed_ = seed;
    for (std::uint32_t i = 0; i < rng_count_; ++i)
        rngs_[i].rng.reseed(derive_seed(seed_, rngs_[i].id.value));
}

std::uint64_t World::seed() const {
    return seed_;
}
std::uint64_t World::tick() const {
    return tick_;
}
void World::advance_tick() {
    ++tick_;
}
bool World::iterating() const {
    return view_depth_ > 0;
}

void World::rebuild_hash_order() const {
    const ComponentRegistry &reg = ComponentRegistry::global();
    if (hash_registry_size_ != reg.size()) {
        hash_registry_size_ = reg.size();
        hash_pools_.clear();
        for (std::size_t id = 0; id < reg.size(); ++id) {
            const ComponentInfo &info = reg.at(static_cast<ComponentId>(id));
            if ((info.flags & kHash) && info.hash)
                hash_pools_.push_back(static_cast<ComponentId>(id));
        }
        std::sort(hash_pools_.begin(), hash_pools_.end(), [&](ComponentId a, ComponentId b) {
            const std::string_view na = reg.at(a).name, nb = reg.at(b).name;
            return na != nb ? na < nb : a < b;
        });
        hash_resource_count_ = static_cast<std::size_t>(-1);
    }
    if (hash_resource_count_ != snapshot_resources_.size()) {
        hash_resource_count_ = snapshot_resources_.size();
        hash_resources_.clear();
        for (std::uint32_t i = 0; i < snapshot_resources_.size(); ++i)
            hash_resources_.push_back(i);
        std::sort(hash_resources_.begin(), hash_resources_.end(),
                  [&](std::uint32_t a, std::uint32_t b) {
                      const std::string_view na = reg.at(snapshot_resources_[a].info).name;
                      const std::string_view nb = reg.at(snapshot_resources_[b].info).name;
                      return na != nb ? na < nb : a < b;
                  });
    }
}

std::uint64_t World::state_hash() const {
    rebuild_hash_order();
    const ComponentRegistry &reg = ComponentRegistry::global();
    Hasher h;
    h.u64(tick_);
    h.u64(rng_count_);
    for (std::uint32_t i = 0; i < rng_count_; ++i) {
        h.u64(rngs_[i].id.value);
        h.u64(rngs_[i].rng.state());
    }
    h.u64(alive_count_);
    for (std::uint32_t i = 0; i < states_.size(); ++i) {
        if (states_[i] != kAlive) continue;
        h.u64(Entity{i, generations_[i]}.bits());
    }
    for (const ComponentId id : hash_pools_) {
        const detail::Pool *p = pool_ptr(id);
        if (!p || p->size() == 0) continue;
        const ComponentInfo &info = reg.at(id);
        h.u64(str_id(info.name).value);
        h.u64(p->size());
        const std::span<const Entity> ents = p->entities();
        for (std::uint32_t d = 0; d < ents.size(); ++d) {
            h.u64(ents[d].bits());
            if (!p->tag()) info.hash(p->at(d), h);
        }
    }
    for (const std::uint32_t i : hash_resources_) {
        const SnapshotResource &r = snapshot_resources_[i];
        if (!(r.flags & kHash)) continue;
        const void *value = resources_.get_raw(r.type);
        const ComponentInfo &info = reg.at(r.info);
        if (!value || !info.hash) continue;
        h.u64(str_id(info.name).value);
        info.hash(value, h);
    }
    return h.value();
}

void World::clear() {
    LAM_CHECK(!iterating(), "World::clear while a View iterates");
    commands_->discard();
    for (std::uint32_t i = 0; i < states_.size(); ++i) {
        if (states_[i] == kAlive) destroy(Entity{i, generations_[i]});
    }
    free_.clear();
    for (std::size_t i = states_.size(); i-- > 0;) {
        if (states_[i] == kReserved) states_[i] = kFree;
        if (states_[i] == kFree) free_.push_back(static_cast<std::uint32_t>(i));
    }
}

void World::sort_slots(detail::Pool &p, std::size_t n,
                       bool (*less)(const void *, std::uint32_t, std::uint32_t), const void *ctx) {
    sort_order_.resize(n);
    sort_scratch_.resize(n);
    for (std::uint32_t i = 0; i < n; ++i)
        sort_order_[i] = i;
    // Bottom-up merge sort: stable, no allocation beyond the reused buffers.
    std::uint32_t *src = sort_order_.data();
    std::uint32_t *dst = sort_scratch_.data();
    for (std::size_t width = 1; width < n; width *= 2) {
        for (std::size_t lo = 0; lo < n; lo += 2 * width) {
            const std::size_t mid = std::min(lo + width, n), hi = std::min(lo + 2 * width, n);
            std::size_t a = lo, b = mid, o = lo;
            while (a < mid && b < hi)
                dst[o++] = less(ctx, src[b], src[a]) ? src[b++] : src[a++];
            while (a < mid)
                dst[o++] = src[a++];
            while (b < hi)
                dst[o++] = src[b++];
        }
        std::swap(src, dst);
    }
    bool identity = true;
    for (std::uint32_t i = 0; i < n && identity; ++i)
        identity = src[i] == i;
    if (identity) return;
    if (src != sort_order_.data()) std::memcpy(sort_order_.data(), src, n * sizeof(std::uint32_t));
    p.permute(std::span<std::uint32_t>(sort_order_.data(), n));
}

void World::align_follow(const detail::Pool &lead, detail::Pool &follow) {
    std::uint32_t pos = 0;
    for (const Entity e : lead.entities()) {
        const std::uint32_t d = follow.find(e);
        if (d == detail::kNoSlot) continue;
        if (d != pos) follow.swap_slots(d, pos);
        ++pos;
    }
}

} // namespace lam
