// PhysicsPlugin: registration, per-world resources, the physics systems, contact events and the
// collider debug view.
#include <lam/physics/physics_plugin.h>

#include <algorithm>
#include <cmath>

#include <lam/core/debug.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/components.h>
#include <lam/physics/events.h>
#include <lam/physics/motors.h>
#include <lam/physics/queries.h>
#include <lam/physics/tile_world.h>

#include "detail/physics_world.h"
#include "detail/tile_math.h"

namespace lam {
namespace {

using detail::PhysicsWorld;

// Per-tick event capacities.
constexpr std::size_t kContactEventCapacity = 1024;
constexpr std::size_t kTriggerEventCapacity = 512;
constexpr std::size_t kHazardEventCapacity = 256;
constexpr std::size_t kProjectileEventCapacity = 512;
constexpr std::size_t kInteractEventCapacity = 64;

// Copy the App's PhysicsConfig into the world. A new cell size rebuilds the grids.
PhysicsWorld &physics(Ctx &ctx) {
    PhysicsWorld &pw = ctx.local<PhysicsWorld>();
    if (const PhysicsConfig *cfg = ctx.try_res<PhysicsConfig>()) {
        pw.config.gravity = cfg->gravity;
        pw.config.solver_iterations = cfg->solver_iterations > 0 ? cfg->solver_iterations : 1;
        pw.config.pair_capacity = cfg->pair_capacity;
        if (cfg->cell_size > 0.0f && std::isfinite(cfg->cell_size) && cfg->cell_size != pw.config.cell_size) {
            pw.config.cell_size = cfg->cell_size;
            pw.static_dirty = true;
            pw.dyn_valid = false;
        }
    }
    return pw;
}

void ensure_velocity(World &w, Entity e, void *) {
    if (!w.has<Velocity>(e)) w.add<Velocity>(e);
}

void sys_path_followers(Ctx &ctx) { detail::run_path_followers(ctx.world(), physics(ctx), ctx.time().tick_dt); }
void sys_moving_platforms(Ctx &ctx) { detail::run_moving_platforms(ctx.world(), physics(ctx), ctx.time().tick_dt); }
void sys_static_index(Ctx &ctx) { detail::update_static(ctx.world(), physics(ctx), true); }
void sys_carry_riders(Ctx &ctx) { detail::run_carry_riders(ctx.world(), physics(ctx)); }
void sys_motors(Ctx &ctx) { detail::run_motors(ctx.world(), physics(ctx), ctx.time().tick_dt); }

void sys_integrate(Ctx &ctx) {
    PhysicsWorld &pw = physics(ctx);
    detail::integrate_bodies(ctx.world(), pw, pw.config.gravity, ctx.time().tick_dt);
}

void sys_projectiles(Ctx &ctx) {
    PhysicsWorld &pw = physics(ctx);
    detail::run_projectiles(ctx.world(), pw, ctx.time().tick_dt);
    for (const ProjectileHit &h : pw.projectile_hits) ctx.emit(h);
}

void sys_broadphase(Ctx &ctx) {
    World &w = ctx.world();
    PhysicsWorld &pw = physics(ctx);
    if (!(pw.dyn_fresh && pw.dyn_valid && pw.dyn_tick == w.tick())) detail::build_dynamic(w, pw);
    pw.dyn_fresh = false;
    detail::find_pairs(pw);
    PhysicsStats &stats = ctx.local<PhysicsStats>();
    stats.bodies = static_cast<std::uint32_t>(pw.bodies.size() + pw.statics.size());
    stats.pairs = static_cast<std::uint32_t>(pw.pairs.size());
    stats.dropped_pairs = pw.dropped_pairs;
    stats.cells = pw.dyn.occupied_buckets() + pw.stat.occupied_buckets();
}

void sys_narrowphase_solve(Ctx &ctx) {
    PhysicsWorld &pw = physics(ctx);
    detail::narrowphase(ctx.world(), pw, true);
    detail::solve_contacts(ctx.world(), pw, ctx.time().tick_dt);
    ctx.local<PhysicsStats>().contacts = static_cast<std::uint32_t>(pw.contacts.size());
}

bool record_less(const detail::ContactRecord &a, const detail::ContactRecord &b) {
    if (a.first.index != b.first.index) return a.first.index < b.first.index;
    if (a.second.index != b.second.index) return a.second.index < b.second.index;
    return a.kind < b.kind;
}

void emit_started(Ctx &ctx, const detail::ContactRecord &r) {
    if (r.kind == 0) {
        ctx.emit(CollisionStarted{r.first, r.second, r.normal});
    } else {
        ctx.emit(TriggerEntered{r.first, r.second});
    }
}

void emit_ended(Ctx &ctx, const detail::ContactRecord &r) {
    if (r.kind == 0) {
        ctx.emit(CollisionEnded{r.first, r.second});
    } else {
        ctx.emit(TriggerExited{r.first, r.second});
    }
}

// Cheap pre-check: skip the per-body hazard test on maps with no hazard tiles.
bool may_have_hazards(const TileWorld &tw, std::size_t bodies) {
    const std::span<const std::uint8_t> cells = tw.flags.cells();
    if (cells.size() > bodies * 64u) return true;
    std::uint8_t all = 0;
    for (const std::uint8_t f : cells) all = static_cast<std::uint8_t>(all | f);
    return (all & kTileHazard) != 0;
}

void sys_contact_events(Ctx &ctx) {
    World &w = ctx.world();
    PhysicsWorld &pw = physics(ctx);
    detail::PhysicsContactSet &previous = ctx.local<detail::PhysicsContactSet>();
    const std::vector<detail::ContactRecord> &prev = previous.records;
    const std::vector<detail::ContactRecord> &cur = pw.contacts;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < prev.size() || j < cur.size()) {
        if (j == cur.size() || (i < prev.size() && record_less(prev[i], cur[j]))) {
            emit_ended(ctx, prev[i++]);
        } else if (i == prev.size() || record_less(cur[j], prev[i])) {
            emit_started(ctx, cur[j++]);
        } else {
            // Same slots: a recycled entity index is a different contact.
            if (!(prev[i].first == cur[j].first && prev[i].second == cur[j].second)) {
                emit_ended(ctx, prev[i]);
                emit_started(ctx, cur[j]);
            }
            ++i;
            ++j;
        }
    }
    previous.records.swap(pw.contacts);
    pw.contacts.clear();
    // Hazards: one event per body per tick, using the body's real bounds.
    if (const TileWorld *tw = ctx.try_local<TileWorld>(); tw != nullptr && detail::has_tiles(*tw) &&
                                                          !pw.bodies.empty() && may_have_hazards(*tw, pw.bodies.size())) {
        const PolygonShapes *polys = ctx.try_local<PolygonShapes>();
        for (const detail::Body &b : pw.bodies) {
            if ((b.flags & detail::kBodyTrigger) != 0) continue;
            const Collider *c = w.try_get<Collider>(b.entity);
            const Transform *t = w.try_get<Transform>(b.entity);
            if (c == nullptr || t == nullptr) continue;
            const Rect bounds = detail::make_shape(*c, t->position, polys).aabb;
            IVec2 cell;
            if (detail::find_flag(*tw, bounds.expanded(detail::kContactSkin), kTileHazard, cell)) {
                ctx.emit(TileHazardTouched{b.entity, cell});
            }
        }
    }
}

bool show_colliders(const Ctx &ctx) {
    const DebugOverlay *o = ctx.try_res<DebugOverlay>();
    return o != nullptr && o->show_colliders && ctx.try_local<DebugDraw>() != nullptr &&
           ctx.try_local<PhysicsWorld>() != nullptr;
}

// Static grey, kinematic blue, dynamic green (dark when asleep), triggers yellow, projectiles red.
void sys_draw_colliders(Ctx &ctx) {
    World &w = ctx.world();
    DebugDraw &dd = ctx.local<DebugDraw>();
    const PolygonShapes *polys = ctx.try_local<PolygonShapes>();
    for (auto [e, t, c] : w.view<Transform, Collider>()) {
        const RigidBody *rb = w.try_get<RigidBody>(e);
        const BodyType type = detail::classify(w, e, rb);
        Color color{150, 150, 150, 255};
        if (c.trigger) {
            color = Color{255, 220, 40, 255};
        } else if (type == BodyType::Kinematic) {
            color = Color{80, 160, 255, 255};
        } else if (type == BodyType::Dynamic) {
            color = rb != nullptr && rb->sleeping ? Color{40, 120, 60, 255} : Color{80, 220, 120, 255};
        }
        const detail::ShapeView s = detail::make_shape(c, t.position, polys);
        if (!is_finite(s.center)) continue;
        if (s.type == ShapeType::Circle) {
            dd.circle(s.center, s.radius, color);
        } else if (s.type == ShapeType::Polygon && s.local != nullptr) {
            for (int i = 0; i < s.count; ++i) {
                dd.line(s.center + s.local[i], s.center + s.local[(i + 1) % s.count], color);
            }
        } else {
            dd.rect(s.aabb, color);
        }
    }
    for (auto [e, p, t] : w.view<Projectile, Transform>()) {
        (void)e;
        dd.circle(t.position, p.radius > 0.0f ? p.radius : 0.5f, Color{255, 80, 80, 255});
    }
}

// Restore hook: rebuild the grids. Old saves without a contact set get one computed from the
// restored state so nothing fires spuriously.
void restore_contacts(World &w) {
    PhysicsWorld *pw = w.resources().try_get<PhysicsWorld>();
    if (pw == nullptr) return;
    pw->static_dirty = true;
    pw->dyn_valid = false;
    pw->dyn_fresh = false;
    pw->contacts.clear();
    pw->solver.clear();
    detail::ensure_grids(w, *pw);
    detail::PhysicsContactSet *previous = w.resources().try_get<detail::PhysicsContactSet>();
    if (previous != nullptr && previous->restored) {
        previous->restored = false;
        // contact_events merges sorted sets; a hand-edited save may not be.
        if (!std::is_sorted(previous->records.begin(), previous->records.end(), record_less)) {
            std::sort(previous->records.begin(), previous->records.end(), record_less);
        }
        return;
    }
    detail::find_pairs(*pw);
    detail::narrowphase(w, *pw, false);
    if (previous != nullptr) previous->records.swap(pw->contacts);
    pw->contacts.clear();
    pw->solver.clear();
}

void add_system(App &app, Stage stage, SystemFn fn, std::string_view name, int order, RunIf run_if = nullptr) {
    SystemDesc d;
    d.stage = stage;
    d.fn = fn;
    d.name = name;
    d.order = order;
    d.scope = SystemScope::PerWorld;
    d.run_if = run_if;
    app.add_system(d);
}

} // namespace

PhysicsPlugin::PhysicsPlugin(PhysicsConfig c) : config_(c) {}

void PhysicsPlugin::build(App &app) {
    if (app.try_res<PhysicsConfig>() == nullptr) app.insert_resource<PhysicsConfig>(config_);
    app.register_component<Velocity>();
    app.register_component<Collider>();
    app.register_component<RigidBody>();
    app.register_component<Projectile>();
    app.register_component<Interactable>();
    app.register_component<PlatformerMotor>();
    app.register_component<TopDownMotor>();
    app.register_component<MovingPlatform>();
    app.register_component<PathFollower>();
    ComponentRegistry &reg = ComponentRegistry::global();
    reg.set_hooks<RigidBody>(&ensure_velocity, nullptr, nullptr);
    reg.set_hooks<PlatformerMotor>(&ensure_velocity, nullptr, nullptr);
    reg.set_hooks<TopDownMotor>(&ensure_velocity, nullptr, nullptr);

    app.add_event<CollisionStarted>(EventScope::World, kContactEventCapacity);
    app.add_event<CollisionEnded>(EventScope::World, kContactEventCapacity);
    app.add_event<TriggerEntered>(EventScope::World, kTriggerEventCapacity);
    app.add_event<TriggerExited>(EventScope::World, kTriggerEventCapacity);
    app.add_event<TileHazardTouched>(EventScope::World, kHazardEventCapacity);
    app.add_event<ProjectileHit>(EventScope::World, kProjectileEventCapacity);
    app.add_event<Interacted>(EventScope::World, kInteractEventCapacity);

    add_system(app, Stage::Physics, &sys_path_followers, "path_followers", -1800);
    add_system(app, Stage::Physics, &sys_moving_platforms, "moving_platforms", -1800);
    add_system(app, Stage::Physics, &sys_static_index, "static_index", -1700);
    add_system(app, Stage::Physics, &sys_carry_riders, "carry_riders", -1600);
    add_system(app, Stage::Physics, &sys_motors, "motors", -1500);
    add_system(app, Stage::Physics, &sys_integrate, "integrate", -1400);
    add_system(app, Stage::Physics, &sys_projectiles, "projectiles", -1300);
    add_system(app, Stage::Physics, &sys_broadphase, "broadphase", -1200);
    add_system(app, Stage::Physics, &sys_narrowphase_solve, "narrowphase_solve", -1100);
    add_system(app, Stage::Physics, &sys_contact_events, "contact_events", -1000);
    add_system(app, Stage::RenderUI, &sys_draw_colliders, "draw_colliders", 1500, &show_colliders);
}

void PhysicsPlugin::init_world(App &app, World &world) {
    const PhysicsConfig *cfg = app.try_res<PhysicsConfig>();
    Resources &res = world.resources();
    if (!res.has<PhysicsWorld>()) res.insert<PhysicsWorld>(cfg != nullptr ? *cfg : config_);
    if (!res.has<PolygonShapes>()) res.insert<PolygonShapes>();
    if (!res.has<TileWorld>()) res.insert<TileWorld>();
    if (!res.has<PhysicsStats>()) res.insert<PhysicsStats>();
    if (!res.has<detail::PhysicsContactSet>()) {
        detail::PhysicsContactSet &previous = res.insert<detail::PhysicsContactSet>();
        previous.records.reserve((cfg != nullptr ? *cfg : config_).pair_capacity / 8);
    }
    world.snapshot_resource<detail::PhysicsContactSet>(kSnapshot | kSerialize);
    world.add_restore_hook(&restore_contacts);
}

namespace detail {

Status lam_read(PhysicsContactSet &s, DataReader &in, const SerializeContext &ctx) {
    (void)ctx;                                                  // entities stored as [index, generation]
    s.records.clear();
    DataArray list = in.array("records");
    if (in.failed()) return make_error(ErrorCode::Invalid, "PhysicsContactSet: missing \"records\" array");
    constexpr std::int64_t kMaxU32 = 0xFFFFFFFFll;
    s.records.reserve(list.size());
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list.type(i) != DataType::Array) {
            list.fail("a contact record must be [a.index, a.generation, b.index, b.generation, kind]");
            return make_error(ErrorCode::Invalid, "PhysicsContactSet: malformed record");
        }
        DataArray r = list.array(i);
        if (r.size() != 5) {
            r.fail("a contact record must be [a.index, a.generation, b.index, b.generation, kind]");
            return make_error(ErrorCode::Invalid, "PhysicsContactSet: malformed record");
        }
        ContactRecord c;
        c.first.index = static_cast<std::uint32_t>(r.integer64(0, 0, kMaxU32));
        c.first.generation = static_cast<std::uint32_t>(r.integer64(1, 0, kMaxU32));
        c.second.index = static_cast<std::uint32_t>(r.integer64(2, 0, kMaxU32));
        c.second.generation = static_cast<std::uint32_t>(r.integer64(3, 0, kMaxU32));
        c.kind = static_cast<std::uint8_t>(r.integer(4, 0, 1));
        s.records.push_back(c);
    }
    if (in.failed()) return make_error(ErrorCode::Invalid, "PhysicsContactSet: malformed records");
    return {};
}

void lam_write(const PhysicsContactSet &s, DataWriter &out, const SerializeContext &ctx) {
    (void)ctx;
    out.begin_object();
    out.key("records").begin_array();
    for (const ContactRecord &c : s.records) {
        out.begin_array();
        out.value(c.first.index).value(c.first.generation).value(c.second.index).value(c.second.generation);
        out.value(static_cast<int>(c.kind));
        out.end_array();
    }
    out.end_array();
    out.end_object();
}

void lam_hash(const PhysicsContactSet &s, Hasher &h) {
    h.u64(s.records.size());
    for (const ContactRecord &c : s.records) {
        h.u64((static_cast<std::uint64_t>(c.first.index) << 32) | c.first.generation);
        h.u64((static_cast<std::uint64_t>(c.second.index) << 32) | c.second.generation);
        h.u64(c.kind);
    }
}

} // namespace detail

}
