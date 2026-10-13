// UiTree storage: ids, generations, builders and setters.
#include <array>
#include <limits>
#include <string>
#include <string_view>

#include <lam/test/support.h>
#include <lam/ui/ui.h>

#include "ui_test_util.h"

using namespace lam;
using namespace lam::literals;

TEST_CASE("ui tree: add returns live ids; get, find and size follow them") {
    UiTree t;
    CHECK(t.size() == 0);
    const UiId root = t.panel(0, {0, 0, 100, 50});
    const UiId play = t.button(root, "play"_id, "Play");
    const UiId label = t.label(root, "Hello");
    CHECK(root != 0);
    CHECK(play != 0);
    CHECK(label != 0);
    CHECK(t.size() == 3);
    REQUIRE(t.get(play) != nullptr);
    CHECK(t.get(play)->kind == UiKind::Button);
    CHECK(t.get(play)->parent == root);
    CHECK(t.get(play)->text.view() == "Play");
    CHECK(t.get(play)->focusable);                              // interactive kinds always focusable
    CHECK_FALSE(t.get(label)->focusable);
    CHECK(t.find("play"_id) == play);
    CHECK(t.find("nope"_id) == 0);
    CHECK(t.find(StrId{}) == 0);
    CHECK(t.get(0) == nullptr);
    CHECK(t.alive(play));
    CHECK(t.child_count(root) == 2);
    CHECK(t.first_child(root) == play);
    CHECK(t.next_sibling(play) == label);
    CHECK(t.next_sibling(label) == 0);
    CHECK(t.first_child(0) == root);                            // roots
}

TEST_CASE("ui tree: remove takes the children and stale ids never resolve again") {
    UiTree t;
    const UiId root = t.panel(0, {});
    const UiId a = t.button(root, "a"_id, "A");
    const UiId inner = t.panel(root, {});
    const UiId b = t.button(inner, "b"_id, "B");
    t.focus(b);
    CHECK(t.focused() == b);
    t.remove(inner);
    CHECK(t.size() == 2);
    CHECK(t.get(inner) == nullptr);
    CHECK(t.get(b) == nullptr);
    CHECK(t.focused() == 0);                                    // focus dropped with the node
    CHECK(t.child_count(root) == 1);
    // The freed slots are reused with a new generation: old ids stay dead.
    const UiId c = t.button(root, "c"_id, "C");
    const UiId d = t.button(root, "d"_id, "D");
    CHECK(c != inner);
    CHECK(c != b);
    CHECK(d != inner);
    CHECK(d != b);
    CHECK(t.get(b) == nullptr);
    CHECK(t.get(inner) == nullptr);
    CHECK(t.get(c)->id == "c"_id);
    t.remove(b);                                                // stale remove is a no-op
    CHECK(t.size() == 4);
    t.remove(a);
    CHECK(t.child_count(root) == 2);
    CHECK(t.first_child(root) == c);
}

TEST_CASE("ui tree: clear keeps generations so old ids stay dead") {
    UiTree t;
    const UiId a = t.button(0, "a"_id, "A");
    const std::uint32_t v0 = t.structure_version();
    t.clear();
    CHECK(t.size() == 0);
    CHECK(t.structure_version() != v0);
    const UiId b = t.button(0, "b"_id, "B");
    CHECK(b != a);
    CHECK(t.get(a) == nullptr);
    CHECK(t.get(b) != nullptr);
}

TEST_CASE("ui tree: adding under a dead parent fails") {
    UiTree t;
    const UiId p = t.panel(0, {});
    t.remove(p);
    // LAM_ASSERT fires in builds with asserts; only check the release behaviour there.
#if !LAM_ENABLE_ASSERTS
    CHECK(t.button(p, "x"_id, "X") == 0);
#endif
    CHECK(t.size() == 0);
}

TEST_CASE("ui tree: set_text truncates into the fixed buffer and never allocates") {
    UiTree t;
    const UiId l = t.label(0, "hi");
    const std::string long_text(200, 'x');
    CHECK_NO_ALLOC(t.set_text(l, "hello"));
    CHECK(t.get(l)->text.view() == "hello");
    CHECK_NO_ALLOC(t.set_text(l, long_text));
    CHECK(t.get(l)->text.size() == 63);
    // UTF-8 boundaries are kept (no half code point at the end).
    std::string utf;
    for (int i = 0; i < 40; ++i) utf += "\xC3\xA9";             // e-acute, 2 bytes each
    t.set_text(l, utf);
    CHECK(t.get(l)->text.size() == 62);
    CHECK(utf8_valid(t.get(l)->text.view()));
}

TEST_CASE("ui tree: values are sanitised and choices bounds-checked") {
    UiTree t;
    const UiId s = t.slider(0, "vol"_id, "Volume", 5.0f, 0.0f, 1.0f);
    CHECK(t.get(s)->value == 1.0f);
    t.set_value(s, -3.0f);
    CHECK(t.get(s)->value == 0.0f);
    t.set_value(s, std::numeric_limits<float>::quiet_NaN());
    CHECK(t.get(s)->value == 0.0f);
    const UiId r = t.slider(0, "rev"_id, "Reversed", 0.5f, 1.0f, 0.0f);   // min/max swapped
    CHECK(t.get(r)->min == 0.0f);
    CHECK(t.get(r)->max == 1.0f);

    const std::array<std::string_view, 3> opts{"Slow", "Normal", "Fast"};
    const UiId c = t.choice(0, "speed"_id, "Speed", opts, 7);
    CHECK(t.get(c)->choice_count == 3);
    CHECK(t.get(c)->value == 2.0f);                             // clamped to the last choice
    t.set_value(c, -1.0f);
    CHECK(t.get(c)->value == 0.0f);
    std::array<std::string_view, 12> many{};
    for (auto &m : many) m = "x";
    t.set_choices(c, many, 11);
    CHECK(t.get(c)->choice_count == kMaxUiChoices);
    CHECK(t.get(c)->value == static_cast<float>(kMaxUiChoices - 1));
    const UiId tg = t.toggle(0, "t"_id, "T", true);
    CHECK(t.get(tg)->value == 1.0f);
    t.set_value(tg, 0.3f);
    CHECK(t.get(tg)->value == 0.0f);
}

TEST_CASE("ui tree: focus ignores dead ids; focus root confines navigation") {
    UiTree t;
    const UiId a = t.button(0, "a"_id, "A");
    const UiId popup = t.panel(0, {});
    const UiId b = t.button(popup, "b"_id, "B");
    const UiId c = t.button(popup, "c"_id, "C");
    t.focus(a);
    CHECK(t.focused() == a);
    t.focus(12345);
    CHECK(t.focused() == a);
    t.set_focus_root(popup);
    CHECK(t.focus_root() == popup);
    t.focus(b);
    uitest::frame(t, uitest::key_down());
    CHECK(t.focused() == c);
    uitest::frame(t, uitest::key_down());
    CHECK(t.focused() == b);                                    // wraps inside the popup, never to `a`
    t.remove(popup);
    CHECK(t.focus_root() == 0);
}

TEST_CASE("ui tree: text blocks own long text; set_text and set_block_text replace it") {
    UiTree t;
    const std::string text(500, 'a');
    const UiId tb = t.text_block(0, {0, 0, 100, 40}, text);
    CHECK(t.block_text(tb).size() == 500);
    t.set_text(tb, "short");
    CHECK(t.block_text(tb) == "short");
    t.set_block_text(tb, std::string("replaced"));
    CHECK(t.block_text(tb) == "replaced");
    CHECK(t.block_text(t.label(0, "x")).empty());
}
