#pragma once
// Snapshots for undo, replay checks and saves. A snapshot keeps the exact ECS layout, so restore
// reproduces iteration order and future entity indices bit for bit.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <lam/core/result.h>
#include <lam/ecs/world.h>
namespace lam {
// State of one entity-table slot in a snapshot.
enum class EntitySlotState : std::uint8_t { Free = 0, Alive = 1, Retired = 2 };
// One RNG stream: its id and PCG32 state.
struct RngStreamState {
    StrId stream;
    std::uint64_t state = 0;
};
// A copy of a World's simulation state (World::snapshot_into). Move-only.
class WorldSnapshot {
public:
    WorldSnapshot();                                            // empty; allocates nothing
    ~WorldSnapshot();
    WorldSnapshot(WorldSnapshot &&) noexcept;
    WorldSnapshot &operator=(WorldSnapshot &&) noexcept;
    std::size_t bytes() const;                                  // payload bytes held (sizes, not capacities)
    std::uint64_t tick() const;                                 // world tick when taken
    bool empty() const;                                         // never filled
    // ---- Engine access: reading (lam_scene serialize_world, tools) -----------------------------
    std::uint64_t seed() const;
    std::span<const std::uint32_t> generations() const;         // per entity index
    std::span<const EntitySlotState> slot_states() const;       // per entity index
    std::span<const std::uint32_t> free_list() const;           // back() is reused first
    std::span<const RngStreamState> rng_streams() const;        // creation order
    std::size_t pool_count() const;                             // pools, ascending ComponentId
    ComponentId pool_component(std::size_t i) const;
    std::span<const Entity> pool_entities(std::size_t i) const; // dense order
    // pool_entities(i).size() constructed values with stride ComponentInfo::size; null for tags.
    const void *pool_data(std::size_t i) const;
    std::size_t resource_count() const;
    ComponentId resource_component(std::size_t i) const;        // registry entry of the resource type
    std::uint32_t resource_flags(std::size_t i) const;          // flags given to snapshot_resource
    const void *resource_data(std::size_t i) const;             // a constructed value
    // ---- Engine access: building (lam_scene deserialize_world) ----------------------------------
    // Empties the snapshot (keeps buffers) and sets its tick and seed.
    void reset(std::uint64_t tick, std::uint64_t seed);
    void set_entities(std::span<const std::uint32_t> generations, std::span<const EntitySlotState> states,
                      std::span<const std::uint32_t> free_list);
    void set_rng_streams(std::span<const RngStreamState> streams);   // <= World::kMaxRngStreams
    // Pools must be added in ascending id order. Returns default-constructed values to fill.
    void *add_pool(ComponentId id, std::span<const Entity> entities);
    // Returns a default-constructed value to fill.
    void *add_resource(ComponentId id, std::uint32_t flags);
    // Consistency check for a hand-built snapshot. Run before World::restore.
    Status validate() const;
private:
    friend class World;
    struct Data;
    Data &data();
    std::unique_ptr<Data> data_;
};
// Undo/redo for puzzle and editor-style games; no allocation after warm-up. Undo rewinds the whole
// world, presentation included, so handles taken after a push may point at a different entity.
class History {
public:
    // Limits cover undo and redo together. The newest undo entry is always kept.
    explicit History(std::size_t max_bytes = 64u << 20, std::size_t max_entries = 256);
    ~History();
    History(const History &) = delete;
    History &operator=(const History &) = delete;
    void push(const World &w);                                  // clears redo; evicts oldest past limits
    // False if there's nothing to undo or redo.
    bool undo(World &w, RestoreOptions o = {.tick = false});
    bool redo(World &w, RestoreOptions o = {.tick = false});
    bool can_undo() const;
    bool can_redo() const;
    void clear();                                               // drops every entry and frees buffers
    std::size_t size() const;                                   // stored snapshots (undo + redo)
    std::size_t bytes() const;                                  // sum of stored WorldSnapshot::bytes()
private:
    void evict();
    WorldSnapshot take_spare();
    void give_spare(WorldSnapshot &&s);
    std::size_t max_bytes_;
    std::size_t max_entries_;
    std::vector<WorldSnapshot> undo_;                           // oldest first
    std::vector<WorldSnapshot> redo_;                           // most recently undone last
    std::vector<WorldSnapshot> spare_;                          // reusable buffers
};
}
