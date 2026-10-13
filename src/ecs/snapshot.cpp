// Snapshots, restore and undo history. A slot that held the same component keeps its storage,
// and for non-trivial types its constructed values, so copy-assignment can reuse their capacity.
#include <lam/ecs/snapshot.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <string>

#include <lam/core/assert.h>
#include <lam/ecs/commands.h>

namespace lam {

namespace {

// Raw aligned storage that survives reuse (contents are managed by the owner).
struct Buffer {
    std::byte *ptr = nullptr;
    std::size_t bytes = 0;
    std::size_t align = 16;
    Buffer() = default;
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    Buffer(Buffer &&o) noexcept : ptr(o.ptr), bytes(o.bytes), align(o.align) {
        o.ptr = nullptr, o.bytes = 0;
    }
    Buffer &operator=(Buffer &&o) noexcept {
        if (this != &o) {
            release();
            ptr = o.ptr, bytes = o.bytes, align = o.align;
            o.ptr = nullptr, o.bytes = 0;
        }
        return *this;
    }
    ~Buffer() { release(); }
    void release() {
        if (ptr) ::operator delete(ptr, std::align_val_t(align));
        ptr = nullptr;
        bytes = 0;
    }
    // True if the existing storage fits (contents kept); otherwise reallocates (contents lost).
    bool fits(std::size_t n, std::size_t a) const { return n <= bytes && a <= align; }
    void reallocate(std::size_t n, std::size_t a) {
        release();
        align = a < 16 ? 16 : a;
        bytes = n < 16 ? 16 : n;
        ptr = static_cast<std::byte *>(::operator new(bytes, std::align_val_t(align)));
    }
};

// Values of one component type (or one resource) held by a snapshot.
struct Values {
    const ComponentInfo *info = nullptr;
    bool trivial = true;
    bool tag = false;
    Buffer data;
    std::size_t constructed = 0; // live non-trivial values in data

    void destroy_values() {
        if (info && !trivial)
            for (std::size_t i = 0; i < constructed; ++i)
                info->destroy(data.ptr + i * info->size);
        constructed = 0;
    }
    // Prepares storage for n values of `ci` (keeps constructed values of the same type).
    void prepare(const ComponentInfo &ci, bool is_trivial, std::size_t n) {
        if (info != &ci) {
            destroy_values();
            info = &ci;
            trivial = is_trivial;
            tag = ci.size == 0;
        }
        if (tag) return;
        const std::size_t need = n * ci.size;
        if (!data.fits(need, ci.align)) {
            destroy_values();
            data.reallocate(need + need / 2, ci.align); // headroom: growth stays rare
        }
    }
    void *at(std::size_t i) const { return tag ? nullptr : data.ptr + i * info->size; }
    // Makes values [0, n) copies of src (stride info->size).
    void copy_from(const std::byte *src, std::size_t n) {
        if (tag || n == 0) {
            if (!tag) destroy_values();
            return;
        }
        if (trivial) {
            std::memcpy(data.ptr, src, n * info->size);
            return;
        }
        LAM_CHECK(info->copy != nullptr && info->construct != nullptr,
                  "snapshot: component is not copyable");
        const std::size_t keep = std::min(constructed, n);
        for (std::size_t i = 0; i < keep; ++i)
            info->copy(at(i), src + i * info->size);
        for (std::size_t i = keep; i < n; ++i) {
            info->construct(at(i));
            info->copy(at(i), src + i * info->size);
        }
        for (std::size_t i = n; i < constructed; ++i)
            info->destroy(at(i));
        constructed = n;
    }
    // Makes values [0, n) default-constructed (builder).
    void construct_defaults(std::size_t n) {
        if (tag) return;
        destroy_values();
        LAM_CHECK(info->construct != nullptr, "snapshot: component is not default-constructible");
        for (std::size_t i = 0; i < n; ++i)
            info->construct(at(i));
        if (!trivial) constructed = n;
    }
    Values() = default;
    Values(Values &&o) noexcept
        : info(o.info), trivial(o.trivial), tag(o.tag), data(std::move(o.data)),
          constructed(o.constructed) {
        o.constructed = 0;
    }
    Values &operator=(Values &&) = delete;
    Values(const Values &) = delete;
    ~Values() { destroy_values(); }
};

struct PoolCopy {
    ComponentId id = 0;
    std::vector<Entity> entities;
    Values values;
};

struct ResourceCopy {
    TypeId type = 0;
    ComponentId info = 0;
    std::uint32_t flags = 0;
    Values value;
};

constexpr std::uint32_t kLastGeneration = 0xFFFFFFFEu;
constexpr std::uint32_t kStoredFlags = kSnapshot | kSerialize;

} // namespace

struct WorldSnapshot::Data {
    bool empty = true;
    std::uint64_t tick = 0;
    std::uint64_t seed = 0;
    std::vector<std::uint32_t> generations;
    std::vector<EntitySlotState> states;
    std::vector<std::uint32_t> free_list;
    std::vector<RngStreamState> rng;
    std::vector<PoolCopy> pools; // [0, pool_count) in use
    std::size_t pool_count = 0;
    std::vector<ResourceCopy> resources; // [0, resource_count) in use
    std::size_t resource_count = 0;

    PoolCopy &next_pool() {
        if (pool_count == pools.size()) pools.emplace_back();
        return pools[pool_count++];
    }
    ResourceCopy &next_resource() {
        if (resource_count == resources.size()) resources.emplace_back();
        return resources[resource_count++];
    }
};

// ------------------------------------------------------------------------------------------------
// WorldSnapshot

WorldSnapshot::WorldSnapshot() = default;
WorldSnapshot::~WorldSnapshot() = default;
WorldSnapshot::WorldSnapshot(WorldSnapshot &&) noexcept = default;
WorldSnapshot &WorldSnapshot::operator=(WorldSnapshot &&) noexcept = default;

WorldSnapshot::Data &WorldSnapshot::data() {
    if (!data_) data_ = std::make_unique<Data>();
    return *data_;
}

std::size_t WorldSnapshot::bytes() const {
    if (!data_ || data_->empty) return 0;
    const Data &d = *data_;
    std::size_t n = sizeof(Data) + d.generations.size() * 4 + d.states.size() +
                    d.free_list.size() * 4 + d.rng.size() * sizeof(RngStreamState);
    for (std::size_t i = 0; i < d.pool_count; ++i) {
        const PoolCopy &p = d.pools[i];
        n += p.entities.size() * sizeof(Entity) +
             (p.values.tag ? 0 : p.entities.size() * p.values.info->size);
    }
    for (std::size_t i = 0; i < d.resource_count; ++i)
        n += d.resources[i].value.info->size;
    return n;
}

std::uint64_t WorldSnapshot::tick() const {
    return data_ ? data_->tick : 0;
}
bool WorldSnapshot::empty() const {
    return !data_ || data_->empty;
}
std::uint64_t WorldSnapshot::seed() const {
    return data_ ? data_->seed : 0;
}

std::span<const std::uint32_t> WorldSnapshot::generations() const {
    return data_ ? std::span<const std::uint32_t>(data_->generations)
                 : std::span<const std::uint32_t>{};
}
std::span<const EntitySlotState> WorldSnapshot::slot_states() const {
    return data_ ? std::span<const EntitySlotState>(data_->states)
                 : std::span<const EntitySlotState>{};
}
std::span<const std::uint32_t> WorldSnapshot::free_list() const {
    return data_ ? std::span<const std::uint32_t>(data_->free_list)
                 : std::span<const std::uint32_t>{};
}
std::span<const RngStreamState> WorldSnapshot::rng_streams() const {
    return data_ ? std::span<const RngStreamState>(data_->rng) : std::span<const RngStreamState>{};
}
std::size_t WorldSnapshot::pool_count() const {
    return data_ ? data_->pool_count : 0;
}
ComponentId WorldSnapshot::pool_component(std::size_t i) const {
    LAM_CHECK(i < pool_count(), "WorldSnapshot: pool index out of range");
    return data_->pools[i].id;
}
std::span<const Entity> WorldSnapshot::pool_entities(std::size_t i) const {
    LAM_CHECK(i < pool_count(), "WorldSnapshot: pool index out of range");
    return data_->pools[i].entities;
}
const void *WorldSnapshot::pool_data(std::size_t i) const {
    LAM_CHECK(i < pool_count(), "WorldSnapshot: pool index out of range");
    return data_->pools[i].values.at(0);
}
std::size_t WorldSnapshot::resource_count() const {
    return data_ ? data_->resource_count : 0;
}
ComponentId WorldSnapshot::resource_component(std::size_t i) const {
    LAM_CHECK(i < resource_count(), "WorldSnapshot: resource index out of range");
    return data_->resources[i].info;
}
std::uint32_t WorldSnapshot::resource_flags(std::size_t i) const {
    LAM_CHECK(i < resource_count(), "WorldSnapshot: resource index out of range");
    return data_->resources[i].flags;
}
const void *WorldSnapshot::resource_data(std::size_t i) const {
    LAM_CHECK(i < resource_count(), "WorldSnapshot: resource index out of range");
    return data_->resources[i].value.at(0);
}

void WorldSnapshot::reset(std::uint64_t tick, std::uint64_t seed) {
    Data &d = data();
    d.empty = false;
    d.tick = tick;
    d.seed = seed;
    d.generations.clear();
    d.states.clear();
    d.free_list.clear();
    d.rng.clear();
    d.pool_count = 0;
    d.resource_count = 0;
}

void WorldSnapshot::set_entities(std::span<const std::uint32_t> generations,
                                 std::span<const EntitySlotState> states,
                                 std::span<const std::uint32_t> free_list) {
    Data &d = data();
    d.empty = false;
    d.generations.assign(generations.begin(), generations.end());
    d.states.assign(states.begin(), states.end());
    d.free_list.assign(free_list.begin(), free_list.end());
}

void WorldSnapshot::set_rng_streams(std::span<const RngStreamState> streams) {
    LAM_CHECK(streams.size() <= World::kMaxRngStreams, "WorldSnapshot: more than 32 RNG streams");
    Data &d = data();
    d.empty = false;
    d.rng.assign(streams.begin(), streams.end());
}

void *WorldSnapshot::add_pool(ComponentId id, std::span<const Entity> entities) {
    Data &d = data();
    d.empty = false;
    LAM_CHECK(d.pool_count == 0 || d.pools[d.pool_count - 1].id < id,
              "WorldSnapshot::add_pool: ids must ascend");
    const ComponentRegistry &reg = ComponentRegistry::global();
    PoolCopy &pc = d.next_pool();
    pc.id = id;
    pc.entities.assign(entities.begin(), entities.end());
    pc.values.prepare(reg.at(id), reg.trivially_copyable(id), entities.size());
    pc.values.construct_defaults(entities.size());
    return pc.values.at(0);
}

void *WorldSnapshot::add_resource(ComponentId id, std::uint32_t flags) {
    Data &d = data();
    d.empty = false;
    const ComponentRegistry &reg = ComponentRegistry::global();
    const ComponentInfo &info = reg.at(id);
    ResourceCopy &rc = d.next_resource();
    rc.type = info.type;
    rc.info = id;
    rc.flags = flags;
    rc.value.prepare(info, reg.trivially_copyable(id), 1);
    rc.value.construct_defaults(1);
    return rc.value.at(0);
}

Status WorldSnapshot::validate() const {
    if (!data_ || data_->empty) return make_error(ErrorCode::Invalid, "snapshot is empty");
    const Data &d = *data_;
    const std::size_t n = d.generations.size();
    if (d.states.size() != n)
        return make_error(ErrorCode::Invalid,
                          "entity table: states and generations differ in size");
    std::vector<std::uint8_t> seen(n, 0);
    for (const std::uint32_t i : d.free_list) {
        if (i >= n)
            return make_error(ErrorCode::Invalid,
                              "free list index " + std::to_string(i) + " out of range");
        if (d.states[i] != EntitySlotState::Free)
            return make_error(ErrorCode::Invalid,
                              "free list index " + std::to_string(i) + " is not a free slot");
        if (seen[i]++)
            return make_error(ErrorCode::Invalid,
                              "free list index " + std::to_string(i) + " repeated");
    }
    for (std::size_t i = 0; i < n; ++i) {
        const auto s = static_cast<std::uint8_t>(d.states[i]);
        if (s > 2)
            return make_error(ErrorCode::Invalid,
                              "entity slot " + std::to_string(i) + ": bad state");
        if (d.states[i] == EntitySlotState::Free && !seen[i])
            return make_error(ErrorCode::Invalid, "free entity slot " + std::to_string(i) +
                                                      " missing from the free list");
        if (d.states[i] != EntitySlotState::Retired &&
            (d.generations[i] == 0 || d.generations[i] > kLastGeneration))
            return make_error(ErrorCode::Invalid,
                              "entity slot " + std::to_string(i) + ": bad generation");
    }
    const ComponentRegistry &reg = ComponentRegistry::global();
    for (std::size_t p = 0; p < d.pool_count; ++p) {
        const PoolCopy &pc = d.pools[p];
        if (pc.id >= reg.size())
            return make_error(ErrorCode::Invalid, "unknown component id in snapshot");
        if (p > 0 && d.pools[p - 1].id >= pc.id)
            return make_error(ErrorCode::Invalid, "snapshot pools out of order");
        std::fill(seen.begin(), seen.end(), 0);
        for (const Entity e : pc.entities) {
            if (e.index >= n || d.states[e.index] != EntitySlotState::Alive ||
                d.generations[e.index] != e.generation)
                return make_error(ErrorCode::Invalid, std::string(reg.at(pc.id).name) +
                                                          ": entity [" + std::to_string(e.index) +
                                                          ", " + std::to_string(e.generation) +
                                                          "] is not alive");
            if (seen[e.index]++)
                return make_error(ErrorCode::Invalid, std::string(reg.at(pc.id).name) +
                                                          ": entity " + std::to_string(e.index) +
                                                          " repeated");
        }
    }
    if (d.rng.size() > World::kMaxRngStreams)
        return make_error(ErrorCode::Invalid, "more than 32 RNG streams");
    return {};
}

// ------------------------------------------------------------------------------------------------
// World::snapshot_into / restore

void World::snapshot_into(WorldSnapshot &out) const {
    WorldSnapshot::Data &d = out.data();
    d.empty = false;
    d.tick = tick_;
    d.seed = seed_;
    d.generations.assign(generations_.begin(), generations_.end());
    d.states.resize(states_.size());
    d.free_list.assign(free_.begin(), free_.end());
    for (std::size_t i = 0; i < states_.size(); ++i) {
        switch (states_[i]) {
        case kAlive:
            d.states[i] = EntitySlotState::Alive;
            break;
        case kRetired:
            d.states[i] = EntitySlotState::Retired;
            break;
        case kFree:
            d.states[i] = EntitySlotState::Free;
            break;
        case kReserved: // pending spawn: recorded as free
            if (generations_[i] >= kLastGeneration) {
                d.states[i] = EntitySlotState::Retired;
            } else {
                d.states[i] = EntitySlotState::Free;
                d.free_list.push_back(static_cast<std::uint32_t>(i));
            }
            break;
        default:
            LAM_UNREACHABLE();
        }
    }
    d.rng.resize(rng_count_);
    for (std::uint32_t i = 0; i < rng_count_; ++i)
        d.rng[i] = {rngs_[i].id, rngs_[i].rng.state()};
    const ComponentRegistry &reg = ComponentRegistry::global();
    d.pool_count = 0;
    for (std::size_t id = 0; id < pools_.size(); ++id) {
        const detail::Pool *p = pools_[id].get();
        // Empty pools are recorded too, so each snapshot slot keeps serving the same component
        // and its buffers are reused without reallocation.
        if (!p || !(p->info().flags & kStoredFlags)) continue;
        PoolCopy &pc = d.next_pool();
        pc.id = static_cast<ComponentId>(id);
        pc.entities.assign(p->entities().begin(), p->entities().end());
        pc.values.prepare(p->info(), p->trivial(), p->size());
        pc.values.copy_from(p->tag() ? nullptr : static_cast<const std::byte *>(p->at(0)),
                            p->size());
    }
    d.resource_count = 0;
    for (const SnapshotResource &r : snapshot_resources_) {
        if (!(r.flags & kStoredFlags)) continue;
        const void *live = resources_.get_raw(r.type);
        if (!live) continue;
        ResourceCopy &rc = d.next_resource();
        rc.type = r.type;
        rc.info = r.info;
        rc.flags = r.flags;
        rc.value.prepare(reg.at(r.info), reg.trivially_copyable(r.info), 1);
        rc.value.copy_from(static_cast<const std::byte *>(live), 1);
    }
}

void World::restore(const WorldSnapshot &s, RestoreOptions o) {
    LAM_CHECK(!iterating(), "World::restore while a View iterates");
    LAM_CHECK(!s.empty(), "World::restore: empty snapshot");
    const WorldSnapshot::Data &d = *s.data_;
    commands_->discard();
    // Entity table.
    generations_.assign(d.generations.begin(), d.generations.end());
    states_.resize(d.states.size());
    alive_count_ = 0;
    for (std::size_t i = 0; i < d.states.size(); ++i) {
        states_[i] = static_cast<std::uint8_t>(d.states[i]);
        if (d.states[i] == EntitySlotState::Alive) ++alive_count_;
    }
    free_.assign(d.free_list.begin(), d.free_list.end());
    // Pools: replace the snapshot's, empty other stored pools, prune runtime pools.
    std::size_t max_id = pools_.size();
    if (d.pool_count > 0 && std::size_t(d.pools[d.pool_count - 1].id) + 1 > max_id)
        max_id = std::size_t(d.pools[d.pool_count - 1].id) + 1;
    std::size_t k = 0;
    for (std::size_t id = 0; id < max_id; ++id) {
        const PoolCopy *pc = (k < d.pool_count && d.pools[k].id == id) ? &d.pools[k++] : nullptr;
        if (pc) {
            detail::Pool &p = pool_for(static_cast<ComponentId>(id));
            p.assign(pc->entities, pc->values.at(0));
            continue;
        }
        detail::Pool *p = pool_ptr(static_cast<ComponentId>(id));
        if (!p || p->size() == 0) continue;
        if (p->info().flags & kStoredFlags) {
            p->clear();
        } else {
            for (std::size_t slot = p->size(); slot-- > 0;)
                if (!alive(p->entities()[slot])) p->erase(static_cast<std::uint32_t>(slot));
        }
    }
    // Resources.
    for (std::size_t i = 0; i < d.resource_count; ++i) {
        const ResourceCopy &rc = d.resources[i];
        void *live = resources_.get_raw(rc.type);
        if (!live || !rc.value.info) continue;
        if (rc.value.trivial)
            std::memcpy(live, rc.value.at(0), rc.value.info->size);
        else
            rc.value.info->copy(live, rc.value.at(0));
    }
    if (o.tick) tick_ = d.tick;
    if (o.rng) {
        seed_ = d.seed;
        rng_count_ = static_cast<std::uint32_t>(d.rng.size());
        for (std::uint32_t i = 0; i < rng_count_; ++i) {
            rngs_[i].id = d.rng[i].stream;
            rngs_[i].rng.set_state(d.rng[i].state);
        }
    }
    for (std::size_t i = 0; i < restore_hooks_.size(); ++i)
        restore_hooks_[i](*this);
    if (Events<WorldRestored> *ch = events_.try_get<WorldRestored>())
        ch->emit(WorldRestored{tick_});
}

// ------------------------------------------------------------------------------------------------
// History

History::History(std::size_t max_bytes, std::size_t max_entries)
    : max_bytes_(max_bytes), max_entries_(max_entries) {
    const std::size_t cap = (max_entries < 1024 ? max_entries : 1024) + 2;
    undo_.reserve(cap);
    redo_.reserve(cap);
    spare_.reserve(4);
}

History::~History() = default;

WorldSnapshot History::take_spare() {
    if (spare_.empty()) return WorldSnapshot{};
    WorldSnapshot s = std::move(spare_.back());
    spare_.pop_back();
    return s;
}

void History::give_spare(WorldSnapshot &&s) {
    if (spare_.size() < 2) spare_.push_back(std::move(s));
}

void History::evict() {
    while ((size() > max_entries_ || bytes() > max_bytes_) && undo_.size() > 1) {
        give_spare(std::move(undo_.front()));
        undo_.erase(undo_.begin());
    }
    while ((size() > max_entries_ || bytes() > max_bytes_) && redo_.size() > 1) {
        give_spare(std::move(redo_.front()));
        redo_.erase(redo_.begin());
    }
}

void History::push(const World &w) {
    while (!redo_.empty()) {
        give_spare(std::move(redo_.back()));
        redo_.pop_back();
    }
    WorldSnapshot s = take_spare();
    w.snapshot_into(s);
    undo_.push_back(std::move(s));
    evict();
}

bool History::undo(World &w, RestoreOptions o) {
    if (undo_.empty()) return false;
    WorldSnapshot current = take_spare();
    w.snapshot_into(current);
    redo_.push_back(std::move(current));
    w.restore(undo_.back(), o);
    give_spare(std::move(undo_.back()));
    undo_.pop_back();
    evict();
    return true;
}

bool History::redo(World &w, RestoreOptions o) {
    if (redo_.empty()) return false;
    WorldSnapshot current = take_spare();
    w.snapshot_into(current);
    undo_.push_back(std::move(current));
    w.restore(redo_.back(), o);
    give_spare(std::move(redo_.back()));
    redo_.pop_back();
    evict();
    return true;
}

bool History::can_undo() const {
    return !undo_.empty();
}
bool History::can_redo() const {
    return !redo_.empty();
}

void History::clear() {
    undo_.clear();
    redo_.clear();
    spare_.clear();
}

std::size_t History::size() const {
    return undo_.size() + redo_.size();
}

std::size_t History::bytes() const {
    std::size_t n = 0;
    for (const WorldSnapshot &s : undo_)
        n += s.bytes();
    for (const WorldSnapshot &s : redo_)
        n += s.bytes();
    return n;
}

} // namespace lam
