#pragma once
// Headless App with PhysicsPlugin and a log of physics events, read in PostPhysics. The log
// allocates as it grows, so reserve it before CHECK_NO_ALLOC sections.
#include <cstdint>
#include <vector>

#include <lam/core/app.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/components.h>
#include <lam/physics/events.h>
#include <lam/physics/motors.h>
#include <lam/physics/physics_plugin.h>
#include <lam/physics/queries.h>
#include <lam/physics/tile_world.h>
#include <lam/test/support.h>

namespace lam::test {

template <class T> struct Logged {
    std::uint64_t tick = 0;
    T e;
};

struct PhysicsEvents {
    std::vector<Logged<CollisionStarted>> started;
    std::vector<Logged<CollisionEnded>> ended;
    std::vector<Logged<TriggerEntered>> entered;
    std::vector<Logged<TriggerExited>> exited;
    std::vector<Logged<TileHazardTouched>> hazards;
    std::vector<Logged<ProjectileHit>> hits;
    bool enabled = true;
    void clear() {
        started.clear();
        ended.clear();
        entered.clear();
        exited.clear();
        hazards.clear();
        hits.clear();
    }
    void reserve(std::size_t n) {
        started.reserve(n);
        ended.reserve(n);
        entered.reserve(n);
        exited.reserve(n);
        hazards.reserve(n);
        hits.reserve(n);
    }
    std::size_t total() const {
        return started.size() + ended.size() + entered.size() + exited.size() + hazards.size() + hits.size();
    }
};

inline void log_physics_events(Ctx &ctx) {
    PhysicsEvents *log = ctx.try_local<PhysicsEvents>();
    if (log == nullptr || !log->enabled) return;
    const std::uint64_t tick = ctx.world().tick();
    for (const auto &e : ctx.read<CollisionStarted>()) log->started.push_back({tick, e});
    for (const auto &e : ctx.read<CollisionEnded>()) log->ended.push_back({tick, e});
    for (const auto &e : ctx.read<TriggerEntered>()) log->entered.push_back({tick, e});
    for (const auto &e : ctx.read<TriggerExited>()) log->exited.push_back({tick, e});
    for (const auto &e : ctx.read<TileHazardTouched>()) log->hazards.push_back({tick, e});
    for (const auto &e : ctx.read<ProjectileHit>()) log->hits.push_back({tick, e});
}

class PhysicsApp {
public:
    explicit PhysicsApp(PhysicsConfig cfg = {}, AppConfig app_cfg = {}) : h_(app_cfg) {
        h_.add<PhysicsPlugin>(cfg);
        world().resources().insert<PhysicsEvents>();
        h_.app().add_system(Stage::PostPhysics, &log_physics_events, "test.log_physics");
    }
    App &app() { return h_.app(); }
    World &world() { return h_.app().main_world(); }
    TileWorld &tiles() { return world().resources().get<TileWorld>(); }
    PhysicsEvents &events() { return world().resources().get<PhysicsEvents>(); }
    const PhysicsStats &stats() { return world().resources().get<PhysicsStats>(); }
    void ticks(int n) { h_.ticks(n); }

private:
    HeadlessApp h_;
};

inline Collider box_collider(Vec2 half, CollisionMask layer = 1, CollisionMask mask = 0xFFFFFFFFu) {
    Collider c;
    c.shape = ShapeType::Box;
    c.half = half;
    c.layer = layer;
    c.mask = mask;
    return c;
}

inline Collider circle_collider(float radius, CollisionMask layer = 1, CollisionMask mask = 0xFFFFFFFFu) {
    Collider c;
    c.shape = ShapeType::Circle;
    c.radius = radius;
    c.half = {radius, radius};
    c.layer = layer;
    c.mask = mask;
    return c;
}

// An entity with Transform and Collider (a Static body unless more is added).
inline Entity spawn_static(World &w, Vec2 pos, const Collider &c) {
    const Entity e = w.create();
    w.add<Transform>(e, Transform{pos});
    w.add<Collider>(e, c);
    return e;
}

// A Dynamic RigidBody (Velocity is added by the plugin's hook).
inline Entity spawn_dynamic(World &w, Vec2 pos, const Collider &c, RigidBody rb = {}, Vec2 velocity = {}) {
    const Entity e = spawn_static(w, pos, c);
    rb.type = BodyType::Dynamic;
    w.add<RigidBody>(e, rb);
    w.get<Velocity>(e).linear = velocity;
    return e;
}

// A kinematic body: Collider + Velocity, no RigidBody.
inline Entity spawn_kinematic(World &w, Vec2 pos, const Collider &c, Vec2 velocity = {}) {
    const Entity e = spawn_static(w, pos, c);
    w.add<Velocity>(e, Velocity{velocity});
    return e;
}

inline Vec2 pos_of(World &w, Entity e) { return w.get<Transform>(e).position; }

}
