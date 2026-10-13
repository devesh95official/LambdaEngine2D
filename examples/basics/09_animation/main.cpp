// animation_demo: a sprite-sheet hero driven by an animation graph. A/D (or the arrows) run and
// Space (or J) attacks; the attack's "hit" frame event reports the sword hitbox, an Aseprite slice.
// A coin bobs on a tick-time Tween, squashes on a frame-time FrameTween and hops on every hit
// (TweenFinished restarts the bob). Without input (--headless, --autoplay, or no lam_input) a short
// script plays the same moves and, headless, quits when it ends.
//   animation_demo [--frames N] [--hidden] [--headless] [--autoplay]
// Assets come from make_assets.py (hero.png/json: idle, run, attack tags; hero.anim.json: graph).
#include <cmath>
#include <cstdio>

#include <lam/animation/animator.h>
#include <lam/animation/tween.h>
#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/input_frame.h>
#include <lam/input/keys.h>
#include <lam/render/camera.h>
#include <lam/render/sprite.h>

using namespace lam;
using namespace lam::literals;

struct Hero {};
struct Coin {};
LAM_REFLECT_TAG(Hero)
LAM_REFLECT_TAG(Coin)

struct Controls {                                               // App resource
    AxisAction move;
    ButtonAction attack;
    bool autoplay = false;
};
struct Stats { int hits = 0; int hops = 0; };                   // App resource

static TweenSpec bob() {                                        // forever, tick time
    return {.target = TweenTarget::Position, .to = {0, -3}, .ticks = 30, .ease = Ease::InOutSine,
            .repeat = 255, .yoyo = true, .relative = true};
}

static void spawn(Ctx &ctx) {                                   // startup: loads are synchronous here
    AssetServer &assets = ctx.res<AssetServer>();
    World &w = ctx.world();
    const Entity hero = w.create();
    w.add<Transform>(hero, Transform{.position = {140, 110}});
    w.add<Sprite>(hero);
    w.add<Animator>(hero, Animator{.sheet = assets.load<SpriteSheet>("hero.json"),
                                   .graph = assets.load<AnimGraph>("hero.anim.json")});
    w.add<AnimParams>(hero);
    w.add<Hero>(hero);

    const Entity coin = w.create();
    w.add<Transform>(coin, Transform{.position = {190, 100}});
    w.add<Sprite>(coin);
    w.add<Animator>(coin, Animator{.sheet = assets.load<SpriteSheet>("coin.png")});
    w.add<Coin>(coin);
    tween_to(w.commands(), coin, bob());
    frame_tween_to(w.commands(), coin, {.target = TweenTarget::Scale, .from = {1, 1}, .to = {1.25f, 0.8f},
                                        .ticks = 20, .ease = Ease::InOutQuad, .repeat = 255, .yoyo = true});

    const Entity cam = w.create();
    w.add<Camera2D>(cam, Camera2D{.position = {160, 90}});
}

// The scripted moves: run right, attack, run left, attack twice, rest.
static void script(std::uint64_t tick, float &move_x, bool &attack) {
    move_x = (tick >= 30 && tick < 90) ? 1.0f : (tick >= 150 && tick < 210) ? -1.0f : 0.0f;
    attack = tick == 100 || tick == 230 || tick == 262;
}

static void drive(Ctx &ctx) {                                   // Simulate, every tick
    const Controls &c = ctx.res<Controls>();
    Stats &stats = ctx.res<Stats>();
    World &w = ctx.world();
    float move_x = 0;
    bool attack = false;
    if (c.autoplay) {
        script(ctx.time().tick, move_x, attack);
        if (ctx.app().options().headless && ctx.time().tick >= 330) ctx.app().quit(0);
    } else {
        const InputFrame &in = input_frame(ctx);
        move_x = in.axis1(c.move);
        attack = in.is_pressed(c.attack);
    }
    const AssetServer &assets = ctx.res<AssetServer>();
    for (auto [e, t, a, p] : w.view<Transform, Animator, AnimParams>(With<Hero>{})) {
        set_param(p, "moving"_id, std::fabs(move_x) > 0.1f ? 1.0f : 0.0f);
        a.facing = facing_index({move_x, 0}, 2, a.facing);       // 0 right, 1 left (mirrored)
        t.position.x += move_x * 1.2f;
        if (attack) fire_trigger(p, "attack"_id);
    }
    // Frame events from last tick's Animate: the sword's hitbox exists only on the hit frame.
    for (const AnimationEvent &ev : ctx.read<AnimationEvent>()) {
        if (ev.name != "hit"_id || !w.alive(ev.entity)) continue;
        const Animator &a = w.get<Animator>(ev.entity);
        const SpriteSheet *sheet = assets.get(a.sheet);
        const bool flip = (w.get<Sprite>(ev.entity).flags & kFlipX) != 0;
        if (const auto box = sheet ? current_slice(a, *sheet, "hitbox"_id, flip) : std::nullopt) {
            const Vec2 at = w.get<Transform>(ev.entity).position;
            std::printf("tick %llu: hit, sword box (%.0f, %.0f) %.0fx%.0f\n",
                        static_cast<unsigned long long>(ctx.time().tick), at.x + box->x, at.y + box->y,
                        box->w, box->h);
        }
        ++stats.hits;
        for (auto [coin, ct] : w.view<Transform>(With<Coin>{})) {
            (void)ct;
            tween_to(ctx.commands(), coin, {.target = TweenTarget::Position, .to = {0, -12}, .ticks = 8,
                                            .ease = Ease::OutQuad, .repeat = 1, .yoyo = true,
                                            .relative = true, .tag = "hop"_id});
        }
    }
    for (const TweenFinished &f : ctx.read<TweenFinished>()) {
        if (f.tag != "hop"_id) continue;
        ++stats.hops;
        tween_to(ctx.commands(), f.entity, bob());              // back to bobbing
    }
}

int main(int argc, char **argv) {
    App app({.title = "animation_demo", .window_size = {960, 540}, .virtual_size = {320, 180}}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    app.register_component<Hero>();
    app.register_component<Coin>();
    register_id_name("hop"_id, "hop");
    Controls controls;
    if (ActionMap *map = app.try_res<ActionMap>()) {
        controls.move = map->axis1("move", {ActionMap::key(Key::D), ActionMap::key(Key::Right)},
                                   {ActionMap::key(Key::A), ActionMap::key(Key::Left)});
        controls.attack = map->button("attack", {ActionMap::key(Key::Space), ActionMap::key(Key::J)});
    }
    controls.autoplay = !controls.move || app.options().headless || app.options().has("--autoplay");
    app.insert_resource<Controls>(controls);
    const Stats &stats = app.insert_resource<Stats>();
    app.add_startup(spawn, "demo.spawn");
    app.add_system(Stage::Simulate, drive, "demo.drive");
    const int code = app.run();
    std::printf("animation_demo: %llu ticks, %d hits, %d coin hops%s\n",
                static_cast<unsigned long long>(app.tick()), stats.hits, stats.hops,
                controls.autoplay ? " (autoplay)" : "");
    return code;
}
