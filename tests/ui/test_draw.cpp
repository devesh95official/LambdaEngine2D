// UI drawing into the RenderQueue.
#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

#include <lam/render/render_queue.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>
#include <lam/ui/dialogue.h>
#include <lam/ui/ui.h>
#include <lam/ui/ui_draw.h>

#include "ui_test_util.h"

using namespace lam;
using namespace lam::literals;
using namespace lam::uitest;

namespace {

struct DrawFixture {
    test::HeadlessApp h;
    AssetServer *assets = nullptr;
    AssetId<Font> font_id;
    const Font *font = nullptr;
    RenderQueue q{4096};
    DrawFixture() {
        h.add<AssetsPlugin>();
        assets = &h.app().res<AssetServer>();
        font_id = test::add_test_font(*assets);
        font = assets->get(font_id);
    }
    std::size_t glyphs() const {
        const std::uint16_t page = texture_slot(font->page);
        return static_cast<std::size_t>(std::count_if(q.quads().begin(), q.quads().end(), [page](const DrawQuad &d) {
            return key_texture(d.key) == page;
        }));
    }
    void draw(UiTree &t, const UiTheme &theme = default_ui_theme()) {
        q.clear();
        t.layout(kScreen, theme, font);
        draw_ui_tree(q, t, theme, font);
    }
};

} // namespace

TEST_CASE("draw: labels push one quad per glyph on the UI layer") {
    DrawFixture f;
    UiTree t;
    t.label(0, "Hi you");                                       // 5 glyphs (space draws nothing)
    f.draw(t);
    CHECK(f.q.size() == 5);
    CHECK(f.glyphs() == 5);
    for (const DrawQuad &d : f.q.quads()) CHECK(key_layer(d.key) == kLayerUi);
    f.q.clear();
    draw_ui_tree(f.q, t, default_ui_theme(), nullptr);         // no font: no text
    CHECK(f.q.size() == 0);
}

TEST_CASE("draw: panels and buttons are a fill and a 1 px frame; hidden nodes draw nothing") {
    DrawFixture f;
    UiTree t;
    const UiId p = t.panel(0, {0, 0, 100, 0});
    const UiId b = t.button(p, "ok"_id, "OK");
    f.draw(t);
    CHECK(f.q.size() == 5 + 5 + 2);                             // panel box, button box, 2 glyphs
    t.get(b)->visible = false;
    f.draw(t);
    CHECK(f.q.size() == 5);
    t.get(p)->style.theme_class = "plain"_id;                   // no background
    f.draw(t);
    CHECK(f.q.size() == 0);
    t.get(p)->color = Color{0, 0, 0, 100};                      // explicit fill wins
    f.draw(t);
    CHECK(f.q.size() == 1);
}

TEST_CASE("draw: focus, press and disabled states pick the theme colours") {
    DrawFixture f;
    UiTree t;
    const UiId a = t.button(0, "a"_id, "A");
    f.draw(t);
    const Color normal = f.q.quads()[0].v[0].color;
    CHECK(normal == default_ui_theme().button);
    t.focus(a);
    f.draw(t);
    CHECK(f.q.quads()[0].v[0].color == default_ui_theme().button_focus);
    t.get(a)->enabled = false;
    f.draw(t);
    CHECK(f.q.quads()[0].v[0].color.a < default_ui_theme().button.a);
    // Disabled text uses text_disabled.
    const DrawQuad &glyph = f.q.quads().back();
    CHECK(glyph.v[0].color == default_ui_theme().text_disabled);
}

TEST_CASE("draw: value widgets draw their value area") {
    DrawFixture f;
    UiTree t;
    const UiId tg = t.toggle(0, "t"_id, "", false);
    f.draw(t);
    const std::size_t off = f.q.size();
    CHECK(off == 5 + 4);                                        // box + empty check frame
    t.set_value(tg, 1);
    f.draw(t);
    CHECK(f.q.size() == off + 1);                               // + the check mark
    UiTree s;
    s.slider(0, "s"_id, "", 0.5f, 0, 1);
    f.draw(s);
    CHECK(f.q.size() == 5 + 3);                                 // box + track, fill, knob
    UiTree c;
    const std::array<std::string_view, 2> opts{"Lo", "Hi"};
    c.choice(0, "c"_id, "", opts, 0);
    f.draw(c);
    CHECK(f.glyphs() == 4);                                     // "<", ">", "Lo"
}

TEST_CASE("draw: nine-slice themes draw panels as nine quads of their image") {
    DrawFixture f;
    UiTheme theme = default_ui_theme();
    theme.nine_slice.texture = test::add_test_texture(*f.assets, {12, 12});
    theme.nine_slice.source = {0, 0, 12, 12};
    theme.nine_slice_border = 4;
    UiTree t;
    t.panel(0, {0, 0, 50, 40});
    f.draw(t, theme);
    REQUIRE(f.q.size() == 9);
    const std::uint16_t slot = texture_slot(theme.nine_slice.texture);
    for (const DrawQuad &d : f.q.quads()) CHECK(key_texture(d.key) == slot);
    // The corners keep the border unscaled: the top-left quad is 4 x 4 at the origin.
    const DrawQuad &tl = f.q.quads()[0];
    CHECK(tl.v[0].pos == Vec2{0, 0});
    CHECK(tl.v[2].pos == Vec2{4, 4});
    CHECK(tl.v[2].uv == Vec2{4, 4});
    // The centre stretches.
    const DrawQuad &mid = f.q.quads()[4];
    CHECK(mid.v[2].pos == Vec2{46, 36});
    CHECK(mid.v[2].uv == Vec2{8, 8});
    // A destination smaller than two borders shrinks them instead of overlapping.
    f.q.clear();
    draw_nine_slice(f.q, theme.nine_slice, 4, {0, 0, 6, 6}, kWhite, kLayerUi);
    CHECK(f.q.size() == 4);                                     // centre and edges collapse to nothing
}

TEST_CASE("draw: list children use a clip rectangle; outside the list there is none") {
    DrawFixture f;
    UiTree t;
    const UiId list = t.list(0, {10, 10, 60, 30});
    for (int i = 0; i < 5; ++i) t.label(list, "Row");
    t.label(0, "Free");
    f.draw(t);
    REQUIRE(f.q.clips().size() == 1);
    CHECK(f.q.clips()[0] == IRect{14, 14, 52, 22});
    std::size_t clipped = 0, free = 0;
    for (const DrawQuad &d : f.q.quads()) (key_clip(d.key) != 0 ? clipped : free) += 1;
    CHECK(clipped > 0);
    CHECK(free == 4 + 2);                                       // "Free" + the list's scroll bar
    (void)list;
}

TEST_CASE("draw: text blocks reveal glyphs for typewriters and scroll by lines") {
    DrawFixture f;
    UiTree t;
    const UiId tb = t.text_block(0, {0, 0, 200, 0}, "abcdef ghijkl");
    f.draw(t);
    CHECK(f.glyphs() == 12);
    t.set_reveal(tb, 5);
    f.draw(t);
    CHECK(f.glyphs() == 5);
    t.set_reveal(tb, -1);
    // Lines outside the visible area are culled.
    const UiId lines = t.text_block(0, {0, 100, 200, 16}, "aa\nbb\ncc\ndd");   // 1 visible line (8 px content)
    f.draw(t);
    CHECK(f.glyphs() == 12 + 2 * 2);                            // culled to the lines near the area
    t.scroll_by(lines, 3);
    f.draw(t);
    CHECK(t.get(lines)->scroll == 3);
}

TEST_CASE("draw: toasts, message log and dialogue box helpers") {
    DrawFixture f;
    ToastQueue toasts;
    toasts.push("Saved");
    toasts.push("Saved");
    toasts.update(0.5f);
    draw_toast_list(f.q, toasts, default_ui_theme(), f.font, kScreen);
    CHECK(f.glyphs() == 7);                                     // "Saved x2" (the space draws nothing)
    CHECK(key_layer(f.q.quads()[0].key) == kLayerOverlay);
    f.q.clear();
    draw_toast_list(f.q, toasts, default_ui_theme(), nullptr, kScreen);
    CHECK(f.q.size() == 0);

    f.q.clear();
    MessageLog log(10);
    for (int i = 0; i < 10; ++i) log.add("ab");
    draw_message_log(f.q, log, *f.font, {0, 0, 100, 24});      // 3 lines of 8 px fit
    CHECK(f.glyphs() == 6);

    f.q.clear();
    DialogueScript s;
    DialogueLine l;
    l.id = intern("l");
    l.speaker = intern("Bo");
    l.text = "Hello there friend";
    l.choices[0] = {"Ok", StrId{}};
    l.choice_count = 1;
    s.lines = {l};
    DialogueRunner d;
    d.start(s, StrId{});
    d.update(0.1f, 50.0f);                                      // 5 code points: "Hello"
    TextLayout cache;
    draw_dialogue_box(f.q, d, cache, default_ui_theme(), *f.font, {0, 200, 200, 60});
    CHECK(f.glyphs() == 2 + 5);                                 // speaker + typed glyphs, no choices yet
    f.q.clear();
    d.update(10.0f);
    draw_dialogue_box(f.q, d, cache, default_ui_theme(), *f.font, {0, 200, 200, 60});
    CHECK(f.glyphs() == 2 + 16 + 3);                            // all text + "> Ok"
}

TEST_CASE("draw: no allocation once the queue and the tree are warm") {
    DrawFixture f;
    UiTree t;
    const UiId p = t.panel(0, {0, 0, 0, 0});
    t.get(p)->anchor = t.get(p)->pivot = {0.5f, 0.5f};
    t.label(p, "Settings");
    const UiId vol = t.slider(p, "v"_id, "Volume", 0.5f, 0, 1);
    t.toggle(p, "f"_id, "Fullscreen", false);
    const UiId list = t.list(p, {0, 0, 120, 40});
    for (int i = 0; i < 10; ++i) t.button(list, "i"_id, "Item");
    t.text_block(p, {0, 0, 120, 0}, "Some long text that wraps over a few lines in the box.");
    t.focus(vol);
    ToastQueue toasts;
    for (int i = 0; i < 3; ++i) f.draw(t);
    CHECK_NO_ALLOC({
        for (int i = 0; i < 200; ++i) {
            f.q.clear();
            frame(t, i % 2 ? key_down() : key_right(), f.font);
            draw_ui_tree(f.q, t, default_ui_theme(), f.font);
            toasts.push("Hello");
            toasts.update(0.016f);
            draw_toast_list(f.q, toasts, default_ui_theme(), f.font, kScreen);
        }
    });
}
