// Pathar's presentation (see look.h).
#include "game/look.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <lam/animation/animator.h>
#include <lam/animation/tween.h>
#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/core/data.h>
#include <lam/core/fixed_string.h>
#include <lam/core/log.h>
#include <lam/core/settings.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/scalar.h>
#include <lam/particles/particles.h>
#include <lam/render/camera.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include <lam/render/sprite.h>
#include <lam/tilemap/autotile.h>
#include <lam/tilemap/grid_mover.h>
#include <lam/ui/toasts.h>
#include "game/game.h"
#include "sim/board.h"
#include "sim/sim_plugin.h"

namespace pathar {

namespace {

using lam::Entity;
using lam::IVec2;
using lam::Vec2;
using lam::World;

constexpr lam::Color kAccent{248, 221, 114, 255};
constexpr lam::Color kText{240, 232, 220, 255};
constexpr lam::Color kDim{170, 150, 130, 255};
constexpr lam::Color kBar{20, 14, 12, 200};
constexpr lam::Color kWarn{214, 123, 82, 255};

// ---- art lookups ----------------------------------------------------------------------------

// A named frame of a sheet. SpriteSheet keeps tag names but not frame names, so every named prop
// frame has a one-frame tag (tools/prepare_assets.py).
const lam::TextureRegion *frame_region(const lam::AssetServer &assets, lam::AssetId<lam::SpriteSheet> id, lam::StrId tag) {
    const lam::SpriteSheet *s = assets.get(id);
    if (s == nullptr) return nullptr;
    const int t = s->find_tag(tag);
    if (t < 0) return nullptr;
    const std::size_t f = s->tags[static_cast<std::size_t>(t)].from;
    return f < s->regions.size() ? &s->regions[f] : nullptr;
}

lam::Result<TileTable> parse_tile_table(std::string_view text) {
    return lam::parse_data<TileTable>(text, "tiles/pathar_tiles.json", [](lam::DataReader &root) {
        TileTable t;
        root.integer("columns", 1, 1024);
        root.integer("tile_size", 1, 1024);
        lam::DataReader terrains = root.object("terrains");
        auto ints = [](lam::DataReader &r, std::string_view key) {
            std::vector<int> out;
            if (!r.has(key)) return out;
            lam::DataArray a = r.array(key);
            for (std::size_t i = 0; i < a.size(); ++i) out.push_back(a.integer(i, 0, 4095));
            return out;
        };
        for (std::string_view name : terrains.keys()) {
            lam::DataReader r = terrains.object(name);
            Terrain x;
            x.name = std::string(name);
            x.base = ints(r, "base");
            x.details = ints(r, "details");
            x.detail_percent = r.integer_or("detail_percent", 0, 0, 100);
            x.edges = r.integer_or("edges", -1, 0, 4095);
            r.ignore("blends");  // bridges are drawn as decor over water here
            t.terrains.push_back(std::move(x));
        }
        return t;
    });
}

// ---- the board's tiles ------------------------------------------------------------------------

constexpr std::uint8_t kWall = 1, kGround = 2, kWater = 3;
constexpr std::uint8_t kLayerDetails = lam::kLayerTiles + 1;  // details and bridges
constexpr std::uint8_t kLayerSlabs = lam::kLayerTiles + 2;
constexpr std::uint8_t kLayerSunk = lam::kLayerTiles + 3;

lam::TileId gid(int local) { return local < 0 ? 0u : static_cast<lam::TileId>(local + 1); }  // first_gid = 1

int pick(const std::vector<int> &v, std::uint32_t hash) {
    return v.empty() ? -1 : v[hash % static_cast<std::uint32_t>(v.size())];
}

// Ground, details and decor layers for `l` in `theme`, the board in the middle of a map at least
// as big as the view, walls all around.
lam::TilemapAsset build_map(const Level &l, const std::string &theme, const TileTable &tt,
                            lam::AssetId<lam::Tileset> tileset, IVec2 size, IVec2 pad) {
    const Terrain *ground = tt.find(theme + "_ground");
    const Terrain *wall = tt.find(theme + "_wall");
    const Terrain *water = tt.find("ghat_water");
    lam::Grid<std::uint8_t> terrain(size.x, size.y, kWall);
    for (int c = 0; c < l.width * l.height; ++c) {
        const IVec2 p = l.pos(c) + pad;
        if (l.tile(c) == Tile::Water || l.bridge[static_cast<std::size_t>(c)]) terrain.set(p.x, p.y, kWater);
        else if (l.tile(c) == Tile::Ground) terrain.set(p.x, p.y, kGround);
    }
    // Edge4 autotiling for walls and water: the 4-bit mask of same-terrain neighbours picks one
    // of 16 tiles starting at `first` (deterministic variants by tile_hash).
    std::vector<lam::AutotileRule> rules;
    if (wall != nullptr && wall->edges >= 0)
        rules.push_back({.terrain = lam::intern(wall->name), .terrain_value = kWall, .first = gid(wall->edges)});
    if (water != nullptr && water->edges >= 0)
        rules.push_back({.terrain = lam::intern(water->name), .terrain_value = kWater, .first = gid(water->edges),
                         .edge_outside = false});
    const auto seed = static_cast<std::uint32_t>(l.width * 131 + l.height * 17 + static_cast<int>(l.name.size()));
    lam::TileLayer base{.name = lam::str_id("ground"), .render_layer = lam::kLayerTiles};
    lam::TileLayer details{.name = lam::str_id("details"), .render_layer = kLayerDetails};
    lam::TileLayer decor{.name = lam::str_id("decor"), .render_layer = kLayerDetails};
    base.tiles.resize(size.x, size.y, 0);
    details.tiles.resize(size.x, size.y, 0);
    decor.tiles.resize(size.x, size.y, 0);
    lam::autotile(terrain, rules, seed, base.tiles);
    for (int y = 0; y < size.y; ++y) {
        for (int x = 0; x < size.x; ++x) {
            if (terrain(x, y) != kGround || ground == nullptr) continue;
            base.tiles(x, y) = gid(pick(ground->base, lam::tile_hash(x, y, seed)));
            const IVec2 b{x - pad.x, y - pad.y};
            const int c = l.cell(b);
            const bool busy_cell = l.is_goal(c) || c == l.player ||
                                   std::find(l.boxes.begin(), l.boxes.end(), c) != l.boxes.end();
            const std::uint32_t h = lam::tile_hash(x, y, seed + 3u);
            if (!busy_cell && static_cast<int>(h % 100u) < ground->detail_percent)
                details.tiles(x, y) = gid(pick(ground->details, h / 100u));
        }
    }
    // Bridges: planks across the river (a river running up and down is crossed left to right).
    for (int c = 0; c < l.width * l.height; ++c) {
        if (!l.bridge[static_cast<std::size_t>(c)]) continue;
        auto wet = [&](int n) { return n >= 0 && (l.tile(n) == Tile::Water || l.bridge[static_cast<std::size_t>(n)]); };
        const bool runs_down = wet(l.step(c, Dir::Up)) || wet(l.step(c, Dir::Down));
        const IVec2 p = l.pos(c) + pad;
        decor.tiles(p.x, p.y) = gid(tt.single(runs_down ? "bridge" : "bridge_v"));
    }
    lam::TilemapAsset m;
    m.width = size.x;
    m.height = size.y;
    m.tile_size = kTileSize;
    m.tilesets = {lam::TilesetRef{.tileset = tileset, .first_gid = 1}};
    m.layers = {std::move(base), std::move(details), std::move(decor)};
    return m;
}

bool alive_with_map(World &w, Entity e) { return w.alive(e) && w.has<lam::Tilemap>(e); }
bool alive_with_camera(World &w, Entity e) { return w.alive(e) && w.has<lam::Camera2D>(e); }

void build_board(lam::App &app, World &w, BoardLook &look, const Board &b, const lam::Tileset &ts) {
    lam::AssetServer &assets = app.res<lam::AssetServer>();
    const Art &art = app.res<Art>();
    const TileTable &tt = app.res<TileTable>();
    const Level &l = b.level;
    // The map covers the view around the board, also when the title shifts the board aside.
    const TitleLook *title = w.resources().try_get<TitleLook>();
    const Vec2 shift = title != nullptr ? title->board_offset : Vec2{};
    const IVec2 view = app.config().virtual_size +
                       IVec2{2 * static_cast<int>(std::abs(shift.x)), 2 * static_cast<int>(std::abs(shift.y))};
    const IVec2 size{std::max(l.width + 2, view.x / kTileSize + 2), std::max(l.height + 2, view.y / kTileSize + 2)};
    const IVec2 pad{(size.x - l.width) / 2, (size.y - l.height) / 2};
    lam::TilemapAsset map = build_map(l, theme_of(app, b.index), tt, art.tileset, size, pad);
    if (!look.map_asset) {
        look.map_asset = assets.add<lam::TilemapAsset>(std::move(map), "pathar.board");
    } else if (lam::TilemapAsset *dst = assets.get_mut(look.map_asset)) {
        *dst = std::move(map);
        assets.mark_modified(look.map_asset);  // tilemap_sync recopies Modified maps
    }
    const Vec2 corner = b.origin - lam::to_vec2(pad) * static_cast<float>(kTileSize);
    if (!alive_with_map(w, look.map)) {
        look.map = w.create();
        w.add<lam::Transform>(look.map, lam::Transform{.position = corner});
        // The simulation owns the TileWorld (board.h); this map is only drawn.
        w.add<lam::Tilemap>(look.map, lam::Tilemap{.map = look.map_asset, .build_collision = false, .spawn_objects = false});
    } else {
        w.teleport(look.map, corner);
    }
    // Slabs: sprites (not tiles) so they can pulse with a frame-time tween (presentation only).
    for (Entity e : look.slabs) {
        if (w.alive(e) && w.has<SlabLook>(e)) w.destroy(e);
    }
    look.slabs.clear();
    const int slab = tt.single("slab");
    for (int c = 0; c < l.width * l.height && slab >= 0; ++c) {
        if (!l.is_goal(c)) continue;
        const Entity e = w.create();
        w.add<lam::Transform>(e, lam::Transform{.position = cell_center(w, l.pos(c))});
        w.add<lam::Sprite>(e, lam::Sprite{.region = ts.region(static_cast<std::uint32_t>(slab)), .pivot = {0.5f, 0.5f},
                                          .layer = kLayerSlabs, .flags = 0});
        w.add<SlabLook>(e);
        lam::FrameTween pulse;
        pulse.steps[0] = lam::TweenSpec{.target = lam::TweenTarget::Alpha, .from = {0.75f, 0}, .to = {1.0f, 0},
                                        .ticks = 50, .delay = static_cast<std::uint16_t>((c * 7) % 50),
                                        .ease = lam::Ease::InOutSine, .repeat = 255, .yoyo = true};
        pulse.count = 1;
        w.add<lam::FrameTween>(e, pulse);
        look.slabs.push_back(e);
    }
    if (!alive_with_camera(w, look.camera)) {
        look.camera = w.create();
        w.add<lam::Camera2D>(look.camera);
    }
    w.get<lam::Camera2D>(look.camera).position =
        b.origin + Vec2{static_cast<float>(l.width * kTileSize) * 0.5f, static_cast<float>(l.height * kTileSize) * 0.5f} -
        shift;
    look.serial = b.serial;
}

// Sprites, shadows and the animator for pieces that have none yet (new level, or an undo past
// the moment they were added).
void attach_pieces(lam::App &app, World &w) {
    const Art &art = app.res<Art>();
    std::vector<Entity> bare;
    for (auto [e, gp] : w.view<lam::GridPosition>(lam::Exclude<lam::Sprite>{})) {
        (void)gp;
        if (w.has<Stone>(e) || w.has<Player>(e)) bare.push_back(e);
    }
    for (Entity e : bare) {
        if (w.has<Player>(e)) {
            // 32x32 frames, feet 22 px down: pivot at the cell centre; y-sorted after stones of the same row.
            w.add<lam::Sprite>(e, lam::Sprite{.pivot = {0.5f, 22.0f / 32.0f}, .sort_offset = 0.5f});
            w.add<lam::Shadow>(e, lam::Shadow{.radii = {5, 2}, .offset = {0, 6}, .color = {0, 0, 0, 70}});
            w.add<lam::Animator>(e, lam::Animator{.sheet = art.hero, .clip = lam::str_id("idle_bare_down")});
        } else {
            w.add<lam::Sprite>(e, lam::Sprite{.pivot = {0.5f, 10.0f / 18.0f}});
            w.add<lam::Shadow>(e, lam::Shadow{.radii = {6, 2}, .offset = {0, 7}, .color = {0, 0, 0, 80}});
        }
    }
}

// ---- systems --------------------------------------------------------------------------------------

void look_board(lam::Ctx &ctx) { refresh_look(ctx.app(), ctx.world()); }

void look_pieces(lam::Ctx &ctx) {
    World &w = ctx.world();
    const lam::AssetServer &assets = ctx.res<lam::AssetServer>();
    const Art &art = ctx.res<Art>();
    const Level &l = ctx.local<Board>().level;
    const lam::TextureRegion *plain = frame_region(assets, art.props, lam::str_id("stone"));
    const lam::TextureRegion *lit = frame_region(assets, art.props, lam::str_id("stone_lit"));
    const lam::TextureRegion *sunk = frame_region(assets, art.props, lam::str_id("stone_sunk"));
    if (plain == nullptr || lit == nullptr || sunk == nullptr) return;
    for (auto [e, stone, gp, mover, sprite] : w.view<Stone, lam::GridPosition, lam::GridMover, lam::Sprite>()) {
        (void)stone;
        lam::Shadow *shadow = w.try_get<lam::Shadow>(e);
        if (w.has<Sunk>(e) && !mover.moving) {  // under the water line: drawn flat, under everything
            sprite.region = *sunk;
            sprite.pivot = {0.5f, 0.5f};
            sprite.layer = kLayerSunk;
            sprite.flags = 0;
            sprite.tint = lam::kWhite;
            if (shadow != nullptr) shadow->color.a = 0;
            continue;
        }
        const bool on_slab = l.inside(gp.cell) && l.is_goal(l.cell(gp.cell)) && !mover.moving;
        sprite.region = on_slab ? *lit : *plain;
        sprite.pivot = {0.5f, 10.0f / 18.0f};
        sprite.layer = lam::kLayerWorld;
        sprite.flags = lam::kYSort;
        const float t = mover.ticks > 0 ? static_cast<float>(mover.elapsed) / static_cast<float>(mover.ticks) : 1.0f;
        const bool sinking = w.has<Sunk>(e);  // gliding into the water: fades as it goes under
        sprite.tint = lam::kWhite.with_alpha(static_cast<std::uint8_t>(sinking ? 255.0f * (1.0f - 0.8f * t) : 255.0f));
        if (shadow != nullptr) shadow->color.a = sinking ? 0 : 80;
    }
}

// Simulate, after puzzle_move: the hero's clip and facing from GridMover + Facing, chosen before
// the animators so it shows this tick. A step started this tick already has GridMover::moving
// set, so walking on doesn't flicker to idle. Mirroring goes through Animator::facing (0 right,
// 1 left) because the animators rewrite kFlipX every tick.
void animate_hero(lam::Ctx &ctx) {
    World &w = ctx.world();
    const lam::SpriteSheet *sheet = ctx.res<lam::AssetServer>().get(ctx.res<Art>().hero);
    if (sheet == nullptr) return;
    static constexpr std::array<lam::StrId, 3> kIdle{lam::str_id("idle_bare_down"), lam::str_id("idle_bare_up"),
                                                     lam::str_id("idle_bare_side")};
    static constexpr std::array<lam::StrId, 3> kWalk{lam::str_id("walk_bare_down"), lam::str_id("walk_bare_up"),
                                                     lam::str_id("walk_bare_side")};
    for (auto [e, anim, mover, facing] : w.view<lam::Animator, lam::GridMover, Facing>()) {
        (void)e;
        const std::size_t side = facing.dir == Dir::Down ? 0 : facing.dir == Dir::Up ? 1 : 2;
        lam::play(anim, *sheet, mover.moving ? kWalk[side] : kIdle[side]);  // no-op while it plays
        anim.facing = facing.dir == Dir::Left ? 1 : 0;
    }
}

void feedback(lam::Ctx &ctx) {
    if (ctx.local<Board>().quiet) return;
    lam::ToastQueue *toasts = ctx.try_res<lam::ToastQueue>();
    for (const PuzzleEvent &e : ctx.read<PuzzleEvent>()) {
        using K = PuzzleEvent::Kind;
        if (toasts != nullptr) {
            if (e.kind == K::Stuck) toasts->push("That stone can no longer reach a slab: undo (Z)", 3.0f, kWarn);
            if (e.kind == K::NoHint) toasts->push("No way on from here: undo (Z)", 3.0f, kWarn);
            if (e.kind == K::Restart) toasts->push("From the start again (Z brings it back)");
        }
        if (e.kind == K::Solved) {
            lam::Audio *audio = ctx.try_res<lam::Audio>();
            const PatharGame &g = ctx.res<PatharGame>();
            const bool last = ctx.local<Board>().index + 1 >= g.catalogue.total();
            if (audio != nullptr) audio->play_stinger(last ? ctx.res<Art>().victory : ctx.res<Art>().discover, 0.3f);
        }
    }
}

void text(lam::RenderQueue &q, const lam::Font &f, std::string_view s, Vec2 at, lam::Color c, bool right = false) {
    if (right) at.x -= measure_text(f, s, 0).x;
    lam::draw_text(q, f, s, at, lam::TextStyle{.color = c, .layer = lam::kLayerUi, .shadow = lam::Color{0, 0, 0, 160}});
}

void draw_hud(lam::Ctx &ctx) {
    lam::AssetServer &assets = ctx.res<lam::AssetServer>();
    const lam::Font *font = assets.get(ctx.res<lam::RenderDefaults>().font);
    if (font == nullptr) return;
    lam::RenderQueue &q = ctx.res<lam::RenderQueue>();
    const PatharGame &g = ctx.res<PatharGame>();
    const IVec2 v = ctx.app().config().virtual_size;
    const float vw = static_cast<float>(v.x), vh = static_cast<float>(v.y);
    lam::FixedString<64> line;
    if (ctx.try_local<TitleLook>() != nullptr) {
        q.rect({0, 0, vw, vh}, lam::Color{16, 10, 8, 110}, lam::kLayerUi);  // dim the demo
        float below = 8;
        if (const lam::TextureRegion *logo = frame_region(assets, ctx.res<Art>().props, lam::str_id("logo"))) {
            q.region(*logo, {vw * 0.5f, 5.0f}, {1, 1}, 0, {0.5f, 0.0f}, lam::kWhite, lam::kLayerUi, 1);
            below = 7.0f + logo->size.y;
        }
        constexpr std::string_view kTagline = "Stone puzzles of Shantipur";
        text(q, *font, kTagline, {(vw - measure_text(*font, kTagline, 0).x) * 0.5f, below}, kAccent);
        lam::format_to(line, "{} / {} stars", g.progress.total_stars(), 3 * g.catalogue.total());
        text(q, *font, line.view(), {vw - 4, vh - 10}, kDim, true);
        return;
    }
    const Board &b = ctx.local<Board>();
    const PuzzleState &ps = ctx.local<PuzzleState>();
    if (b.index < 0) return;
    const LevelInfo &info = g.catalogue.at(b.index);
    const ChapterInfo &ch = g.catalogue.chapter_of(b.index);
    q.rect({0, 0, vw, 13}, kBar, lam::kLayerUi);
    q.rect({0, vh - 13, vw, 13}, kBar, lam::kLayerUi);
    lam::format_to(line, "{}. {}", b.index + 1, info.name);
    text(q, *font, line.view(), {4, 3}, kAccent);
    lam::format_to(line, "{} - {}", ch.name, info.english);     // the level's subtitle
    text(q, *font, line.view(), {vw - 4, 3}, kDim, true);
    int on = 0;
    for (auto [e, stone, gp] : ctx.world().view<Stone, lam::GridPosition>()) {
        (void)stone;
        if (!ctx.world().has<Sunk>(e) && b.level.inside(gp.cell) && b.level.is_goal(b.level.cell(gp.cell))) ++on;
    }
    // Single spaces and no "Esc menu" so both lines fit 320 px in the built-in font (6 px a
    // character) at par 106.
    lam::format_to(line, "Moves {} Par {} Stones {}/{}", ps.moves, b.level.par, on, b.level.goal_count());
    text(q, *font, line.view(), {4, vh - 10}, ps.helped ? kDim : kText);
    const lam::SettingsStore &settings = ctx.res<lam::SettingsStore>();
    if (settings.defined(kHudKeysSetting) && settings.get_bool(kHudKeysSetting))
        text(q, *font, "Z undo R again H hint", {vw - 4, vh - 10}, kDim, true);
}

// Music starts once its track is Ready (it streams; at startup it may still be loading).
struct MusicStarted {
    bool done = false;
};
void start_music(lam::Ctx &ctx) {
    MusicStarted &m = ctx.res<MusicStarted>();
    lam::Audio *audio = ctx.try_res<lam::Audio>();
    if (m.done || audio == nullptr) return;
    const lam::AssetId<lam::MusicTrack> track = ctx.res<Art>().music;
    const lam::LoadState state = ctx.res<lam::AssetServer>().state(track);
    if (state == lam::LoadState::Ready) audio->play_music(track);
    m.done = state == lam::LoadState::Ready || state == lam::LoadState::Failed;
}

// ---- startup: sounds and particles ------------------------------------------------------------------

// The SoundBank is built in code: groups of variants (never the same twice in a row) on a bus.
lam::AssetId<lam::SoundBank> make_sounds(lam::AssetServer &assets) {
    lam::SoundBank bank;
    auto group = [&](std::string_view id, std::initializer_list<std::string_view> files, std::string_view bus,
                     std::uint16_t min_interval_ms = 30) {
        lam::SoundBank::Group g;
        g.id = lam::intern(id);
        g.bus = lam::intern(bus);
        g.min_interval_ms = min_interval_ms;
        for (std::string_view f : files) {
            g.variants.push_back(assets.load<lam::SoundClip>(std::string("audio/sfx/") + std::string(f) + ".wav"));
        }
        bank.groups.push_back(std::move(g));
    };
    group("footstep_grass", {"footstep_grass_1", "footstep_grass_2", "footstep_grass_3"}, "sfx", 80);
    group("footstep_stone", {"footstep_stone_1", "footstep_stone_2", "footstep_stone_3"}, "sfx", 80);
    group("stone_push", {"stone_push_1", "stone_push_2", "stone_push_3"}, "sfx");
    group("stone_splash", {"stone_splash_1", "stone_splash_2"}, "sfx");
    group("stone_set", {"stone_set_1"}, "sfx");
    group("lamp_wrong", {"lamp_wrong_1"}, "sfx");
    group("bump", {"bump_1"}, "sfx", 120);
    group("undo", {"undo_1"}, "sfx", 50);
    group("gate_open", {"gate_open_1"}, "sfx");
    group("puzzle_solved", {"puzzle_solved_1"}, "sfx");
    group("item_gain", {"item_gain_1"}, "sfx");
    group("level_up", {"level_up_1"}, "sfx");
    group("ui_error", {"ui_error_1"}, "ui", 120);
    group("ui_move", {"ui_move_1"}, "ui");
    group("ui_confirm", {"ui_confirm_1"}, "ui");
    group("ui_back", {"ui_back_1"}, "ui");
    return assets.add<lam::SoundBank>(std::move(bank), "pathar.sounds");
}

PatharFx make_fx(lam::AssetServer &assets) {
    auto config = [&](float speed_min, float speed_max, float angle, float spread, float life_min, float life_max,
                      lam::Color from, lam::Color to, std::string_view name) {
        lam::EmitterConfig c;
        c.capacity = 128;
        c.speed_min = speed_min;
        c.speed_max = speed_max;
        c.angle = angle;
        c.spread = spread;
        c.life_min = life_min;
        c.life_max = life_max;
        c.size_start = 2;
        c.size_end = 0;
        c.color_start = from;
        c.color_end = to;
        c.gravity = {0, 30};
        c.drag = 1.5f;
        c.layer = lam::kLayerForeground;
        return assets.add<lam::EmitterConfig>(lam::sanitize(c), name);
    };
    PatharFx fx;
    fx.dust = config(10, 35, 0, lam::kTau, 0.3f, 0.6f, {196, 188, 174, 180}, {154, 146, 140, 0}, "pathar.dust");
    fx.splash = config(25, 70, -lam::kPi * 0.5f, lam::deg_to_rad(150), 0.3f, 0.6f, {143, 208, 189, 255},
                       {79, 163, 154, 0}, "pathar.splash");
    fx.sparkle = config(20, 70, 0, lam::kTau, 0.5f, 1.1f, {248, 221, 114, 255}, {232, 137, 43, 0}, "pathar.sparkle");
    return fx;
}

}  // namespace

const Terrain *TileTable::find(std::string_view name) const {
    for (const Terrain &t : terrains) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

int TileTable::single(std::string_view name) const {
    const Terrain *t = find(name);
    return t != nullptr && !t->base.empty() ? t->base.front() : -1;
}

std::string theme_of(lam::App &app, int index) {
    const PatharGame *g = app.try_res<PatharGame>();
    if (g == nullptr || index < 0 || index >= g->catalogue.total()) return "village";
    return g->catalogue.chapter_of(index).theme;
}

void refresh_look(lam::App &app, World &w) {
    const Board *b = w.resources().try_get<Board>();
    if (b == nullptr || b->index < 0) return;
    BoardLook *look = w.resources().try_get<BoardLook>();
    if (look == nullptr) look = &w.resources().insert<BoardLook>();
    const lam::Tileset *ts = app.res<lam::AssetServer>().get(app.res<Art>().tileset);
    if (ts == nullptr) return;  // not Ready yet: frames run while startup assets load
    const bool slabs_ok = std::all_of(look->slabs.begin(), look->slabs.end(),
                                      [&](Entity e) { return w.alive(e) && w.has<SlabLook>(e); });
    if (look->serial != b->serial || !alive_with_map(w, look->map) || !alive_with_camera(w, look->camera) || !slabs_ok)
        build_board(app, w, *look, *b, *ts);
    attach_pieces(app, w);
}

void PatharLookPlugin::build(lam::App &app) {
    app.require_plugin("pathar.sim", name());
    // Presentation-only tag: kSnapshot so undo keeps it consistent, never saved or hashed.
    lam::ComponentRegistry::global().register_type<SlabLook>(lam::kTag | lam::kSnapshot);
    lam::AssetServer &assets = app.res<lam::AssetServer>();
    // Plugin build runs with the root asset scope current: these live for the whole run.
    Art &art = app.insert_resource<Art>();
    art.tileset = assets.load<lam::Tileset>("tiles/pathar_tiles.tsj");
    art.hero = assets.load<lam::SpriteSheet>("sprites/hero.json");
    art.props = assets.load<lam::SpriteSheet>("sprites/props.json");
    art.music = assets.load<lam::MusicTrack>("audio/music/pathar_title.qoa");
    art.discover = assets.load<lam::MusicTrack>("audio/music/discover.qoa");
    art.victory = assets.load<lam::MusicTrack>("audio/music/victory.qoa");
    lam::Result<std::string> table_text = assets.read_text("tiles/pathar_tiles.json");
    lam::Result<TileTable> table = table_text ? parse_tile_table(table_text.value()) : lam::Result<TileTable>(table_text.error());
    if (!table) LAM_LOG_ERROR("pathar: {}", table.error().message);
    app.insert_resource<TileTable>(table ? std::move(table.value()) : TileTable{});
    app.insert_resource<PatharFx>(make_fx(assets));
    if (lam::Audio *audio = app.try_res<lam::Audio>()) audio->add_bank(make_sounds(assets));
    app.insert_resource<MusicStarted>();
    app.add_system({.stage = lam::Stage::Update, .fn = start_music, .name = "pathar.music", .order = 0,
                    .scope = lam::SystemScope::Once});
    const lam::RunIf board = lam::has_local<Board>;
    app.add_system({.stage = lam::Stage::Update, .fn = look_board, .name = "pathar.look_board", .order = -1700, .run_if = board});
    app.add_system({.stage = lam::Stage::Update, .fn = look_pieces, .name = "pathar.look_pieces", .order = -1600, .run_if = board});
    app.add_system({.stage = lam::Stage::Update, .fn = feedback, .name = "pathar.feedback", .order = 0, .run_if = board});
    app.add_system({.stage = lam::Stage::Simulate, .fn = animate_hero, .name = "pathar.animate_hero", .order = 10, .run_if = board});
    app.add_system({.stage = lam::Stage::RenderUI, .fn = draw_hud, .name = "pathar.hud", .order = 100, .run_if = board});
}

}  // namespace pathar
