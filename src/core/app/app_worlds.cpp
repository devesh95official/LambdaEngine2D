// Worlds: creation with derived seeds, per-world engine resources and events,
// queued destruction, the world stack (read at the frame's snapshot points) and focus.
#include <lam/core/app.h>

#include <algorithm>

#include <lam/core/assert.h>
#include <lam/core/debug.h>
#include <lam/core/log.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>

#include "detail/app_impl.h"

namespace lam {

World &App::main_world() {
    return *impl_->worlds.front()->world;
}

World &App::create_world(std::string_view name) {
    Impl &m = *impl_;
    detail::WorldRecord &r = detail::AppInternals::create_record(*this, name);
    World &w = *r.world;
    // Plugins built so far, in build order. A plugin whose build() is running gets its
    // init_world on this world when that build() returns (add_plugin).
    const std::size_t plugin_count = m.plugins.size();
    for (std::size_t i = 0; i < plugin_count; ++i)
        m.plugins[i]->init_world(*this, w);
    return w;
}

World *App::find_world(StrId name) {
    const Impl &m = *impl_;
    for (const auto &r : m.worlds) {
        World &w = *r->world;
        if (w.name() != name) continue;
        // A world whose destroy_world is queued (inside a pass) is no longer offered.
        if (std::find(m.pending_destroys.begin(), m.pending_destroys.end(), w.id()) !=
            m.pending_destroys.end())
            continue;
        return &w;
    }
    return nullptr;
}

void App::destroy_world(World &w) {
    Impl &m = *impl_;
    if (&w == m.worlds.front()->world.get()) {
        LAM_ASSERT(false, "destroy_world: the main world cannot be destroyed");
        return;
    }
    detail::WorldRecord *r = detail::AppInternals::record_of(*this, w);
    LAM_ASSERT(r != nullptr, "destroy_world: world not owned by this App (already destroyed?)");
    if (!r) return;
    const std::uint32_t id = w.id();
    if (m.in_frame || m.pass_depth > 0) {
        if (std::find(m.pending_destroys.begin(), m.pending_destroys.end(), id) ==
            m.pending_destroys.end())
            m.pending_destroys.push_back(id);
        return;
    }
    detail::AppInternals::destroy_now(*this, id);
}

void App::set_world_stack(std::span<const WorldEntry> entries) {
    Impl &m = *impl_;
    LAM_CHECK(entries.size() <= static_cast<std::size_t>(kMaxWorlds),
              "set_world_stack: at most kMaxWorlds entries");
    std::size_t n = 0;
    for (const WorldEntry &e : entries) {
        const bool owned =
            e.world != nullptr && detail::AppInternals::record_of(*this, *e.world) != nullptr;
        LAM_ASSERT(owned, "set_world_stack: entry world is null or not owned by this App");
        if (!owned) continue;
        bool duplicate = false;
        for (std::size_t i = 0; i < n; ++i)
            duplicate = duplicate || m.requested[i].world == e.world;
        LAM_ASSERT(!duplicate, "set_world_stack: a world may appear only once");
        if (duplicate) continue;
        m.requested[n++] = e;
    }
    m.requested_count = n;
}

std::span<const WorldEntry> App::world_stack() const {
    const Impl &m = *impl_;
    return {m.active.data(), m.active_count};
}

World *App::focused_world() const {
    const Impl &m = *impl_;
    for (std::size_t i = m.active_count; i-- > 0;)
        if (m.active[i].input) return m.active[i].world;
    return nullptr;
}

namespace detail {

WorldRecord &AppInternals::create_record(App &app, std::string_view name) {
    Impl &m = impl(app);
    LAM_ASSERT(!name.empty(), "create_world: a world needs a name");
    const StrId id = intern(name);
    const std::uint64_t stream = id.value ^ m.world_counter++;
    WorldConfig wc;
    wc.entity_capacity = m.config.entity_capacity;
    wc.seed = derive_seed(m.seed, stream);
    wc.name = id;
    auto rec = std::make_unique<WorldRecord>();
    rec->world = std::make_unique<World>(wc);
    rec->stream = stream;
    World &w = *rec->world;
    w.set_prefabs(resources(app).try_get<PrefabLibrary>());
    w.resources().insert<DebugDraw>();
    for (const EventRegistration &ev : m.world_events)
        ev.add(w.events(), ev.capacity);
    // World's constructor already installed restore_engine_state as its first restore hook.
    const std::uint32_t seq = w.events().sequence();
    rec->marks.prev.fill(seq);
    rec->marks.cur.fill(seq);
    rec->marks.occurrence.fill(0);
    rec->skip.eligible_end_seq = seq;
    rec->time = m.app_time;
    rec->time.tick = w.tick();
    rec->time.alpha = 1.0f;
    rec->time.ticks_this_frame = 0;
    m.worlds.push_back(std::move(rec));
    return *m.worlds.back();
}

WorldRecord *AppInternals::record_of(App &app, const World &w) {
    for (auto &r : impl(app).worlds)
        if (r->world.get() == &w) return r.get();
    return nullptr;
}

void AppInternals::destroy_now(App &app, std::uint32_t world_id) {
    Impl &m = impl(app);
    auto it = std::find_if(m.worlds.begin() + 1, m.worlds.end(),
                           [world_id](const auto &r) { return r->world->id() == world_id; });
    if (it == m.worlds.end()) return;
    World &w = *(*it)->world;
    for (std::size_t p = m.plugins.size(); p-- > 0;)
        m.plugins[p]->shutdown_world(app, w);
    remove_systems_now(app, world_id);
    std::erase_if(m.pending_ops, [world_id](const PendingOp &op) {
        return op.kind == PendingOp::Kind::Add && op.entry.desc.only_world == world_id;
    });
    // Drop it from the stored and the active stack (keeping order).
    std::size_t n = 0;
    for (std::size_t i = 0; i < m.requested_count; ++i)
        if (m.requested[i].world != &w) m.requested[n++] = m.requested[i];
    m.requested_count = n;
    n = 0;
    for (std::size_t i = 0; i < m.active_count; ++i) {
        if (m.active[i].world == &w) continue;
        m.active[n] = m.active[i];
        m.active_records[n] = m.active_records[i];
        ++n;
    }
    m.active_count = n;
    // shutdown_world may have destroyed or created worlds; find the record again.
    it = std::find_if(m.worlds.begin() + 1, m.worlds.end(),
                      [world_id](const auto &r) { return r->world->id() == world_id; });
    if (it != m.worlds.end()) m.worlds.erase(it);
    if (m.active_count == 0) refresh_stack(app);
}

void AppInternals::apply_pending_destroys(App &app) {
    Impl &m = impl(app);
    if (m.pending_destroys.empty()) return;
    for (std::size_t i = 0; i < m.pending_destroys.size(); ++i)
        destroy_now(app, m.pending_destroys[i]);
    m.pending_destroys.clear();
}

void AppInternals::refresh_stack(App &app) {
    Impl &m = impl(app);
    if (m.requested_count == 0) {
        m.active[0] = WorldEntry{m.worlds.front()->world.get(), true, true, true};
        m.active_records[0] = m.worlds.front().get();
        m.active_count = 1;
        return;
    }
    for (std::size_t i = 0; i < m.requested_count; ++i) {
        m.active[i] = m.requested[i];
        m.active_records[i] = record_of(app, *m.requested[i].world);
        LAM_CHECK(m.active_records[i] != nullptr, "world stack refers to a destroyed world");
    }
    m.active_count = m.requested_count;
}

WorldRecord &AppInternals::ctx_record(App &app) {
    Impl &m = impl(app);
    for (std::size_t i = m.active_count; i-- > 0;)
        if (m.active[i].input) return *m.active_records[i];
    return *m.worlds.front();
}

} // namespace detail
} // namespace lam
