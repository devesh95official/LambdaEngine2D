// 4-direction animation from simulation state. The sheets have the Aseprite tags "<clip>_down",
// "<clip>_up" and "<clip>_side"; left is "side"
// mirrored with kFlipX. Animator is tick-time and hashed, so the choice is made in tick
// stages from hashed state only, in two steps (SystemDesc::order):
//   PostPhysics 20  choose_clips  after the rules moved and bit: the engine's animators
//                                 (Animate -1400) advance the new clip this same tick;
//   Animate      0  face_sprites  after the animators, which rewrite Sprite kFlipX every tick.
#include <lam/animation/animator.h>
#include <lam/animation/sprite_sheet.h>
#include <lam/assets/asset_server.h>
#include <lam/core/id.h>
#include <lam/ecs/world.h>
#include <lam/physics/components.h>
#include <lam/render/sprite.h>

#include "sim/rules.h"

namespace lantern {

namespace {

using lam::str_id;
using Clip3 = lam::StrId[3];                                     // down, up, side

constexpr Clip3 kIdle{str_id("idle_down"), str_id("idle_up"), str_id("idle_side")};
constexpr Clip3 kWalk{str_id("walk_down"), str_id("walk_up"), str_id("walk_side")};
constexpr Clip3 kRoll{str_id("roll_down"), str_id("roll_up"), str_id("roll_side")};
constexpr Clip3 kHurt{str_id("hurt_down"), str_id("hurt_up"), str_id("hurt_side")};
constexpr Clip3 kWolfIdle{str_id("bhediya_idle_down"), str_id("bhediya_idle_up"), str_id("bhediya_idle_side")};
constexpr Clip3 kWolfMove{str_id("bhediya_move_down"), str_id("bhediya_move_up"), str_id("bhediya_move_side")};
constexpr Clip3 kWolfTelegraph{str_id("bhediya_telegraph_down"), str_id("bhediya_telegraph_up"),
                               str_id("bhediya_telegraph_side")};
constexpr Clip3 kWolfAttack{str_id("bhediya_attack_down"), str_id("bhediya_attack_up"),
                            str_id("bhediya_attack_side")};
constexpr Clip3 kWolfRecover{str_id("bhediya_recover_down"), str_id("bhediya_recover_up"),
                             str_id("bhediya_recover_side")};

int column(Facing f) { return f == Facing::Down ? 0 : f == Facing::Up ? 1 : 2; }

// play() needs the sheet; before it is Ready (or in tests without loaders) the clip name is
// stored and the tag left for the animators system to resolve.
void set_clip(const lam::AssetServer *assets, lam::Animator &a, lam::StrId tag) {
    const lam::SpriteSheet *sheet = assets != nullptr ? assets->get<lam::SpriteSheet>(a.sheet) : nullptr;
    if (sheet != nullptr) {
        lam::play(a, *sheet, tag);
        return;
    }
    if (a.clip == tag) return;
    a.clip = tag;
    a.tag = -1;
    a.frame = 0;
    a.elapsed_milliticks = 0;
    a.finished = false;
}

void set_flip(lam::Sprite *s, Facing f) {
    if (s == nullptr) return;
    if (f == Facing::Left) s->flags = static_cast<std::uint8_t>(s->flags | lam::kFlipX);
    else s->flags = static_cast<std::uint8_t>(s->flags & ~lam::kFlipX);
}

} // namespace

void choose_clips(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    const lam::AssetServer *assets = ctx.try_res<lam::AssetServer>();
    const float walk_min = tuning(ctx).walk_min_speed;

    for (auto [e, p, anim] : w.view<Player, lam::Animator>()) {
        const lam::Velocity *v = w.try_get<lam::Velocity>(e);
        const bool walking = v != nullptr && lam::length(v->linear) >= walk_min;
        const bool hurt = !p.alive || p.knockback.x != 0.0f || p.knockback.y != 0.0f;
        const Clip3 &clip = hurt ? kHurt : p.dashing() ? kRoll : walking ? kWalk : kIdle;
        set_clip(assets, anim, clip[column(p.facing)]);
    }
    for (auto [e, wolf, anim] : w.view<Wolf, lam::Animator>()) {
        (void)e;
        const Clip3 *clip = wolf.moving ? &kWolfMove : &kWolfIdle;
        if (wolf.fsm.in(WolfState::Telegraph)) clip = &kWolfTelegraph;
        if (wolf.fsm.in(WolfState::Lunge)) clip = &kWolfAttack;
        if (wolf.fsm.in(WolfState::Recover)) clip = &kWolfRecover;
        set_clip(assets, anim, (*clip)[column(wolf.facing)]);
    }
}

void face_sprites(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    for (auto [e, p, sprite] : w.view<Player, lam::Sprite>()) {
        (void)e;
        set_flip(&sprite, p.facing);
    }
    for (auto [e, wolf, sprite] : w.view<Wolf, lam::Sprite>()) {
        (void)e;
        set_flip(&sprite, wolf.facing);
    }
}

} // namespace lantern
