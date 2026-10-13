// Runs: loading the forest, starting a run (player, wolves, lanterns), spawning wolves, placing
// lanterns and the title -> playing -> over cycle.
#include <array>
#include <optional>
#include <string>
#include <lam/assets/asset_server.h>
#include <lam/core/log.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/input_frame.h>
#include <lam/physics/components.h>
#include <lam/physics/motors.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/grid_path.h>
#include <lam/tilemap/path_follow.h>

#include "sim/controls.h"
#include "sim/level.h"
#include "sim/plugin.h"
#include "sim/rules.h"

namespace lantern {

namespace {

// Wolves change speed at once (they move at the steering velocity directly), so their motors
// accelerate "instantly": one tick always reaches the commanded velocity.
constexpr float kInstant = 100000.0f;

// Presentation comes from a prefab when the App loaded one (sprite, animator, shadow, light);
// the simulation components are always added here, so the rules never depend on prefab data.
lam::Entity spawn_actor(lam::World &w, std::string_view prefab, lam::Vec2 feet) {
    const lam::PrefabLibrary *prefabs = w.prefabs();
    const lam::StrId id = lam::str_id(prefab);
    if (prefabs != nullptr && prefabs->has(id)) return prefabs->spawn(w, id, feet);
    const lam::Entity e = w.create();
    w.add<lam::Transform>(e, lam::Transform{.position = feet});
    return e;
}

// A foot box as a motor body: half extents and an offset that puts its bottom edge at the feet.
// mask 0: bodies report no contacts with each other.
lam::Collider body_collider(lam::Vec2 size, std::uint32_t layer) {
    lam::Collider c;
    c.half = size * 0.5f;
    c.offset = {0.0f, -size.y * 0.5f};
    c.layer = layer;
    c.mask = 0;
    return c;
}

template <class T> std::size_t collect(lam::World &w, std::span<lam::Entity> out) {
    std::size_t n = 0;
    for (auto [e, c] : w.view<T>()) {
        (void)c;
        if (n < out.size()) out[n++] = e;
    }
    return n;
}

} // namespace

bool is_playing(const lam::Ctx &ctx) {
    const RunState *run = ctx.try_local<RunState>();
    return run != nullptr && run->phase == Phase::Playing;
}

void setup_forest(lam::Ctx &ctx) {
    const SimOptions &options = ctx.res<SimOptions>();
    std::string text = options.level_text;
    if (text.empty()) {
        lam::AssetServer *assets = ctx.try_res<lam::AssetServer>();
        if (assets == nullptr) {
            LAM_LOG_ERROR("lantern_run: no AssetServer to load {}", options.level_path);
            ctx.app().quit(1);
            return;
        }
        lam::Result<lam::AssetId<lam::TextAsset>> id = assets->load_now<lam::TextAsset>(options.level_path);
        const lam::TextAsset *asset = id ? assets->get(*id) : nullptr;
        if (asset == nullptr) {
            LAM_LOG_ERROR("lantern_run: cannot load {}", options.level_path);
            ctx.app().quit(1);
            return;
        }
        text = asset->text;
    }
    if (lam::Status s = load_level(ctx.world(), text); !s) {
        LAM_LOG_ERROR("lantern_run: {}", s.error().message);
        ctx.app().quit(1);
        return;
    }
    // A* searches per tick are shared by all wolves together (Tuning::path_searches_per_tick).
    if (lam::PathService *paths = ctx.try_local<lam::PathService>()) {
        paths->searches_per_tick = tuning(ctx).path_searches_per_tick;
    }
    start_run(ctx);
    ctx.local<RunState>().phase = options.start_playing ? Phase::Playing : Phase::Title;
}

void start_run(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    const Tuning &t = tuning(ctx);
    const Level *level = ctx.try_local<Level>();
    if (level == nullptr) return;

    // The previous run's actors go now (immediate destroy: no View is alive here).
    std::array<lam::Entity, 64> old{};
    std::size_t n = collect<Player>(w, old);
    n += collect<Wolf>(w, std::span(old).subspan(n));
    n += collect<Lantern>(w, std::span(old).subspan(n));
    for (std::size_t i = 0; i < n; ++i) w.destroy(old[i]);

    RunState &run = ctx.local<RunState>();
    run.collected = 0;
    run.wave = 1;
    run.wolves_spawned = 0;
    ++run.runs;

    const lam::Entity player = spawn_actor(w, "player", tile_feet(level->start));
    w.add<lam::Collider>(player, body_collider(t.player_box, kPlayerLayer));
    w.add<lam::Velocity>(player);
    lam::TopDownMotor motor;
    motor.max_speed = t.walk_speed;
    motor.accel = t.walk_accel;
    motor.decel = t.walk_decel;
    motor.corner_assist = t.corner_assist;
    motor.solid_mask = kSolidLayer;
    w.add<lam::TopDownMotor>(player, motor);
    w.add<Player>(player, Player{.hearts = t.hearts});

    for (int i = 0; i < t.start_wolves; ++i) spawn_wolf(ctx);
    for (int i = 0; i < t.lanterns_at_once; ++i) {
        const lam::Entity lantern = spawn_actor(w, "lantern", tile_feet(level->start));
        w.add<Lantern>(lantern);
        place_lantern(ctx, lantern);
    }
}

lam::Entity spawn_wolf(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    const Tuning &t = tuning(ctx);
    const Level &level = ctx.local<Level>();
    RunState &run = ctx.local<RunState>();
    if (static_cast<int>(w.count<Wolf>()) >= t.max_wolves) return {};
    lam::Rng &rng = rules_rng(ctx);

    const lam::Vec2 home = tile_feet(level.dens[run.wolves_spawned % level.dens.size()]);
    // Several wolves of one den stand a little apart: the nearest free spot (half-tile raster,
    // 4 tiles round) to a jittered point, else the den itself.
    const float jx = static_cast<float>(rng.between(-8, 8));
    const float jy = static_cast<float>(rng.between(-8, 8));
    const lam::Vec2 wanted = home + lam::Vec2{jx, jy};
    lam::Vec2 feet = home;
    if (const lam::TileWorld *tiles = ctx.try_local<lam::TileWorld>()) {
        const lam::Rect box = body_at(wanted, t.wolf_box);
        // It returns the box's top-left; the feet keep their place relative to the box.
        if (const std::optional<lam::Vec2> spot = lam::nearest_free_spot(*tiles, box, 4)) {
            feet = *spot + (wanted - box.min());
        }
    }

    const lam::Entity e = spawn_actor(w, "wolf", feet);
    w.add<lam::Collider>(e, body_collider(t.wolf_box, kWolfLayer));
    w.add<lam::Velocity>(e);
    lam::TopDownMotor motor;
    motor.max_speed = t.wolf_wander_speed;
    motor.accel = kInstant;
    motor.decel = kInstant;
    motor.corner_assist = 0.0f;
    motor.solid_mask = kSolidLayer;
    w.add<lam::TopDownMotor>(e, motor);
    w.add<lam::PathRoute>(e);
    Wolf wolf;
    wolf.home = home;
    wolf.goal = feet;
    wolf.timer = rng.between(20, 90);                           // first pause
    wolf.slot = run.wolves_spawned++;
    w.add<Wolf>(e, wolf);
    return e;
}

void place_lantern(lam::Ctx &ctx, lam::Entity lantern) {
    lam::World &w = ctx.world();
    const Tuning &t = tuning(ctx);
    const Level &level = ctx.local<Level>();
    lam::Rng &rng = rules_rng(ctx);
    w.get<Lantern>(lantern).present = false;

    lam::Vec2 player_feet{-1e6f, -1e6f};
    lam::Entity pe;
    if (w.single_or_null<Player>(&pe) != nullptr) player_feet = w.get<lam::Transform>(pe).position;

    for (int attempt = 0; attempt < 300; ++attempt) {
        const lam::IVec2 tile{rng.between(1, level.width - 2), rng.between(1, level.height - 2)};
        if (level.solid_at(tile)) continue;
        const lam::Vec2 feet = tile_feet(tile);
        if (lam::distance(feet, player_feet) < t.lantern_min_distance) continue;
        bool crowded = false;
        for (auto [other, l, tr] : w.view<Lantern, lam::Transform>()) {
            if (other != lantern && l.present && lam::distance(tr.position, feet) < t.lantern_spacing) crowded = true;
        }
        if (crowded) continue;
        w.teleport(lantern, feet);                              // appears there; never glides in
        w.get<Lantern>(lantern).present = true;
        return;
    }
}

void stop_actors(lam::World &w) {
    for (auto [e, motor, velocity] : w.view<lam::TopDownMotor, lam::Velocity>()) {
        (void)e;
        motor.move = {};
        velocity.linear = {};
    }
}

void run_phase(lam::Ctx &ctx) {
    RunState *run = ctx.try_local<RunState>();
    const Controls *controls = ctx.try_res<Controls>();
    if (run == nullptr || controls == nullptr) return;
    if (!lam::input_frame(ctx).is_pressed(controls->confirm)) return;
    // Registered after the rules (order 100): the title press itself is not a game tick.
    if (run->phase == Phase::Title) {
        run->phase = Phase::Playing;
    } else if (run->phase == Phase::Over) {
        start_run(ctx);
        run->phase = Phase::Playing;
    }
}

} // namespace lantern
