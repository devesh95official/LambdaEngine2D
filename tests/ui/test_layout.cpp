// Layout and hit testing with the 6x8 fallback metrics: line height 8, padding 4, spacing 2.
#include <algorithm>

#include <lam/assets/asset_server.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>
#include <lam/ui/ui.h>

#include "ui_test_util.h"

using namespace lam;
using namespace lam::literals;
using namespace lam::uitest;

namespace {
bool same(const Rect &a, const Rect &b) {
    return a.x == doctest::Approx(b.x) && a.y == doctest::Approx(b.y) && a.w == doctest::Approx(b.w) &&
           a.h == doctest::Approx(b.h);
}
} // namespace

TEST_CASE("layout: a vertical stack sizes to its children and stretches them") {
    UiTree t;
    const UiId p = t.panel(0, {10, 20, 0, 0});
    const UiId play = t.button(p, "play"_id, "Play");           // 4 x 6 = 24 px + 2 x 4 padding
    const UiId opts = t.button(p, "opts"_id, "Options");        // 42 + 8 = 50 px
    lay(t);
    CHECK(same(t.screen_rect(p), {10, 20, 58, 42}));            // 50 + 8; 16 + 2 + 16 + 8
    CHECK(same(t.screen_rect(play), {14, 24, 50, 16}));         // stretched to the content width
    CHECK(same(t.screen_rect(opts), {14, 42, 50, 16}));
}

TEST_CASE("layout: anchors and pivots place roots and None children") {
    UiTree t;
    const UiId p = t.panel(0, {0, 0, 0, 0});
    t.get(p)->anchor = t.get(p)->pivot = {0.5f, 0.5f};
    t.button(p, "play"_id, "Play");
    t.button(p, "opts"_id, "Options");
    lay(t);
    CHECK(same(t.screen_rect(p), {211, 114, 58, 42}));          // centred on 480 x 270

    UiTree u;
    const UiId box = u.panel(0, {0, 0, 200, 100}, UiLayout::None);
    const UiId x = u.button(box, "x"_id, "X");
    UiNode *xn = u.get(x);
    xn->anchor = xn->pivot = {1, 1};
    xn->rect.x = xn->rect.y = -2;
    lay(u);
    CHECK(same(u.screen_rect(x), {180, 78, 14, 16}));           // bottom-right corner, 2 px inset
    // Fill takes the parent's content size.
    UiNode bar;
    bar.kind = UiKind::Panel;
    bar.fill = kUiFillX;
    bar.rect.h = 6;
    const UiId b = u.add(box, bar);
    lay(u);
    CHECK(same(u.screen_rect(b), {4, 4, 192, 6}));
    UiNode full;
    full.kind = UiKind::Panel;
    full.fill = kUiFillX | kUiFillY;
    const UiId f = u.add(0, full);
    lay(u);
    CHECK(same(u.screen_rect(f), {0, 0, 480, 270}));
}

TEST_CASE("layout: horizontal stacks, padding and spacing") {
    UiTree t;
    const UiId row = t.panel(0, {}, UiLayout::Horizontal);
    const UiId a = t.button(row, "a"_id, "Play");
    const UiId b = t.button(row, "b"_id, "Options");
    lay(t);
    CHECK(same(t.screen_rect(row), {0, 0, 92, 24}));
    CHECK(same(t.screen_rect(a), {4, 4, 32, 16}));
    CHECK(same(t.screen_rect(b), {38, 4, 50, 16}));
    t.get(row)->style.padding = {10, 5};
    t.get(row)->style.spacing = 6;
    lay(t);
    CHECK(same(t.screen_rect(row), {0, 0, 108, 26}));
    CHECK(same(t.screen_rect(b), {48, 5, 50, 16}));
    // Anchor aligns across the stack: a shorter child centred vertically.
    t.get(a)->rect.h = 10;
    t.get(a)->anchor.y = 0.5f;
    lay(t);
    CHECK(same(t.screen_rect(a), {10, 8, 32, 10}));
}

TEST_CASE("layout: grids place cells row-major") {
    UiTree t;
    const UiId g = t.panel(0, {}, UiLayout::Grid);
    t.get(g)->style.grid_columns = 3;
    t.get(g)->style.padding = {0, 0};
    t.get(g)->style.spacing = 1;
    TextureRegion r;
    r.source = {0, 0, 16, 16};
    UiId cells[5];
    for (UiId &c : cells) c = t.image(g, r);
    lay(t);
    CHECK(same(t.screen_rect(g), {0, 0, 50, 33}));
    for (int k = 0; k < 5; ++k) {
        const float x = static_cast<float>((k % 3) * 17), y = static_cast<float>((k / 3) * 17);
        CHECK(same(t.screen_rect(cells[k]), {x, y, 16, 16}));
    }
}

TEST_CASE("layout: invisible nodes take no space and are not shown") {
    UiTree t;
    const UiId p = t.panel(0, {});
    const UiId a = t.button(p, "a"_id, "A");
    const UiId b = t.button(p, "b"_id, "B");
    const UiId c = t.button(p, "c"_id, "C");
    t.get(b)->visible = false;
    lay(t);
    CHECK(t.shown(a));
    CHECK_FALSE(t.shown(b));
    CHECK(t.screen_rect(b).w == 0);                             // {} for hidden nodes
    CHECK(t.screen_rect(c).y == doctest::Approx(t.screen_rect(a).y + 18));
    t.get(p)->visible = false;
    lay(t);
    CHECK_FALSE(t.shown(a));
    CHECK_FALSE(t.shown(c));
}

TEST_CASE("layout: max_size caps a list, which then scrolls and clips") {
    UiTree t;
    const UiId list = t.list(0, {});
    UiNode *ln = t.get(list);
    ln->style.padding = {0, 0};
    ln->max_size.y = 40;
    UiId items[6];
    for (UiId &i : items) i = t.button(list, "i"_id, "Item");
    lay(t);
    CHECK(t.screen_rect(list).h == doctest::Approx(40));
    CHECK(t.shown(items[2]));                                   // y 36: partly visible
    CHECK_FALSE(t.shown(items[3]));
    CHECK(same(t.clip_rect(items[0]), t.screen_rect(list)));
    t.scroll_by(list, 100);                                     // clamped to the last full page
    lay(t);
    CHECK(t.get(list)->scroll == 4);
    CHECK(t.shown(items[5]));
    CHECK_FALSE(t.shown(items[3]));
    CHECK(t.screen_rect(items[4]).y == doctest::Approx(0));
    t.scroll_by(list, -1);
    lay(t);
    CHECK(t.get(list)->scroll == 3);
    // A grid list scrolls by rows.
    UiTree g;
    const UiId gl = g.grid(0, {0, 0, 60, 20}, 3);
    g.get(gl)->style.padding = {0, 0};
    TextureRegion r;
    r.source = {0, 0, 8, 8};
    for (int k = 0; k < 9; ++k) g.image(gl, r);
    lay(g);
    g.scroll_by(gl, 1);
    lay(g);
    CHECK(g.get(gl)->scroll == 3);
}

TEST_CASE("layout: text blocks wrap to their width and scroll by lines") {
    UiTree t;
    const UiId tb = t.text_block(0, {0, 0, 64, 0}, "aaa bbb ccc ddd");
    lay(t);
    CHECK(t.screen_rect(tb).h == doctest::Approx(2 * 8 + 8));   // "aaa bbb" / "ccc ddd"
    const UiId small = t.text_block(0, {0, 100, 64, 20}, "one two three four five six");
    lay(t);
    t.scroll_by(small, 10);
    lay(t);
    CHECK(t.get(small)->scroll > 0);
    CHECK(t.get(small)->scroll <= 3);
    // A block stretched by a sized vertical parent wraps at the parent's content width.
    const UiId p = t.panel(0, {0, 150, 72, 0});
    const UiId inner = t.text_block(p, {}, "aaa bbb ccc ddd");
    t.get(inner)->style.padding = {0, 0};
    lay(t);
    CHECK(t.screen_rect(inner).w == doctest::Approx(64));
    CHECK(t.screen_rect(inner).h == doctest::Approx(16));
}

TEST_CASE("layout: labels re-measure and blocks re-wrap only when something changed") {
    UiTree t;
    const UiId l = t.label(0, "Score 0");
    const UiId tb = t.text_block(0, {0, 20, 100, 0}, "the quick brown fox jumps over the lazy dog");
    lay(t);
    const UiTree::Stats s0 = t.stats();
    CHECK(s0.measures >= 1);
    CHECK(s0.wraps >= 1);
    for (int i = 0; i < 10; ++i) lay(t);
    CHECK(t.stats().measures == s0.measures);
    CHECK(t.stats().wraps == s0.wraps);
    t.set_text(l, "Score 10");
    lay(t);
    CHECK(t.stats().measures == s0.measures + 1);
    CHECK(t.stats().wraps == s0.wraps);
    t.get(l)->text = "Score 20";                                // direct edits are seen too
    lay(t);
    CHECK(t.stats().measures == s0.measures + 2);
    t.set_text(tb, "new text");
    lay(t);
    CHECK(t.stats().wraps == s0.wraps + 1);
    t.get(tb)->rect.w = 50;                                     // width change re-wraps
    lay(t);
    CHECK(t.stats().wraps == s0.wraps + 2);
    lay(t);
    CHECK(t.stats().wraps == s0.wraps + 2);
}

TEST_CASE("layout: value widgets reserve a value area at their right") {
    UiTree t;
    const UiId p = t.panel(0, {0, 0, 200, 0});
    const UiId s = t.slider(p, "s"_id, "Vol", 0.5f, 0, 1);
    const UiId tg = t.toggle(p, "t"_id, "On", true);
    lay(t);
    const Rect sr = t.screen_rect(s);
    const Rect bar = t.value_rect(s);
    CHECK(bar.w == doctest::Approx(64));
    CHECK(bar.x + bar.w == doctest::Approx(sr.x + sr.w - 4));
    const Rect box = t.value_rect(tg);
    CHECK(box.w == doctest::Approx(8));
    CHECK(box.h == doctest::Approx(8));
    CHECK(t.value_rect(p).w == 0);
}

TEST_CASE("layout: theme classes change text size (font scale)") {
    test::HeadlessApp h;
    h.add<AssetsPlugin>();
    AssetServer &assets = h.app().res<AssetServer>();
    const Font &font = *assets.get(test::add_test_font(assets));
    UiTheme theme = default_ui_theme();
    UiThemeClass big = theme.base_class();
    big.id = "big"_id;
    big.font_size = 16;                                         // 2x the 8 px base
    theme.classes.push_back(big);
    std::sort(theme.classes.begin(), theme.classes.end(),
              [](const UiThemeClass &a, const UiThemeClass &b) { return a.id.value < b.id.value; });
    UiTree t;
    const UiId small = t.label(0, "ab");
    const UiId large = t.label(0, "ab");
    t.get(large)->style.theme_class = "big"_id;
    t.layout(kScreen, theme, &font);
    CHECK(t.screen_rect(small).w == doctest::Approx(12 + 8));
    CHECK(t.screen_rect(large).w == doctest::Approx(24 + 8));
    CHECK(t.screen_rect(large).h == doctest::Approx(16 + 8));
    CHECK(t.text_scale(large) == doctest::Approx(2.0f));
}

TEST_CASE("hit testing: topmost shown node, clips and the focus root") {
    UiTree t;
    const UiId under = t.panel(0, {0, 0, 100, 100}, UiLayout::None);
    const UiId btn = t.button(under, "b"_id, "B");
    const UiId over = t.panel(0, {50, 50, 100, 100}, UiLayout::None);
    lay(t);
    CHECK(t.hit_test({5, 5}) == btn);
    CHECK(t.hit_test({60, 60}) == over);                        // later roots draw on top
    CHECK(t.hit_test({40, 40}) == under);
    CHECK(t.hit_test({300, 300}) == 0);
    t.get(over)->visible = false;
    lay(t);
    CHECK(t.hit_test({60, 60}) == under);
    t.set_focus_root(btn);
    CHECK(t.hit_test({40, 40}) == 0);                           // outside the focus root
    CHECK(t.hit_test({5, 5}) == btn);
    t.set_focus_root(0);
    // Children of a list outside its clip are not hit.
    UiTree l;
    const UiId list = l.list(0, {0, 0, 60, 30});
    l.get(list)->style.padding = {0, 0};
    l.button(list, "a"_id, "A");
    const UiId b2 = l.button(list, "b"_id, "B");                // y 18..34, clipped at 30
    lay(l);
    CHECK(l.hit_test({5, 20}) == b2);
    CHECK(l.hit_test({5, 31}) == 0);
}
