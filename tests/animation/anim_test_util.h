#pragma once
// lam_animation test helpers: hand-built sheets, header-only PNGs and a headless app.
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <lam/animation/animation_plugin.h>
#include <lam/animation/animator.h>
#include <lam/animation/sprite_sheet.h>
#include <lam/animation/tween.h>
#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/events.h>
#include <lam/ecs/world.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>

#include "detail/animation_detail.h"

namespace anim_test {

inline bool contains(const std::string &text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// PNG signature plus an IHDR chunk. The headless decoder only reads the size.
inline std::string png_header(int w, int h) {
    std::string s = "\x89PNG\r\n\x1a\n";
    const auto be32 = [&](std::uint32_t v) {
        s.push_back(static_cast<char>((v >> 24) & 0xFF));
        s.push_back(static_cast<char>((v >> 16) & 0xFF));
        s.push_back(static_cast<char>((v >> 8) & 0xFF));
        s.push_back(static_cast<char>(v & 0xFF));
    };
    be32(13);
    s += "IHDR";
    be32(static_cast<std::uint32_t>(w));
    be32(static_cast<std::uint32_t>(h));
    s += std::string("\x08\x06\x00\x00\x00", 5); // depth 8, RGBA, no interlace
    be32(0);                                     // CRC (not checked by the header reader)
    return s;
}

struct TagDef {
    const char *name;
    std::uint16_t from, to;
    lam::AnimDirection dir = lam::AnimDirection::Forward;
    std::uint16_t repeat = 0;
};

// A sheet of `durations.size()` 16x16 frames in a row, with the given tags.
inline lam::SpriteSheet make_sheet(std::initializer_list<std::uint16_t> durations,
                                   std::initializer_list<TagDef> tags) {
    lam::SpriteSheet s;
    float x = 0;
    for (std::uint16_t d : durations) {
        lam::SheetFrame f;
        f.source = {x, 0, 16, 16};
        f.source_size = {16, 16};
        f.duration_ms = d;
        s.frames.push_back(f);
        x += 16;
    }
    for (const TagDef &t : tags) {
        s.tags.push_back(lam::SheetTag{lam::intern(t.name), t.from, t.to, t.dir, t.repeat});
    }
    s.image_size = {static_cast<int>(x), 16};
    return s;
}

// One animator driven tick by tick, collecting its events.
struct Rig {
    lam::SpriteSheet sheet;
    lam::AnimGraph graph;
    bool use_graph = false;
    lam::Animator a;
    lam::AnimParams params;
    lam::Sprite sprite;
    int tick_rate = 60;
    std::vector<lam::AnimationEvent> events;

    static void sink(void *user, const lam::AnimationEvent &ev) {
        static_cast<Rig *>(user)->events.push_back(ev);
    }
    void tick(int n = 1) {
        for (int i = 0; i < n; ++i) {
            lam::detail::tick_animator(lam::Entity{7, 1}, a, &params, &sprite, sheet,
                                       use_graph ? &graph : nullptr, tick_rate, &sink, this);
        }
    }
    // Frames shown after each of n ticks.
    std::vector<int> frames(int n) {
        std::vector<int> out;
        for (int i = 0; i < n; ++i) {
            tick();
            out.push_back(a.frame);
        }
        return out;
    }
};

// HeadlessApp with AssetsPlugin (root = dir) and AnimationPlugin.
struct AnimApp {
    lam::test::TempDir dir;
    lam::test::HeadlessApp h;
    explicit AnimApp(lam::AppConfig config = {}) : h(config) {
        h.add<lam::AssetsPlugin>(lam::AssetsConfig{.root = dir.path()});
        h.add<lam::AnimationPlugin>();
    }
    lam::App &app() { return h.app(); }
    lam::World &world() { return h.app().main_world(); }
    lam::AssetServer &assets() { return h.app().res<lam::AssetServer>(); }
    void write(std::string_view name, std::string_view content) {
        (void)lam::test::write_test_file(dir, name, content);
    }
    void ticks(int n) { h.ticks(n); }
};

// AnimationEvents and TweenFinished events as a Simulate system sees them (App resource).
struct Seen {
    std::uint64_t tick = 0;                                     // world tick of the reading Simulate
    lam::AnimationEvent anim;
    lam::TweenFinished tween;
    bool is_tween = false;
};
struct EventLog {
    std::vector<Seen> items;
    std::size_t count_anim(lam::StrId name) const {
        std::size_t n = 0;
        for (const Seen &s : items) n += (!s.is_tween && s.anim.name == name) ? 1 : 0;
        return n;
    }
    std::size_t count_tween(lam::StrId tag) const {
        std::size_t n = 0;
        for (const Seen &s : items) n += (s.is_tween && s.tween.tag == tag) ? 1 : 0;
        return n;
    }
};
inline void record_events(lam::Ctx &ctx) {
    EventLog &log = ctx.res<EventLog>();
    for (const lam::AnimationEvent &e : ctx.read<lam::AnimationEvent>())
        log.items.push_back(Seen{ctx.world().tick(), e, {}, false});
    for (const lam::TweenFinished &e : ctx.read<lam::TweenFinished>())
        log.items.push_back(Seen{ctx.world().tick(), {}, e, true});
}
// Inserts EventLog and a Simulate reader; returns the log.
inline EventLog &log_events(lam::App &app) {
    EventLog &log = app.insert_resource<EventLog>();
    app.add_system(lam::Stage::Simulate, &record_events, "test.record_events");
    return log;
}

} // namespace anim_test
