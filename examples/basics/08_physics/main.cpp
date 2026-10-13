// physics_demo: a platformer hero patrols a tile world (slopes, a one-way ledge, a ladder, floor
// spikes) while crates and balls rain down, bounce, stack and get shoved aside; a lift carries a
// one-way platform back and forth and a flag trigger counts the hero's visits. Covers:
// TileWorld filled by hand, PlatformerMotor intents (level-triggered, written every tick),
// Dynamic RigidBodies with PhysicsConfig::gravity, PathFollower + MovingPlatform, a trigger
// collider, layers/masks/solid_mask, CollisionStarted/TriggerEntered/TileHazardTouched read in
// PostPhysics with other(), and drawing through RenderQueue (F4 also outlines every collider).
// No input is needed: an autopilot drives the hero, so every run is the same.
//   ./physics_demo                           window until closed
//   ./physics_demo --frames 600 --hidden     scripted run in a hidden window, exits 0
//   ./physics_demo --headless --frames 1200  no window; prints what happened and the state hash
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string_view>

#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/ecs/component.h>
#include <lam/ecs/lifetime.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/components.h>
#include <lam/physics/events.h>
#include <lam/physics/motors.h>
#include <lam/physics/physics_plugin.h>
#include <lam/physics/queries.h>
#include <lam/physics/tile_world.h>
#include <lam/render/camera.h>
#include <lam/render/render_queue.h>
#include <lam/render/sprite.h>

struct Hero {};
LAM_REFLECT_TAG(Hero)
struct Flag {};
LAM_REFLECT_TAG(Flag)

using namespace lam;
using namespace lam::literals;

namespace {

constexpr int kTile = 16;
constexpr CollisionMask kLayerHero = 1;
constexpr CollisionMask kLayerCrate = 2;
constexpr CollisionMask kLayerLift = 4;
constexpr CollisionMask kLayerFlag = 8;
constexpr std::uint64_t kSpawnEvery = 20;                       // ticks between falling bodies
constexpr std::uint32_t kCrateLife = 900;                       // ticks; keeps about 45 alive

// '#' Solid, '=' OneWay, '/' and '\' 45-degree slopes, 'H' ladder, '~' spikes (hazard, walkable).
constexpr std::string_view kMap[] = {
    "##############################",
    "#............................#",
    "#............................#",
    "#............................#",
    "#............................#",
    "#............................#",
    "#............................#",
    "#...=====..............H.....#",
    "#......................H.....#",
    "#......................H.....#",
    "#......................H.....#",
    "#.............../#\\....H.....#",
    "#............../###\\...H.....#",
    "#.....~~....../#####\\..H.....#",
    "##############################",
    "##############################",
    "##############################",
};

// World resource: what happened (printed at exit).
struct DemoStats {
    std::uint32_t spawned = 0;
    std::uint32_t collisions = 0;                               // CollisionStarted, any pair
    std::uint32_t shoves = 0;                                   // hero touched a crate
    std::uint32_t flag_visits = 0;
    std::uint32_t hazard_ticks = 0;
    std::uint32_t jumps = 0;
};

void build_map(TileWorld &tw) {
    const int height = static_cast<int>(std::size(kMap));
    const int width = static_cast<int>(kMap[0].size());
    tw.resize(width, height, kTile, {});
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            TileShape s = TileShape::Empty;
            std::uint8_t f = 0;
            switch (kMap[y][static_cast<std::size_t>(x)]) {
            case '#': s = TileShape::Solid; f = kTileOpaque; break;
            case '=': s = TileShape::OneWay; break;
            case '/': s = TileShape::SlopeUpRight; break;
            case '\\': s = TileShape::SlopeUpLeft; break;
            case 'H': f = kTileLadder; break;
            case '~': f = kTileHazard; break;
            default: break;
            }
            tw.set({x, y}, s, f);
            tw.cost(x, y) = s == TileShape::Solid ? 0 : 10;     // path finding agrees with the walls
        }
    }
}

// Startup: the map, the hero, the lift, the flag and a camera.
void spawn(Ctx &ctx) {
    World &w = ctx.world();
    w.resources().insert<DemoStats>();
    build_map(ensure_tile_world(w));

    // The hero: feet at Transform.position, a 10 x 14 body above them.
    const Entity hero = w.create();
    w.add<Transform>(hero, Transform{{48, 224}});
    w.add<Collider>(hero, Collider{.offset = {0, -7}, .half = {5, 7}, .layer = kLayerHero});
    w.add<PlatformerMotor>(hero, PlatformerMotor{.solid_mask = kLayerLift});   // stands on the lift
    w.add<Hero>(hero);

    // The lift: a one-way platform driven by a PathFollower (needs Velocity to be kinematic).
    const Entity lift = w.create();
    w.add<Transform>(lift, Transform{{288, 112}});
    w.add<Velocity>(lift);
    w.add<Collider>(lift, Collider{.half = {20, 3}, .layer = kLayerLift, .one_way = true});
    PathFollower path{.count = 2, .speed = 40};
    path.points[0] = {288, 112};
    path.points[1] = {440, 112};
    w.add<PathFollower>(lift, path);
    w.add<MovingPlatform>(lift);

    // The flag: a trigger near the right wall that only reports the hero's layer.
    const Entity flag = w.create();
    w.add<Transform>(flag, Transform{{448, 208}});
    w.add<Collider>(flag, Collider{.half = {8, 16}, .layer = kLayerFlag, .mask = kLayerHero, .trigger = true});
    w.add<Flag>(flag);

    const Entity cam = w.create();
    w.add<Camera2D>(cam, Camera2D{.position = {240, 136}});
}

// Input stage: the autopilot writes the hero's intent every tick (intents persist).
void autopilot(Ctx &ctx) {
    World &w = ctx.world();
    const std::uint64_t tick = ctx.time().tick;
    for (auto [e, t, m] : w.view<Transform, PlatformerMotor>(With<Hero>{})) {
        (void)e;
        if (t.position.x > 448.0f) m.move_x = -1.0f;            // inside the flag: turn round
        if (t.position.x < 40.0f || m.move_x == 0.0f) m.move_x = 1.0f;
        const bool hop = tick % 75 == 0 || m.on_wall_left || m.on_wall_right;
        m.jump_pressed = hop && m.on_ground;
        m.jump_held = m.jump_pressed || (m.jump_held && !m.on_ground);
        m.drop = false;
        m.climb_y = 0.0f;
        if (m.jump_pressed) ++ctx.local<DemoStats>().jumps;
    }
}

// Simulate: crates and balls fall from the ceiling (deterministic per-world rng).
void rain(Ctx &ctx) {
    if (ctx.time().tick % kSpawnEvery != 0) return;
    World &w = ctx.world();
    DemoStats &stats = ctx.local<DemoStats>();
    Rng &rng = ctx.rng("rain"_id);
    const Entity e = w.create();
    w.add<Transform>(e, Transform{{rng.range(40.0f, 440.0f), 24.0f}});
    Collider c{.layer = kLayerCrate};
    if (stats.spawned % 3 == 2) {
        c.shape = ShapeType::Circle;
        c.radius = 5;
    } else {
        c.half = {rng.range(4.0f, 8.0f), rng.range(4.0f, 7.0f)};
    }
    w.add<Collider>(e, c);
    w.add<RigidBody>(e, RigidBody{.mass = c.shape == ShapeType::Circle ? 0.5f : 1.0f,
                                  .restitution = c.shape == ShapeType::Circle ? 0.6f : 0.1f,
                                  .friction = 0.5f});
    w.get<Velocity>(e).linear = {rng.range(-40.0f, 40.0f), 0.0f};
    w.add<Lifetime>(e, Lifetime{kCrateLife});
    ++stats.spawned;
}

// PostPhysics: this tick's physics events, read from the hero's side with other().
void tally(Ctx &ctx) {
    World &w = ctx.world();
    DemoStats &stats = ctx.local<DemoStats>();
    for (const CollisionStarted &c : ctx.read<CollisionStarted>()) {
        ++stats.collisions;
        for (const Entity side : {c.a, c.b}) {
            if (w.alive(side) && w.has<Hero>(side) && w.alive(other(c, side)) && w.has<RigidBody>(other(c, side))) {
                ++stats.shoves;
            }
        }
    }
    for (const TriggerEntered &t : ctx.read<TriggerEntered>()) {
        if (w.alive(t.trigger) && w.has<Flag>(t.trigger)) ++stats.flag_visits;
    }
    for (const TileHazardTouched &h : ctx.read<TileHazardTouched>()) {
        if (w.alive(h.entity) && w.has<Hero>(h.entity)) ++stats.hazard_ticks;
    }
}

// Render: tiles and bodies as flat shapes (interpolated). Skipped without a RenderQueue.
void draw(Ctx &ctx) {
    RenderQueue *q = ctx.try_res<RenderQueue>();
    const TileWorld *tw = ctx.try_local<TileWorld>();
    if (q == nullptr || tw == nullptr) return;
    World &w = ctx.world();
    const float s = static_cast<float>(tw->tile_size);
    for (int y = 0; y < tw->shapes.height(); ++y) {
        for (int x = 0; x < tw->shapes.width(); ++x) {
            const Rect r = tw->tile_rect({x, y});
            const TileShape shape = tw->shapes(x, y);
            const std::uint8_t flags = tw->flags(x, y);
            if (shape == TileShape::Solid) q->rect(r, Color{70, 74, 92, 255}, kLayerTiles);
            if (shape == TileShape::OneWay) q->rect(Rect{r.x, r.y, s, 3}, Color{170, 120, 70, 255}, kLayerTiles);
            if (shape == TileShape::SlopeUpRight || shape == TileShape::SlopeUpLeft) {
                for (int i = 0; i < 4; ++i) {                   // four steps approximate the ramp
                    const float h = s * static_cast<float>(i + 1) / 4.0f;
                    const float x0 = shape == TileShape::SlopeUpRight ? r.x + s - h : r.x;
                    q->rect(Rect{x0, r.y + s - h, h, s / 4.0f}, Color{70, 74, 92, 255}, kLayerTiles);
                }
            }
            if ((flags & kTileLadder) != 0) {
                q->rect(Rect{r.x + 3, r.y, 2, s}, Color{190, 160, 110, 255}, kLayerTiles);
                q->rect(Rect{r.x + s - 5, r.y, 2, s}, Color{190, 160, 110, 255}, kLayerTiles);
                q->rect(Rect{r.x + 3, r.y + 6, s - 6, 2}, Color{190, 160, 110, 255}, kLayerTiles);
            }
            if ((flags & kTileHazard) != 0) q->rect(Rect{r.x, r.y + s - 5, s, 5}, Color{230, 60, 60, 255}, kLayerTiles);
        }
    }
    const float alpha = ctx.time().alpha;
    for (auto [e, c] : w.view<Collider>()) {
        if (!w.has<Transform>(e)) continue;
        const Vec2 at = interpolated(w, e, alpha).position;
        Color color{240, 150, 60, 255};                         // crates and balls
        if (w.has<Hero>(e)) color = Color{80, 170, 255, 255};
        if (w.has<MovingPlatform>(e)) color = Color{170, 110, 230, 255};
        if (c.trigger) color = Color{250, 220, 60, 120};
        if (c.shape == ShapeType::Circle) {
            q->circle(at + c.offset, c.radius, color, kLayerWorld);
        } else {
            q->rect(collider_box(c, at), color, kLayerWorld);
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    App app({.title = "physics_demo", .virtual_size = {480, 272}, .audio = false, .settings_file = ""}, argc, argv);
    DefaultPlugins plugins;
    plugins.physics.gravity = {0, 600};                         // Dynamic bodies only; motors have their own
    app.add_plugins(plugins);
    app.register_component<Hero>();
    app.register_component<Flag>();
    app.add_startup(&spawn, "demo.spawn");
    app.add_system(Stage::Input, &autopilot, "demo.autopilot");
    app.add_system(Stage::Simulate, &rain, "demo.rain");
    app.add_system(Stage::PostPhysics, &tally, "demo.tally");
    app.add_system(Stage::Render, &draw, "demo.draw");
    const int code = app.run();

    World &w = app.main_world();
    const DemoStats *stats = w.resources().try_get<DemoStats>();
    if (stats == nullptr) return code;
    std::size_t resting = 0;
    for (auto [e, rb, v] : w.view<RigidBody, Velocity>()) {
        (void)e;
        (void)rb;
        if (length(v.linear) < 1.0f) ++resting;
    }
    for (auto [e, t, m] : w.view<Transform, PlatformerMotor>(With<Hero>{})) {
        (void)e;
        std::printf("physics_demo: %llu ticks; hero at (%.1f, %.1f), on_ground %d, %u jumps\n",
                    static_cast<unsigned long long>(w.tick()), t.position.x, t.position.y, m.on_ground ? 1 : 0,
                    stats->jumps);
    }
    const PhysicsStats &ps = w.resources().get<PhysicsStats>();
    std::printf("  bodies: %u spawned, %zu alive, %zu at rest; broadphase %u bodies, %u pairs, %u contacts\n",
                stats->spawned, w.count<RigidBody>(), resting, ps.bodies, ps.pairs, ps.contacts);
    std::printf("  events: %u collisions, %u hero shoves, %u flag visits, %u hazard ticks\n", stats->collisions,
                stats->shoves, stats->flag_visits, stats->hazard_ticks);
    std::printf("  state hash %016llx\n", static_cast<unsigned long long>(w.state_hash()));
    return code;
}
