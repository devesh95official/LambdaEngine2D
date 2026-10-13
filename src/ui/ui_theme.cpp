// UI themes: the built-in default, theme classes, the strict .theme.json parser and its loader.
// The nine-slice image is packed into the theme's atlas group when its pixels are available.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/data.h>
#include <lam/core/log.h>
#include <lam/render/atlas.h>
#include <lam/render/font.h>
#include <lam/render/texture.h>
#include <lam/ui/ui.h>

namespace lam {

const UiThemeClass *UiTheme::find_class(StrId id) const {
    if (!id) return nullptr;
    const auto it = std::lower_bound(classes.begin(), classes.end(), id,
                                     [](const UiThemeClass &c, StrId v) { return c.id.value < v.value; });
    return (it != classes.end() && it->id == id) ? &*it : nullptr;
}

UiThemeClass UiTheme::base_class() const {
    UiThemeClass c;
    c.font_size = font_size;
    c.text = text;
    c.text_disabled = text_disabled;
    c.panel = panel;
    c.panel_border = panel_border;
    c.button = button;
    c.button_focus = button_focus;
    c.button_down = button_down;
    return c;
}

namespace {

void sort_classes(UiTheme &t) {
    std::sort(t.classes.begin(), t.classes.end(),
              [](const UiThemeClass &a, const UiThemeClass &b) { return a.id.value < b.id.value; });
}

// Built-in classes used by the engine menus, added to every theme that does not define them:
// "title" and "heading" stand out, "plain" containers draw nothing of their own.
void add_builtin_classes(UiTheme &t) {
    const auto missing = [&t](StrId id) {
        for (const UiThemeClass &c : t.classes)
            if (c.id == id) return false;
        return true;
    };
    if (missing(str_id("title"))) {
        UiThemeClass title = t.base_class();
        title.id = intern("title");
        title.text = {255, 215, 94, 255};
        title.background = false;
        title.align = TextAlign::Center;
        title.align_set = true;
        t.classes.push_back(title);
    }
    if (missing(str_id("heading"))) {
        UiThemeClass heading = t.base_class();
        heading.id = intern("heading");
        heading.text = {255, 215, 94, 255};
        heading.background = false;
        t.classes.push_back(heading);
    }
    if (missing(str_id("plain"))) {
        UiThemeClass plain = t.base_class();
        plain.id = intern("plain");
        plain.background = false;
        t.classes.push_back(plain);
    }
    sort_classes(t);
}

} // namespace

const UiTheme &default_ui_theme() {
    static const UiTheme theme = [] {
        UiTheme t;
        add_builtin_classes(t);
        return t;
    }();
    return theme;
}

namespace {

void read_color(DataReader &in, std::string_view key, Color &out) {
    if (in.has(key)) out = in.color(key);
}

std::optional<TextAlign> parse_align(std::string_view s) {
    if (s == "left") return TextAlign::Left;
    if (s == "center" || s == "centre") return TextAlign::Center;
    if (s == "right") return TextAlign::Right;
    return std::nullopt;
}

UiThemeClass read_class(DataReader &c, const UiTheme &base, std::string_view name) {
    UiThemeClass k = base.base_class();
    k.id = intern(name);
    k.font_size = c.number_or("font_size", base.font_size, 0.0f, 512.0f);
    read_color(c, "text", k.text);
    read_color(c, "text_disabled", k.text_disabled);
    read_color(c, "panel", k.panel);
    read_color(c, "panel_border", k.panel_border);
    read_color(c, "button", k.button);
    read_color(c, "button_focus", k.button_focus);
    read_color(c, "button_down", k.button_down);
    k.background = c.boolean_or("background", true);
    if (c.has("align")) {
        const std::string a = c.string("align");
        if (const auto al = parse_align(a)) {
            k.align = *al;
            k.align_set = true;
        } else {
            c.fail("align must be \"left\", \"center\" or \"right\"");
        }
    }
    return k;
}

UiTheme read_theme(DataReader &in, LoadContext *deps) {
    UiTheme t;
    if (in.has("font")) {
        const std::string path = in.string("font");
        if (deps != nullptr && !path.empty()) t.font = deps->depend<Font>(path);
    }
    t.font_size = in.number_or("font_size", 0.0f, 0.0f, 512.0f);
    read_color(in, "text", t.text);
    read_color(in, "text_disabled", t.text_disabled);
    read_color(in, "panel", t.panel);
    read_color(in, "panel_border", t.panel_border);
    read_color(in, "button", t.button);
    read_color(in, "button_focus", t.button_focus);
    read_color(in, "button_down", t.button_down);
    read_color(in, "overlay_dim", t.overlay_dim);
    if (in.has("nine_slice")) {
        DataReader ns = in.object("nine_slice");
        const std::string image = ns.string("image");
        if (deps != nullptr && !image.empty()) t.nine_slice.texture = deps->depend<Texture>(image);
        if (ns.has("rect")) {
            const Rect r = ns.rect("rect");
            if (!(r.w > 0.0f && r.h > 0.0f) || r.x < 0.0f || r.y < 0.0f) ns.fail("rect must have x, y >= 0 and w, h > 0");
            t.nine_slice.source = r;
            t.nine_slice.size = {r.w, r.h};
        }
        t.nine_slice_border = ns.number_or("border", 4.0f, 0.0f, 1024.0f);
    }
    if (in.has("atlas")) {
        const std::string group = in.string("atlas");
        t.atlas_group = group == "none" ? StrId{} : intern(group);
    }
    if (in.has("click_sound")) t.click_sound = in.id("click_sound");
    if (in.has("focus_sound")) t.focus_sound = in.id("focus_sound");
    if (in.has("classes")) {
        DataReader classes = in.object("classes");
        for (std::string_view name : classes.keys()) {
            DataReader c = classes.object(name);
            t.classes.push_back(read_class(c, t, name));
        }
    }
    add_builtin_classes(t);
    return t;
}

class UiThemeLoader final : public AssetLoader<UiTheme> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExt[] = {".theme.json"};
        return kExt;
    }
    Result<UiTheme> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        return parse_ui_theme(text, ctx.path(), &ctx);
    }
    Status finalize(UiTheme &theme, FinalizeContext &ctx) override {
        TextureRegion &ns = theme.nine_slice;
        if (!ns.texture) return {};
        const Texture *tex = ctx.server.get(ns.texture);
        if (tex == nullptr)
            return make_error(ErrorCode::Invalid, std::string(ctx.path) + ": nine_slice image is not ready");
        if (ns.source.w <= 0.0f || ns.source.h <= 0.0f) {
            ns.source = {0, 0, static_cast<float>(tex->width), static_cast<float>(tex->height)};
        }
        if (ns.source.x + ns.source.w > static_cast<float>(tex->width) ||
            ns.source.y + ns.source.h > static_cast<float>(tex->height)) {
            return make_error(ErrorCode::OutOfRange,
                              std::string(ctx.path) + ": nine_slice rect lies outside its image");
        }
        ns.size = {ns.source.w, ns.source.h};
        AtlasSet *atlas = ctx.app.try_res<AtlasSet>();
        if (!theme.atlas_group || atlas == nullptr || !tex->pixels.has_pixels()) return {};
        // Crop the region and pack it with the other UI images (one batch with sprites and text).
        ImageData img;
        img.width = static_cast<int>(ns.source.w);
        img.height = static_cast<int>(ns.source.h);
        img.rgba.resize(static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height) * 4u);
        const int sx = static_cast<int>(ns.source.x), sy = static_cast<int>(ns.source.y);
        for (int y = 0; y < img.height; ++y) {
            const std::size_t src = (static_cast<std::size_t>(sy + y) * static_cast<std::size_t>(tex->width) +
                                     static_cast<std::size_t>(sx)) * 4u;
            const std::size_t dst = static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) * 4u;
            std::memcpy(img.rgba.data() + dst, tex->pixels.rgba.data() + src, static_cast<std::size_t>(img.width) * 4u);
        }
        Result<TextureRegion> packed = atlas->add(ctx.server, img, theme.atlas_group);
        if (packed) {
            ns = *packed;
        } else {
            LAM_LOG_WARN("{}: nine_slice kept as a standalone texture ({})", ctx.path, packed.error().message);
        }
        return {};
    }
};

} // namespace

Result<UiTheme> parse_ui_theme(std::string_view json, std::string_view file_name, LoadContext *deps) {
    return parse_data<UiTheme>(json, file_name, [deps](DataReader &root) { return read_theme(root, deps); });
}

std::unique_ptr<AssetLoader<UiTheme>> make_ui_theme_loader() {
    return std::make_unique<UiThemeLoader>();
}

} // namespace lam
