// The rules. Input comes only from input_frame(ctx), structural changes go through
// ctx.commands(), and effects leave as events (PlaySound, BurstAt).
//   Input           player_input    input -> motor/gun intent; fire restarts after death
//   Simulate     0  tick_rules      timers, enemy chase, spawning
//   Simulate    10  fire            bullets (prefabs) through Commands
//   PostPhysics  0  bullet_hits     ProjectileHit -> damage, score, sound + particles
//   PostPhysics 10  contact_damage  overlap_box(player, enemies) -> hurt, game over
#include <game/game.h>

#include <algorithm>
#include <array>
#include <lam/audio/components.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/keys.h>
#include <lam/physics/components.h>
#include <lam/physics/events.h>
#include <lam/physics/motors.h>
#include <lam/physics/queries.h>
#include <lam/physics/steering.h>
#include <lam/physics/tile_world.h>

namespace game {
using namespace lam;
using namespace lam::literals;

namespace {
// TODO(tune): the numbers that shape the game.
constexpr std::uint16_t kHurtInvulnTicks = 60;
constexpr std::size_t kMaxEnemies = 40;
constexpr float kSafeSpawnDistance = 96;                        // never spawn on top of the player

// The skip-th cell holding `c`, or {-1, -1}. constexpr, so a broken layout fails the build.
constexpr IVec2 find_cell(char c, int skip = 0) {
    for (int y = 0; y < kArenaH; ++y)
        for (int x = 0; x < kArenaW; ++x)
            if (arena_at(x, y) == c && skip-- == 0) return {x, y};
    return {-1, -1};
}
constexpr int count_cells(char c) {
    int n = 0;
    while (find_cell(c, n).x >= 0) ++n;
    return n;
}
constexpr int kPads = count_cells('S');
static_assert(kPads > 0 && count_cells('P') == 1, "kArena needs 'S' spawn pads and one 'P'");

// Applied by a tick system from input, so a restart replays like any other move.
void restart(Ctx &ctx) {
    World &w = ctx.world();
    for (auto [e, enemy] : w.view<Enemy>()) ctx.commands().destroy(e);
    for (auto [e, bullet] : w.view<Projectile>()) ctx.commands().destroy(e);
    Run &run = ctx.local<Run>();
    run = Run{.best = run.best, .deaths = run.deaths};
    for (auto [e, h] : w.view<Health>(With<Player>{})) {
        h = Health{.hp = h.max_hp, .max_hp = h.max_hp, .invuln = kHurtInvulnTicks};
        ctx.commands().teleport(e, cell_center(find_cell('P')));
    }
}

void player_input(Ctx &ctx) {
    const Controls &c = ctx.res<Controls>();
    const InputFrame &in = input_frame(ctx);
    const bool over = ctx.local<Run>().over;
    if (over && in.is_pressed(c.fire)) {
        restart(ctx);
        return;
    }
    auto players = ctx.world().view<Transform, Collider, TopDownMotor, Gun>(With<Player>{});
    for (auto [e, t, col, motor, gun] : players) {
        motor.move = over ? Vec2{} : clamp_length(in.axis2(c.move), 1.0f);
        gun.trigger = !over && in.is_held(c.fire);
        // TODO(controls): twin-stick aim with an "aim" axis2 on the right stick
        // (see examples/games/04_shooter).
        if (in.pointer_valid)
            gun.aim = normalize_or(in.pointer_world - (t.position + col.offset), gun.aim);
    }
}

void tick_rules(Ctx &ctx) {
    World &w = ctx.world();
    Run &run = ctx.local<Run>();
    for (auto [e, h] : w.view<Health>())
        if (h.invuln > 0) --h.invuln;
    for (auto [e, gun] : w.view<Gun>())
        if (gun.cooldown > 0) --gun.cooldown;
    if (run.spawn_timer > 0) --run.spawn_timer;
    const Entity player = find_player(w);
    const bool playing = player && !run.over;
    const Vec2 target = player ? w.get<Transform>(player).position : Vec2{};
    // Enemies walk straight at the player; their motors slide along walls.
    // TODO(ai): path round walls and spread crowds with DijkstraMap + FlowField + separation
    // (reference game: examples/games/04_shooter/src/shooter/crowd.cpp).
    for (auto [e, t, motor, enemy] : w.view<Transform, TopDownMotor, Enemy>())
        motor.move = playing ? seek(t.position, target, 1.0f) : Vec2{};
    if (!playing || run.spawn_timer > 0) return;
    run.spawn_timer = static_cast<std::uint32_t>(std::max(20, 80 - run.score / 25));   // speeds up
    if (w.count<Enemy>() >= kMaxEnemies) return;
    // Own RNG stream, so adding another random rule doesn't shift these rolls.
    const std::uint32_t roll = ctx.rng("game.spawn"_id).below(static_cast<std::uint32_t>(kPads));
    const Vec2 at = cell_center(find_cell('S', static_cast<int>(roll)));
    if (distance_sq(at, target) > kSafeSpawnDistance * kSafeSpawnDistance)
        ctx.commands().spawn_prefab("enemy"_id, at);            // TODO(enemies): more prefabs
}

void fire(Ctx &ctx) {
    Commands &cmd = ctx.commands();
    for (auto [e, t, col, gun] : ctx.world().view<Transform, Collider, Gun>(With<Player>{})) {
        if (!gun.trigger || gun.cooldown > 0) continue;
        gun.cooldown = gun.fire_ticks;
        // The prefab brings Projectile, Lifetime and the sprite; Velocity is added after it.
        const Entity b = cmd.spawn_prefab("bullet"_id, t.position + col.offset + gun.aim * 8.0f);
        cmd.add<Velocity>(b, Velocity{gun.aim * gun.bullet_speed});
    }
}

void bullet_hits(Ctx &ctx) {
    World &w = ctx.world();
    Run &run = ctx.local<Run>();
    for (const ProjectileHit &hit : ctx.read<ProjectileHit>()) {
        ctx.commands().destroy(hit.projectile);                 // physics never destroys bullets
        if (hit.tile || !w.alive(hit.target) || run.over) continue;
        Health *h = w.try_get<Health>(hit.target);
        const Enemy *enemy = w.try_get<Enemy>(hit.target);
        if (h == nullptr || enemy == nullptr || h->hp <= 0) continue;   // not an enemy, or dead
        if (--h->hp > 0) continue;                              // hurt, not killed
        run.score += enemy->points;
        ++run.kills;
        const Vec2 at = w.get<Transform>(hit.target).position;
        ctx.emit(PlaySound{.group = kSndPop, .position = at, .volume = 0.7f});
        if (const Fx *fx = ctx.try_res<Fx>(); fx != nullptr && fx->pop)
            ctx.emit(BurstAt{.config = fx->pop, .position = at, .count = 14});
        ctx.commands().destroy(hit.target);
    }
}

void contact_damage(Ctx &ctx) {
    World &w = ctx.world();
    Run &run = ctx.local<Run>();
    for (auto [e, t, col, h] : w.view<Transform, Collider, Health>(With<Player>{})) {
        std::array<Entity, 4> touching{};
        const Rect box = collider_box(col, t.position);
        if (run.over || h.invuln > 0 || overlap_box(w, box, kLayerEnemy, touching) == 0) continue;
        h.invuln = kHurtInvulnTicks;
        if (--h.hp > 0) continue;                               // TODO(feel): hurt event + shake
        run.over = true;
        ++run.deaths;
        run.best = std::max(run.best, run.score);
    }
}

void rule(App &app, Stage stage, SystemFn fn, std::string_view name, int order = 0) {
    app.add_system(
        {.stage = stage, .fn = fn, .name = name, .order = order, .run_if = &has_local<Run>});
}

// Fills TileWorld from kArena. Also a restore hook, since TileWorld isn't saved.
void build_walls(World &w) {
    TileWorld &tiles = w.resources().get<TileWorld>();
    tiles.resize(kArenaW, kArenaH, kTile, Vec2{0, 0});
    for (int y = 0; y < kArenaH; ++y)
        for (int x = 0; x < kArenaW; ++x)
            if (arena_at(x, y) == '#') {
                tiles.set({x, y}, TileShape::Solid, kTileOpaque);
                tiles.cost(x, y) = 0;                           // set() leaves cost at 10
            }
}
}

Vec2 cell_center(IVec2 c) { return (to_vec2(c) + Vec2{0.5f, 0.5f}) * static_cast<float>(kTile); }

Entity find_player(World &w) {
    for (auto [e, t] : w.view<Transform>(With<Player>{})) return e;
    return {};
}

Entity start_run(App &app, World &w) {
    build_walls(w);
    if (!w.has_restore_hook(&build_walls)) w.add_restore_hook(&build_walls);   // hooks never dedupe
    w.resources().insert<Run>();
    w.snapshot_resource<Run>();
    return app.res<PrefabLibrary>().spawn(w, "player"_id, cell_center(find_cell('P')));
}

void RulesPlugin::build(App &app) {
    app.require_plugin("lam.input", name());
    app.require_plugin("lam.physics", name());
    app.register_component<Player>().register_component<Health>();
    app.register_component<Enemy>().register_component<Gun>();
    app.add_event<PlaySound>();                                 // for tests without audio/particles
    app.add_event<BurstAt>();
    // TODO(controls): default bindings; rebinds are saved in the settings file.
    ActionMap &map = app.res<ActionMap>();
    const auto key = [](Key k) { return ActionMap::key(k); };
    app.insert_resource<Controls>(Controls{
        .move = map.axis2("move", {{key(Key::W), key(Key::S), key(Key::A), key(Key::D)}},
                          {{PadAxis::LeftX, PadAxis::LeftY}}),
        .fire = map.button("fire", {ActionMap::mouse(MouseButton::Left), key(Key::Space),
                                    ActionMap::pad(PadButton::RB)})});
    rule(app, Stage::Input, player_input, "game.player_input");
    rule(app, Stage::Simulate, tick_rules, "game.tick_rules", 0);
    rule(app, Stage::Simulate, fire, "game.fire", 10);
    rule(app, Stage::PostPhysics, bullet_hits, "game.bullet_hits", 0);
    rule(app, Stage::PostPhysics, contact_damage, "game.contact_damage", 10);
}
}
