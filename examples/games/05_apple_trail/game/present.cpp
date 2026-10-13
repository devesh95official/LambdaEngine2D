#include "present.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <lam/animation/animator.h>
#include <lam/assets/asset_server.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/tile_world.h>
#include <lam/render/camera.h>
#include <lam/render/sprite.h>
#include "trail_components.h"

namespace game {
namespace {

using trail::Dir;
using trail::TrailState;

constexpr lam::Color kGrassA{94, 140, 62, 255};
constexpr lam::Color kGrassB{86, 130, 56, 255};
constexpr lam::Color kBlade{120, 170, 80, 255};
constexpr lam::Color kWallTop{128, 116, 104, 255};
constexpr lam::Color kWallSide{88, 78, 70, 255};
constexpr lam::Color kWallChip{150, 140, 128, 255};
constexpr std::uint8_t kLayerApples = 60;                       // under the traveller (kLayerWorld)
constexpr float kTileHalf = static_cast<float>(trail::kTileSize) / 2;

// Clip tags of the traveller sheet per heading (Dir order: up, down, left, right). The sheet has
// one "side" tag; left is the side tag flipped (face_traveller).
constexpr std::array<lam::StrId, 4> kWalk{lam::str_id("walk_bare_up"), lam::str_id("walk_bare_down"),
                                          lam::str_id("walk_bare_side"), lam::str_id("walk_bare_side")};
constexpr std::array<lam::StrId, 4> kHurt{lam::str_id("hurt_up"), lam::str_id("hurt_down"),
                                          lam::str_id("hurt_side"), lam::str_id("hurt_side")};

void draw_apple(lam::RenderQueue &q, lam::Vec2 centre, float radius, bool golden, float bob) {
    const lam::Color body = golden ? kGold : lam::Color{210, 44, 36, 255};
    const lam::Color shade = golden ? lam::Color{190, 140, 30, 255} : lam::Color{150, 28, 24, 255};
    const lam::Vec2 c{centre.x, centre.y + bob};
    const float depth = centre.y;                               // nearer apples overlap farther ones;
    // the parts of one apple share a depth, so they keep submission order (RenderQueue tiebreak).
    q.ellipse({centre.x, centre.y + kTileHalf - 3}, {radius * 0.9f, 2}, {0, 0, 0, 60}, kLayerApples, depth);
    q.circle(c, radius, shade, kLayerApples, depth);
    q.circle({c.x - 1, c.y - 1}, radius - 1, body, kLayerApples, depth);
    q.rect({c.x - 2, c.y - radius + 1, 2, 2}, {255, 255, 255, 170}, kLayerApples, depth);
    q.rect({c.x, c.y - radius - 2, 1, 3}, {96, 64, 32, 255}, kLayerApples, depth);
    q.rect({c.x + 1, c.y - radius - 2, 3, 2}, {70, 160, 60, 255}, kLayerApples, depth);
}

// Render (frame time): the board, the orchard's apples and the trail, from interpolated state.
void draw_orchard(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    lam::RenderQueue &q = ctx.res<lam::RenderQueue>();
    const lam::TileWorld &tiles = ctx.local<lam::TileWorld>();
    const float t = static_cast<float>(ctx.time().real_seconds);

    for (int y = 0; y < tiles.shapes.height(); ++y) {
        for (int x = 0; x < tiles.shapes.width(); ++x) {
            const lam::Rect r = tiles.tile_rect({x, y});
            if (tiles.at({x, y}) == lam::TileShape::Solid) {
                q.rect(r, kWallSide, lam::kLayerTiles, 0);
                q.rect({r.x, r.y, r.w, r.h - 4}, kWallTop, lam::kLayerTiles, 1);
                q.rect({r.x + 2, r.y + 2, 5, 3}, kWallChip, lam::kLayerTiles, 2);
                continue;
            }
            q.rect(r, (x + y) % 2 == 0 ? kGrassA : kGrassB, lam::kLayerTiles, 0);
            if ((x * 7 + y * 13) % 5 == 0)                      // a few blades, the same ones per tile
                q.rect({r.x + 4 + static_cast<float>(x % 3) * 3, r.y + 6 + static_cast<float>(y % 2) * 4, 1, 2},
                       kBlade, lam::kLayerTiles, 1);
        }
    }

    const float bob = std::sin(t * 4.0f);
    for (auto [e, apple, at] : w.view<trail::Apple, lam::Transform>()) {
        const bool golden = apple.kind == trail::AppleKind::Golden;
        // A golden apple blinks in its last two seconds.
        if (golden && apple.ticks_left < trail::kRules.golden_warn_ticks && static_cast<int>(t * 8.0f) % 2 != 0)
            continue;
        draw_apple(q, at.position, golden ? 6.0f : 5.0f, golden, bob);
    }

    const float alpha = ctx.time().alpha;
    for (auto [e, piece] : w.view<trail::Segment>()) {
        if (piece.index == 0) continue;                         // the traveller is a Sprite
        const lam::Vec2 at = lam::interpolated(w, e, alpha).position;   // lerp(PrevTransform, Transform)
        const float radius = std::max(3.0f, 5.0f - static_cast<float>(piece.index) * 0.08f);
        draw_apple(q, at, radius, false, 0);
    }
}

// RenderUI (screen space, virtual pixels): the top bar. Never shakes: the camera does not apply.
void draw_hud(lam::Ctx &ctx) {
    const TrailState &s = ctx.local<TrailState>();
    const Session &session = ctx.res<Session>();
    lam::RenderQueue &q = ctx.res<lam::RenderQueue>();
    const float w = static_cast<float>(kVirtual.x);
    q.rect({0, 0, w, kHudHeight - 4}, kHudBg, lam::kLayerUi);

    char buf[32];
    const auto label = [&](lam::Vec2 pos, bool centred, std::string_view name, int value) {
        const auto r = std::format_to_n(buf, sizeof buf, "{} {}", name, value);   // no allocation
        draw_label(ctx, std::string_view(buf, static_cast<std::size_t>(r.out - buf)), pos, 8, kHudInk, centred);
    };
    draw_label(ctx, "APPLE TRAIL", {8, 5}, 8, kGold);
    label({w / 2, 5}, true, "SCORE", s.score);
    label({w - 150, 5}, false, "TRAIL", s.length);
    label({w - 70, 5}, false, "BEST", std::max(session.best, s.score));

    for (auto [e, apple] : ctx.world().view<trail::Apple>()) {  // golden timer bar
        if (apple.kind != trail::AppleKind::Golden) continue;
        const float left = static_cast<float>(apple.ticks_left) * 60.0f / static_cast<float>(trail::kRules.golden_ticks);
        q.rect({w / 2 - 30, kHudHeight - 6, left, 2}, kGold, lam::kLayerUi);
    }
}

// PostPhysics: the traveller's clip follows the run. Animator is hashed, so it changes in a tick
// stage, before Animate so the clip starts this tick. The flip is set in Update.
void traveller_clip(lam::Ctx &ctx) {
    const TrailState &s = ctx.local<TrailState>();
    lam::Animator *anim = ctx.world().try_get<lam::Animator>(s.head);
    if (anim == nullptr) return;
    const lam::SpriteSheet *sheet = ctx.res<lam::AssetServer>().get(anim->sheet);
    if (sheet == nullptr) return;
    const auto heading = static_cast<std::size_t>(s.heading);
    const lam::StrId want = s.alive ? kWalk[heading] : kHurt[heading];
    if (anim->clip != want) lam::play(*anim, *sheet, want);   // sets clip; restarts only on a change
}

// Update: mirror the side clip when walking left. The animators rewrite kFlipX every tick (plain
// tags: no flip), and Update runs after the frame's ticks, so this wins for the rendered frame.
void face_traveller(lam::Ctx &ctx) {
    const TrailState &s = ctx.local<TrailState>();
    lam::Sprite *sprite = ctx.world().try_get<lam::Sprite>(s.head);
    if (sprite == nullptr) return;
    // The animators write Sprite.region only when the world ticks. The board under the title
    // overlay never has, so the sprite would be invisible: show the clip's first frame instead
    // (presentation only).
    if (sprite->region.size.x <= 0) {
        const lam::Animator *anim = ctx.world().try_get<lam::Animator>(s.head);
        const lam::SpriteSheet *sheet = anim != nullptr ? ctx.res<lam::AssetServer>().get(anim->sheet) : nullptr;
        const int tag = sheet != nullptr ? sheet->find_tag(anim->clip) : -1;
        if (tag >= 0) sprite->region = lam::frame_region(*sheet, sheet->tags[static_cast<std::size_t>(tag)].from);
    }
    const auto flip = static_cast<std::uint8_t>(lam::kFlipX);
    sprite->flags = static_cast<std::uint8_t>(s.heading == Dir::Left ? (sprite->flags | flip) : (sprite->flags & ~flip));
    if (!s.alive) sprite->tint = {255, 160, 160, 255};
}

} // namespace

void draw_label(const lam::Ctx &ctx, std::string_view text, lam::Vec2 pos, float size, lam::Color color,
                bool centred) {
    const lam::RenderDefaults *defaults = ctx.try_res<lam::RenderDefaults>();
    if (defaults == nullptr) return;
    const lam::Font *font = ctx.res<lam::AssetServer>().get(defaults->font);
    if (font == nullptr) return;
    if (centred) pos.x -= lam::measure_text(*font, text, size).x * 0.5f;
    lam::draw_text(ctx.res<lam::RenderQueue>(), *font, text, pos,
                   lam::TextStyle{.size = size, .color = color, .layer = lam::kLayerUi});
}

void draw_dim(const lam::Ctx &ctx, lam::Color color) {
    ctx.res<lam::RenderQueue>().rect({0, kHudHeight - 4, static_cast<float>(kVirtual.x), static_cast<float>(kVirtual.y)},
                                     color, lam::kLayerUi);
}

void PresentPlugin::build(lam::App &app) {
    constexpr auto has_run = lam::has_local<TrailState>;
    app.add_system({.stage = lam::Stage::PostPhysics, .fn = traveller_clip, .name = "present.traveller_clip",
                    .run_if = has_run});
    app.add_system({.stage = lam::Stage::Update, .fn = face_traveller, .name = "present.face", .run_if = has_run});
    app.add_system({.stage = lam::Stage::Render, .fn = draw_orchard, .name = "present.orchard", .run_if = has_run});
    app.add_system({.stage = lam::Stage::RenderUI, .fn = draw_hud, .name = "present.hud", .run_if = has_run});
}

void dress_run(lam::App &app, lam::World &w) {
    const TrailState &s = w.resources().get<TrailState>();
    w.add<lam::Sprite>(s.head, lam::Sprite{.pivot = {0.5f, 0.8125f}, .layer = lam::kLayerWorld});   // feet 6 px low
    w.add<lam::Shadow>(s.head, lam::Shadow{.radii = {6, 2}, .offset = {0, 6}});
    w.add<lam::Animator>(s.head, lam::Animator{.sheet = app.res<GameArt>().traveller,
                                               .clip = lam::str_id("idle_bare_side")});

    // The board sits centred below the HUD; only world layers follow the camera (and its shake).
    const lam::TileWorld &tiles = w.resources().get<lam::TileWorld>();
    const lam::Vec2 board{static_cast<float>(tiles.shapes.width() * tiles.tile_size),
                          static_cast<float>(tiles.shapes.height() * tiles.tile_size)};
    const lam::Entity cam = w.create();
    w.add<lam::Camera2D>(cam, lam::Camera2D{.position = {board.x / 2, board.y / 2 - kHudHeight / 2.0f}});
    w.add<lam::CameraShake>(cam, lam::CameraShake{.max_offset = 4});
}

} // namespace game
