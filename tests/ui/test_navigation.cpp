// Focus navigation and value changes from keys.
#include <array>
#include <string_view>

#include <lam/test/support.h>
#include <lam/ui/ui.h>

#include "ui_test_util.h"

using namespace lam;
using namespace lam::literals;
using namespace lam::uitest;

TEST_CASE("menu selection skips labels and disabled items and wraps") {
    UiTree m;
    const UiId root = m.panel(0, {});
    m.label(root, "-- Farm --");
    const UiId new_game = m.button(root, "new"_id, "New game");
    const UiId load = m.button(root, "load"_id, "Load");
    m.get(load)->enabled = false;
    const UiId quit = m.button(root, "quit"_id, "Quit");
    // Nothing focused: the first navigation key focuses the first item (the label is skipped).
    frame(m, key_down());
    CHECK(m.focused() == new_game);
    CHECK(count_events(m, UiEvent::Kind::Focused) == 1);
    frame(m, key_down());
    REQUIRE(m.events().size() == 1);
    CHECK(m.events()[0].kind == UiEvent::Kind::Focused);
    CHECK(m.events()[0].id == "quit"_id);                       // "load" is disabled
    CHECK(m.focused() == quit);
    frame(m, key_down());
    CHECK(m.focused() == new_game);                             // wrapped, label skipped
    frame(m, key_up());
    CHECK(m.focused() == quit);
    frame(m, key_confirm());
    REQUIRE(m.events().size() == 1);
    CHECK(m.events()[0].kind == UiEvent::Kind::Clicked);
    CHECK(m.events()[0].id == "quit"_id);
    CHECK(m.events()[0].node == quit);
    // Clicking the disabled item does nothing.
    click(m, center_of(m.screen_rect(load)));
    CHECK(m.events().empty());
    CHECK(m.focused() == quit);
    frame(m, key_back());
    REQUIRE(m.events().size() == 1);
    CHECK(m.events()[0].kind == UiEvent::Kind::Back);
}

TEST_CASE("menu values change with left, right and confirm") {
    UiTree m;
    const UiId root = m.panel(0, {});
    const UiId vol = m.slider(root, "vol"_id, "Volume", 9, 0, 10, 1);
    const UiId full = m.toggle(root, "full"_id, "Fullscreen", false);
    const std::array<std::string_view, 3> speeds{"Slow", "Normal", "Fast"};
    const UiId speed = m.choice(root, "speed"_id, "Text", speeds, 1);
    m.focus(vol);
    frame(m, key_right());
    REQUIRE(m.events().size() == 1);
    CHECK(m.events()[0].kind == UiEvent::Kind::Changed);
    CHECK(m.events()[0].value == 10.0f);
    CHECK(m.get(vol)->value == 10.0f);
    frame(m, key_right());
    CHECK(m.events().empty());                                  // at the end: no change
    m.focus(full);
    frame(m, key_confirm());
    REQUIRE(m.events().size() == 1);
    CHECK(m.events()[0].kind == UiEvent::Kind::Changed);
    CHECK(m.get(full)->value == 1.0f);
    frame(m, key_left());                                       // left/right flip a toggle too
    CHECK(m.get(full)->value == 0.0f);
    m.focus(speed);
    frame(m, key_right());
    frame(m, key_right());
    CHECK(m.get(speed)->value == 0.0f);                         // wraps
    CHECK(m.get(speed)->choices[0].view() == "Slow");
    frame(m, key_left());
    CHECK(m.get(speed)->value == 2.0f);                         // wraps backwards
    // Pointer on the slider bar sets it from the fraction.
    const Rect bar = m.value_rect(vol);
    REQUIRE(bar.w > 0);
    frame(m, pointer_at({bar.x + bar.w * 0.31f, bar.y + 1}, true, true));
    CHECK(m.get(vol)->value == 3.0f);
    CHECK(m.focused() == vol);
}

TEST_CASE("slider steps snap to the step grid without drift") {
    UiTree m;
    const UiId s = m.slider(0, "s"_id, "S", 0.0f, 0.0f, 1.0f, 0.1f);
    m.focus(s);
    for (int i = 0; i < 7; ++i) frame(m, key_right());
    CHECK(m.get(s)->value == doctest::Approx(0.7f).epsilon(1e-6));
    for (int i = 0; i < 20; ++i) frame(m, key_right());
    CHECK(m.get(s)->value == 1.0f);
    for (int i = 0; i < 30; ++i) frame(m, key_left());
    CHECK(m.get(s)->value == 0.0f);
    const UiId z = m.slider(0, "z"_id, "Z", 5.0f, 0.0f, 10.0f, 0.0f);   // step 0 -> range / 10
    m.focus(z);
    frame(m, key_right());
    CHECK(m.get(z)->value == 6.0f);
}

TEST_CASE("integer choice steppers wrap between min and max") {
    UiTree m;
    UiNode n;
    n.kind = UiKind::Choice;
    n.id = "lives"_id;
    n.text = "Lives";
    n.min = 1;
    n.max = 3;
    n.value = 3;
    const UiId c = m.add(0, n);
    m.focus(c);
    frame(m, key_right());
    CHECK(m.get(c)->value == 1.0f);
    frame(m, key_left());
    CHECK(m.get(c)->value == 3.0f);
    frame(m, key_confirm());
    CHECK(m.get(c)->value == 1.0f);
}

TEST_CASE("each action emits one event: confirm never repeats, focus moves emit once") {
    UiTree m;
    const UiId a = m.button(0, "a"_id, "A");
    m.focus(a);
    frame(m, key_confirm());
    CHECK(count_events(m, UiEvent::Kind::Clicked) == 1);
    frame(m, UiInput{});
    CHECK(m.events().empty());                                  // nothing carried over
    // Programmatic focus is not a user action: no Focused event.
    const UiId b = m.button(0, "b"_id, "B");
    m.focus(b);
    frame(m, UiInput{});
    CHECK(m.events().empty());
}

TEST_CASE("hidden and invisible-parent nodes are skipped by navigation") {
    UiTree m;
    const UiId a = m.button(0, "a"_id, "A");
    const UiId box = m.panel(0, {});
    const UiId b = m.button(box, "b"_id, "B");
    const UiId c = m.button(0, "c"_id, "C");
    m.get(box)->visible = false;
    m.focus(a);
    frame(m, key_down());
    CHECK(m.focused() == c);
    m.get(box)->visible = true;
    frame(m, key_up());
    CHECK(m.focused() == b);
    // The focused node becoming hidden drops the focus; the next key refocuses.
    m.get(b)->visible = false;
    frame(m, key_down());
    CHECK(m.focused() == a);
}

TEST_CASE("rows: left/right move between cells and wrap; up/down leave the row") {
    UiTree m;
    const UiId col = m.panel(0, {});
    const UiId top = m.button(col, "top"_id, "Top");
    const UiId row = m.panel(col, {}, UiLayout::Horizontal);
    const UiId yes = m.button(row, "yes"_id, "Yes");
    const UiId maybe = m.button(row, "maybe"_id, "Maybe");
    const UiId no = m.button(row, "no"_id, "No");
    const UiId bottom = m.button(col, "bottom"_id, "Bottom");
    m.focus(yes);
    frame(m, key_right());
    CHECK(m.focused() == maybe);
    frame(m, key_right());
    frame(m, key_right());
    CHECK(m.focused() == yes);                                  // wrapped
    frame(m, key_left());
    CHECK(m.focused() == no);
    frame(m, key_down());
    CHECK(m.focused() == bottom);                               // leaves the row
    frame(m, key_up());
    CHECK(m.focused() == no);                                   // document order: the row's last cell
    frame(m, key_up());
    CHECK(m.focused() == top);                                  // leaves the row upwards
    // Left/right on a button outside any row does nothing.
    frame(m, key_left());
    CHECK(m.focused() == top);
    CHECK(m.events().empty());
}

TEST_CASE("grids: arrows move by cell and column, leave at the edges, wrap when alone") {
    UiTree m;
    const UiId col = m.panel(0, {});
    const UiId g = m.grid(col, {0, 0, 0, 0}, 3);
    std::array<UiId, 7> cells{};
    for (int i = 0; i < 7; ++i) {
        FixedString<8> name;
        format_to(name, "c{}", i);
        cells[static_cast<std::size_t>(i)] = m.button(g, intern(name.view()), name.view());
    }
    const UiId back = m.button(col, "back"_id, "Back");
    m.focus(cells[0]);
    frame(m, key_right());
    CHECK(m.focused() == cells[1]);
    frame(m, key_down());
    CHECK(m.focused() == cells[4]);
    frame(m, key_down());
    CHECK(m.focused() == back);                                 // below cell 4 there is no cell 7
    frame(m, key_up());
    CHECK(m.focused() == cells[6]);                             // enters at the grid's last item
    frame(m, key_up());
    CHECK(m.focused() == cells[3]);
    frame(m, key_left());
    CHECK(m.focused() == cells[2]);                             // row-major wrap to the previous row
    frame(m, key_up());
    CHECK(m.focused() == back);                                 // above the first row: leaves (wraps around the menu)
    // Disabled cells are skipped by column moves.
    m.get(cells[3])->enabled = false;
    m.focus(cells[0]);
    frame(m, key_down());
    CHECK(m.focused() == cells[6]);
    // A grid alone wraps within its column.
    UiTree solo;
    const UiId g2 = solo.grid(0, {}, 2);
    const UiId s0 = solo.button(g2, "s0"_id, "0");
    solo.button(g2, "s1"_id, "1");
    const UiId s2 = solo.button(g2, "s2"_id, "2");
    solo.focus(s0);
    frame(solo, key_up());
    CHECK(solo.focused() == s2);
    frame(solo, key_down());
    CHECK(solo.focused() == s0);
}

TEST_CASE("side-by-side columns: up/down stay in the column and wrap there; left/right cross") {
    UiTree m;
    const UiId page = m.panel(0, {});                           // vertical page
    const UiId row = m.panel(page, {}, UiLayout::Horizontal);
    const UiId left = m.panel(row, {});
    const UiId l0 = m.button(left, "l0"_id, "Play");
    const UiId l1 = m.button(left, "l1"_id, "Options");
    const UiId l2 = m.button(left, "l2"_id, "Quit");
    const UiId right = m.list(row, {0, 0, 60, 0});
    const UiId r0 = m.button(right, "r0"_id, "One");
    const UiId r1 = m.button(right, "r1"_id, "Two");
    const UiId r2 = m.button(right, "r2"_id, "Three");
    const UiId footer = m.button(page, "footer"_id, "Footer");
    m.focus(l0);
    frame(m, key_down());
    CHECK(m.focused() == l1);                                   // inside the left column
    frame(m, key_right());
    CHECK(m.focused() == r1);                                   // the nearest item across
    frame(m, key_down());
    frame(m, key_down());
    CHECK(m.focused() == footer);                               // leaves both columns downwards
    frame(m, key_down());
    CHECK(m.focused() == l0);                                   // wraps around the page
    frame(m, key_up());
    CHECK(m.focused() == footer);
    frame(m, key_up());
    CHECK(m.focused() == r2);                                   // document order: the row's last item
    frame(m, key_left());
    CHECK(m.focused() == l2);
    // Without the footer and the page, each column wraps on its own.
    UiTree two;
    const UiId cols = two.panel(0, {}, UiLayout::Horizontal);
    const UiId a = two.panel(cols, {});
    const UiId a0 = two.button(a, "a0"_id, "A0");
    const UiId a1 = two.button(a, "a1"_id, "A1");
    const UiId b = two.panel(cols, {});
    const UiId b0 = two.button(b, "b0"_id, "B0");
    two.focus(a1);
    frame(two, key_down());
    CHECK(two.focused() == a0);                                 // wraps inside column a
    frame(two, key_up());
    CHECK(two.focused() == a1);
    frame(two, key_right());
    CHECK(two.focused() == b0);
    frame(two, key_down());
    CHECK(two.focused() == b0);                                 // alone in its column: stays
    CHECK(two.events().empty());
    (void)r0;
}

TEST_CASE("lists scroll to keep the focused item visible") {
    UiTree m;
    const UiId list = m.list(0, {0, 0, 80, 40});
    m.get(list)->style.padding = {0, 0};
    std::array<UiId, 6> items{};
    for (int i = 0; i < 6; ++i) {
        FixedString<8> name;
        format_to(name, "i{}", i);
        items[static_cast<std::size_t>(i)] = m.button(list, intern(name.view()), name.view());
    }
    m.focus(items[0]);
    lay(m);
    CHECK(m.get(list)->scroll == 0);
    CHECK(m.shown(items[0]));
    CHECK(m.shown(items[1]));
    CHECK(m.shown(items[2]));                                   // partly visible (clipped)
    CHECK_FALSE(m.shown(items[3]));
    for (int i = 0; i < 3; ++i) frame(m, key_down());
    CHECK(m.focused() == items[3]);
    CHECK(m.get(list)->scroll == 2);                            // rows 2..3 fit in 40 px
    CHECK(m.shown(items[3]));
    CHECK_FALSE(m.shown(items[1]));
    frame(m, key_down());
    frame(m, key_down());
    CHECK(m.get(list)->scroll == 4);
    frame(m, key_down());                                       // wraps to the top
    CHECK(m.focused() == items[0]);
    CHECK(m.get(list)->scroll == 0);
}

TEST_CASE("text inputs take typed text and erase; direction keys stay inside") {
    UiTree m;
    const UiId name = m.text_input(0, "name"_id, "Ab");
    const UiId ok = m.button(0, "ok"_id, "OK");
    m.focus(name);
    const char32_t typed[] = {U'c', U'é', U'\n', U'd'};
    UiInput in;
    in.typed = typed;
    frame(m, in);
    CHECK(m.get(name)->text.view() == "Abc\xC3\xA9" "d");      // control characters dropped
    CHECK(count_events(m, UiEvent::Kind::Changed) == 1);
    UiInput erase;
    erase.erase = true;
    frame(m, erase);
    frame(m, erase);
    CHECK(m.get(name)->text.view() == "Abc");                   // a 2-byte code point erased whole
    frame(m, key_down());
    CHECK(m.focused() == name);
    frame(m, key_confirm());
    REQUIRE(m.events().size() == 2);
    CHECK(m.events()[0].kind == UiEvent::Kind::Submitted);
    CHECK(m.events()[0].node == name);
    CHECK(m.events()[1].kind == UiEvent::Kind::Focused);       // confirm moves on, like a form
    CHECK(m.focused() == ok);
    // A full field drops the rest.
    m.focus(name);
    std::array<char32_t, 80> lots{};
    lots.fill(U'z');
    UiInput many;
    many.typed = lots;
    frame(m, many);
    CHECK(m.get(name)->text.size() == 63);
}

TEST_CASE("focusable images and panels report Clicked on confirm") {
    UiTree m;
    TextureRegion r;
    r.source = {0, 0, 16, 16};
    const UiId img = m.image(0, r, {}, "slot"_id);
    CHECK(m.get(img)->focusable);
    CHECK(m.get(img)->rect.w == 16.0f);
    m.focus(img);
    frame(m, key_confirm());
    REQUIRE(m.events().size() == 1);
    CHECK(m.events()[0].kind == UiEvent::Kind::Clicked);
    CHECK(m.events()[0].id == "slot"_id);
    const UiId deco = m.image(0, r);
    CHECK_FALSE(m.get(deco)->focusable);
}

TEST_CASE("navigation with nothing focusable does nothing; back still reports") {
    UiTree m;
    m.label(0, "only text");
    frame(m, key_down());
    CHECK(m.focused() == 0);
    CHECK(m.events().empty());
    frame(m, key_back());
    REQUIRE(m.events().size() == 1);
    CHECK(m.events()[0].kind == UiEvent::Kind::Back);
    CHECK(m.events()[0].node == 0);
}
