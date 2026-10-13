// lam_ui regressions. Each one failed before its fix. The menu-fit case is in test_menus.cpp.
#include <array>
#include <climits>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

#include <lam/render/render_queue.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>
#include <lam/ui/dialogue.h>
#include <lam/ui/toasts.h>
#include <lam/ui/ui.h>
#include <lam/ui/ui_draw.h>

#include "ui_test_util.h"

using namespace lam;
using namespace lam::literals;
using namespace lam::uitest;

TEST_CASE("add() copies a node of the same tree before the store grows") {
    UiTree t;
    const UiId list = t.list(0, {0, 0, 100, 100});
    const UiId tmpl = t.button(list, "tmpl"_id, "Template label");
    t.get(tmpl)->rect = {1, 2, 30, 12};
    for (int i = 0; i < 300; ++i) {                             // grows the store many times
        const UiId c = t.add(list, *t.get(tmpl));
        REQUIRE(c != 0);
        CHECK(t.get(c)->text.view() == "Template label");
        CHECK(t.get(c)->rect.w == 30.0f);
    }
    // set_text with a view into the node's own text (overlapping copy).
    t.set_text(tmpl, t.get(tmpl)->text.view().substr(9));
    CHECK(t.get(tmpl)->text.view() == "label");
}

TEST_CASE("a space typed into a focused text field is text, not confirm") {
    UiTree m;
    const UiId name = m.text_input(0, "name"_id, "Mary");
    m.button(0, "ok"_id, "OK");
    m.focus(name);
    const char32_t space[] = {U' '};
    UiInput in;
    in.typed = space;
    in.confirm = true;                                          // Space is bound to ui.confirm
    frame(m, in);
    CHECK(m.focused() == name);
    CHECK(count_events(m, UiEvent::Kind::Submitted) == 0);
    const char32_t ann[] = {U'A', U'n', U'n'};
    UiInput rest;
    rest.typed = ann;
    frame(m, rest);
    CHECK(m.get(name)->text.view() == "Mary Ann");
    frame(m, key_confirm());                                    // Enter alone still submits
    CHECK(count_events(m, UiEvent::Kind::Submitted) == 1);
}

TEST_CASE("focus() before the first layout scrolls the list once it is laid out") {
    UiTree m;
    const UiId list = m.list(0, {0, 0, 80, 40});
    m.get(list)->style.padding = {0, 0};
    std::array<UiId, 30> items{};
    for (int i = 0; i < 30; ++i) {
        FixedString<8> n;
        format_to(n, "i{}", i);
        items[static_cast<std::size_t>(i)] = m.button(list, intern(n.view()), n.view());
    }
    m.focus(items[25]);                                         // nothing laid out yet
    lay(m);
    CHECK(m.focused() == items[25]);
    CHECK(m.get(list)->scroll > 0);
    CHECK(m.shown(items[25]));
    CHECK_FALSE(m.shown(items[0]));
}

TEST_CASE("a choice_count edited past kMaxUiChoices is clamped at every use") {
    UiTree m;
    const std::array<std::string_view, 3> opts{"a", "b", "c"};
    const UiId c = m.choice(0, "c"_id, "Pick", opts, 0);
    m.focus(c);
    m.get(c)->choice_count = 12;                                // through get(), unsanitised
    for (int i = 0; i < 20; ++i) {
        frame(m, key_right());
        CHECK(m.get(c)->value < static_cast<float>(kMaxUiChoices));
    }
    lay(m);
    CHECK(m.value_rect(c).w > 0.0f);
}

TEST_CASE("scroll_by(INT_MAX) scrolls to the end every time, without overflow") {
    UiTree m;
    std::string text;
    for (int i = 0; i < 40; ++i) text += "line\n";
    const UiId tb = m.text_block(0, {0, 0, 100, 40}, text);
    lay(m);
    m.scroll_by(tb, INT_MAX);
    const std::uint16_t end = m.get(tb)->scroll;
    CHECK(end > 0);
    m.scroll_by(tb, INT_MAX);
    CHECK(m.get(tb)->scroll == end);
    m.scroll_by(tb, INT_MIN);
    CHECK(m.get(tb)->scroll == 0);
    const UiId grid = m.grid(0, {0, 100, 100, 40}, 4);
    for (int i = 0; i < 64; ++i) m.button(grid, StrId{}, "x");
    lay(m);
    m.scroll_by(grid, INT_MAX);
    const std::uint16_t grid_end = m.get(grid)->scroll;
    CHECK(grid_end > 0);
    CHECK(grid_end % 4 == 0);
    m.scroll_by(grid, INT_MAX);
    CHECK(m.get(grid)->scroll == grid_end);
}

TEST_CASE("an integer stepper with a wide range still steps") {
    UiTree m;
    UiNode n;
    n.kind = UiKind::Choice;
    n.min = -2e9f;
    n.max = 2e9f;
    n.value = 0;
    const UiId c = m.add(0, n);
    m.focus(c);
    frame(m, key_right());
    CHECK(m.get(c)->value == 1.0f);
    frame(m, key_left());
    frame(m, key_left());
    CHECK(m.get(c)->value == -1.0f);
    m.get(c)->min = -1e10f;                                     // beyond int: no narrowing
    m.get(c)->max = 1e10f;
    frame(m, key_right());
    CHECK(m.get(c)->value == 0.0f);
}

TEST_CASE("a theme that changes only a class's align re-wraps the text block") {
    UiTree m;
    const UiId tb = m.text_block(0, {0, 0, 200, 40}, "Credits");
    m.get(tb)->style.theme_class = "credits"_id;
    m.layout(kScreen, default_ui_theme(), nullptr);
    const std::uint32_t wraps = m.stats().wraps;
    UiTheme centred;
    UiThemeClass cls = centred.base_class();
    cls.id = "credits"_id;
    cls.align = TextAlign::Center;
    cls.align_set = true;
    centred.classes.push_back(cls);
    m.layout(kScreen, centred, nullptr);
    CHECK(m.stats().wraps == wraps + 1);
    m.layout(kScreen, centred, nullptr);                        // nothing changed: no re-wrap
    CHECK(m.stats().wraps == wraps + 1);
}

TEST_CASE("dialogue re-wraps on a new font size; an unbounded log draws every line") {
    test::HeadlessApp h;
    h.add<AssetsPlugin>();
    AssetServer &assets = h.app().res<AssetServer>();
    const Font *font = assets.get(test::add_test_font(assets));
    REQUIRE(font != nullptr);
    RenderQueue q{4096};

    DialogueScript s;
    DialogueLine l;
    l.id = intern("l");
    l.text = "Hello there";
    s.lines = {l};
    DialogueRunner d;
    d.start(s, StrId{});
    d.update(10.0f);
    TextLayout cache;
    draw_dialogue_box(q, d, cache, default_ui_theme(), *font, {0, 200, 300, 60});
    CHECK(cache.font_size() == default_ui_theme().font_size);
    UiTheme big;
    big.font_size = 16;
    q.clear();
    draw_dialogue_box(q, d, cache, big, *font, {0, 200, 300, 60});
    CHECK(cache.font_size() == 16.0f);
    CHECK(cache.font() == font);

    MessageLog log(10);
    for (int i = 0; i < 10; ++i) log.add("ab");
    q.clear();
    draw_message_log(q, log, *font, {4, 4, 300, std::numeric_limits<float>::infinity()});
    const std::size_t all = q.size();
    CHECK(all >= 20);                                           // every line's glyphs
    q.clear();
    draw_message_log(q, log, *font, {4, 4, 300, std::numeric_limits<float>::max()});
    CHECK(q.size() == all);
    q.clear();
    draw_message_log(q, log, *font, {4, 4, 300, std::numeric_limits<float>::quiet_NaN()});
    CHECK(q.size() == 0);
}
