// Presentation systems (see view.h for the schedule). The engine does most of it by itself:
// y-sorting (Sprite kYSort), shadows (Shadow), canopy fading (FadeOccluder + FadeRevealer), the
// camera (CameraFollow), screen shake (CameraShake), the night and the lantern glow (Lighting +
// Light2D) and particles (BurstAt).
#include "view/view.h"

#include <algorithm>
#include <cmath>
#include <lam/animation/sprite_sheet.h>
#include <lam/assets/asset_server.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/input_frame.h>
#include <lam/particles/components.h>
#include <lam/particles/particles.h>
#include <lam/render/camera.h>
#include <lam/render/lighting.h>
#include <lam/render/render_queue.h>
#include <lam/render/shader.h>
#include <lam/render/sprite.h>
#include <lam/tilemap/tilemap.h>

#include "sim/components.h"
#include "sim/controls.h"
#include "sim/level.h"

namespace lantern {

namespace {

// Night: a multiply ambient can only scale colours, so this one keeps greys grey (a 160 grey comes
// out as 93,94,109) and leaves the floor a little green. A bluer ambient (147, 148, 218) tinted
// every rock and wolf lavender.
constexpr lam::Color kNight{154, 156, 182, 255};
constexpr float kLanternLight = 0.3f;                           // Light2D intensity of a lit lantern (0..1)

lam::EmitterConfig burst_config(float speed_max, float size, lam::Color from, lam::Color to) {
    lam::EmitterConfig c;
    c.capacity = 128;
    c.speed_max = speed_max;
    c.size_start = size;
    c.color_start = from;
    c.color_end = to;
    c.gravity = {0.0f, -10.0f};                                 // sparks drift slowly upwards
    return c;
}

void spawn_view(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    if (lam::AssetServer *assets = ctx.try_res<lam::AssetServer>()) {
        // The ground: a Tiled map made from the level by tools/gen_assets.py. It only draws;
        // collision stays the rules' own TileWorld (build_collision = false).
        const lam::Entity ground = w.create();
        w.add<lam::Transform>(ground);
        w.add<lam::Tilemap>(ground, lam::Tilemap{.map = assets->load<lam::TilemapAsset>("maps/forest.tmj"),
                                                 .build_collision = false,
                                                 .spawn_objects = false});
        FxAssets &fx = ctx.res<FxAssets>();
        fx.sparks = assets->add(burst_config(70, 2, {255, 214, 110, 255}, {255, 120, 40, 0}), "fx/sparks");
        fx.dust = assets->add(burst_config(30, 3, {190, 180, 150, 200}, {190, 180, 150, 0}), "fx/dust");
        fx.hit = assets->add(burst_config(90, 2, {255, 240, 220, 255}, {220, 40, 40, 0}), "fx/hit");
        fx.world_sheet = assets->load<lam::SpriteSheet>("sprites/world.json");   // shared with the prefabs
    }

    lam::Entity player;
    w.single_or_null<Player>(&player);
    const lam::Entity cam = w.create();
    lam::Camera2D camera;
    if (player) camera.position = w.get<lam::Transform>(player).position;
    w.add<lam::Camera2D>(cam, camera);
    // Follow half-life, then look-ahead seconds, maximum and half-life.
    w.add<lam::CameraFollow>(cam, lam::CameraFollow{.target = player,
                                                    .half_life = 0.12f,
                                                    .lookahead_seconds = 0.25f,
                                                    .lookahead_max = 24.0f,
                                                    .lookahead_half_life = 0.2f,
                                                    .snap_next = true});
    if (const Level *level = ctx.try_local<Level>()) {
        const lam::Rect world{0, 0, static_cast<float>(level->width * kTile), static_cast<float>(level->height * kTile)};
        w.add<lam::CameraBounds>(cam, lam::CameraBounds{.world = world, .enabled = true});
    }
    w.add<lam::CameraShake>(cam);                               // default shake settings

    if (lam::Lighting *light = ctx.try_local<lam::Lighting>()) {
        light->enabled = true;
        light->ambient = kNight;
    }
}

// Tick stage: the sim's events become particle bursts (World event BurstAt).
void fx_bursts(lam::Ctx &ctx) {
    const FxAssets &fx = ctx.res<FxAssets>();
    for (const LanternCollected &e : ctx.read<LanternCollected>()) {
        ctx.emit(lam::BurstAt{fx.sparks, e.at - lam::Vec2{0, 6}, 24});
    }
    for (const PlayerDashed &e : ctx.read<PlayerDashed>()) ctx.emit(lam::BurstAt{fx.dust, e.at, 10});
    for (const PlayerHurt &e : ctx.read<PlayerHurt>()) ctx.emit(lam::BurstAt{fx.hit, e.at - lam::Vec2{0, 8}, 16});
}

// Frame stage: camera target, shake, banner, pause.
void frame_fx(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    HudState &hud = ctx.res<HudState>();
    const bool paused = ctx.app().paused();
    if (!paused) {
        hud.time += ctx.time().frame_dt;
        hud.wave_banner = std::max(0.0f, hud.wave_banner - ctx.time().frame_dt);
    }
    // Trauma: 0.1 per lantern and 0.6 per bite, capped at 1.
    const float trauma = 0.1f * static_cast<float>(ctx.read<LanternCollected>().size()) +
                         0.6f * static_cast<float>(ctx.read<PlayerHurt>().size());
    if (!ctx.read<WaveUp>().empty()) hud.wave_banner = 2.0f;

    lam::Entity player;
    w.single_or_null<Player>(&player);
    for (auto [cam, follow, shake] : w.view<lam::CameraFollow, lam::CameraShake>()) {
        (void)cam;
        if (player && follow.target != player) {                // a new run made a new traveller
            follow.target = player;
            follow.snap_next = true;
        }
        shake.trauma = std::min(1.0f, shake.trauma + trauma);
    }

    // Pause is frame-time: App::set_paused holds ticks, so it never reaches the hashed state.
    const Controls *controls = ctx.try_res<Controls>();
    const RunState *run = ctx.try_local<RunState>();
    if (controls == nullptr || run == nullptr || run->phase != Phase::Playing) return;
    const lam::FrameInput &in = lam::frame_input(ctx);
    if (in.is_pressed(controls->pause) || (paused && in.is_pressed(controls->confirm))) {
        ctx.app().set_paused(!paused);
    }
}

void set_flag(lam::Sprite &s, std::uint8_t flag, bool on) {
    s.flags = static_cast<std::uint8_t>(on ? (s.flags | flag) : (s.flags & ~flag));
}

// Frame stage: the wind-up glows red (the cue to dash), the traveller blinks while protected,
// collected lanterns go dark until they are lit again.
void night_tints(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    const float time = ctx.res<HudState>().time;
    for (auto [e, wolf, sprite] : w.view<Wolf, lam::Sprite>()) {
        (void)e;
        const bool flash = wolf.fsm.in(WolfState::Telegraph) && static_cast<int>(time * 16.0f) % 2 == 0;
        sprite.tint = flash ? lam::Color{255, 120, 120, 255} : lam::kWhite;
    }
    for (auto [e, p, sprite] : w.view<Player, lam::Sprite>()) {
        (void)e;
        set_flag(sprite, lam::kHidden, p.protection > 0 && static_cast<int>(time * 12.0f) % 2 == 0);
    }
    for (auto [e, lantern, sprite] : w.view<Lantern, lam::Sprite>()) {
        set_flag(sprite, lam::kHidden, !lantern.present);
        if (lam::Light2D *light = w.try_get<lam::Light2D>(e)) light->intensity = lantern.present ? kLanternLight : 0.0f;
    }
}

// Render: the warm glows. A multiply light only gives back the surface colour, so on grass it
// reads green; a warm "diya_glow" sprite drawn over the night gives the warmth instead. Twice per
// lit lantern in (255, 200, 120) at alpha 200 x pulse, 8 px above the feet, and once for the
// traveller in (120, 110, 90, 90), 6 px above. kLayerScreenFx draws after the light composite,
// so positions go through the CameraView.
void draw_glows(lam::Ctx &ctx) {
    lam::RenderQueue *queue = ctx.try_res<lam::RenderQueue>();
    const lam::AssetServer *assets = ctx.try_res<lam::AssetServer>();
    const lam::CameraView *view = ctx.try_local<lam::CameraView>();
    if (queue == nullptr || assets == nullptr || view == nullptr || !view->valid) return;
    const lam::SpriteSheet *sheet = assets->get(ctx.res<FxAssets>().world_sheet);
    const int tag = sheet != nullptr ? sheet->find_tag(lam::str_id("diya_glow")) : -1;
    if (tag < 0) return;
    const lam::TextureRegion glow = lam::frame_region(*sheet, sheet->tags[static_cast<std::size_t>(tag)].from);
    const float pulse = 0.85f + 0.15f * std::sin(ctx.res<HudState>().time * 6.0f);
    const auto alpha = static_cast<std::uint8_t>(200.0f * pulse);
    const lam::Vec2 scale{view->zoom, view->zoom};
    const auto add_glow = [&](lam::Vec2 world, lam::Color tint) {
        queue->region(glow, lam::world_to_screen(*view, world), scale, 0.0f, {0.5f, 0.5f}, tint, lam::kLayerScreenFx,
                      0.0f, lam::kMaterialAdditive);
    };
    lam::World &w = ctx.world();
    for (auto [e, lantern, tr] : w.view<Lantern, lam::Transform>()) {
        (void)e;
        if (!lantern.present) continue;
        for (int pass = 0; pass < 2; ++pass) add_glow(tr.position + lam::Vec2{0.0f, -8.0f}, {255, 200, 120, alpha});
    }
    lam::Entity player;
    if (w.single_or_null<Player>(&player) != nullptr) {
        const lam::Vec2 feet = lam::interpolated(w, player, ctx.time().alpha).position;   // as its sprite is drawn
        add_glow(feet + lam::Vec2{0.0f, -6.0f}, {120, 110, 90, 90});
    }
}

} // namespace

void LanternRunViewPlugin::build(lam::App &app) {
    app.insert_resource<FxAssets>();
    app.insert_resource<HudState>();
    app.add_event<lam::BurstAt>();                              // idempotent; ParticlesPlugin registers it too
    using lam::Stage;
    app.add_startup(spawn_view, "lantern.view.spawn");
    app.add_system(Stage::PostPhysics, fx_bursts, "lantern.view.fx_bursts", 500);
    app.add_system(Stage::Update, frame_fx, "lantern.view.frame_fx", 0);
    app.add_system(Stage::Update, night_tints, "lantern.view.night_tints", 10);
    app.add_system(Stage::Render, draw_glows, "lantern.view.glows", 0);
    app.add_system(Stage::RenderUI, draw_hud, "lantern.view.hud", 0);
}

} // namespace lantern
