#pragma once
// The ECS world: sparse-set pools, world resources and events, per-world RNG streams. Iteration
// order depends only on the sequence of structural changes, so it's deterministic. Changing a pool
// while a View iterates it fails LAM_CHECK in every build.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <lam/core/assert.h>
#include <lam/core/events.h>
#include <lam/core/id.h>
#include <lam/core/resources.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/rng.h>
#include <lam/math/vec2.h>
namespace lam {
class Commands;
class PrefabLibrary;
class WorldSnapshot;
template <class... T> class View;
namespace detail {
inline constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;
// One component pool: paged sparse array plus dense entities and data (none for empty types).
class Pool {
public:
    static constexpr std::uint32_t kPageBits = 12;
    static constexpr std::uint32_t kPageSize = 1u << kPageBits;
    Pool(ComponentId id, const ComponentInfo &info, bool trivial);
    ~Pool();
    Pool(const Pool &) = delete;
    Pool &operator=(const Pool &) = delete;
    // Dense slot of e, or kNoSlot (also for a stale generation).
    std::uint32_t find(Entity e) const {
        const std::uint32_t page = e.index >> kPageBits;
        if (page >= pages_.size()) return kNoSlot;
        const std::uint32_t *p = pages_[page].get();
        if (!p) return kNoSlot;
        const std::uint32_t d = p[e.index & (kPageSize - 1)];
        return (d != kNoSlot && dense_[d].generation == e.generation) ? d : kNoSlot;
    }
    bool contains(Entity e) const { return find(e) != kNoSlot; }
    // Component storage of slot d (a shared dummy for empty types).
    void *at(std::uint32_t d) const {
        return tag_ ? tag_dummy() : data_ + std::size_t(d) * stride_;
    }
    std::size_t size() const { return dense_.size(); }
    std::span<const Entity> entities() const { return dense_; }
    ComponentId id() const { return id_; }
    const ComponentInfo &info() const { return *info_; }
    bool tag() const { return tag_; }
    bool trivial() const { return trivial_; }
    std::uint32_t version() const { return version_; }
    std::uint32_t locks() const { return locks_; }
    // Raw lookup state for Views (stable while the pool is locked).
    struct Probe {
        const std::unique_ptr<std::uint32_t[]> *pages = nullptr;
        std::size_t page_count = 0;
        std::byte *data = nullptr;
        std::uint32_t stride = 0;
        // Like find() without the generation check: pools only hold live entities.
        std::uint32_t slot(Entity e) const {
            const std::uint32_t page = e.index >> kPageBits;
            if (page >= page_count) return kNoSlot;
            const std::uint32_t *p = pages[page].get();
            return p ? p[e.index & (kPageSize - 1)] : kNoSlot;
        }
    };
    Probe probe() const { return {pages_.data(), pages_.size(), data_, stride_}; }
    void lock() { ++locks_; }
    void unlock() { --locks_; }
    // Structural operations (LAM_CHECK unlocked). insert returns raw, unconstructed storage.
    void *insert(Entity e);
    void erase(std::uint32_t d); // destroys the component
    void clear();
    void reserve(std::size_t n, std::size_t index_space);
    void swap_slots(std::uint32_t a, std::uint32_t b);
    void permute(std::span<std::uint32_t> order); // new slot k = old slot order[k]; clobbers order
    // Replaces the contents with `ents` and copies of the values at src (stride info().size).
    void assign(std::span<const Entity> ents, const void *src);
    void bump_version() { ++version_; }
    static void *tag_dummy();

private:
    void ensure_data(std::size_t n);
    void set_sparse(std::uint32_t index, std::uint32_t slot);
    void relocate(void *dst, void *src); // move-construct dst, destroy src
    void check_unlocked() const;
    ComponentId id_;
    const ComponentInfo *info_;
    bool tag_ = false;
    bool trivial_ = false;
    std::uint32_t stride_ = 0;
    std::uint32_t align_ = 1;
    std::vector<Entity> dense_;
    std::byte *data_ = nullptr;
    std::size_t capacity_ = 0; // elements (+1 scratch slot allocated)
    std::vector<std::unique_ptr<std::uint32_t[]>> pages_;
    std::uint32_t version_ = 0;
    std::uint32_t locks_ = 0;
};
// Storage for `T &` of empty component types.
template <class T>
T &tag_instance() {
    static T instance{};
    return instance;
}
template <class T, class... A>
void construct_value(void *p, A &&...args) {
    if constexpr (std::is_constructible_v<T, A &&...>)
        ::new (p) T(std::forward<A>(args)...);
    else
        ::new (p) T{std::forward<A>(args)...};
}
template <class T, class... A>
T make_value(A &&...args) {
    if constexpr (std::is_constructible_v<T, A &&...>)
        return T(std::forward<A>(args)...);
    else
        return T{std::forward<A>(args)...};
}
// Engine use. Empty if the pool doesn't exist; valid until the pool changes.
std::span<const Entity> pool_entities(const World &w, ComponentId id);
} // namespace detail
template <class... W> struct With {};                           // required tags/components, not yielded
template <class... X> struct Exclude {};                        // entities with any X are skipped
// Entities with every T. While iterating, the pools are locked: use world.commands() for
// structural changes. Tags yield a shared dummy reference; With<Tag> filters without one.
//   for (auto [e, t] : w.view<Transform>()) ...
template <class... T> class View {                              // drives from the smallest pool, dense order
public:
    static constexpr std::size_t kMaxFilters = 8;               // With<> and Exclude<> types
    class Iterator;                                             // yields std::tuple<Entity, T&...>
    // Locks the pools and returns the first match.
    Iterator begin();
    Iterator end();
    // Fastest way to iterate.
    template <class F> void each(F &&f);                        // f(Entity, T&...)
    std::size_t size_hint() const;                              // upper bound on matches
    ~View();
    View(const View &) = delete;
    View &operator=(const View &) = delete;
    View(View &&o) noexcept;                                    // takes over the lock
    View &operator=(View &&) = delete;
private:
    friend class World;
    static constexpr std::size_t N = sizeof...(T);
    View(World &w, const std::array<detail::Pool *, N> &pools, std::span<detail::Pool *const> with,
         std::span<detail::Pool *const> without);
    void lock();
    void unlock();
    // True if e (at driver slot i) passes; fills the dense slots of every T.
    bool match(Entity e, std::uint32_t i, std::array<std::uint32_t, N> &slots) const;
    template <std::size_t... K>
    std::tuple<Entity, T &...> make_tuple(Entity e, const std::array<std::uint32_t, N> &slots,
                                          std::index_sequence<K...>) const;
    template <class F, std::size_t... K> void each_impl(F &f, std::index_sequence<K...>);
    World *world_;
    std::array<detail::Pool *, N> pools_{};
    std::array<detail::Pool *, kMaxFilters> with_{};
    std::array<detail::Pool *, kMaxFilters> without_{};
    std::uint8_t with_count_ = 0;
    std::uint8_t without_count_ = 0;
    detail::Pool *driver_ = nullptr;                            // null: empty view
    std::array<detail::Pool::Probe, N> probes_{};              // filled by lock()
    std::array<bool, N> is_driver_{};
    bool locked_ = false;
};
// Forward iterator over a View's matches; valid while its View is alive.
template <class... T> class View<T...>::Iterator {
public:
    using value_type = std::tuple<Entity, T &...>;
    using difference_type = std::ptrdiff_t;
    Iterator() = default;
    value_type operator*() const {
        return view_->make_tuple(entity_, slots_, std::index_sequence_for<T...>{});
    }
    Iterator &operator++() {
        ++i_;
        settle();
        return *this;
    }
    bool operator==(const Iterator &o) const { return i_ == o.i_; }
private:
    friend class View;
    Iterator(View *v, std::uint32_t i, std::uint32_t end) : view_(v), i_(i), end_(end) { settle(); }
    void settle();
    View *view_ = nullptr;
    std::uint32_t i_ = 0;
    std::uint32_t end_ = 0;
    Entity entity_;
    std::array<std::uint32_t, sizeof...(T)> slots_{};
};
// App::create_world fills this from AppConfig.
struct WorldConfig {
    std::size_t entity_capacity = 16384;                        // entity table pre-size (no-alloc below it)
    std::uint64_t seed = 1;                                     // base of every RNG stream
    std::size_t commands_arena_bytes = 256 * 1024;              // Commands pre-reserved bytes
    StrId name;
};
// What World::restore puts back besides entities, components and resources.
struct RestoreOptions {
    bool tick = true;                                           // false: keep the current tick (undo)
    bool rng = true;                                            // false: keep the current RNG streams and seed
};
struct WorldRestored { std::uint64_t tick = 0; };               // World event after restore/deserialize
// Main thread only. Component hooks fire on add, remove and destroy, never on restore or when
// the World itself is destroyed. Not movable.
class World {
public:
    static constexpr std::uint32_t kMaxRngStreams = 32;
    explicit World(WorldConfig config = {});
    ~World();
    World(const World &) = delete;
    World &operator=(const World &) = delete;
    std::uint32_t id() const;                                   // unique per process, never reused, != 0
    StrId name() const;
    // Entities
    Entity create();                                            // O(1); reuses freed index with gen+1
    void destroy(Entity e);                                     // immediate; cascades to Children; LAM_CHECK(!iterating())
    bool alive(Entity e) const;                                 // false for null and stale handles
    std::size_t entity_count() const;                           // pending spawns not included
    // Components. References stay valid until pool T changes structurally.
    // Assigns if e already has T (on_replace), else constructs it (on_add).
    template <class T, class... A> T &add(Entity e, A &&...args);
    template <class T> void remove(Entity e);                   // on_remove; no-op if absent
    template <class T> bool has(Entity e) const;
    template <class T> T &get(Entity e);                        // LAM_ASSERT has
    template <class T> const T &get(Entity e) const;
    template <class T> T *try_get(Entity e);                    // null if absent, dead or stale
    template <class T> const T *try_get(Entity e) const;
    template <class... T> View<T...> view();
    template <class... T, class... X> View<T...> view(Exclude<X...>);
    template <class... T, class... W, class... X> View<T...> view(With<W...>, Exclude<X...> = {});
    template <class T> Entity single();                         // LAM_ASSERT exactly one entity has T
    // Null if none; asserts there's at most one.
    template <class T> T *single_or_null(Entity *out = nullptr);
    template <class T> std::size_t count() const;
    template <class T> void reserve(std::size_t n);             // pre-size (no-alloc guarantee)
    template <class T> std::uint32_t pool_version() const;      // ++ on every structural change of pool T
    // Moves the entities each Follow pool shares with Lead to the front, in Lead's order.
    template <class Lead, class... Follow> void align();
    // Stable. less(const T&, const T&), or less(Entity, Entity) for tags.
    template <class T, class Less> void sort(Less less);
    void teleport(Entity e, Vec2 position);                     // Transform + PrevTransform; subtree JustSpawned
    // Type-erased access (serialisation, prefabs, inspector)
    void *get_raw(Entity e, ComponentId id);                    // null if absent; empty types: a dummy
    void *add_raw(Entity e, ComponentId id, bool fire_hooks = true);   // default-constructed
    void remove_raw(Entity e, ComponentId id);                  // on_remove; no-op if absent
    // Fills `out` with e's component ids in ascending order; returns the total count.
    std::size_t components_of(Entity e, std::span<ComponentId> out) const;
    // Resources, events, commands, prefabs
    Resources &resources();
    const Resources &resources() const;
    EventBus &events();
    Commands &commands();                                       // applied by the App after each stage
    // Includes resource T in snapshots, saves and the hash. kSerialize and kHash need reflection.
    template <class T> void snapshot_resource(std::uint32_t flags = kSnapshot | kHash | kSerialize);
    // Run after restore/deserialize to rebuild derived state. Adding a hook twice runs it twice,
    // so add them in Plugin::init_world or check has_restore_hook first.
    void add_restore_hook(void (*hook)(World &));
    bool has_restore_hook(void (*hook)(World &)) const;
    void set_prefabs(const PrefabLibrary *prefabs);             // set by App for every world
    const PrefabLibrary *prefabs() const;                       // null until set
    // Determinism
    Rng &rng(StrId stream);                                     // created on first use, at most kMaxRngStreams
    void reseed(std::uint64_t seed);                            // every stream re-derived
    std::uint64_t seed() const;
    std::uint64_t tick() const;                                 // ticks this world has run
    void advance_tick();                                        // engine
    // Copies the entity table, RNG state and every kSnapshot/kSerialize pool and resource.
    void snapshot_into(WorldSnapshot &out) const;               // reuses buffers: no alloc after warm-up
    // Puts the snapshot back exactly and discards pending commands. Component hooks don't fire;
    // restore hooks do. Validate hand-built snapshots first.
    void restore(const WorldSnapshot &s, RestoreOptions o = {});
    // Hash of the tick, RNG states, live entities and kHash pools and resources. Frame stages must
    // not create or destroy entities in a ticking world, or replays will diverge.
    std::uint64_t state_hash() const;
    // Destroys every entity (hooks fire) and resets the free list, so indices restart from 0.
    void clear();                                               // keeps resources
    bool iterating() const;                                     // a View holds the pools locked
private:
    template <class...> friend class View;
    friend class Commands;
    friend class PrefabLibrary;
    friend class WorldSnapshot;
    friend std::span<const Entity> detail::pool_entities(const World &w, ComponentId id);
    enum SlotState : std::uint8_t { kFree = 0, kAlive = 1, kRetired = 2, kReserved = 3 };
    struct RngSlot {
        StrId id;
        Rng rng{0};
    };
    struct SnapshotResource {
        TypeId type = 0;
        ComponentId info = 0;
        std::uint32_t flags = 0;
    };
    detail::Pool *pool_ptr(ComponentId id) const {
        return id < pools_.size() ? pools_[id].get() : nullptr;
    }
    detail::Pool &pool_for(ComponentId id);
    template <class T> detail::Pool *pool_of() const {
        return pool_ptr(ComponentRegistry::global().id<T>());
    }
    Entity allocate(SlotState state);
    Entity reserve_entity();                                    // Commands::spawn
    bool commit_reserved(Entity e);
    void release_slot(std::uint32_t index);
    // Inserts or assigns component `id` from a constructed value at src (moved from when
    // `move`), firing hooks if asked. Returns the stored component.
    void *add_from(Entity e, ComponentId id, void *src, bool move, bool fire_hooks = true);
    void fire_remove_and_erase(detail::Pool &p, Entity e);
    void add_snapshot_resource(TypeId type, ComponentId info, std::uint32_t flags);
    void lock_view() { ++view_depth_; }
    void unlock_view() { --view_depth_; }
    using SlotLess = bool (*)(const void *ctx, std::uint32_t a, std::uint32_t b);
    void sort_slots(detail::Pool &p, std::size_t n, SlotLess less, const void *ctx);
    void align_follow(const detail::Pool &lead, detail::Pool &follow);
    void rebuild_hash_order() const;

    std::uint32_t id_ = 0;
    WorldConfig config_;
    std::uint64_t seed_ = 1;
    std::uint64_t tick_ = 0;
    std::vector<std::uint32_t> generations_;
    std::vector<std::uint8_t> states_;
    std::vector<std::uint32_t> free_;                           // back() is reused first
    std::size_t alive_count_ = 0;
    std::vector<std::unique_ptr<detail::Pool>> pools_;          // indexed by ComponentId
    std::uint32_t view_depth_ = 0;
    std::array<RngSlot, kMaxRngStreams> rngs_{};
    std::uint32_t rng_count_ = 0;
    std::vector<SnapshotResource> snapshot_resources_;
    std::vector<void (*)(World &)> restore_hooks_;
    const PrefabLibrary *prefabs_ = nullptr;
    std::vector<std::uint32_t> sort_order_;
    std::vector<std::uint32_t> sort_scratch_;
    mutable std::vector<ComponentId> hash_pools_;               // kHash pool ids in name order
    mutable std::vector<std::uint32_t> hash_resources_;         // snapshot_resources_ by name
    mutable std::size_t hash_registry_size_ = 0;
    mutable std::size_t hash_pool_count_ = 0;
    mutable std::size_t hash_resource_count_ = 0;
    Resources resources_;
    EventBus events_;
    std::unique_ptr<Commands> commands_;
};

// ------------------------------------------------------------------------------------------------
// World templates

template <class T, class... A>
T &World::add(Entity e, A &&...args) {
    LAM_CHECK(alive(e), "World::add: entity is not alive");
    const ComponentId cid = ComponentRegistry::global().id<T>();
    detail::Pool &p = pool_for(cid);
    const std::uint32_t d = p.find(e);
    if (d != detail::kNoSlot) {
        if constexpr (std::is_empty_v<T>) {
            if (p.info().on_replace) p.info().on_replace(*this, e, &detail::tag_instance<T>());
            return detail::tag_instance<T>();
        } else {
            T *cur = static_cast<T *>(p.at(d));
            *cur = detail::make_value<T>(std::forward<A>(args)...);
            if (!p.info().on_replace) return *cur;
            p.info().on_replace(*this, e, cur);
        }
    } else {
        void *slot = p.insert(e);
        if constexpr (std::is_empty_v<T>) {
            if (p.info().on_add) p.info().on_add(*this, e, &detail::tag_instance<T>());
            return detail::tag_instance<T>();
        } else {
            detail::construct_value<T>(slot, std::forward<A>(args)...);
            if (!p.info().on_add) return *static_cast<T *>(slot);
            p.info().on_add(*this, e, slot);
        }
    }
    T *now = try_get<T>(e); // hooks may have moved the pool
    LAM_CHECK(now != nullptr, "World::add: a hook removed the component it was called for");
    return *now;
}
template <class T>
void World::remove(Entity e) {
    detail::Pool *p = pool_of<T>();
    if (p && p->contains(e)) fire_remove_and_erase(*p, e);
}
template <class T>
bool World::has(Entity e) const {
    const detail::Pool *p = pool_of<T>();
    return p && p->contains(e);
}
template <class T>
T *World::try_get(Entity e) {
    detail::Pool *p = pool_of<T>();
    if (!p) return nullptr;
    const std::uint32_t d = p->find(e);
    if (d == detail::kNoSlot) return nullptr;
    if constexpr (std::is_empty_v<T>)
        return &detail::tag_instance<T>();
    else
        return static_cast<T *>(p->at(d));
}
template <class T>
const T *World::try_get(Entity e) const {
    return const_cast<World *>(this)->try_get<T>(e);
}
template <class T>
T &World::get(Entity e) {
    T *c = try_get<T>(e);
    LAM_CHECK(c != nullptr, "World::get: entity does not have the component");
    return *c;
}
template <class T>
const T &World::get(Entity e) const {
    return const_cast<World *>(this)->get<T>(e);
}
template <class T>
Entity World::single() {
    const detail::Pool *p = pool_of<T>();
    LAM_ASSERT(p && p->size() == 1,
               "World::single: expected exactly one entity with the component");
    LAM_CHECK(p && p->size() >= 1, "World::single: no entity has the component");
    return p->entities()[0];
}
template <class T>
T *World::single_or_null(Entity *out) {
    detail::Pool *p = pool_of<T>();
    if (!p || p->size() == 0) {
        if (out) *out = {};
        return nullptr;
    }
    LAM_ASSERT(p->size() == 1, "World::single_or_null: more than one entity has the component");
    const Entity e = p->entities()[0];
    if (out) *out = e;
    return try_get<T>(e);
}
template <class T>
std::size_t World::count() const {
    const detail::Pool *p = pool_of<T>();
    return p ? p->size() : 0;
}
template <class T>
void World::reserve(std::size_t n) {
    detail::Pool &p = pool_for(ComponentRegistry::global().id<T>());
    p.reserve(n, n > generations_.size() ? n : generations_.size());
}
template <class T>
std::uint32_t World::pool_version() const {
    const detail::Pool *p = pool_of<T>();
    return p ? p->version() : 0;
}
template <class Lead, class... Follow>
void World::align() {
    const detail::Pool *lead = pool_of<Lead>();
    if (!lead) return;
    (
        [&] {
            if (detail::Pool *f = pool_of<Follow>()) align_follow(*lead, *f);
        }(),
        ...);
}
template <class T, class Less>
void World::sort(Less less) {
    detail::Pool *p = pool_of<T>();
    if (!p || p->size() < 2) return;
    struct Ctx {
        detail::Pool *pool;
        Less *less;
    } ctx{p, &less};
    auto cmp = [](const void *c, std::uint32_t a, std::uint32_t b) -> bool {
        const Ctx &x = *static_cast<const Ctx *>(c);
        if constexpr (std::is_invocable_r_v<bool, Less &, const T &, const T &> &&
                      !std::is_empty_v<T>) {
            return (*x.less)(*static_cast<const T *>(x.pool->at(a)),
                             *static_cast<const T *>(x.pool->at(b)));
        } else {
            static_assert(std::is_invocable_r_v<bool, Less &, Entity, Entity>,
                          "World::sort: less must take (const T&, const T&) or (Entity, Entity)");
            return (*x.less)(x.pool->entities()[a], x.pool->entities()[b]);
        }
    };
    sort_slots(*p, p->size(), cmp, &ctx);
}
template <class T>
void World::snapshot_resource(std::uint32_t flags) {
    add_snapshot_resource(type_id<T>(), ComponentRegistry::global().id<T>(), flags);
}
template <class... T>
View<T...> World::view() {
    return view<T...>(With<>{}, Exclude<>{});
}
template <class... T, class... X>
View<T...> World::view(Exclude<X...> x) {
    return view<T...>(With<>{}, x);
}
template <class... T, class... W, class... X>
View<T...> World::view(With<W...>, Exclude<X...>) {
    static_assert(sizeof...(T) > 0, "View needs at least one component type");
    static_assert(sizeof...(W) <= View<T...>::kMaxFilters &&
                      sizeof...(X) <= View<T...>::kMaxFilters,
                  "too many With/Exclude types");
    const std::array<detail::Pool *, sizeof...(W)> with{pool_of<W>()...};
    const std::array<detail::Pool *, sizeof...(X)> without{pool_of<X>()...};
    return View<T...>(*this, {pool_of<T>()...}, with, without);
}

// ------------------------------------------------------------------------------------------------
// View templates

template <class... T>
View<T...>::View(World &w, const std::array<detail::Pool *, N> &pools,
                 std::span<detail::Pool *const> with, std::span<detail::Pool *const> without)
    : world_(&w), pools_(pools) {
    with_count_ = static_cast<std::uint8_t>(with.size());
    without_count_ = static_cast<std::uint8_t>(without.size());
    for (std::size_t i = 0; i < with.size(); ++i)
        with_[i] = with[i];
    for (std::size_t i = 0; i < without.size(); ++i)
        without_[i] = without[i];
    for (detail::Pool *p : pools_) {
        if (!p) {
            driver_ = nullptr;
            return;
        }
        if (!driver_ || p->size() < driver_->size()) driver_ = p;
    }
    for (std::size_t i = 0; i < with_count_; ++i) {
        detail::Pool *p = with_[i];
        if (!p) {
            driver_ = nullptr;
            return;
        }
        if (p->size() < driver_->size()) driver_ = p;
    }
}
template <class... T>
View<T...>::View(View &&o) noexcept
    : world_(o.world_), pools_(o.pools_), with_(o.with_), without_(o.without_),
      with_count_(o.with_count_), without_count_(o.without_count_), driver_(o.driver_),
      probes_(o.probes_), is_driver_(o.is_driver_), locked_(o.locked_) {
    o.locked_ = false;
}
template <class... T>
View<T...>::~View() {
    unlock();
}
template <class... T>
void View<T...>::lock() {
    if (locked_) return;
    locked_ = true;
    for (std::size_t k = 0; k < N; ++k) {
        if (!pools_[k]) continue;
        pools_[k]->lock();
        probes_[k] = pools_[k]->probe();
        is_driver_[k] = pools_[k] == driver_;
    }
    for (std::size_t i = 0; i < with_count_; ++i)
        if (with_[i]) with_[i]->lock();
    for (std::size_t i = 0; i < without_count_; ++i)
        if (without_[i]) without_[i]->lock();
    world_->lock_view();
}
template <class... T>
void View<T...>::unlock() {
    if (!locked_) return;
    locked_ = false;
    for (detail::Pool *p : pools_)
        if (p) p->unlock();
    for (std::size_t i = 0; i < with_count_; ++i)
        if (with_[i]) with_[i]->unlock();
    for (std::size_t i = 0; i < without_count_; ++i)
        if (without_[i]) without_[i]->unlock();
    world_->unlock_view();
}
template <class... T>
bool View<T...>::match(Entity e, std::uint32_t i, std::array<std::uint32_t, N> &slots) const {
    for (std::size_t k = 0; k < N; ++k) {
        const std::uint32_t d = is_driver_[k] ? i : probes_[k].slot(e);
        if (d == detail::kNoSlot) return false;
        slots[k] = d;
    }
    for (std::size_t k = 0; k < with_count_; ++k)
        if (with_[k] != driver_ && !with_[k]->contains(e)) return false;
    for (std::size_t k = 0; k < without_count_; ++k)
        if (without_[k] && without_[k]->contains(e)) return false;
    return true;
}
template <class... T>
template <std::size_t... K>
std::tuple<Entity, T &...> View<T...>::make_tuple(Entity e,
                                                  const std::array<std::uint32_t, N> &slots,
                                                  std::index_sequence<K...>) const {
    return std::tuple<Entity, T &...>(e, [&]() -> T & {
        if constexpr (std::is_empty_v<T>)
            return detail::tag_instance<T>();
        else
            return *static_cast<T *>(pools_[K]->at(slots[K]));
    }()...);
}
template <class... T>
typename View<T...>::Iterator View<T...>::begin() {
    if (!driver_) return Iterator(this, 0, 0);
    lock();
    const auto n = static_cast<std::uint32_t>(driver_->size());
    return Iterator(this, 0, n);
}
template <class... T>
typename View<T...>::Iterator View<T...>::end() {
    const auto n = driver_ ? static_cast<std::uint32_t>(driver_->size()) : 0u;
    return Iterator(this, n, n);
}
template <class... T>
void View<T...>::Iterator::settle() {
    while (i_ < end_) {
        entity_ = view_->driver_->entities()[i_];
        if (view_->match(entity_, i_, slots_)) return;
        ++i_;
    }
}
template <class... T>
std::size_t View<T...>::size_hint() const {
    return driver_ ? driver_->size() : 0;
}
template <class... T>
template <class F, std::size_t... K>
void View<T...>::each_impl(F &f, std::index_sequence<K...>) {
    // Locals, so the compiler need not reload view state that `f` could alias.
    const auto n = static_cast<std::uint32_t>(driver_->size());
    const Entity *ents = driver_->entities().data();
    const std::array<detail::Pool::Probe, N> probes = probes_;
    const std::array<bool, N> is_driver = is_driver_;
    const bool filtered = with_count_ != 0 || without_count_ != 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const Entity e = ents[i];
        std::array<std::uint32_t, N> slots{};
        bool ok = true;
        for (std::size_t k = 0; k < N && ok; ++k) {
            slots[k] = is_driver[k] ? i : probes[k].slot(e);
            ok = slots[k] != detail::kNoSlot;
        }
        if (!ok) continue;
        if (filtered && !match(e, i, slots)) continue;
        f(e, [&]() -> T & {
            if constexpr (std::is_empty_v<T>)
                return detail::tag_instance<T>();
            else
                return *reinterpret_cast<T *>(probes[K].data +
                                              std::size_t(slots[K]) * probes[K].stride);
        }()...);
    }
}
template <class... T>
template <class F>
void View<T...>::each(F &&f) {
    if (!driver_) return;
    const bool was_locked = locked_;
    lock();
    each_impl(f, std::index_sequence_for<T...>{});
    if (!was_locked) unlock();
}
} // namespace lam
