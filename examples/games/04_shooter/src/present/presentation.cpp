// HUD, camera shake, sprite looks, aim reticle and the fx/sound assets. Frame stages only; they
// write Sprite, CameraShake and UI, nothing hashed.
#include "present/presentation.h"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/core/fixed_string.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/input/input_frame.h>
#include <lam/particles/particles.h>
#include <lam/physics/components.h>
#include <lam/physics/queries.h>
#include <lam/render/camera.h>
#include <lam/render/render_queue.h>
#include <lam/render/sprite.h>
#include <lam/ui/toasts.h>
#include <shooter/components.h>
#include <shooter/ids.h>
#include "present/scenes.h"

namespace shooter_app {
using namespace lam;
using shooter::Arena;

GameFlags parse_flags(const LaunchOptions &options) {
    GameFlags f;
    f.bot = options.has("--bot");
    // Game value flags: LaunchOptions::int_value reads "--enemies 2000" (or "--enemies=2000").
    if (const int n = options.int_value("--enemies", 0); n > 0) {
        f.arena.stress_enemies = static_cast<std::uint32_t>(n);
        f.arena.god = true;                                     // a benchmark should not end
    }
    if (options.has("--god")) f.arena.god = true;
    return f;
}

void build_hud(World &w) {
    UiTree &ui = w.resources().get<UiTree>();                   // inserted by UiPlugin::init_world
    Hud hud;
    // Fixed widths so the panel doesn't jitter as numbers grow; height 0 fits the labels.
    const UiId left = ui.panel(0, Rect{4, 4, 132, 0}, UiLayout::Vertical);
    hud.score = ui.label(left, "SCORE 0");
    hud.wave = ui.label(left, "WAVE 0");
    hud.hp = ui.label(left, "HP");
    // Top-right panel: anchor and pivot (1, 0) pin it to the right edge of the virtual screen.
    const UiId right = ui.add(0, UiNode{.kind = UiKind::Panel,
                                        .rect = Rect{-4, 4, 112, 0},
                                        .anchor = {1, 0},
                                        .pivot = {1, 0},
                                        .layout = UiLayout::Vertical});
    hud.enemies = ui.label(right, "ENEMIES 0");
    hud.power = ui.label(right, "");
    w.resources().insert<Hud>(hud);
}

namespace {
// Presentation-only sound group (looped by low_hp_alarm through a voice handle).
constexpr StrId kSndHeartbeat = str_id("heartbeat");

// Update: HUD text from Arena and the player's components.
void hud_update(Ctx &ctx) {
    World &w = ctx.world();
    const Arena &a = ctx.local<Arena>();
    const Hud &hud = ctx.local<Hud>();
    UiTree &ui = ctx.local<UiTree>();
    FixedString<64> s;
    format_to(s, "SCORE {}", a.score);
    ui.set_text(hud.score, s.view());
    format_to(s, "WAVE {}  BEST {}", a.wave, a.best);
    ui.set_text(hud.wave, s.view());
    format_to(s, "ENEMIES {}", w.count<shooter::Enemy>() + a.to_spawn);
    ui.set_text(hud.enemies, s.view());
    const Entity p = shooter::find_player(w);
    if (!p) return;
    if (const shooter::Health *h = w.try_get<shooter::Health>(p)) {
        format_to(s, "HP {}/{}", std::max(h->hp, 0), h->max_hp);
        ui.set_text(hud.hp, s.view());
    }
    if (const shooter::Weapon *wpn = w.try_get<shooter::Weapon>(p)) {
        const int secs = std::max(wpn->rapid, wpn->spread) / 60;
        if (wpn->rapid > 0 && wpn->spread > 0) format_to(s, "RAPID+SPREAD {}", secs);
        else if (wpn->rapid > 0) format_to(s, "RAPID {}", secs);
        else if (wpn->spread > 0) format_to(s, "SPREAD {}", secs);
        else s.clear();
        ui.set_text(hud.power, s.view());                       // marks the layout dirty
        // An invisible node takes no space, so the fit-to-children panel shrinks back to one row.
        if (UiNode *n = ui.get(hud.power)) n->visible = !s.empty();
    }
}

// Update: game events -> camera trauma and toasts.
void juice(Ctx &ctx) {
    float trauma = 0;
    for (const shooter::EnemyKilled &e : ctx.read<shooter::EnemyKilled>())
        trauma += e.kind == shooter::EnemyKind::Brute ? 0.35f : 0.05f;
    for (const shooter::PlayerHurt &e : ctx.read<shooter::PlayerHurt>()) {
        (void)e;
        trauma += 0.5f;
    }
    for (const shooter::GameOver &e : ctx.read<shooter::GameOver>()) {
        (void)e;
        trauma += 0.9f;
    }
    if (trauma > 0)
        for (auto [e, shake] : ctx.world().view<CameraShake>())
            shake.trauma = std::min(1.0f, shake.trauma + trauma);

    ToastQueue *toasts = ctx.try_res<ToastQueue>();
    if (toasts == nullptr) return;
    FixedString<64> s;
    for (const shooter::WaveStarted &e : ctx.read<shooter::WaveStarted>()) {
        format_to(s, "WAVE {}: {} INCOMING", e.wave, e.enemies);
        toasts->push(s.view(), 2.5f, Color{255, 220, 120, 255});
    }
    for (const shooter::WaveCleared &e : ctx.read<shooter::WaveCleared>()) {
        format_to(s, "WAVE {} CLEARED +{}", e.wave, e.bonus);
        toasts->push(s.view(), 2.0f, Color{140, 255, 160, 255});
    }
    for (const shooter::PickupTaken &e : ctx.read<shooter::PickupTaken>()) {
        const char *text = e.kind == shooter::PickupKind::Heart ? "+1 HP"
                           : e.kind == shooter::PickupKind::Rapid ? "RAPID FIRE" : "SPREAD SHOT";
        toasts->push(text, 1.5f, Color{120, 200, 255, 255});
    }
}

// Update: sprite looks from tick state (hit flash, invulnerability blink, facing, death).
void actor_looks(Ctx &ctx) {
    World &w = ctx.world();
    const bool dead = ctx.local<Arena>().phase == shooter::Phase::GameOver;
    for (auto [e, sprite, h] : w.view<Sprite, shooter::Health>())
        sprite.tint = h.flash > 0 ? Color{255, 90, 90, 255} : kWhite;
    auto players = w.view<Sprite, shooter::Health, shooter::Weapon>(With<shooter::Player>{});
    for (auto [e, sprite, h, wpn] : players) {
        const bool blink = h.invuln > 0 && (h.invuln / 4) % 2 == 0;
        sprite.tint = sprite.tint.with_alpha(blink ? 90 : 255);
        unsigned flags = sprite.flags & ~unsigned{kFlipX | kHidden};
        if (wpn.aim.x < 0) flags |= kFlipX;
        if (dead) flags |= kHidden;
        sprite.flags = static_cast<std::uint8_t>(flags);
    }
}

// Update: each new pickup gets a continuous ParticleEmitter. It isn't hashed, so presentation may
// add it to sim entities (through Commands, since we're in a view).
void pickup_sparkle(Ctx &ctx) {
    const PresentFx &fx = ctx.res<PresentFx>();
    if (!fx.sparkle) return;
    for (auto [e, p] : ctx.world().view<shooter::Pickup>(Exclude<ParticleEmitter>{}))
        ctx.commands().add<ParticleEmitter>(e, ParticleEmitter{.config = fx.sparkle});
}

// Update: a looping heartbeat while the player is on 1 HP, kept as a VoiceHandle and faded out
// later.
void low_hp_alarm(Ctx &ctx) {
    PresentFx &fx = ctx.res<PresentFx>();
    Audio &audio = ctx.res<Audio>();
    World &w = ctx.world();
    const Entity p = shooter::find_player(w);
    const shooter::Health *h = p ? w.try_get<shooter::Health>(p) : nullptr;
    const bool danger =
        h != nullptr && h->hp == 1 && ctx.local<Arena>().phase != shooter::Phase::GameOver;
    if (danger && !fx.alarm) {
        fx.alarm = audio.play(kSndHeartbeat, PlayParams{.volume = 0.8f, .loop = true});
    } else if (!danger && fx.alarm) {
        audio.stop(fx.alarm, 0.25f);
        fx.alarm = {};
    }
}

// Render: aim reticle at the interpolated player position, a laser sight, and a mouse cross.
void draw_aim(Ctx &ctx) {
    World &w = ctx.world();
    if (ctx.local<Arena>().phase == shooter::Phase::GameOver) return;
    RenderQueue &q = ctx.res<RenderQueue>();
    const Entity p = shooter::find_player(w);
    if (!p) return;
    const Collider *col = w.try_get<Collider>(p);
    const shooter::Weapon *wpn = w.try_get<shooter::Weapon>(p);
    if (wpn == nullptr) return;
    const Vec2 c = interpolated(w, p, ctx.time().alpha).position + (col ? col->offset : Vec2{});
    const Color reticle{255, 255, 255, 150};
    q.line(c + wpn->aim * 12.0f, c + wpn->aim * 20.0f, 1.0f, reticle, kLayerForeground);
    // Laser sight: raycast (tiles + broadphase) to the first enemy or wall. Render stage reads
    // the latest tick's grid and writes nothing, so a physics query is safe here.
    const Vec2 muzzle = c + wpn->aim * wpn->muzzle;
    float reach = 180;
    if (const auto hit = raycast(w, muzzle, wpn->aim, reach, shooter::kLayerEnemy, p))
        reach = hit->distance;
    q.line(muzzle + wpn->aim * 14.0f, muzzle + wpn->aim * reach, 1.0f, Color{255, 70, 70, 60},
           kLayerForeground);
    const FrameInput &fi = frame_input(ctx);
    if (fi.pointer.valid) {
        const Vec2 m = fi.pointer.world;
        q.line(m - Vec2{3, 0}, m + Vec2{4, 0}, 1.0f, Color{255, 230, 120, 200}, kLayerForeground);
        q.line(m - Vec2{0, 3}, m + Vec2{0, 4}, 1.0f, Color{255, 230, 120, 200}, kLayerForeground);
    }
}

// FrameBegin, Once (--bench only): the wall-clock length of the frame that just ended, from
// real_seconds (frame_dt is the tick length under --step).
void bench_sample(Ctx &ctx) {
    Bench &b = ctx.res<Bench>();
    const double now = ctx.time().real_seconds;
    const double last = b.last_real;
    b.last_real = now;
    if (ctx.time().frame <= Bench::kBenchWarmup || last < 0.0) return;
    b.frame_ms.push_back(static_cast<float>((now - last) * 1000.0));   // reserved: no allocation
    if (const RenderStats *rs = ctx.try_res<RenderStats>()) {
        b.draw_calls_sum += rs->draw_calls;
        b.quads_sum += rs->quads;
    }
    if (const World *w = ctx.res<Session>().arena) {
        const std::size_t n = w->count<shooter::Enemy>();
        b.enemies_sum += static_cast<double>(n);
        b.enemies_max = std::max(b.enemies_max, n);
        if (n > 0) {
            b.busy_ms_sum += static_cast<double>(b.frame_ms.back());
            ++b.busy_frames;
        }
    }
}

void add(App &app, Stage stage, SystemFn fn, std::string_view name, int order, RunIf run_if) {
    app.add_system(
        SystemDesc{.stage = stage, .fn = fn, .name = name, .order = order, .run_if = run_if});
}

// Particle configs are made in code and registered with AssetServer::add (Ready at once).
EmitterConfig make_burst(Color a, Color b, float speed_max, float life, float size) {
    EmitterConfig c;
    c.capacity = 2048;
    c.speed_min = speed_max * 0.3f;
    c.speed_max = speed_max;
    c.life_min = life * 0.5f;
    c.life_max = life;
    c.size_start = size;
    c.size_end = 0;
    c.color_start = a;
    c.color_end = b.with_alpha(0);
    c.drag = 2.0f;
    c.layer = kLayerForeground;
    return c;
}

// The sound bank: SoundClips loaded by path (retained by the root scope), grouped in code.
void build_sound_bank(App &app) {
    AssetServer &assets = app.res<AssetServer>();
    SoundBank bank;
    using Files = std::initializer_list<std::string_view>;
    const auto group = [&](StrId id, Files files, float volume, std::uint8_t voices) {
        SoundBank::Group g;
        g.id = id;
        for (std::string_view f : files) g.variants.push_back(assets.load<SoundClip>(f));
        g.volume = volume;
        g.bus = str_id("sfx");
        g.max_voices = voices;
        g.min_interval_ms = 35;
        bank.groups.push_back(std::move(g));
    };
    group(shooter::ids::kSndShoot, {"audio/shoot.wav"}, 0.35f, 4);
    group(shooter::ids::kSndHit, {"audio/hit.wav"}, 0.4f, 3);
    group(shooter::ids::kSndEnemyDie, {"audio/die1.wav", "audio/die2.wav"}, 0.5f, 4);
    group(shooter::ids::kSndHurt, {"audio/hurt.wav"}, 0.9f, 1);
    group(shooter::ids::kSndPickup, {"audio/pickup.wav"}, 0.7f, 2);
    group(shooter::ids::kSndWave, {"audio/wave.wav"}, 0.8f, 1);
    group(shooter::ids::kSndGameOver, {"audio/game_over.wav"}, 0.9f, 1);
    group(kSndHeartbeat, {"audio/heartbeat.wav"}, 0.8f, 1);
    Audio &audio = app.res<Audio>();
    audio.add_bank(assets.add<SoundBank>(std::move(bank), "shooter.sounds"));
}
}

void ShooterPresentationPlugin::build(App &app) {
    app.require_plugin("game.shooter.sim", name());
    AssetServer &assets = app.res<AssetServer>();
    const auto fx = [&](std::string_view id, const EmitterConfig &c) {
        return assets.add<EmitterConfig>(c, id);
    };
    app.insert_resource<shooter::FxAssets>(shooter::FxAssets{
        .death = fx("fx.death", make_burst({255, 120, 60, 255}, {120, 30, 30, 255}, 110, 0.6f, 3)),
        .spark = fx("fx.spark",
                    make_burst({255, 250, 200, 255}, {255, 200, 80, 255}, 70, 0.2f, 1.5f)),
        .pickup = fx("fx.pickup",
                     make_burst({140, 220, 255, 255}, {60, 120, 255, 255}, 80, 0.5f, 2)),
        .hurt = fx("fx.hurt", make_burst({255, 60, 60, 255}, {90, 0, 0, 255}, 90, 0.5f, 2.5f)),
    });
    EmitterConfig sparkle = make_burst({255, 255, 210, 255}, {255, 230, 120, 255}, 18, 0.7f, 1.5f);
    sparkle.capacity = 64;
    sparkle.rate = 10;                                          // continuous: particles per second
    sparkle.spawn_area = Rect{-5, -5, 10, 10};
    sparkle.gravity = Vec2{0, -20};
    app.insert_resource<PresentFx>(PresentFx{.sparkle = fx("fx.sparkle", sparkle)});
    build_sound_bank(app);

    Bench &bench = app.insert_resource<Bench>();
    bench.on = app.options().has("--bench");
    if (bench.on) {
        bench.frame_ms.reserve(static_cast<std::size_t>(std::max(app.options().frames, 0)) + 16);
        app.add_system(SystemDesc{.stage = Stage::FrameBegin,
                                  .fn = bench_sample,
                                  .name = "shooter.bench",
                                  .scope = SystemScope::Once});
    }

    const RunIf arena = &has_local<Arena>;
    add(app, Stage::Input, flow_pause, "shooter.flow_pause", -10, arena);
    add(app, Stage::TickEnd, flow_game_over, "shooter.flow_game_over", 0, arena);
    add(app, Stage::Update, hud_update, "shooter.hud", 0, &has_local<Hud>);
    add(app, Stage::Update, juice, "shooter.juice", 10, arena);
    add(app, Stage::Update, actor_looks, "shooter.looks", 20, arena);
    add(app, Stage::Update, pickup_sparkle, "shooter.pickup_sparkle", 30, arena);
    add(app, Stage::Update, low_hp_alarm, "shooter.low_hp_alarm", 40, arena);
    add(app, Stage::Render, draw_aim, "shooter.aim", 0, arena);
}

void print_bench(App &app) {
    Bench &b = app.res<Bench>();
    if (!b.on || b.frame_ms.empty()) return;
    std::vector<float> sorted = b.frame_ms;
    std::sort(sorted.begin(), sorted.end());
    const auto pct = [&](double q) {
        return sorted[static_cast<std::size_t>(q * static_cast<double>(sorted.size() - 1))];
    };
    double sum = 0;
    for (float ms : b.frame_ms) sum += ms;
    const double n = static_cast<double>(b.frame_ms.size());
    const double avg = sum / n;
    const double busy_avg =
        b.busy_frames > 0 ? b.busy_ms_sum / static_cast<double>(b.busy_frames) : 0.0;
    std::printf("shooter bench: frames=%zu avg_ms=%.3f p50_ms=%.3f p99_ms=%.3f max_ms=%.3f "
                "fps=%.1f enemies_avg=%.0f enemies_max=%zu draw_calls=%.1f quads=%.0f "
                "busy_frames=%zu busy_avg_ms=%.3f busy_fps=%.1f\n",
                b.frame_ms.size(), avg, static_cast<double>(pct(0.5)),
                static_cast<double>(pct(0.99)), static_cast<double>(sorted.back()),
                avg > 0 ? 1000.0 / avg : 0.0, b.enemies_sum / n, b.enemies_max,
                b.draw_calls_sum / n, b.quads_sum / n, b.busy_frames, busy_avg,
                busy_avg > 0 ? 1000.0 / busy_avg : 0.0);
}
}
