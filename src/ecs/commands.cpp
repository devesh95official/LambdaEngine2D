// Deferred structural changes. Arena chunks never move, so payloads stay put until they're
// applied or discarded.
#include <lam/ecs/commands.h>

#include <cstring>
#include <new>

#include <lam/core/assert.h>
#include <lam/core/log.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>

namespace lam {

namespace {
constexpr std::size_t round_up(std::size_t n, std::size_t a) {
    return (n + a - 1) & ~(a - 1);
}
} // namespace

Commands::Commands(World &world, std::size_t arena_bytes) : world_(world) {
    chunks_.reserve(8);
    add_chunk(arena_bytes < 4096 ? 4096 : arena_bytes);
}

Commands::~Commands() {
    discard();
    for (Chunk &c : chunks_)
        ::operator delete(c.memory, std::align_val_t(kAlign));
}

void Commands::add_chunk(std::size_t min_bytes) {
    Chunk c;
    c.capacity = round_up(min_bytes, kAlign);
    c.memory = static_cast<std::byte *>(::operator new(c.capacity, std::align_val_t(kAlign)));
    chunks_.push_back(c);
}

void *Commands::push(Op op, Entity e, ComponentId component, std::size_t payload_bytes) {
    const std::size_t size = kHeaderBytes + round_up(payload_bytes, kAlign);
    LAM_CHECK(size <= 0xFFFFFFFFu, "Commands: record too large");
    while (chunks_[current_].used + size > chunks_[current_].capacity) {
        if (current_ + 1 < chunks_.size() && chunks_[current_ + 1].capacity >= size) {
            ++current_;
            continue;
        }
        if (LAM_DEV && !warned_) {
            warned_ = true;
            LAM_LOG_WARN(
                "Commands: arena of {} bytes exceeded; raise WorldConfig::commands_arena_bytes",
                chunks_[0].capacity);
        }
        // Insert a fresh chunk right after the current one so record order stays chunk order.
        Chunk c;
        c.capacity = round_up(size > chunks_[0].capacity ? size : chunks_[0].capacity, kAlign);
        c.memory = static_cast<std::byte *>(::operator new(c.capacity, std::align_val_t(kAlign)));
        chunks_.insert(chunks_.begin() + static_cast<std::ptrdiff_t>(current_ + 1), c);
        ++current_;
    }
    Chunk &chunk = chunks_[current_];
    std::byte *rec = chunk.memory + chunk.used;
    Header h;
    h.size = static_cast<std::uint32_t>(size);
    h.op = op;
    h.has_payload = payload_bytes > 0 ? 1 : 0;
    h.component = component;
    h.entity = e;
    ::new (rec) Header(h);
    chunk.used += size;
    ++pending_;
    return payload_bytes > 0 ? rec + kHeaderBytes : nullptr;
}

void *Commands::push_add(Entity e, ComponentId id) {
    const ComponentInfo &info = ComponentRegistry::global().at(id);
    LAM_CHECK(info.align <= kAlign, "Commands: component alignment above 16 bytes");
    return push(Op::Add, e, id, info.size);
}

Entity Commands::spawn() {
    const Entity e = world_.reserve_entity();
    push(Op::Spawn, e, 0, 0);
    return e;
}

void Commands::destroy(Entity e) {
    push(Op::Destroy, e, 0, 0);
}

void Commands::teleport(Entity e, Vec2 position) {
    push(Op::Teleport, e, 0, 0);
    // The position lives in the header of the record just written.
    Chunk &chunk = chunks_[current_];
    auto *h = reinterpret_cast<Header *>(chunk.memory + chunk.used - kHeaderBytes);
    h->position = position;
}

Entity Commands::spawn_prefab(StrId prefab, Vec2 position) {
    const PrefabLibrary *lib = world_.prefabs();
    LAM_ASSERT(lib && lib->has(prefab), "Commands::spawn_prefab: unknown prefab");
    const Entity e = spawn();
    if (lib && lib->has(prefab)) lib->spawn_into(*this, e, prefab, position);
    return e;
}

template <class F>
void Commands::for_each_record(F &&f) {
    // Records appended while iterating (hooks, prefab spawns) are visited too: sizes are re-read.
    for (std::size_t c = 0; c < chunks_.size(); ++c) {
        for (std::size_t off = 0; off < chunks_[c].used;) {
            auto *h = reinterpret_cast<Header *>(chunks_[c].memory + off);
            const std::uint32_t size = h->size;
            f(*h, h->has_payload ? chunks_[c].memory + off + kHeaderBytes : nullptr);
            off += size;
        }
    }
}

void Commands::apply() {
    if (pending_ == 0) return;
    LAM_CHECK(!applying_, "Commands::apply re-entered");
    LAM_CHECK(!world_.iterating(), "Commands::apply while a View iterates");
    applying_ = true;
    const ComponentRegistry &reg = ComponentRegistry::global();
    const ComponentId just_spawned = ComponentRegistry::global().id<JustSpawned>();
    for_each_record([&](Header &h, std::byte *payload) {
        const bool live = world_.alive(h.entity);
        switch (h.op) {
        case Op::Spawn:
            if (world_.commit_reserved(h.entity)) world_.add_raw(h.entity, just_spawned);
            break;
        case Op::Add:
            if (live)
                world_.add_from(h.entity, h.component,
                                payload ? payload : detail::Pool::tag_dummy(), true);
            if (payload && !reg.trivially_copyable(h.component))
                reg.at(h.component).destroy(payload);
            break;
        case Op::Remove:
            if (live) world_.remove_raw(h.entity, h.component);
            break;
        case Op::Destroy:
            if (live) world_.destroy(h.entity);
            break;
        case Op::Teleport:
            if (live) world_.teleport(h.entity, h.position);
            break;
        }
    });
    for (Chunk &c : chunks_)
        c.used = 0;
    current_ = 0;
    pending_ = 0;
    applying_ = false;
}

void Commands::discard() {
    LAM_CHECK(!applying_, "Commands discarded while applying");
    if (pending_ == 0) return;
    const ComponentRegistry &reg = ComponentRegistry::global();
    for_each_record([&](Header &h, std::byte *payload) {
        if (h.op == Op::Add && payload && !reg.trivially_copyable(h.component))
            reg.at(h.component).destroy(payload);
    });
    for (Chunk &c : chunks_)
        c.used = 0;
    current_ = 0;
    pending_ = 0;
}

std::size_t Commands::pending() const {
    return pending_;
}

std::size_t Commands::bytes_used() const {
    std::size_t n = 0;
    for (const Chunk &c : chunks_)
        n += c.used;
    return n;
}

World &Commands::world() {
    return world_;
}

} // namespace lam
