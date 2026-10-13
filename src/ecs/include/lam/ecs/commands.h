#pragma once
// Deferred structural changes, applied by the App after every stage run in record order.
// Records sit in arena chunks that never move and only allocate once the reserve is used up.
// Records for stale entities are dropped.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>
#include <lam/core/id.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/vec2.h>
namespace lam {
class World;
class PrefabLibrary;
// One per World. Safe to record into while iterating Views.
class Commands {
public:
    // Reserves `arena_bytes` up front.
    Commands(World &world, std::size_t arena_bytes);
    // Pending commands are discarded, not applied.
    ~Commands();
    Commands(const Commands &) = delete;
    Commands &operator=(const Commands &) = delete;
    // The index is reserved now; the entity and its components appear at apply().
    Entity spawn();
    template <class T> void add(Entity e, T value);             // hooks fire at apply()
    template <class T> void remove(Entity e);
    void destroy(Entity e);                                     // cascades to descendants
    void teleport(Entity e, Vec2 position);
    // Records the prefab's components now, so a later add<T>() for the returned entity replaces
    // the prefab's whole T.
    Entity spawn_prefab(StrId prefab, Vec2 position);
    // Engine use. Runs every record in order, including ones added while applying.
    void apply();
    std::size_t pending() const;                                // records not yet applied
    std::size_t bytes_used() const;                             // arena bytes held by pending records
    World &world();                                             // the owning World
private:
    friend class World;
    friend class PrefabLibrary;
    enum class Op : std::uint8_t { Spawn, Add, Remove, Destroy, Teleport };
    struct Header {
        std::uint32_t size = 0;                                 // header + payload, 16-byte multiple
        Op op = Op::Spawn;
        std::uint8_t has_payload = 0;
        ComponentId component = 0;
        Entity entity;
        Vec2 position;
    };
    struct Chunk {
        std::byte *memory = nullptr;
        std::size_t capacity = 0;
        std::size_t used = 0;
    };
    static constexpr std::size_t kAlign = 16;
    static constexpr std::size_t kHeaderBytes = (sizeof(Header) + kAlign - 1) & ~(kAlign - 1);
    // Appends a record; returns its payload storage (payload_bytes, 16-byte aligned) or null.
    void *push(Op op, Entity e, ComponentId component, std::size_t payload_bytes);
    // Records an add of component `id` and returns raw storage for the value (constructed by the
    // caller). Used by PrefabLibrary::spawn_into.
    void *push_add(Entity e, ComponentId id);
    void add_chunk(std::size_t min_bytes);
    // Destroys every pending payload and forgets the records (World::restore and World::clear).
    void discard();
    template <class F> void for_each_record(F &&f);
    World &world_;
    std::vector<Chunk> chunks_;
    std::size_t current_ = 0;                                   // chunk receiving records
    std::size_t pending_ = 0;
    bool applying_ = false;
    bool warned_ = false;
};
template <class T> void Commands::add(Entity e, T value) {
    const ComponentId cid = ComponentRegistry::global().id<T>();
    static_assert(alignof(T) <= kAlign, "Commands: component alignment above 16 bytes");
    if constexpr (std::is_empty_v<T>) {
        (void)value;
        push(Op::Add, e, cid, 0);
    } else {
        void *p = push(Op::Add, e, cid, sizeof(T));
        ::new (p) T(std::move(value));
    }
}
template <class T> void Commands::remove(Entity e) { push(Op::Remove, e, ComponentRegistry::global().id<T>(), 0); }
}
