// UiPlugin in a HeadlessApp. FrameInput, RenderQueue and RenderDefaults are inserted by hand, so
// most tests run without those plugins. The queue_intent test uses the real InputPlugin.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/input/input_plugin.h>
#include <lam/input/raw_input.h>
#include <lam/math/rng.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>
#include <lam/ui/toasts.h>
#include <lam/ui/ui.h>
#include <lam/ui/ui_plugin.h>

#include "ui_test_util.h"

using namespace lam;
using namespace lam::literals;

namespace {

// Hand-picked action ids: these apps have no InputPlugin/ActionMap, so UiNav is wired by hand.
constexpr ButtonAction kUp{1}, kDown{2}, kLeft{3}, kRight{4}, kConfirm{5}, kBack{6};

void set_bit(ActionBits &bits, ButtonAction a) {
    bits[a.id >> 6] |= std::uint64_t{1} << (a.id & 63u);
}

struct Seen {                                                   // App resource filled by `reader`
    std::vector<UiEvent> events;
    std::uint64_t frames = 0;
};

// A game system in Update after ui_update: reads the world's UiEvent window.
void reader(Ctx &ctx) {
    Seen &seen = ctx.res<Seen>();
    for (const UiEvent &e : ctx.read<UiEvent>()) seen.events.push_back(e);
}

// Stand-in for render_clear (FrameEnd +9000): UiApp has no RenderPlugin.
void clear_queue(Ctx &ctx) {
    if (RenderQueue *q = ctx.try_res<RenderQueue>()) q->clear();
}

struct UiApp {
    test::HeadlessApp h;
    UiApp() {
        h.add<AssetsPlugin>();
        h.add<UiPlugin>();
        h.app().insert_resource<FrameInput>();
        UiNav &nav = h.app().res<UiNav>();
        nav = UiNav{kUp, kDown, kLeft, kRight, kConfirm, kBack, {}};
    }
    App &app() { return h.app(); }
    FrameInput &input() { return app().res<FrameInput>(); }
    UiTree &tree(World &w) { return w.resources().get<UiTree>(); }
    UiTree &tree() { return tree(app().main_world()); }
    // One frame with `a` pressed (no action: a neutral frame).
    void press(ButtonAction a = {}) {
        input() = FrameInput{};
        if (a) set_bit(input().pressed, a);
        h.frames(1);
        input() = FrameInput{};
    }
};

// The schedule line of system `name` ("Update      PerWorld   -800  ui_update  [lam.ui] ...").
std::string schedule_line(const App &app, std::string_view name) {
    std::istringstream lines(app.schedule_text());
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string stage, scope, order, sys;
        words >> stage >> scope >> order >> sys;
        if (sys == name) return stage + " " + scope + " " + order;
    }
    return "missing";
}

} // namespace

TEST_CASE("plugin: systems sit at their stage, scope and order") {
    UiApp u;
    CHECK(schedule_line(u.app(), "ui_update") == "Update PerWorld -800");
    CHECK(schedule_line(u.app(), "toasts_update") == "Update Once 500");
    CHECK(schedule_line(u.app(), "draw_ui") == "RenderUI PerWorld -1000");
    CHECK(schedule_line(u.app(), "draw_toasts") == "RenderUI Once 500");
}

TEST_CASE("plugin: every world gets a UiTree; UiNav and ToastQueue are App resources") {
    UiApp u;
    CHECK(u.app().main_world().resources().has<UiTree>());
    World &w = u.app().create_world("popup");
    CHECK(w.resources().has<UiTree>());
    CHECK(&u.tree(w) != &u.tree());
    CHECK(u.app().try_res<UiNav>() != nullptr);
    CHECK(u.app().try_res<ToastQueue>() != nullptr);
    u.h.frames(2);                                              // empty trees: nothing to do
}

TEST_CASE("plugin: ui_update navigates from FrameInput and emits UiEvent into the world") {
    UiApp u;
    u.app().insert_resource<Seen>();
    u.app().add_system(Stage::Update, reader, "test.reader", 100);
    UiTree &t = u.tree();
    const UiId menu = t.panel(0, {});
    t.get(menu)->anchor = t.get(menu)->pivot = {0.5f, 0.5f};
    const UiId play = t.button(menu, "play"_id, "Play");
    const UiId vol = t.slider(menu, "vol"_id, "Volume", 0.5f, 0, 1, 0.25f);
    t.focus(play);
    u.press();
    CHECK(t.shown(play));                                       // laid out against 480x270
    CHECK(t.screen_rect(menu).x == doctest::Approx(240 - t.screen_rect(menu).w * 0.5f).epsilon(0.01));
    u.press(kDown);
    CHECK(t.focused() == vol);
    REQUIRE(t.events().size() == 1);                            // same frame: the tree's own list
    u.press(kRight);
    CHECK(t.get(vol)->value == 0.75f);
    u.press(kUp);
    u.press(kConfirm);
    u.press(kBack);
    u.press();                                                  // the reader sees the last frame's events
    std::vector<UiEvent::Kind> kinds;
    for (const UiEvent &e : u.app().res<Seen>().events) kinds.push_back(e.kind);
    using K = UiEvent::Kind;
    CHECK(kinds == std::vector<K>{K::Focused, K::Changed, K::Focused, K::Clicked, K::Back});
    const UiEvent &changed = u.app().res<Seen>().events[1];
    CHECK(changed.id == "vol"_id);
    CHECK(changed.node == vol);
    CHECK(changed.value == 0.75f);
    CHECK(u.app().res<Seen>().events[3].id == "play"_id);
}

TEST_CASE("plugin: only the focused world's tree takes input") {
    UiApp u;
    World &top = u.app().create_world("top");
    UiTree &below = u.tree();
    UiTree &above = u.tree(top);
    const UiId b0 = below.button(0, "b0"_id, "B0");
    const UiId b1 = below.button(0, "b1"_id, "B1");
    const UiId a0 = above.button(0, "a0"_id, "A0");
    const UiId a1 = above.button(0, "a1"_id, "A1");
    below.focus(b0);
    above.focus(a0);
    const WorldEntry stack[] = {{&u.app().main_world(), true, true, false}, {&top, true, true, true}};
    u.app().set_world_stack(stack);
    u.press(kDown);
    CHECK(above.focused() == a1);
    CHECK(below.focused() == b0);                               // neutral FrameInput below
    (void)b1;
}

TEST_CASE("plugin: toasts follow frame time; draw_ui and draw_toasts push on their layers") {
    UiApp u;
    AssetServer &assets = u.app().res<AssetServer>();
    u.app().insert_resource<RenderDefaults>(RenderDefaults{test::add_test_font(assets)});
    RenderQueue &q = u.app().insert_resource<RenderQueue>(4096);
    UiTree &t = u.tree();
    t.label(0, "HP");
    ToastQueue &toasts = u.app().res<ToastQueue>();
    u.h.frames(1);                                              // the first frame has dt 0
    toasts.push("Saved", 1.0f);
    u.h.frames(1);
    REQUIRE(toasts.toasts().size() == 1);
    CHECK(toasts.toasts()[0].remaining == doctest::Approx(1.0f - 1.0f / 60.0f));
    CHECK(toasts.toasts()[0].alpha > 0.0f);
    q.clear();
    u.h.frames(1);
    int ui = 0, overlay = 0;
    for (const DrawQuad &d : q.quads()) {
        if (key_layer(d.key) == kLayerUi) ++ui;
        if (key_layer(d.key) == kLayerOverlay) ++overlay;
    }
    CHECK(ui == 2);                                             // "HP"
    CHECK(overlay == 1 + 4 + 5);                                // fill, 4 frame edges, "Saved"
    u.h.frames(60);
    CHECK(toasts.toasts().empty());
    q.clear();
    u.h.frames(1);
    overlay = 0;
    for (const DrawQuad &d : q.quads()) overlay += key_layer(d.key) == kLayerOverlay ? 1 : 0;
    CHECK(overlay == 0);
}

TEST_CASE("plugin: a tree edited after ui_update is laid out again before drawing") {
    UiApp u;
    AssetServer &assets = u.app().res<AssetServer>();
    u.app().insert_resource<RenderDefaults>(RenderDefaults{test::add_test_font(assets)});
    u.app().insert_resource<RenderQueue>(1024);
    u.app().add_system({.stage = Stage::FrameEnd, .fn = clear_queue, .name = "test.clear", .order = 9000,
                        .scope = SystemScope::Once});
    UiTree &t = u.tree();
    const UiId l = t.label(0, "a");
    u.h.frames(1);
    t.set_text(l, "abc");                                       // e.g. by a Render system
    CHECK(t.layout_dirty());
    u.h.frames(1);
    CHECK_FALSE(t.layout_dirty());
    CHECK(t.screen_rect(l).w == doctest::Approx(18 + 8));
}

TEST_CASE("plugin: the same input gives the same events and values (determinism)") {
    // Two independent trees driven by the same seeded random input stream.
    const auto run = [](std::uint64_t seed) {
        UiTree t;
        const UiId root = t.panel(0, {});
        t.button(root, "a"_id, "A");
        t.slider(root, "s"_id, "S", 0.3f, 0, 1, 0.05f);
        t.toggle(root, "t"_id, "T", false);
        const std::string_view opts[] = {"x", "y", "z"};
        t.choice(root, "c"_id, "C", opts, 0);
        const UiId list = t.list(root, {0, 0, 120, 30});
        for (int i = 0; i < 8; ++i) t.button(list, "i"_id, "Item");
        Rng rng(seed);
        std::vector<UiEvent> log;
        for (int f = 0; f < 400; ++f) {
            UiInput in;
            switch (rng.between(0, 6)) {
            case 0: in.up = true; break;
            case 1: in.down = true; break;
            case 2: in.left = true; break;
            case 3: in.right = true; break;
            case 4: in.confirm = true; break;
            default: break;
            }
            in.pointer_valid = rng.below(3) == 0;
            in.pointer = {rng.range(0.0f, 480.0f), rng.range(0.0f, 270.0f)};
            in.pointer_pressed = in.pointer_down = in.pointer_valid && rng.below(4) == 0;
            uitest::frame(t, in);
            for (const UiEvent &e : t.events()) log.push_back(e);
        }
        return log;
    };
    const std::vector<UiEvent> a = run(7), b = run(7), c = run(8);
    REQUIRE(a.size() == b.size());
    CHECK(a.size() > 50);
    bool same = true;
    for (std::size_t i = 0; i < a.size(); ++i)
        same = same && a[i].kind == b[i].kind && a[i].node == b[i].node && a[i].value == b[i].value;
    CHECK(same);
    CHECK((a.size() != c.size() || a[0].node != c[0].node || a.back().value != c.back().value ||
           a[a.size() / 2].kind != c[a.size() / 2].kind));
}

namespace {

// Scripted input for the allocation test: keys, pointer and toasts change every few frames.
void script(Ctx &ctx) {
    FrameInput &in = ctx.res<FrameInput>();
    const std::uint64_t f = ctx.time().frame;
    in = FrameInput{};
    if (f % 7 == 0) set_bit(in.pressed, kDown);
    if (f % 11 == 0) set_bit(in.pressed, kRight);
    if (f % 13 == 0) set_bit(in.pressed, kConfirm);
    if (f % 17 == 0) set_bit(in.pressed, kBack);
    in.pointer.valid = f % 50 < 25;
    in.pointer.virtual_pos = {static_cast<float>(f % 480), static_cast<float>((f * 3) % 270)};
    in.pointer.pressed = in.pointer.down = f % 19 == 0 ? 1 : 0;
    in.pointer.released = f % 19 == 1 ? 1 : 0;
    in.pointer.wheel = f % 23 == 0 ? -1.0f : 0.0f;
    if (f % 90 == 0) ctx.res<ToastQueue>().push(f % 180 == 0 ? "Saved" : "Loaded");
    if (UiTree *t = ctx.try_local<UiTree>()) {
        FixedString<32> s;
        format_to(s, "Frame {}", f);
        t->set_text(t->find("counter"_id), s.view());
    }
}

} // namespace

TEST_CASE("plugin: ui_update, draw_ui and the toasts allocate nothing in steady state") {
    UiApp u;
    AssetServer &assets = u.app().res<AssetServer>();
    u.app().insert_resource<RenderDefaults>(RenderDefaults{test::add_test_font(assets)});
    u.app().insert_resource<RenderQueue>(8192);
    u.app().insert_resource<Seen>();
    u.app().add_system({.stage = Stage::FrameBegin, .fn = script, .name = "test.script", .order = 100,
                        .scope = SystemScope::Once});
    u.app().add_system({.stage = Stage::FrameEnd, .fn = clear_queue, .name = "test.clear", .order = 9000,
                        .scope = SystemScope::Once});
    UiTree &t = u.tree();
    const UiId root = t.panel(0, {});
    t.get(root)->anchor = t.get(root)->pivot = {0.5f, 0.5f};
    UiNode counter;
    counter.kind = UiKind::Label;
    counter.id = "counter"_id;
    t.add(root, counter);
    t.slider(root, "vol"_id, "Volume", 0.5f, 0, 1);
    t.toggle(root, "full"_id, "Fullscreen", false);
    const std::string_view speeds[] = {"Slow", "Normal", "Fast"};
    t.choice(root, "speed"_id, "Speed", speeds, 1);
    const UiId list = t.list(root, {0, 0, 160, 48});
    for (int i = 0; i < 12; ++i) t.button(list, "item"_id, "Item");
    const UiId grid = t.grid(root, {0, 0, 160, 40}, 4);
    for (int i = 0; i < 12; ++i) t.button(grid, "cell"_id, "C");
    t.text_block(root, {0, 0, 160, 24}, "A text block long enough to wrap over several lines of the box.");
    t.focus(t.find("vol"_id));
    test::run_frames(u.app(), 120);                             // warm-up: every path taken once
    CHECK_NO_ALLOC(test::run_frames(u.app(), 600));
    CHECK(t.stats().dropped_events == 0);
}

TEST_CASE("plugin: frame cost of a 240-node menu (printed budget)") {
    UiApp u;
    AssetServer &assets = u.app().res<AssetServer>();
    u.app().insert_resource<RenderDefaults>(RenderDefaults{test::add_test_font(assets)});
    RenderQueue &q = u.app().insert_resource<RenderQueue>(16384);
    u.app().add_system({.stage = Stage::FrameBegin, .fn = script, .name = "test.script", .order = 100,
                        .scope = SystemScope::Once});
    u.app().add_system({.stage = Stage::FrameEnd, .fn = clear_queue, .name = "test.clear", .order = 9000,
                        .scope = SystemScope::Once});
    UiTree &t = u.tree();
    const UiId root = t.panel(0, {}, UiLayout::Horizontal);
    UiNode counter;
    counter.kind = UiKind::Label;
    counter.id = "counter"_id;
    t.add(root, counter);
    for (int c = 0; c < 4; ++c) {
        const UiId list = t.list(root, {0, 0, 110, 240});
        for (int i = 0; i < 59; ++i) {
            if (i % 3 == 0) t.slider(list, "s"_id, "Volume", 0.5f, 0, 1);
            else t.button(list, "b"_id, "A button label");
        }
    }
    t.focus(t.find("s"_id));
    test::run_frames(u.app(), 60);
    const auto start = std::chrono::steady_clock::now();
    constexpr int kFrames = 300;
    std::size_t quads = 0;
    for (int i = 0; i < kFrames; ++i) {
        test::run_frames(u.app(), 1);
        quads = std::max(quads, q.size());
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("ui frame (ui_update + draw_ui, " << t.size() << " nodes): " << us / kFrames << " us/frame incl. App loop; "
            << t.stats().measures << " label measures, " << t.stats().wraps << " wraps total");
    CHECK(t.size() == 242);
}

TEST_CASE("plugin: node intents reach the next tick through InputSystem::queue_intent") {
    test::HeadlessApp h;
    h.add<AssetsPlugin>();
    h.add<InputPlugin>();
    h.add<UiPlugin>();
    struct Got { std::vector<Intent> intents; };
    h.app().insert_resource<Got>();
    h.app().add_system(Stage::Simulate, [](Ctx &ctx) {
        for (const Intent &i : input_frame(ctx).intent_list()) ctx.res<Got>().intents.push_back(i);
    }, "test.intents");
    UiTree &t = h.app().main_world().resources().get<UiTree>();
    const UiId buy = t.button(0, "buy"_id, "Buy");
    t.get(buy)->intent = "shop.buy"_id;
    t.get(buy)->intent_a = 42;
    t.focus(buy);
    RawInput raw;
    raw.set_key(Key::Enter, true);
    h.app().res<InputSystem>().feed_raw(raw);
    h.ticks(1);                                                 // frame: Clicked -> queue_intent
    h.app().res<InputSystem>().feed_raw(RawInput{});
    h.ticks(2);                                                 // next tick: delivered once
    const std::vector<Intent> &got = h.app().res<Got>().intents;
    REQUIRE(got.size() == 1);
    CHECK(got[0].kind == "shop.buy"_id);
    CHECK(got[0].a == 42);
}
