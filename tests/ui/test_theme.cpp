// Themes: the built-in default, .theme.json parsing and the asset loader.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <lam/assets/asset_server.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>
#include <lam/ui/ui.h>
#include <lam/ui/ui_plugin.h>

using namespace lam;
using namespace lam::literals;

namespace {

// PNG signature plus an IHDR chunk. The headless decoder only reads the size.
std::string png_header(std::uint32_t w, std::uint32_t h) {
    std::string s = "\x89PNG\r\n\x1a\n";
    const auto be32 = [&s](std::uint32_t v) {
        s.push_back(static_cast<char>(v >> 24));
        s.push_back(static_cast<char>(v >> 16));
        s.push_back(static_cast<char>(v >> 8));
        s.push_back(static_cast<char>(v));
    };
    be32(13);
    s += "IHDR";
    be32(w);
    be32(h);
    s += std::string("\x08\x06\x00\x00\x00", 5);
    be32(0);                                                    // CRC (not checked by the header reader)
    return s;
}

} // namespace

TEST_CASE("theme: the built-in default has the engine classes") {
    const UiTheme &t = default_ui_theme();
    CHECK_FALSE(t.font);
    REQUIRE(t.find_class("title"_id) != nullptr);
    CHECK(t.find_class("title"_id)->align == TextAlign::Center);
    CHECK_FALSE(t.find_class("plain"_id)->background);
    CHECK(t.find_class("heading"_id) != nullptr);
    CHECK(t.find_class("missing"_id) == nullptr);
    CHECK(t.find_class(StrId{}) == nullptr);
    const UiThemeClass base = t.base_class();
    CHECK(base.text == t.text);
    CHECK(base.button_focus == t.button_focus);
}

TEST_CASE("theme: a full document parses; classes inherit the base values") {
    const char *json = R"({
      "font_size": 16,
      "text": "#102030",
      "text_disabled": [1, 2, 3, 4],
      "panel": "#11223344",
      "panel_border": [9, 9, 9],
      "button": "#202020", "button_focus": "#303030", "button_down": "#404040",
      "overlay_dim": [0, 0, 0, 200],
      "nine_slice": {"image": "panel.png", "rect": [0, 0, 12, 12], "border": 3},
      "atlas": "ui",
      "click_sound": "ui_click",
      "focus_sound": "ui_move",
      "classes": {
        "title": {"text": "#ffd75e", "font_size": 24, "align": "center", "background": false},
        "warning": {"button": [200, 40, 40]}
      }
    })";
    Result<UiTheme> r = parse_ui_theme(json, "test.theme.json");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const UiTheme &t = *r;
    CHECK(t.font_size == 16);
    CHECK(t.text == Color{0x10, 0x20, 0x30, 255});
    CHECK(t.text_disabled == Color{1, 2, 3, 4});
    CHECK(t.panel == Color{0x11, 0x22, 0x33, 0x44});
    CHECK(t.panel_border == Color{9, 9, 9, 255});
    CHECK(t.overlay_dim.a == 200);
    CHECK(t.nine_slice.source == Rect{0, 0, 12, 12});
    CHECK(t.nine_slice_border == 3);
    CHECK_FALSE(t.nine_slice.texture);                          // no LoadContext: path validated only
    CHECK(t.atlas_group == "ui"_id);
    CHECK(t.click_sound == "ui_click"_id);
    CHECK(t.focus_sound == "ui_move"_id);
    const UiThemeClass *title = t.find_class("title"_id);
    REQUIRE(title != nullptr);
    CHECK(title->font_size == 24);
    CHECK(title->align == TextAlign::Center);
    CHECK(title->align_set);
    CHECK_FALSE(title->background);
    CHECK(title->button == t.button);                           // inherited
    const UiThemeClass *warning = t.find_class("warning"_id);
    REQUIRE(warning != nullptr);
    CHECK(warning->button == Color{200, 40, 40, 255});
    CHECK(warning->text == t.text);
    CHECK(warning->font_size == 16);
    CHECK(t.find_class("plain"_id) != nullptr);                 // built-ins added when missing
}

TEST_CASE("theme: an empty document is the default theme; atlas none opts out") {
    Result<UiTheme> r = parse_ui_theme("{}", "empty.theme.json");
    REQUIRE(r.ok());
    CHECK(r->text == default_ui_theme().text);
    CHECK(r->atlas_group == "default"_id);
    Result<UiTheme> n = parse_ui_theme(R"({"atlas": "none"})", "n.theme.json");
    REQUIRE(n.ok());
    CHECK_FALSE(n->atlas_group);
}

TEST_CASE("theme: errors name the file and the JSON path") {
    struct Case { const char *json; const char *needle; };
    const Case cases[] = {
        {R"({"txt": "#ffffff"})", "txt"},                                       // unknown key
        {R"({"text": "#zzzzzz"})", "text"},                                     // bad colour
        {R"({"font_size": -3})", "font_size"},                                  // out of range
        {R"({"nine_slice": {"rect": [0, 0, 4, 4]}})", "image"},                 // missing image
        {R"({"nine_slice": {"image": "a.png", "rect": [0, 0, 0, 4]}})", "rect"},
        {R"({"classes": {"x": {"align": "middle"}}})", "align"},
        {R"({"classes": {"x": {"colour": "#fff"}}})", "colour"},
        {R"({"classes": {"x": 3}})", "x"},
        {R"([1, 2])", "theme"},
        {R"({"text": )", "theme"},                                              // syntax error
    };
    for (const Case &c : cases) {
        CAPTURE(c.json);
        Result<UiTheme> r = parse_ui_theme(c.json, "bad.theme.json");
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().message.find("bad.theme.json") != std::string::npos);
        CHECK(r.error().message.find(c.needle) != std::string::npos);
    }
}

TEST_CASE("theme loader: loads through the asset server with its nine-slice image") {
    test::TempDir dir;
    test::write_test_file(dir, "ui/panel.png", png_header(24, 16));
    test::write_test_file(dir, "ui/main.theme.json",
                          R"({"text": "#c0c0c0", "nine_slice": {"image": "panel.png", "border": 5}})");
    test::write_test_file(dir, "ui/cut.theme.json",
                          R"({"nine_slice": {"image": "panel.png", "rect": [8, 0, 8, 8]}})");
    test::write_test_file(dir, "ui/outside.theme.json",
                          R"({"nine_slice": {"image": "panel.png", "rect": [20, 0, 8, 8]}})");
    test::HeadlessApp h;
    AssetsConfig cfg;
    cfg.root = dir.path();
    h.add<AssetsPlugin>(cfg);
    h.add<UiPlugin>();
    AssetServer &assets = h.app().res<AssetServer>();
    assets.register_loader<Texture>(make_texture_loader());     // RenderPlugin's job; this app has none
    Result<AssetId<UiTheme>> id = assets.load_now<UiTheme>("ui/main.theme.json");
    REQUIRE_MESSAGE(id.ok(), (id.ok() ? "" : id.error().message));
    const UiTheme *t = assets.get(*id);
    REQUIRE(t != nullptr);
    CHECK(t->text == Color{0xc0, 0xc0, 0xc0, 255});
    CHECK(t->nine_slice.texture);
    CHECK(t->nine_slice.source == Rect{0, 0, 24, 16});          // whole image (no pixels headless: not packed)
    CHECK(t->nine_slice.size == Vec2{24, 16});
    CHECK(t->nine_slice_border == 5);
    Result<AssetId<UiTheme>> cut = assets.load_now<UiTheme>("ui/cut.theme.json");
    REQUIRE(cut.ok());
    CHECK(assets.get(*cut)->nine_slice.source == Rect{8, 0, 8, 8});
    test::CaptureLog log;
    Result<AssetId<UiTheme>> outside = assets.load_now<UiTheme>("ui/outside.theme.json");
    CHECK_FALSE(outside.ok());
    Result<AssetId<UiTheme>> missing = assets.load_now<UiTheme>("ui/none.theme.json");
    CHECK_FALSE(missing.ok());
}

TEST_CASE("theme loader: a theme font comes from render's font loaders") {
    // The ".fnt" loader handles the theme's font dependency.
    test::TempDir dir;
    test::write_test_file(dir, "ui/font.theme.json", R"({"font": "ui.fnt"})");
    test::HeadlessApp h;
    AssetsConfig cfg;
    cfg.root = dir.path();
    h.add<AssetsPlugin>(cfg);
    h.add<UiPlugin>();
    Result<AssetId<UiTheme>> id = h.app().res<AssetServer>().load_now<UiTheme>("ui/font.theme.json");
    CHECK_FALSE(id.ok());                                       // the file does not exist: the parent fails
}
