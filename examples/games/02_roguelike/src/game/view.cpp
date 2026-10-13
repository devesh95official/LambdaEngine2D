// Presentation systems (schedule in view.h).
#include <game/view.h>

#include <game/scenes.h>
#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/core/log.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/particles/particles.h>
#include <lam/render/camera.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include <lam/render/sprite.h>
#include <lam/tilemap/fov.h>
#include <lam/tilemap/grid_mover.h>
#include <lam/ui/message_log.h>
#include <optional>
#include <sim/components.h>
#include <sim/controls.h>
#include <sim/data.h>
#include <sim/run.h>
#include <sim/save.h>

namespace game {
namespace {
using namespace lam::literals;

struct FxAssets {
    lam::AssetId<lam::EmitterConfig> spark;
}; // App resource (runtime asset)

// HUD layout in virtual pixels (480x270). The map shows through kMapView; the camera keeps the
// hero at its centre and the map's edges inside it (camera_frame).
constexpr lam::Vec2 kScreen{480, 270};
constexpr lam::Rect kTopRect{0, 0, 480, 12};
constexpr lam::Rect kPackRect{346, 14, 134, 0}; // height 0: fits its children
constexpr lam::Rect kLogRect{0, 214, 480, 56};
constexpr lam::Rect kMapView{0, 12, 346, 202};
constexpr std::size_t kLogLines = 6;
constexpr lam::StrId kSlotIds[8] = {"pack_1"_id, "pack_2"_id, "pack_3"_id, "pack_4"_id,
                                    "pack_5"_id, "pack_6"_id, "pack_7"_id, "pack_8"_id};

void set_text_if_changed(lam::UiTree &ui, lam::UiId id, std::string_view text) {
    const lam::UiNode *n = ui.get(id);
    if (n && n->text.view() != text) ui.set_text(id, text); // set_text re-lays out: skip no-ops
}

void log_line(const lam::Ctx &ctx, std::string_view text, lam::Color c) {
    if (lam::MessageLog *log = ctx.try_local<lam::MessageLog>()) log->add(text, c);
}

// Update +0: what the hero cannot see is not drawn. Monsters need the live FOV; items, corpses
// stay visible once explored (the fog then dims them).
void view_sync(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    for (auto [e, sprite, pos] : w.view<lam::Sprite, lam::GridPosition>()) {
        bool show = true;
        if (w.has<rl::Monster>(e))
            show = rl::cell_visible(w, pos.cell);
        else if (w.has<rl::FloorLocal>(e))
            show = rl::cell_explored(w, pos.cell);
        sprite.flags = show ? static_cast<std::uint8_t>(sprite.flags & ~lam::kHidden)
                            : static_cast<std::uint8_t>(sprite.flags | lam::kHidden);
    }
}

// Update +5: frame the part of the screen the HUD leaves free. The follow target is an anchor child
// of the hero offset by (screen centre - kMapView centre), and CameraBounds is the map grown by
// the HUD margins, so the map edges stop at the HUD.
void camera_frame(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    const lam::TileWorld *tiles = ctx.try_local<lam::TileWorld>();
    lam::Entity cam;
    if (!tiles || !w.single_or_null<lam::Camera2D>(&cam)) return;
    lam::CameraBounds *bounds = w.try_get<lam::CameraBounds>(cam);
    if (!bounds) return;
    const float size = static_cast<float>(tiles->tile_size);
    const lam::Rect map{tiles->origin.x, tiles->origin.y,
                        static_cast<float>(tiles->shapes.width()) * size,
                        static_cast<float>(tiles->shapes.height()) * size};
    *bounds =
        lam::CameraBounds{.world = {map.x - kMapView.x, map.y - kMapView.y,
                                    map.w + kScreen.x - kMapView.w, map.h + kScreen.y - kMapView.h},
                          .enabled = true};
}

// Update +10: status line and pack labels.
void hud_update(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    const rl::RunState *run = rl::run_state(w);
    const Hud *hud = ctx.try_local<Hud>();
    lam::UiTree *ui = ctx.try_local<lam::UiTree>();
    if (!run || !hud || !ui || !w.alive(run->player)) return;
    const rl::Stats &st = w.get<rl::Stats>(run->player);
    lam::FixedString<64> line;
    lam::format_to(line, "HP {}/{}  Lv {}  Atk {} Def {}   Depth {}   Score {}   Turn {}",
                   st.hp > 0 ? int{st.hp} : 0, st.max_hp, st.level, st.attack, st.defense,
                   run->depth, run->score, run->turns);
    set_text_if_changed(*ui, hud->status, line.view());
    const rl::Inventory &inv = w.get<rl::Inventory>(run->player);
    const rl::GameData &data = ctx.res<rl::GameData>();
    for (int i = 0; i < rl::kInventorySlots; ++i) {
        if (i < inv.count) {
            const rl::ItemDef *def = data.items.find(inv.slots[static_cast<std::size_t>(i)]);
            lam::format_to(line, "{} {}", i + 1, def ? def->name.view() : std::string_view("?"));
        } else {
            line = "-";
        }
        set_text_if_changed(*ui, hud->slots[i], line.view());
    }
}

void save_now(const lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    const rl::RunState *run = rl::run_state(w);
    if (!run || run->phase == rl::Phase::Dead)
        return log_line(ctx, "The dead cannot save.", {170, 170, 170, 255});
    std::optional<lam::SaveSlots> slots = save_slots(ctx.app());
    if (!slots) return log_line(ctx, "No data folder: saving is off.", {170, 170, 170, 255});
    lam::Result<lam::SaveEnvelope> env = rl::save_envelope(w, ctx.app().serialize_context());
    const lam::Status s = env ? slots->save(1, *env) : lam::Status(env.error());
    if (s)
        log_line(ctx, "Game saved. F9 loads it.", {160, 200, 255, 255});
    else
        log_line(ctx, "Save failed (see log).", {255, 110, 100, 255});
    if (!s) LAM_LOG_ERROR("roguelike: save failed: {}", s.error().message);
}

// Update +20: UI and hotkeys become intents for the next tick (replayable), saves are read-only.
void hud_input(lam::Ctx &ctx) {
    lam::InputSystem *input = ctx.try_res<lam::InputSystem>();
    const rl::Controls *c = ctx.try_res<rl::Controls>();
    const Hud *hud = ctx.try_local<Hud>();
    if (!input || !c || !hud) return;
    for (const lam::UiEvent &ev : ctx.read<lam::UiEvent>()) // UiPlugin is required
        for (int i = 0; i < rl::kInventorySlots; ++i)
            if (ev.kind == lam::UiEvent::Kind::Clicked && ev.id == kSlotIds[i])
                input->queue_intent(lam::Intent{.kind = rl::kUseItem, .a = i});
    // Workaround: a button with focusable = false ignores the pointer, and a focused one eats the
    // game's keys (arrows move focus, Enter/Space click). The pack is pointer-only, so drop the
    // focus every frame: keys then only focus and never activate, while clicks still work (they
    // track UiTree::pressed). Replace with UiTree::set_key_navigation(false).
    if (lam::UiTree *ui = ctx.try_local<lam::UiTree>(); ui && ui->focused()) ui->focus(0);
    const lam::FrameInput &in = lam::frame_input(ctx);
    for (int i = 0; i < rl::kInventorySlots; ++i)
        if (in.is_pressed(c->use[i]))
            input->queue_intent(lam::Intent{.kind = rl::kUseItem, .a = i});
    if (in.is_pressed(c->save)) save_now(ctx);
    if (in.is_pressed(c->load)) input->queue_intent(lam::Intent{.kind = rl::kLoadGame});
}

// Update +30: combat feedback. Sparks only where the hero can see; shake when the hero is hit.
void hit_fx(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    const rl::RunState *run = rl::run_state(w);
    const FxAssets *fx = ctx.try_res<FxAssets>();
    const lam::TileWorld *tiles = ctx.try_local<lam::TileWorld>();
    const bool bursts = fx && fx->spark && tiles;
    lam::Entity cam;
    lam::CameraShake *shake =
        w.single_or_null<lam::Camera2D>(&cam) ? w.try_get<lam::CameraShake>(cam) : nullptr;
    for (const rl::Hit &h : ctx.read<rl::Hit>()) {
        if (bursts && rl::cell_visible(w, h.cell))
            ctx.emit(lam::BurstAt{.config = fx->spark,
                                  .position = tiles->tile_center(h.cell),
                                  .count = static_cast<std::uint16_t>(h.killed ? 24 : 10)});
        if (shake && run && h.target == run->player)
            shake->trauma = shake->trauma + 0.35f > 1 ? 1 : shake->trauma + 0.35f;
    }
}

// RenderUI +0: the message log, newest at the bottom, older lines dimmed. UiTree labels have no
// per-node colour, so the coloured log is drawn with draw_text over the log panel.
void draw_log(lam::Ctx &ctx) {
    const lam::MessageLog *log = ctx.try_local<lam::MessageLog>();
    lam::RenderQueue *q = ctx.try_res<lam::RenderQueue>();
    const lam::RenderDefaults *defaults = ctx.try_res<lam::RenderDefaults>();
    const lam::AssetServer *assets = ctx.try_res<lam::AssetServer>();
    if (!log || !q || !defaults || !assets) return;
    const lam::Font *font = assets->get<lam::Font>(defaults->font);
    if (!font) return;
    const std::size_t n = log->size();
    const std::size_t first = n > kLogLines ? n - kLogLines : 0;
    for (std::size_t i = first; i < n; ++i) {
        const float age = static_cast<float>(n - 1 - i);
        const lam::Color c = lam::lerp(log->color(i), lam::Color{60, 60, 70, 255}, age * 0.12f);
        const lam::Vec2 at{kLogRect.x + 4, kLogRect.y + 2 + 9 * static_cast<float>(i - first)};
        lam::draw_text(*q, *font, log->line(i), at,
                       lam::TextStyle{.color = c, .layer = lam::kLayerUi});
    }
}

template <class T>
lam::SystemDesc run_system(lam::Stage stage, lam::SystemFn fn, std::string_view name, int order) {
    return lam::SystemDesc{
        .stage = stage, .fn = fn, .name = name, .order = order, .run_if = lam::has_local<T>};
}
} // namespace

void ViewPlugin::build(lam::App &app) {
    app.require_plugin("game.roguelike.rules", name());
    app.require_plugin("lam.ui", name());
    if (!app.try_res<RunTracker>()) app.insert_resource<RunTracker>();
    app.add_event<lam::BurstAt>(); // idempotent: emitted even if ParticlesPlugin is absent
    if (lam::AssetServer *assets = app.try_res<lam::AssetServer>()) {
        lam::EmitterConfig spark; // built in code
        spark.capacity = 256;
        spark.speed_min = 25;
        spark.speed_max = 80;
        spark.life_min = 0.15f;
        spark.life_max = 0.4f;
        spark.size_start = 2;
        spark.size_end = 0;
        spark.color_start = lam::Color{255, 230, 140, 255};
        spark.color_end = lam::Color{255, 70, 40, 0};
        spark.drag = 2;
        app.insert_resource<FxAssets>(FxAssets{assets->add<lam::EmitterConfig>(spark, "fx/spark")});
        // The groups the rules emit as PlaySound: hit, kill, death, stairs, pickup.
        if (lam::Audio *audio = app.try_res<lam::Audio>())
            audio->add_bank(assets->load<lam::SoundBank>("sfx/roguelike.sounds.json"));
    }
    using lam::Stage;
    app.add_system(run_system<rl::RunState>(Stage::Update, view_sync, "game.view_sync", 0));
    app.add_system(run_system<Hud>(Stage::Update, camera_frame, "game.camera_frame", 5));
    app.add_system(run_system<Hud>(Stage::Update, hud_update, "game.hud_update", 10));
    app.add_system(run_system<Hud>(Stage::Update, hud_input, "game.hud_input", 20));
    app.add_system(run_system<rl::RunState>(Stage::Update, hit_fx, "game.hit_fx", 30));
    app.add_system(run_system<Hud>(Stage::RenderUI, draw_log, "game.draw_log", 0));
    app.add_system(run_system<rl::RunState>(Stage::Input, load_intent, "game.load_intent", 0));
    app.add_system(run_system<Hud>(Stage::PostPhysics, watch_death, "game.watch_death", 0));
}

void build_view(lam::App &app, lam::World &world) {
    (void)app;
    // Fog: the tilemap draws unseen cells black and explored-but-not-visible cells dimmed
    // whenever the world holds FovMemory (the rules) and FogStyle (a presentation choice).
    world.resources().insert<lam::FogStyle>(
        lam::FogStyle{.unseen = {6, 5, 10, 255}, .explored = {6, 5, 10, 150}});
    Hud &hud = world.resources().insert<Hud>();
    const rl::RunState *run = rl::run_state(world);
    // The camera follows an anchor child of the hero, offset through Parent::local.
    lam::Entity anchor;
    if (run && world.alive(run->player)) {
        anchor = world.create();
        world.add<lam::Transform>(anchor);
        const lam::Vec2 view_centre{kMapView.x + kMapView.w / 2, kMapView.y + kMapView.h / 2};
        lam::set_parent(world, anchor, run->player,
                        lam::Transform{.position = kScreen / 2.0f - view_centre});
    }
    const lam::Entity cam = world.create();
    world.add<lam::Camera2D>(cam);
    world.add<lam::CameraFollow>(cam, lam::CameraFollow{.target = anchor, .half_life = 0.06f});
    world.add<lam::CameraBounds>(cam); // filled by camera_frame once the map exists
    world.add<lam::CameraShake>(cam);
    lam::UiTree *ui = world.resources().try_get<lam::UiTree>();
    if (!ui) return;
    const lam::UiId top = ui->panel(0, kTopRect, lam::UiLayout::Horizontal);
    if (lam::UiNode *n = ui->get(top)) n->style.padding = {4, 2}; // 8 px text in a 12 px bar
    hud.status = ui->label(top, "");
    hud.log_panel = ui->panel(0, kLogRect, lam::UiLayout::None);
    const lam::UiId pack = ui->panel(0, kPackRect, lam::UiLayout::Vertical);
    hud.pack_title = ui->label(pack, "Pack: click or 1-8");
    for (int i = 0; i < rl::kInventorySlots; ++i) {
        // Pointer + number keys only (see the focus workaround in hud_input).
        hud.slots[i] = ui->button(pack, kSlotIds[i], "-");
        if (lam::UiNode *n = ui->get(hud.slots[i])) {
            n->style.padding = {4, 2}; // 12 px rows: the pack fits above the log
            n->fill = lam::kUiFillX;
        }
    }
    ui->label(pack, "Arrows/WASD move");
    ui->label(pack, "Space rest  . stairs");
    ui->label(pack, "F5 save  F9 load");
}
} // namespace game
