#pragma once
// Animator: plays sheet tags or AnimGraph clips on tick time, in integer milliticks, so frame
// events and hitboxes replay exactly. MotorAnimBridge feeds graph params from physics motors.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <lam/animation/sprite_sheet.h>
#include <lam/core/asset_id.h>
#include <lam/core/id.h>
#include <lam/core/result.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/rect.h>
#include <lam/math/vec2.h>
namespace lam {
// A graph clip: the sheet tag per facing. Facings go clockwise from right (2: right, left;
// 4: right, down, left, up; 8 adds diagonals). With mirror_left, left facings flip the right tag.
struct AnimClip {
    StrId name;                                                 // unique in the graph; Animator::clip names it
    std::array<StrId, 8> tags{};                                // tag per facing (dir_count used)
    std::uint8_t dir_count = 1;                                 // 1, 2 (mirror), 4 or 8
    bool mirror_left = true;                                    // left facings play the right tag flipped
};
// A clip index meaning "from any clip" in AnimTransition::from.
inline constexpr std::uint8_t kAnyClip = 255;
// Switches to `to` when every condition that is set holds. Tried in file order, first match wins.
// A trigger is consumed, and only a trigger can restart the current clip.
struct AnimTransition {
    std::uint8_t from = 0, to = 0;                              // clip indices; from = 255: any
    StrId param;                                                // empty: no param condition
    float threshold = 0.5f;                                     // compared strictly (> or <)
    bool greater = true;                                        // param > threshold (else <)
    bool on_finish = false;                                     // only once the current tag finished
    StrId trigger;                                              // empty: no trigger condition
};
// Emits AnimationEvent when the animator enters `frame` of the tag (or clip) named `tag`.
struct FrameEvent { StrId tag; std::uint16_t frame = 0; StrId name; };   // frame relative to the tag start
// The .anim.json asset, format in src/animation/README.md. Its sheet is a load dependency, and
// finalize checks every tag against it.
struct AnimGraph {                                              // asset ".anim.json"
    AssetId<SpriteSheet> sheet;                                 // "sheet" dependency (none if not named)
    std::vector<AnimClip> clips;                                // 1..255, names unique
    std::vector<AnimTransition> transitions;                    // tried in order; first match wins
    std::vector<FrameEvent> frame_events;                       // AnimationEvents by tag/clip and frame
    std::uint8_t start = 0;                                     // clip played when Animator::clip is empty
    std::string sheet_path;                                     // "sheet" as written in the file ("" = none)
    int find_clip(StrId name) const;                            // -1 if missing; linear (clips are few)
    std::uint8_t facing_dirs() const;                           // largest clip dir_count, >= 2
};
// Plays a sheet tag or graph clip on tick time and writes Sprite.region/kFlipX. Only runs on
// ticks, so a world frozen from its first frame needs frame_region() set by hand.
struct Animator {                                               // component; writes Sprite.region and kFlipX
    AssetId<SpriteSheet> sheet;                                 // none: the graph's sheet
    AssetId<AnimGraph> graph;                                   // optional
    StrId clip;                                                 // tag name, or graph clip name
    std::int16_t tag = -1;                                      // resolved tag index (engine)
    std::uint16_t frame = 0;                                    // index into sheet frames
    std::uint32_t elapsed_milliticks = 0;                       // time spent on `frame`, 1/1000 tick
    std::uint16_t plays = 0;                                    // completed passes (saturates)
    float speed = 1;                                            // >= 0; non-finite or negative = 0
    std::uint8_t facing = 0;                                    // 0..dir_count-1
    std::int8_t ping_dir = 1;                                   // +1 toward `to`, -1 toward `from`
    bool finished = false;                                      // repeat count reached; holds last frame
    bool playing = true;                                        // false: frozen (no time, no events)
};
// One named float parameter of an animation graph.
struct AnimParam { StrId name; float value = 0; };
// Graph inputs. Params keep their slot once set; triggers are cleared after each graph update.
struct AnimParams {                                             // component
    std::array<AnimParam, 8> values{};                          // empty name = free slot
    std::array<StrId, 4> triggers{};                            // cleared after the graph consumes them
};
// Tag: motor_anim_bridge sets the speed, vx, vy, grounded, moving and climbing params and the
// facing from PlatformerMotor/TopDownMotor and Velocity, while AnimParams has free slots.
struct MotorAnimBridge {};
// Most frame steps one animator takes per tick (very high speeds drop the remaining time).
inline constexpr int kMaxFrameStepsPerTick = 64;
// Requests sheet tag `tag`; it starts this tick unless it's already playing and !restart.
// An unknown tag only sets clip.
void play(Animator &a, const SpriteSheet &s, StrId tag, bool restart = false);
// Facing of `dir` among dir_count directions. A zero dir, or a tie involving `current`, keeps
// `current`. No trig, so it's deterministic.
std::uint8_t facing_index(Vec2 dir, std::uint8_t dir_count, std::uint8_t current);
// Sets param `name`, taking a free slot if it's new. Asserts if all 8 are used.
void set_param(AnimParams &p, StrId name, float value);
// The value of `name`, 0 if unset.
float get_param(const AnimParams &p, StrId name);
// Fires a trigger for this tick's graph update. Duplicates merge; past 4 they're dropped.
void fire_trigger(AnimParams &p, StrId name);
// The sheet tag `clip` plays for `facing`, and whether it's mirrored. Empty if it has none.
StrId clip_tag(const AnimClip &clip, std::uint8_t facing, std::uint8_t facing_dirs, bool *flip_x);
// Aseprite slice `name` at the current frame, relative to the sprite pivot (0.5, 1). flip_x
// mirrors it the way the renderer does. nullopt if there's no such slice or key yet.
std::optional<Rect> current_slice(const Animator &a, const SpriteSheet &s, StrId name, bool flip_x);
// As above, with the sprite's own normalised pivot (Sprite::pivot).
std::optional<Rect> current_slice(const Animator &a, const SpriteSheet &s, StrId name, bool flip_x,
                                  Vec2 pivot);
// Emitted in Animate -1400 when an entity enters a FrameEvent's frame; read it in Simulate.
struct AnimationEvent { Entity entity; StrId name; std::uint16_t frame = 0; };   // World event
// Parses an .anim.json graph. Strict: unknown keys are errors. The sheet path is left in
// sheet_path for the loader.
Result<AnimGraph> parse_anim_graph(std::string_view text, std::string_view file_name);
// Checks every clip tag, facing and frame event of a graph against its sheet.
Status check_anim_graph(const AnimGraph &g, const SpriteSheet &s);
}
LAM_REFLECT_ENUM(lam::AnimDirection, "forward", "reverse", "pingpong", "pingpong_reverse")
LAM_REFLECT(lam::Animator, sheet, graph, clip, tag, frame, elapsed_milliticks, plays, speed, facing, ping_dir,
            finished, playing)
LAM_REFLECT_STRUCT(lam::AnimParam, name, value)
LAM_REFLECT(lam::AnimParams, values, triggers)
LAM_REFLECT_TAG(lam::MotorAnimBridge)
