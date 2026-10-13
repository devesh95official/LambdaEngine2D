// Animator playback, graph evaluation, facings, params and slices. All per-tick time is integer
// milliticks: a d ms frame lasts d * tick_rate, each tick adds round(1000 * speed). No allocation.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <lam/animation/animator.h>
#include <lam/assets/asset_server.h>
#include <lam/core/assert.h>
#include <lam/ecs/world.h>
#include <lam/physics/components.h>
#include <lam/physics/motors.h>
#include <lam/render/sprite.h>

#include "detail/animation_detail.h"

namespace lam {
namespace {

constexpr StrId kParamSpeed = str_id("speed");
constexpr StrId kParamVx = str_id("vx");
constexpr StrId kParamVy = str_id("vy");
constexpr StrId kParamGrounded = str_id("grounded");
constexpr StrId kParamMoving = str_id("moving");
constexpr StrId kParamClimbing = str_id("climbing");
// Speeds at or below this (px/s) count as standing still for the "moving" param and facing.
constexpr float kMovingSpeed = 1.0f;

bool forward_dir(AnimDirection d) {
    return d == AnimDirection::Forward || d == AnimDirection::PingPong;
}

bool valid_tag(const SpriteSheet &s, int tag) {
    if (tag < 0 || static_cast<std::size_t>(tag) >= s.tags.size()) return false;
    const SheetTag &t = s.tags[static_cast<std::size_t>(tag)];
    return t.from <= t.to && t.to < s.frames.size();
}

std::uint32_t frame_milliticks(const SpriteSheet &s, std::uint16_t frame, int tick_rate) {
    const std::uint64_t d = std::uint64_t(s.frames[frame].duration_ms) *
                            std::uint64_t(std::max(tick_rate, 1));
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(d, 1, 0xFFFFFFFFu));
}

} // namespace

// ---- params, facings -------------------------------------------------------------------------

void set_param(AnimParams &p, StrId name, float value) {
    LAM_ASSERT(static_cast<bool>(name), "set_param: empty param name");
    AnimParam *free_slot = nullptr;
    for (AnimParam &slot : p.values) {
        if (slot.name == name) {
            slot.value = value;
            return;
        }
        if (!slot.name && free_slot == nullptr) free_slot = &slot;
    }
    LAM_ASSERT(free_slot != nullptr, "set_param: AnimParams holds 8 params already");
    if (free_slot != nullptr && name) {
        free_slot->name = name;
        free_slot->value = value;
    }
}

float get_param(const AnimParams &p, StrId name) {
    if (!name) return 0.0f;
    for (const AnimParam &slot : p.values) {
        if (slot.name == name) return slot.value;
    }
    return 0.0f;
}

void fire_trigger(AnimParams &p, StrId name) {
    if (!name) return;
    for (StrId &t : p.triggers) {
        if (t == name) return;
        if (!t) {
            t = name;
            return;
        }
    }
}

std::uint8_t facing_index(Vec2 dir, std::uint8_t dir_count, std::uint8_t current) {
    const std::uint8_t n = detail::normalize_dirs(dir_count);
    const std::uint8_t keep = current < n ? current : std::uint8_t{0};
    if (n == 1) return 0;
    if (!std::isfinite(dir.x) || !std::isfinite(dir.y) || (dir.x == 0.0f && dir.y == 0.0f))
        return keep;
    const float ax = std::fabs(dir.x);
    const float ay = std::fabs(dir.y);
    if (n == 2) {
        if (dir.x > 0.0f) return 0;
        if (dir.x < 0.0f) return 1;
        return keep;
    }
    if (n == 4) {
        const std::uint8_t h = dir.x > 0.0f ? 0 : 2;
        const std::uint8_t v = dir.y > 0.0f ? 1 : 3;
        if (ax > ay) return h;
        if (ay > ax) return v;
        return (keep == h || keep == v) ? keep : h;
    }
    // 8 directions: sectors are 45 degrees wide, centred on the axes and diagonals.
    constexpr float kTan22 = 0.41421356237f; // tan(22.5 degrees)
    const std::uint8_t h = dir.x > 0.0f ? 0 : 4;
    const std::uint8_t v = dir.y > 0.0f ? 2 : 6;
    std::uint8_t d = 0;
    if (dir.x > 0.0f) d = dir.y > 0.0f ? 1 : 7;
    else d = dir.y > 0.0f ? 3 : 5;
    const float h_edge = ax * kTan22; // ay below this: horizontal
    const float v_edge = ay * kTan22; // ax below this: vertical
    if (ay < h_edge) return h;
    if (ax < v_edge) return v;
    if (ay > h_edge && ax > v_edge) return d;
    // Exactly on a sector boundary.
    if (ay == h_edge) return (keep == h || keep == d) ? keep : h;
    return (keep == v || keep == d) ? keep : v;
}

StrId clip_tag(const AnimClip &clip, std::uint8_t facing, std::uint8_t facing_dirs, bool *flip_x) {
    const std::uint8_t space = detail::normalize_dirs(facing_dirs);
    const std::uint8_t n = detail::normalize_dirs(clip.dir_count);
    const std::uint8_t f = detail::convert_facing(facing < space ? facing : 0, space, n);
    bool flip = false;
    StrId tag;
    if (n == 1) {
        tag = clip.tags[0];
        flip = clip.mirror_left && detail::facing_is_left(facing < space ? facing : 0, space);
    } else {
        const bool left = detail::facing_is_left(f, n);
        // Mirror of facing k about the vertical axis: (n/2 - k) mod n.
        const std::uint8_t mirror = static_cast<std::uint8_t>((n / 2 + n - f) % n);
        if (left && clip.mirror_left && clip.tags[mirror]) {
            tag = clip.tags[mirror];
            flip = true;
        } else if (clip.tags[f]) {
            tag = clip.tags[f];
        } else if (left && clip.tags[mirror]) {
            tag = clip.tags[mirror];
            flip = true;
        } else {
            tag = clip.tags[0];
        }
    }
    if (flip_x != nullptr) *flip_x = flip;
    return tag;
}

int AnimGraph::find_clip(StrId name) const {
    if (!name) return -1;
    for (std::size_t i = 0; i < clips.size(); ++i) {
        if (clips[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

// At least a left/right space: a graph of 1-tag clips must still face left, so its clips flip
// (mirror_left) like a graph-less animator's tag does.
std::uint8_t AnimGraph::facing_dirs() const {
    std::uint8_t n = 2;
    for (const AnimClip &c : clips) n = std::max(n, detail::normalize_dirs(c.dir_count));
    return n;
}

// ---- play, slices ----------------------------------------------------------------------------

void play(Animator &a, const SpriteSheet &s, StrId tag, bool restart) {
    const int index = s.find_tag(tag);
    if (!restart && a.clip == tag && a.tag >= 0 && a.tag == index) return;
    a.clip = tag;
    if (!valid_tag(s, index)) return;
    detail::start_tag(a, s, index);
    a.tag = -1; // the animators system starts it this tick and emits the first frame's events
}

std::optional<Rect> current_slice(const Animator &a, const SpriteSheet &s, StrId name, bool flip_x) {
    return current_slice(a, s, name, flip_x, Vec2{0.5f, 1.0f});
}

std::optional<Rect> current_slice(const Animator &a, const SpriteSheet &s, StrId name, bool flip_x,
                                  Vec2 pivot) {
    const int index = s.find_slice(name);
    if (index < 0 || a.frame >= s.frames.size()) return std::nullopt;
    const SheetSlice &slice = s.slices[static_cast<std::size_t>(index)];
    const SliceKey *key = nullptr;
    for (const SliceKey &k : slice.keys) {
        if (k.frame > a.frame) break;
        key = &k;
    }
    if (key == nullptr || key->bounds.empty()) return std::nullopt;
    const Vec2 size = s.frames[a.frame].source_size;
    const Rect &b = key->bounds;
    const float px = pivot.x * size.x;
    const float py = pivot.y * size.y;
    // Mirrored about the pivot, as the renderer draws a kFlipX sprite (texel u at px - u).
    const float x = flip_x ? px - b.x - b.w : b.x - px;
    return Rect{x, b.y - py, b.w, b.h};
}

// ---- playback core ---------------------------------------------------------------------------

namespace detail {

std::uint8_t normalize_dirs(std::uint8_t dirs) {
    if (dirs >= 8) return 8;
    if (dirs >= 4) return 4;
    if (dirs >= 2) return 2;
    return 1;
}

bool facing_is_left(std::uint8_t facing, std::uint8_t dirs) {
    const int n = normalize_dirs(dirs);
    if (n == 1) return false;
    const int k = facing % n;
    return 4 * k > n && 4 * k < 3 * n;
}

std::uint8_t convert_facing(std::uint8_t facing, std::uint8_t from_dirs, std::uint8_t to_dirs) {
    const std::uint8_t from = normalize_dirs(from_dirs);
    const std::uint8_t to = normalize_dirs(to_dirs);
    const std::uint8_t f = static_cast<std::uint8_t>(facing % from);
    if (to == from) return f;
    if (to == 1) return 0;
    if (to > from) return static_cast<std::uint8_t>(f * (to / from));
    if (to == 2) return facing_is_left(f, from) ? 1 : 0;
    // 8 -> 4: axes map directly; diagonals prefer their horizontal neighbour.
    if (f % 2 == 0) return static_cast<std::uint8_t>(f / 2);
    return (f == 1 || f == 7) ? 0 : 2;
}

std::uint32_t speed_milliticks(float speed) {
    if (!std::isfinite(speed) || !(speed > 0.0f)) return 0;
    const double m = std::floor(static_cast<double>(speed) * 1000.0 + 0.5);
    return static_cast<std::uint32_t>(std::min(m, 1.0e6));
}

void start_tag(Animator &a, const SpriteSheet &s, int tag) {
    LAM_ASSERT(valid_tag(s, tag), "start_tag: invalid tag");
    if (!valid_tag(s, tag)) return;
    const SheetTag &t = s.tags[static_cast<std::size_t>(tag)];
    const bool fwd = forward_dir(t.dir);
    a.tag = static_cast<std::int16_t>(tag);
    a.frame = fwd ? t.from : t.to;
    a.ping_dir = fwd ? std::int8_t{1} : std::int8_t{-1};
    a.elapsed_milliticks = 0;
    a.plays = 0;
    a.finished = false;
}

bool advance_animator(Animator &a, const SpriteSheet &s, std::uint32_t milliticks, int tick_rate,
                      FrameSink sink, void *user) {
    if (a.finished || !a.playing || !valid_tag(s, a.tag)) return false;
    const SheetTag &t = s.tags[static_cast<std::size_t>(a.tag)];
    if (a.frame < t.from || a.frame > t.to) a.frame = forward_dir(t.dir) ? t.from : t.to;
    const std::uint64_t sum = std::uint64_t(a.elapsed_milliticks) + milliticks;
    a.elapsed_milliticks = static_cast<std::uint32_t>(std::min<std::uint64_t>(sum, 0xFFFFFFFFu));
    const bool ping = t.dir == AnimDirection::PingPong || t.dir == AnimDirection::PingPongReverse;
    for (int steps = 0;; ++steps) {
        const std::uint32_t dur = frame_milliticks(s, a.frame, tick_rate);
        if (a.elapsed_milliticks < dur) return false;
        if (steps == kMaxFrameStepsPerTick) {
            a.elapsed_milliticks = 0; // drop the rest: bounded work per tick
            return false;
        }
        a.elapsed_milliticks -= dur;
        // Where the playhead goes after a.frame; pass_end when a pass (or sweep) ends here.
        bool pass_end = false;
        std::uint16_t next = a.frame;
        if (!ping) {
            if (t.dir == AnimDirection::Forward) {
                if (a.frame < t.to) next = static_cast<std::uint16_t>(a.frame + 1);
                else {
                    pass_end = true;
                    next = t.from;
                }
            } else {
                if (a.frame > t.from) next = static_cast<std::uint16_t>(a.frame - 1);
                else {
                    pass_end = true;
                    next = t.to;
                }
            }
        } else if (t.from == t.to) {
            pass_end = true;
        } else if (a.ping_dir >= 0) {
            if (a.frame < t.to) next = static_cast<std::uint16_t>(a.frame + 1);
            else {
                pass_end = true;
                next = static_cast<std::uint16_t>(t.to - 1);
            }
        } else {
            if (a.frame > t.from) next = static_cast<std::uint16_t>(a.frame - 1);
            else {
                pass_end = true;
                next = static_cast<std::uint16_t>(t.from + 1);
            }
        }
        if (pass_end) {
            if (a.plays < 0xFFFFu) ++a.plays;
            if (t.repeat > 0 && a.plays >= t.repeat) {
                a.finished = true;
                a.elapsed_milliticks = 0;
                return true; // holds the last frame shown
            }
            if (ping && t.from != t.to) a.ping_dir = static_cast<std::int8_t>(-a.ping_dir);
        }
        a.frame = next;
        if (sink != nullptr) sink(user, static_cast<std::uint16_t>(a.frame - t.from));
    }
}

namespace {

// The tag a graph-less or graph animator should show now (by clip and facing).
struct Resolved {
    int tag = -1;
    int clip = -1;        // graph clip index, -1 for a direct tag
    bool flip = false;
    StrId tag_name;
};

Resolved resolve(const Animator &a, const SpriteSheet &s, const AnimGraph *g) {
    Resolved r;
    if (g != nullptr) {
        r.clip = g->find_clip(a.clip);
        if (r.clip >= 0) {
            r.tag_name = clip_tag(g->clips[static_cast<std::size_t>(r.clip)], a.facing,
                                  g->facing_dirs(), &r.flip);
        } else {
            r.tag_name = a.clip;
            r.flip = facing_is_left(a.facing, g->facing_dirs());
        }
    } else {
        r.tag_name = a.clip;
        r.flip = facing_is_left(a.facing, 2);
    }
    if (!r.tag_name) return r;
    if (valid_tag(s, a.tag) && s.tags[static_cast<std::size_t>(a.tag)].name == r.tag_name)
        r.tag = a.tag;
    else
        r.tag = s.find_tag(r.tag_name);
    if (!valid_tag(s, r.tag)) r.tag = -1;
    return r;
}

bool has_trigger(const AnimParams *p, StrId name) {
    if (p == nullptr) return false;
    for (StrId t : p->triggers) {
        if (t == name) return true;
    }
    return false;
}

void consume_trigger(AnimParams *p, StrId name) {
    if (p == nullptr) return;
    for (StrId &t : p->triggers) {
        if (t == name) t = StrId{};
    }
}

// Fires the first matching transition: sets clip and tag = -1 (start pending). True if fired.
bool fire_transition(Animator &a, const AnimGraph &g, AnimParams *p, int current) {
    for (const AnimTransition &tr : g.transitions) {
        if (tr.from != kAnyClip && static_cast<int>(tr.from) != current) continue;
        if (tr.to >= g.clips.size()) continue;
        if (static_cast<int>(tr.to) == current && !tr.trigger) continue;
        if (tr.on_finish && !a.finished) continue;
        if (tr.param) {
            const float v = p != nullptr ? get_param(*p, tr.param) : 0.0f;
            if (tr.greater ? !(v > tr.threshold) : !(v < tr.threshold)) continue;
        }
        if (tr.trigger && !has_trigger(p, tr.trigger)) continue;
        if (tr.trigger) consume_trigger(p, tr.trigger);
        a.clip = g.clips[tr.to].name;
        a.tag = -1;
        return true;
    }
    return false;
}

struct EventCtx {
    Entity entity;
    const AnimGraph *graph = nullptr;
    StrId tag_name;
    StrId clip_name;
    EventSink emit = nullptr;
    void *user = nullptr;
};

void emit_frame_events(void *user, std::uint16_t relative_frame) {
    const EventCtx &c = *static_cast<const EventCtx *>(user);
    if (c.graph == nullptr || c.emit == nullptr) return;
    for (const FrameEvent &ev : c.graph->frame_events) {
        if (ev.frame != relative_frame) continue;
        if (ev.tag != c.tag_name && !(c.clip_name && ev.tag == c.clip_name)) continue;
        c.emit(c.user, AnimationEvent{c.entity, ev.name, relative_frame});
    }
}

// Starts tag r.tag and reports its first frame.
void start_and_report(Animator &a, const SpriteSheet &s, const Resolved &r, EventCtx &ec) {
    start_tag(a, s, r.tag);
    ec.tag_name = r.tag_name;
    ec.clip_name = r.clip >= 0 ? ec.graph->clips[static_cast<std::size_t>(r.clip)].name : StrId{};
    if (a.playing) {
        emit_frame_events(&ec, static_cast<std::uint16_t>(
                                   a.frame - s.tags[static_cast<std::size_t>(r.tag)].from));
    }
}

// True when sheet tag `tag` is one of the facing tags of graph clip `clip`.
bool clip_has_tag(const AnimGraph *g, int clip, const SpriteSheet &s, int tag) {
    if (g == nullptr || clip < 0 || !valid_tag(s, tag)) return false;
    const AnimClip &c = g->clips[static_cast<std::size_t>(clip)];
    const StrId name = s.tags[static_cast<std::size_t>(tag)].name;
    const std::uint8_t n = normalize_dirs(c.dir_count);
    for (std::uint8_t i = 0; i < n; ++i) {
        if (c.tags[i] == name) return true;
    }
    return false;
}

// Moves a playing animator from tag `from` to tag `to` of the same clip (a facing change): the
// position inside the tag, the time on the frame and the pass count are kept.
void remap(Animator &a, const SpriteSheet &s, int from, int to) {
    const SheetTag &old_tag = s.tags[static_cast<std::size_t>(from)];
    const SheetTag &new_tag = s.tags[static_cast<std::size_t>(to)];
    const int offset = std::clamp(static_cast<int>(a.frame) - static_cast<int>(old_tag.from), 0,
                                  static_cast<int>(new_tag.to - new_tag.from));
    a.tag = static_cast<std::int16_t>(to);
    a.frame = static_cast<std::uint16_t>(new_tag.from + offset);
    if (new_tag.dir == AnimDirection::Forward) a.ping_dir = 1;
    if (new_tag.dir == AnimDirection::Reverse) a.ping_dir = -1;
}

} // namespace

void tick_animator(Entity e, Animator &a, AnimParams *params, Sprite *sprite, const SpriteSheet &s,
                   const AnimGraph *graph, int tick_rate, EventSink emit, void *user) {
    EventCtx ec{e, graph, {}, {}, emit, user};
    if (graph != nullptr) {
        if (!a.clip && graph->start < graph->clips.size()) a.clip = graph->clips[graph->start].name;
        // Param- and trigger-driven switches take effect before this tick's time is spent.
        fire_transition(a, *graph, params, graph->find_clip(a.clip));
    }
    Resolved r = resolve(a, s, graph);
    if (r.tag >= 0) {
        bool started = false;
        if (!valid_tag(s, a.tag)) {
            start_and_report(a, s, r, ec);
            started = true;
        } else if (a.tag != r.tag) {
            if (clip_has_tag(graph, r.clip, s, a.tag)) {
                remap(a, s, a.tag, r.tag);
            } else {
                start_and_report(a, s, r, ec);
                started = true;
            }
        }
        if (!started) {
            ec.tag_name = r.tag_name;
            ec.clip_name = (graph != nullptr && r.clip >= 0)
                               ? graph->clips[static_cast<std::size_t>(r.clip)].name
                               : StrId{};
            const bool finished_now = advance_animator(a, s, speed_milliticks(a.speed), tick_rate,
                                                       &emit_frame_events, &ec);
            // Clip-end switches happen on the tick the clip ends: the last frame lasts exactly.
            if (finished_now && graph != nullptr &&
                fire_transition(a, *graph, params, r.clip)) {
                r = resolve(a, s, graph);
                if (r.tag >= 0) start_and_report(a, s, r, ec);
            }
        }
    }
    if (graph != nullptr && params != nullptr) params->triggers = {};
    if (sprite != nullptr && a.frame < s.frames.size()) {
        sprite->region = frame_region(s, a.frame);
        if (r.flip) sprite->flags = static_cast<std::uint8_t>(sprite->flags | kFlipX);
        else sprite->flags = static_cast<std::uint8_t>(sprite->flags & ~kFlipX);
    }
}

// ---- systems ---------------------------------------------------------------------------------

namespace {

// Remembers the last asset looked up: neighbouring entities usually share sheets and graphs.
template <class T> struct AssetCache {
    AssetId<T> id;
    const T *value = nullptr;
    bool valid = false;
    const T *get(const AssetServer &assets, AssetId<T> want) {
        if (!want) return nullptr;
        if (!valid || want != id) {
            id = want;
            value = assets.get<T>(want);
            valid = true;
        }
        return value;
    }
};

void emit_to_world(void *user, const AnimationEvent &ev) {
    static_cast<Ctx *>(user)->emit(ev);
}

void set_if_room(AnimParams &p, StrId name, float value) {
    for (AnimParam &slot : p.values) {
        if (slot.name == name) {
            slot.value = value;
            return;
        }
    }
    for (AnimParam &slot : p.values) {
        if (!slot.name) {
            slot.name = name;
            slot.value = value;
            return;
        }
    }
}

} // namespace

void animators_system(Ctx &ctx) {
    const AssetServer *assets = ctx.try_res<AssetServer>();
    if (assets == nullptr) return;
    World &w = ctx.world();
    const int tick_rate = ctx.time().tick_rate;
    AssetCache<SpriteSheet> sheets;
    AssetCache<AnimGraph> graphs;
    w.view<Animator>().each([&](Entity e, Animator &a) {
        const AnimGraph *g = graphs.get(*assets, a.graph);
        if (a.graph && g == nullptr) return; // graph not Ready yet
        const SpriteSheet *s = sheets.get(*assets, a.sheet ? a.sheet : (g ? g->sheet : a.sheet));
        if (s == nullptr) return;
        tick_animator(e, a, w.try_get<AnimParams>(e), w.try_get<Sprite>(e), *s, g, tick_rate,
                      &emit_to_world, &ctx);
    });
}

void motor_anim_bridge_system(Ctx &ctx) {
    World &w = ctx.world();
    const AssetServer *assets = ctx.try_res<AssetServer>();
    AssetCache<AnimGraph> graphs;
    w.view<Animator>(With<MotorAnimBridge>{}).each([&](Entity e, Animator &a) {
        const Velocity *vel = w.try_get<Velocity>(e);
        const PlatformerMotor *pm = w.try_get<PlatformerMotor>(e);
        const TopDownMotor *tm = w.try_get<TopDownMotor>(e);
        const Vec2 v = vel != nullptr ? vel->linear : Vec2{};
        float speed = 0, grounded = 1, climbing = 0;
        Vec2 face{};
        if (pm != nullptr) {
            speed = std::fabs(v.x);
            grounded = pm->on_ground ? 1.0f : 0.0f;
            climbing = (pm->on_ladder && !pm->on_ground) ? 1.0f : 0.0f;
            if (std::fabs(pm->move_x) > 0.01f) face = Vec2{pm->move_x, 0};
        } else {
            speed = length(v);
            if (tm != nullptr) {
                if (length_sq(tm->move) > 1e-4f) face = tm->move;
            } else if (speed > kMovingSpeed) {
                face = v;
            }
        }
        const float moving = speed > kMovingSpeed ? 1.0f : 0.0f;
        if (AnimParams *p = w.try_get<AnimParams>(e)) {
            set_if_room(*p, kParamSpeed, speed);
            set_if_room(*p, kParamVx, v.x);
            set_if_room(*p, kParamVy, v.y);
            set_if_room(*p, kParamGrounded, grounded);
            set_if_room(*p, kParamMoving, moving);
            set_if_room(*p, kParamClimbing, climbing);
        }
        const AnimGraph *g = assets != nullptr ? graphs.get(*assets, a.graph) : nullptr;
        const std::uint8_t dirs = g != nullptr ? g->facing_dirs() : std::uint8_t{2};
        a.facing = facing_index(face, dirs, a.facing);
    });
}

void register_bridge_param_names() {
    register_id_name(kParamSpeed, "speed");
    register_id_name(kParamVx, "vx");
    register_id_name(kParamVy, "vy");
    register_id_name(kParamGrounded, "grounded");
    register_id_name(kParamMoving, "moving");
    register_id_name(kParamClimbing, "climbing");
}

} // namespace detail
} // namespace lam
