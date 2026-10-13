// Engine menus and the built-in menu, confirm, settings and text scenes.
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/debug.h>
#include <lam/core/settings.h>
#include <lam/input/actions.h>
#include <lam/input/input_frame.h>
#include <lam/input/keys.h>
#include <lam/scene/scene.h>
#include <lam/test/support.h>
#include <lam/ui/menu_scenes.h>
#include <lam/ui/ui.h>
#include <lam/ui/ui_plugin.h>

#include "ui_test_util.h"

using namespace lam;
using namespace lam::literals;
using namespace lam::uitest;

namespace {

MenuItem button(std::string_view id, std::string_view label, float value = 0) {
    MenuItem m;
    m.id = intern(id);
    m.label = label;
    m.value = value;
    return m;
}

MenuSpec pause_spec() {
    MenuSpec spec;
    spec.title = "Paused";
    MenuItem heading;
    heading.kind = UiKind::Label;
    heading.label = "-- Game --";
    MenuItem load = button("load", "Load");
    load.enabled = false;
    spec.items = {heading, load, button("resume", "Resume", 3), button("quit", "Quit")};
    return spec;
}

constexpr ButtonAction kUp{1}, kDown{2}, kLeft{3}, kRight{4}, kConfirm{5}, kBack{6};

// Headless app with UiPlugin and a FrameInput driven by hand. No InputPlugin, since
// input_frame_update would overwrite FrameInput every frame.
struct MenuApp {
    test::HeadlessApp h;
    MenuApp() {
        h.add<AssetsPlugin>();
        h.add<UiPlugin>();
        h.app().insert_resource<FrameInput>();
        h.app().res<UiNav>() = UiNav{kUp, kDown, kLeft, kRight, kConfirm, kBack, {}};
    }
    App &app() { return h.app(); }
    World &world() { return app().main_world(); }
    UiTree &tree() { return world().resources().get<UiTree>(); }
    MenuState &state() { return world().resources().get<MenuState>(); }
    void enter(Scene &s) { s.enter(app(), world()); }
    void press(ButtonAction a = {}) {
        FrameInput &in = app().res<FrameInput>();
        in = FrameInput{};
        if (a) in.pressed[a.id >> 6] |= std::uint64_t{1} << (a.id & 63u);
        h.frames(1);
        in = FrameInput{};
    }
    UiId item(std::string_view id) { return tree().find(str_id(id)); }
};

std::string schedule_of(const App &app, std::string_view name) {
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

TEST_CASE("menus: build_menu makes a centred panel and focuses the first enabled item") {
    UiTree t;
    MenuState st = build_menu(t, pause_spec(), {480, 270});
    REQUIRE(st.nodes.size() == 4);
    CHECK(t.get(st.nodes[0])->kind == UiKind::Label);
    CHECK_FALSE(t.get(st.nodes[1])->enabled);
    CHECK(t.focused() == st.nodes[2]);                          // label and disabled item skipped
    CHECK(t.get(st.nodes[2])->value == 3.0f);                   // the button's payload
    CHECK(st.on_change == &menu_settings_change);
    lay(t);
    const Rect panel = t.screen_rect(st.panel);
    CHECK(panel.x + panel.w * 0.5f == doctest::Approx(240).epsilon(0.01));
    CHECK(panel.y + panel.h * 0.5f == doctest::Approx(135).epsilon(0.01));
    CHECK(t.screen_rect(st.root).w == 480);                     // full-screen dim layer
    CHECK(t.get(st.root)->color.a == 140);
    MenuSpec spec = pause_spec();
    spec.initial_focus = "quit"_id;
    spec.overlay = false;
    UiTree t2;
    MenuState st2 = build_menu(t2, spec, {480, 270});
    CHECK(t2.focused() == st2.nodes[3]);
    CHECK(t2.get(st2.root)->color.a == 0);
}

TEST_CASE("menus: a click becomes a select intent with its payload; the tick closes with it") {
    UiTree t;
    MenuState st = build_menu(t, pause_spec(), {480, 270});
    frame(t, key_confirm());
    REQUIRE(t.events().size() == 1);
    const std::optional<Intent> in = menu_event_intent(st, t, t.events()[0]);
    REQUIRE(in.has_value());
    CHECK(in->kind == kMenuSelectIntent);
    CHECK(in->a == 2);
    CHECK(in->b == 3);
    // Tick time: the message comes from the intent alone.
    const std::optional<SceneMessage> msg = menu_intent_message(st, *in);
    REQUIRE(msg.has_value());
    CHECK(msg->kind == "resume"_id);
    CHECK(msg->value == 3);
    CHECK(st.closed);
    REQUIRE(st.result.has_value());
    CHECK(st.result->kind == "resume"_id);
    CHECK_FALSE(menu_intent_message(st, *in).has_value());      // closing: later intents ignored
    CHECK_FALSE(menu_event_intent(st, t, t.events()[0]).has_value());
}

TEST_CASE("menus: the message depends only on the intent, so replays reproduce it") {
    UiTree live, replay;
    MenuState a = build_menu(live, pause_spec(), {480, 270});
    MenuState b = build_menu(replay, pause_spec(), {320, 180});  // another screen, no input at all
    const Intent recorded{kMenuSelectIntent, 3, 0};
    const std::optional<SceneMessage> ma = menu_intent_message(a, recorded);
    const std::optional<SceneMessage> mb = menu_intent_message(b, recorded);
    REQUIRE(ma.has_value());
    REQUIRE(mb.has_value());
    CHECK(ma->kind == mb->kind);
    CHECK(ma->value == mb->value);
    // Bad indices, labels and unknown kinds never close a menu.
    UiTree t;
    MenuState st = build_menu(t, pause_spec(), {480, 270});
    CHECK_FALSE(menu_intent_message(st, Intent{kMenuSelectIntent, 9, 0}).has_value());
    CHECK_FALSE(menu_intent_message(st, Intent{kMenuSelectIntent, -1, 0}).has_value());
    CHECK_FALSE(menu_intent_message(st, Intent{kMenuSelectIntent, 0, 0}).has_value());   // the label
    CHECK_FALSE(menu_intent_message(st, Intent{"other"_id, 2, 0}).has_value());
    CHECK_FALSE(st.closed);
}

TEST_CASE("menus: back answers back_kind, unless back_pops is off") {
    UiTree t;
    MenuSpec spec = pause_spec();
    spec.back_kind = "resume"_id;
    MenuState st = build_menu(t, spec, {480, 270});
    frame(t, key_back());
    REQUIRE(t.events().size() == 1);
    const std::optional<Intent> in = menu_event_intent(st, t, t.events()[0]);
    REQUIRE(in.has_value());
    CHECK(in->kind == kMenuBackIntent);
    const std::optional<SceneMessage> msg = menu_intent_message(st, *in);
    REQUIRE(msg.has_value());
    CHECK(msg->kind == "resume"_id);
    UiTree t2;
    spec.back_pops = false;
    MenuState st2 = build_menu(t2, spec, {480, 270});
    frame(t2, key_back());
    REQUIRE(t2.events().size() == 1);
    CHECK_FALSE(menu_event_intent(st2, t2, t2.events()[0]).has_value());
    CHECK_FALSE(menu_intent_message(st2, Intent{kMenuBackIntent, 0, 0}).has_value());
}

TEST_CASE("menus: a text field submits its text as an interned id") {
    UiTree t;
    MenuSpec spec;
    MenuItem name;
    name.id = "name"_id;
    name.kind = UiKind::TextInput;
    spec.items = {name, button("cancel", "Cancel")};
    MenuState st = build_menu(t, spec, {480, 270});
    REQUIRE(t.focused() == st.nodes[0]);
    const std::array<char32_t, 4> typed{U'H', U'e', U'r', U'o'};
    UiInput in;
    in.typed = typed;
    frame(t, in);
    frame(t, key_confirm());
    REQUIRE(t.events().size() == 2);                            // Submitted, then Focused (moves on)
    CHECK(t.events()[0].kind == UiEvent::Kind::Submitted);
    CHECK_FALSE(menu_event_intent(st, t, t.events()[1]).has_value());
    const std::optional<Intent> intent = menu_event_intent(st, t, t.events()[0]);
    REQUIRE(intent.has_value());
    const std::optional<SceneMessage> msg = menu_intent_message(st, *intent);
    REQUIRE(msg.has_value());
    CHECK(msg->kind == "name"_id);
    CHECK(msg->detail == "Hero"_id);
    CHECK(id_name(msg->detail) == "Hero");
}

TEST_CASE("menu scene: enter adds the menu systems; confirm closes it at the next tick") {
    MenuApp m;
    MenuScene scene(pause_spec());
    CHECK(scene.name() == "lam.menu");
    CHECK(scene.overlay());
    m.enter(scene);
    CHECK(schedule_of(m.app(), "ui.menu_frame") == "Update PerWorld -700");
    CHECK(schedule_of(m.app(), "ui.menu_tick") == "Input PerWorld -1000");
    m.press();
    CHECK(m.tree().focused() == m.state().nodes[2]);
    m.press(kDown);
    CHECK(m.tree().focused() == m.state().nodes[3]);
    m.press(kConfirm);                                          // frame: Clicked -> intent
    CHECK_FALSE(m.state().closed);
    m.press();                                                  // next tick: message
    REQUIRE(m.state().result.has_value());
    CHECK(m.state().result->kind == "quit"_id);
    m.press(kConfirm);
    m.press();
    CHECK(m.state().result->kind == "quit"_id);                 // closed: nothing else happens
}

TEST_CASE("menu scene: value items write the settings key named by their id") {
    MenuApp m;
    SettingsStore &settings = m.app().res<SettingsStore>();
    settings.define_int("game.difficulty", 1, 0, 2);
    settings.define_bool("game.hints", true);
    MenuSpec spec;
    MenuItem diff;
    diff.id = intern("game.difficulty");
    diff.label = "Difficulty";
    diff.kind = UiKind::Choice;
    diff.choices = {"Easy", "Normal", "Hard"};
    diff.choice_count = 3;
    diff.value = 1;
    MenuItem hints;
    hints.id = intern("game.hints");
    hints.label = "Hints";
    hints.kind = UiKind::Toggle;
    hints.value = 1;
    MenuItem unbound;
    unbound.id = "not.a.setting"_id;
    unbound.kind = UiKind::Toggle;
    spec.items = {diff, hints, unbound, button("back", "Back")};
    MenuScene scene(spec);
    m.enter(scene);
    m.press(kRight);
    CHECK(settings.get_int("game.difficulty") == 2);
    m.press(kDown);
    m.press(kConfirm);
    CHECK_FALSE(settings.get_bool("game.hints"));
    m.press(kDown);
    m.press(kConfirm);                                          // unbound: no settings key, no crash
    CHECK_FALSE(m.state().closed);
}

TEST_CASE("settings scene: volumes, fullscreen and show FPS edit the settings") {
    MenuApp m;
    SettingsStore &settings = m.app().res<SettingsStore>();
    settings.define_float("game.speed", 1.0f, 0.5f, 2.0f);
    MenuItem speed;
    speed.id = intern("game.speed");
    speed.label = "Game speed";
    speed.kind = UiKind::Slider;
    speed.min = 0.5f;
    speed.max = 2.0f;
    speed.step = 0.5f;
    speed.value = 0.5f;                                         // replaced by the stored value
    std::unique_ptr<Scene> scene = make_settings_scene(std::span<const MenuItem>(&speed, 1));
    CHECK(scene->name() == "lam.settings");
    m.enter(*scene);
    UiTree &t = m.tree();
    REQUIRE(m.item("audio.master.volume") != 0);
    CHECK(m.item("audio.music.volume") != 0);
    CHECK(m.item("audio.sfx.volume") != 0);
    CHECK(m.item("audio.ambience.volume") != 0);
    CHECK(t.focused() == m.item("audio.master.volume"));
    CHECK(t.get(m.item("game.speed"))->value == 1.0f);          // the stored value
    m.press(kLeft);
    REQUIRE(settings.defined("audio.master.volume"));
    CHECK(settings.get_float("audio.master.volume") == doctest::Approx(0.9f));
    t.focus(m.item("display.fullscreen"));
    m.press(kConfirm);
    CHECK(settings.get_bool("display.fullscreen"));
#if LAM_DEV
    t.focus(m.item("debug.show_fps"));
    m.press(kConfirm);
    CHECK(m.app().res<DebugOverlay>().visible);
#else
    CHECK(m.item("debug.show_fps") == 0);
#endif
    t.focus(m.item("game.speed"));
    m.press(kRight);
    CHECK(settings.get_float("game.speed") == 1.5f);
    m.press(kBack);
    m.press();
    REQUIRE(m.state().result.has_value());
    CHECK(m.state().result->kind == "back"_id);
}

namespace {
// Scripted menu input for the allocation test: directions only (no confirm, so it never closes).
void menu_script(Ctx &ctx) {
    FrameInput &in = ctx.res<FrameInput>();
    const std::uint64_t f = ctx.time().frame;
    in = FrameInput{};
    const ButtonAction a = f % 5 == 0 ? kDown : f % 5 == 1 ? kLeft : f % 5 == 2 ? kRight : f % 5 == 3 ? kUp : kDown;
    if (f % 3 == 0) in.pressed[a.id >> 6] |= std::uint64_t{1} << (a.id & 63u);
    in.pointer.valid = f % 40 < 20;
    in.pointer.virtual_pos = {static_cast<float>(160 + f % 160), static_cast<float>(40 + (f * 7) % 190)};
}
} // namespace

TEST_CASE("settings scene: navigating and editing values allocates nothing in steady state") {
    MenuApp m;
    m.app().add_system({.stage = Stage::FrameBegin, .fn = menu_script, .name = "test.menu_script", .order = 100,
                        .scope = SystemScope::Once});
    std::unique_ptr<Scene> scene = make_settings_scene();
    m.enter(*scene);
    test::run_frames(m.app(), 240);                             // every key defined, every path taken
    CHECK_NO_ALLOC(test::run_frames(m.app(), 600));
    CHECK_FALSE(m.state().closed);
}

TEST_CASE("settings scene: rebinding captures the next key and saves it") {
    MenuApp m;
    // MenuApp has no InputPlugin, so add the ActionMap the scene needs ourselves.
    ActionMap &map = m.app().insert_resource<ActionMap>();
    const ButtonAction jump = map.button("jump", {ActionMap::key(Key::Space)});
    std::unique_ptr<Scene> scene = make_settings_scene();
    m.enter(*scene);
    const UiId rebind = m.item("input.rebind");
    REQUIRE(rebind != 0);
    CHECK(m.tree().get(rebind)->text.view() == "jump: key:space");
    m.tree().focus(rebind);
    m.press(kConfirm);
    CHECK(map.capturing());
    CHECK(m.tree().get(rebind)->text.view() == "jump: press a key...");
    map.rebind(jump, 0, 0, ActionMap::key(Key::J));             // what the capture does
    map.cancel_capture();
    m.press();
    CHECK(m.tree().get(rebind)->text.view() == "jump: key:j");
    CHECK(m.app().res<SettingsStore>().defined("input.p0.jump.0"));
}

TEST_CASE("confirm scene: focus starts on no; back answers no; long questions become the body") {
    MenuApp m;
    std::unique_ptr<Scene> scene = make_confirm_scene("Quit?", "yes"_id, "stay"_id);
    m.enter(*scene);
    CHECK(m.tree().focused() == m.state().nodes[1]);
    CHECK(m.state().spec.title.view() == "Quit?");
    m.press(kBack);
    m.press();
    REQUIRE(m.state().result.has_value());
    CHECK(m.state().result->kind == "stay"_id);
    UiTree t;
    const std::string long_q(80, 'q');
    std::unique_ptr<Scene> s2 = make_confirm_scene(long_q, "yes"_id);
    MenuApp m2;
    m2.enter(*s2);
    CHECK(m2.state().spec.title.empty());
    CHECK(m2.state().spec.body == long_q);
    CHECK(m2.tree().block_text(m2.state().body) == long_q);
    m2.press(kUp);                                              // to "Yes"
    m2.press(kConfirm);
    m2.press();
    REQUIRE(m2.state().result.has_value());
    CHECK(m2.state().result->kind == "yes"_id);
}

TEST_CASE("text scene: heading marks dropped, the body scrolls, close closes") {
    MenuApp m;
    std::string body = "# Credits\n";
    for (int i = 0; i < 60; ++i) body += "Line of credits text\n";
    std::unique_ptr<Scene> scene = make_text_scene("About", body);
    CHECK(scene->name() == "lam.text");
    m.enter(*scene);
    const UiId block = m.state().body;
    REQUIRE(block != 0);
    CHECK(m.tree().block_text(block).substr(0, 8) == " Credits");
    m.press();
    CHECK(m.tree().get(block)->scroll == 0);
    m.press(kDown);
    m.press(kDown);
    CHECK(m.tree().get(block)->scroll == 2);
    m.press(kUp);
    CHECK(m.tree().get(block)->scroll == 1);
    m.press(kConfirm);                                          // focus is on "Close"
    m.press();
    REQUIRE(m.state().result.has_value());
    CHECK(m.state().result->kind == "close"_id);
}

TEST_CASE("menu scenes on the SceneStack: push, choose, resumed gets the message") {
    struct Game final : Scene {
        std::optional<SceneMessage> got;
        std::string_view name() const override { return "test.game"; }
        void enter(App &, World &) override {}
        void resumed(App &, const SceneMessage *msg) override {
            if (msg != nullptr) got = *msg;
        }
    };
    test::HeadlessApp h;
    h.add<AssetsPlugin>();
    h.add<ScenePlugin>();
    h.add<UiPlugin>();
    h.app().insert_resource<FrameInput>();
    h.app().res<UiNav>() = UiNav{kUp, kDown, kLeft, kRight, kConfirm, kBack, {}};
    SceneStack &stack = h.app().res<SceneStack>();
    auto game = std::make_unique<Game>();
    Game *g = game.get();
    stack.push(std::move(game));
    h.frames(1);
    stack.push(std::make_unique<MenuScene>(pause_spec()));
    h.frames(2);
    REQUIRE(stack.size() == 2);
    FrameInput &in = h.app().res<FrameInput>();
    in.pressed[0] |= std::uint64_t{1} << kConfirm.id;
    h.frames(1);
    in = FrameInput{};
    h.frames(3);
    CHECK(stack.size() == 1);
    REQUIRE(g->got.has_value());
    CHECK(g->got->kind == "resume"_id);
    CHECK(g->got->value == 3);
}

TEST_CASE("a menu with a long body and many items stays on screen") {
    MenuApp m;
    MenuSpec spec;
    spec.title = "Load";
    for (int i = 0; i < 60; ++i) spec.body += "Pick a slot. ";            // about 780 characters
    for (int i = 0; i < 20; ++i) {
        FixedString<8> id;
        format_to(id, "s{}", i);
        spec.items.push_back(button(id.view(), id.view()));
    }
    spec.initial_focus = "s19"_id;
    MenuScene scene(std::move(spec));
    m.enter(scene);
    m.press();
    UiTree &t = m.tree();
    const Rect screen = t.screen_rect(m.state().root);
    const Rect panel = t.screen_rect(m.state().panel);
    CHECK(panel.y >= 0.0f);
    CHECK(panel.y + panel.h <= screen.h);
    // Every item the list shows lies on screen, and the focused last item is one of them.
    for (const UiId item : m.state().nodes) {
        if (!t.shown(item)) continue;
        const Rect r = t.screen_rect(item);
        CHECK(r.y >= 0.0f);
        CHECK(r.y + r.h <= screen.h);
    }
    CHECK(t.focused() == m.state().nodes.back());
    CHECK(t.shown(m.state().nodes.back()));
    m.press(kDown);                                             // wraps to the first, still visible
    CHECK(t.focused() == m.state().nodes.front());
    CHECK(t.shown(m.state().nodes.front()));
    const Rect first = t.screen_rect(m.state().nodes.front());
    CHECK(first.y + first.h <= screen.h);
}
