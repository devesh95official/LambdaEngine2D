#pragma once
// lam_animation internals shared by the systems, the plugin and white-box tests. Not installed.
#include <cstdint>
#include <memory>
#include <lam/animation/animator.h>
#include <lam/animation/sprite_sheet.h>
#include <lam/animation/tween.h>
#include <lam/core/app.h>
#include <lam/ecs/transform.h>
#include <lam/render/sprite.h>
namespace lam {
class AssetServer;
template <class T> class AssetLoader;
namespace detail {
// Called once per frame entered, with the frame relative to the tag's `from`.
using FrameSink = void (*)(void *user, std::uint16_t relative_frame);
// 1, 2, 4 or 8 (other counts round down; 0 -> 1).
std::uint8_t normalize_dirs(std::uint8_t dirs);
// True for facings whose direction points left (x < 0) among `dirs` facings.
bool facing_is_left(std::uint8_t facing, std::uint8_t dirs);
// A facing of a `from_dirs` space expressed in a `to_dirs` space (to_dirs <= from_dirs keeps the
// nearest direction; diagonals prefer horizontal; 2-space keeps only left/right).
std::uint8_t convert_facing(std::uint8_t facing, std::uint8_t from_dirs, std::uint8_t to_dirs);
// round(1000 * speed) clamped to [0, 1e6]; non-finite or negative speeds give 0.
std::uint32_t speed_milliticks(float speed);
// Puts `a` on the first frame of sheet tag `tag` (by direction), resetting time, plays and
// finished. LAM_ASSERT(tag in range).
void start_tag(Animator &a, const SpriteSheet &s, int tag);
// Advances a started animator (a.tag valid) by `milliticks`; calls sink for every frame entered.
// Returns true if the tag finished in this call. No allocation.
bool advance_animator(Animator &a, const SpriteSheet &s, std::uint32_t milliticks, int tick_rate,
                      FrameSink sink, void *user);
// Emits one AnimationEvent per (entity, event) pair.
using EventSink = void (*)(void *user, const AnimationEvent &ev);
// The animators system for one entity, exposed for tests. No allocation.
void tick_animator(Entity e, Animator &a, AnimParams *params, Sprite *sprite, const SpriteSheet &s,
                   const AnimGraph *graph, int tick_rate, EventSink emit, void *user);
// One tick of a Tween. `xf` is Parent::local for children, else the Transform; null skips
// Position/Scale/Rotation. Returns true when the tween finished.
using StepDoneSink = void (*)(void *user, StrId tag, bool last);
bool tick_tween(Tween &t, Transform *xf, Sprite *sprite, StepDoneSink done, void *user);
// Applies frame time `dt` (seconds) to a FrameTween; tick_dt converts TweenSpec ticks to seconds.
// Returns true when it finished. No allocation.
bool tick_frame_tween(FrameTween &t, float dt, float tick_dt, RenderOffset *offset, Sprite *sprite);
// Systems.
void motor_anim_bridge_system(Ctx &ctx);                        // Animate PerWorld -1500
void animators_system(Ctx &ctx);                                // Animate PerWorld -1400
void tweens_system(Ctx &ctx);                                   // Animate PerWorld -1300
void frame_tweens_system(Ctx &ctx);                             // Update PerWorld -1500
// Registers the names of the params the bridge writes (saves print names, not hashes).
void register_bridge_param_names();
// Asset loaders (".json"/".png" SpriteSheet, ".anim.json" AnimGraph).
std::unique_ptr<AssetLoader<SpriteSheet>> make_sprite_sheet_loader();
std::unique_ptr<AssetLoader<AnimGraph>> make_anim_graph_loader();
}
}
