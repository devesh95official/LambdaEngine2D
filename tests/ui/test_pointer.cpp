// Pointer input: hover, click, drag and wheel.
#include <array>
#include <string_view>

#include <lam/test/support.h>
#include <lam/ui/ui.h>

#include "ui_test_util.h"

using namespace lam;
using namespace lam::literals;
using namespace lam::uitest;

TEST_CASE("pointer: hover focuses on movement; a resting pointer keeps the initial focus") {
    UiTree t;
    const UiId p = t.panel(0, {0, 0, 100, 0});
    const UiId a = t.button(p, "a"_id, "A");
    const UiId b = t.button(p, "b"_id, "B");
    t.focus(a);
    lay(t);
    const Vec2 over_b = center_of(t.screen_rect(b));
    frame(t, pointer_at(over_b));                               // first sighting: not a move
    CHECK(t.focused() == a);
    CHECK(t.hovered() == b);
    frame(t, pointer_at(over_b));
    CHECK(t.focused() == a);
    frame(t, pointer_at({over_b.x + 1, over_b.y}));             // moved
    CHECK(t.focused() == b);
    REQUIRE(t.events().size() == 1);
    CHECK(t.events()[0].kind == UiEvent::Kind::Focused);
    frame(t, key_up());                                         // keys still work afterwards
    CHECK(t.focused() == a);
    frame(t, UiInput{});                                        // pointer gone
    CHECK(t.hovered() == 0);
}

TEST_CASE("pointer: press and release on the same button clicks exactly once") {
    UiTree t;
    const UiId p = t.panel(0, {0, 0, 100, 0});
    const UiId a = t.button(p, "a"_id, "A");
    const UiId b = t.button(p, "b"_id, "B");
    lay(t);
    const Vec2 pa = center_of(t.screen_rect(a));
    frame(t, pointer_at(pa, true, true));
    CHECK(t.pressed() == a);
    CHECK(t.focused() == a);
    CHECK(count_events(t, UiEvent::Kind::Clicked) == 0);       // not on press
    frame(t, pointer_at(pa, true));
    CHECK(count_events(t, UiEvent::Kind::Clicked) == 0);
    frame(t, pointer_at(pa, false, false, true));
    CHECK(count_events(t, UiEvent::Kind::Clicked) == 1);
    CHECK(t.events().back().id == "a"_id);
    CHECK(t.pressed() == 0);
    frame(t, pointer_at(pa));
    CHECK(t.events().empty());
    // Release over another button cancels.
    frame(t, pointer_at(pa, true, true));
    const Vec2 pb = center_of(t.screen_rect(b));
    frame(t, pointer_at(pb, true));
    frame(t, pointer_at(pb, false, false, true));
    CHECK(count_events(t, UiEvent::Kind::Clicked) == 0);
    CHECK(t.pressed() == 0);
}

TEST_CASE("pointer: toggles flip and choices step by the half that was clicked") {
    UiTree t;
    const UiId p = t.panel(0, {0, 0, 200, 0});
    const UiId tg = t.toggle(p, "t"_id, "Sound", false);
    const std::array<std::string_view, 3> opts{"Low", "Mid", "High"};
    const UiId ch = t.choice(p, "q"_id, "Quality", opts, 1);
    lay(t);
    click(t, center_of(t.screen_rect(tg)));
    CHECK(t.get(tg)->value == 1.0f);
    REQUIRE(count_events(t, UiEvent::Kind::Changed) == 1);
    const Rect v = t.value_rect(ch);
    click(t, {v.x + 2, v.y + 2});                               // "<" half
    CHECK(t.get(ch)->value == 0.0f);
    click(t, {v.x + v.w - 2, v.y + 2});                         // ">" half
    CHECK(t.get(ch)->value == 1.0f);
}

TEST_CASE("pointer: sliders follow a drag, snapped to the step, one event per change") {
    UiTree t;
    const UiId p = t.panel(0, {0, 0, 200, 0});
    const UiId s = t.slider(p, "s"_id, "Vol", 0.0f, 0.0f, 10.0f, 1.0f);
    lay(t);
    const Rect bar = t.value_rect(s);
    frame(t, pointer_at({bar.x, bar.y}, true, true));
    CHECK(t.get(s)->value == 0.0f);
    CHECK(count_events(t, UiEvent::Kind::Changed) == 0);       // unchanged value: no event
    frame(t, pointer_at({bar.x + bar.w * 0.5f, bar.y}, true));
    CHECK(t.get(s)->value == 5.0f);
    CHECK(count_events(t, UiEvent::Kind::Changed) == 1);
    frame(t, pointer_at({bar.x + bar.w * 0.52f, bar.y}, true));   // still rounds to 5
    CHECK(count_events(t, UiEvent::Kind::Changed) == 0);
    frame(t, pointer_at({bar.x + bar.w * 2.0f, bar.y + 50}, true));   // dragging past the end clamps
    CHECK(t.get(s)->value == 10.0f);
    frame(t, pointer_at({bar.x - 100, bar.y}, false, false, true));
    CHECK(t.get(s)->value == 10.0f);                            // release does not move it
    CHECK(t.pressed() == 0);
    frame(t, pointer_at({bar.x, bar.y}));                       // not pressed: hovering does not drag
    CHECK(t.get(s)->value == 10.0f);
}

TEST_CASE("pointer: the wheel scrolls the list under the pointer") {
    UiTree t;
    const UiId list = t.list(0, {0, 0, 80, 40});
    t.get(list)->style.padding = {0, 0};
    for (int i = 0; i < 8; ++i) t.button(list, "i"_id, "Item");
    lay(t);
    UiInput in = pointer_at({10, 10});
    in.wheel = -1;                                              // towards the user: scroll down
    frame(t, in);
    CHECK(t.get(list)->scroll == 1);
    in.wheel = -3;
    frame(t, in);
    CHECK(t.get(list)->scroll == 4);
    in.wheel = 2;
    frame(t, in);
    CHECK(t.get(list)->scroll == 2);
    in.pointer = {300, 300};                                    // not over the list
    in.wheel = -1;
    frame(t, in);
    CHECK(t.get(list)->scroll == 2);
}

TEST_CASE("pointer: disabled widgets and labels never take focus or click") {
    UiTree t;
    const UiId p = t.panel(0, {0, 0, 100, 0});
    const UiId l = t.label(p, "Title");
    const UiId d = t.button(p, "d"_id, "Off");
    t.get(d)->enabled = false;
    lay(t);
    frame(t, pointer_at(center_of(t.screen_rect(l))));
    frame(t, pointer_at(center_of(t.screen_rect(d))));
    CHECK(t.focused() == 0);
    CHECK(t.hovered() == 0);
    click(t, center_of(t.screen_rect(d)));
    CHECK(t.events().empty());
}
