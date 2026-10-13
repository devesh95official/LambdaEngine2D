// AnimationPlugin and the sprite sheet / animation graph loaders. Sheets are packed into their
// atlas group at finalize, in AssetId order, so headless and windowed runs get the same regions.
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lam/animation/animation_plugin.h>
#include <lam/animation/animator.h>
#include <lam/animation/sprite_sheet.h>
#include <lam/animation/tween.h>
#include <lam/assets/asset_server.h>
#include <lam/core/log.h>
#include <lam/render/atlas.h>
#include <lam/render/texture.h>

#include "detail/animation_detail.h"

namespace lam {
namespace {

// Lower-case suffix of the file name from its last dot (".png"); "#fragment" stripped.
std::string extension_of(std::string_view path) {
    if (const std::size_t hash = path.find('#'); hash != std::string_view::npos)
        path = path.substr(0, hash);
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = file.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    std::string ext(file.substr(dot));
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ext;
}

std::string_view file_name_of(std::string_view path) {
    if (const std::size_t hash = path.find('#'); hash != std::string_view::npos)
        path = path.substr(0, hash);
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string_view as_text(std::span<const std::byte> bytes) {
    return std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

// The asset server prefixes errors with the asset path; parse errors already start with it.
Error without_path_prefix(Error e, std::string_view path) {
    const std::string prefix = std::string(path) + ": ";
    if (e.message.compare(0, prefix.size(), prefix) == 0) e.message.erase(0, prefix.size());
    return e;
}

class SpriteSheetLoader final : public AssetLoader<SpriteSheet> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExtensions[] = {".json", ".png"};
        return kExtensions;
    }

    Result<SpriteSheet> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        const std::string_view path = ctx.path();
        if (extension_of(path) == ".png") {
            Result<ImageData> image = decode_image(bytes, ".png");
            if (!image) return image.error();
            SpriteSheet sheet =
                make_single_frame_sheet({image->width, image->height}, file_name_of(path));
            sheet.pending_image = std::move(image.value());
            return sheet;
        }
        Result<SpriteSheet> parsed = parse_aseprite_json(as_text(bytes), path);
        if (!parsed) return without_path_prefix(parsed.error(), path);
        SpriteSheet sheet = std::move(parsed.value());
        Result<std::vector<std::byte>> image_bytes = ctx.read(sheet.image_path);
        if (!image_bytes) return image_bytes.error();
        Result<ImageData> image = decode_image(*image_bytes, extension_of(sheet.image_path));
        if (!image) {
            return make_error(image.error().code,
                              "image '" + sheet.image_path + "': " + image.error().message);
        }
        if (image->width != sheet.image_size.x || image->height != sheet.image_size.y) {
            return make_error(ErrorCode::Invalid,
                              "image '" + sheet.image_path + "' is " + std::to_string(image->width) +
                                  " x " + std::to_string(image->height) + " but meta.size says " +
                                  std::to_string(sheet.image_size.x) + " x " +
                                  std::to_string(sheet.image_size.y));
        }
        sheet.pending_image = std::move(image.value());
        return sheet;
    }

    Status finalize(SpriteSheet &sheet, FinalizeContext &ctx) override {
        Vec2 base{};
        bool placed = false;
        AtlasSet *atlas = ctx.app.try_res<AtlasSet>();
        if (atlas != nullptr && sheet.atlas_group != str_id("none")) {
            Result<TextureRegion> r = atlas->add(ctx.server, sheet.pending_image, sheet.atlas_group);
            if (r) {
                sheet.texture = r->texture;
                base = r->source.min();
                placed = true;
            } else {
                LAM_LOG_WARN("{}: {} (the sheet keeps its own texture)", ctx.path, r.error().message);
            }
        }
        if (!placed) {
            Texture t;
            t.width = sheet.pending_image.width;
            t.height = sheet.pending_image.height;
            t.pixels = std::move(sheet.pending_image);
            sheet.texture = ctx.server.add<Texture>(std::move(t));
        }
        sheet.regions.clear();
        sheet.regions.reserve(sheet.frames.size());
        for (const SheetFrame &f : sheet.frames) {
            TextureRegion r;
            r.texture = sheet.texture;
            r.source = f.source.translated(base);
            r.size = f.source_size;
            r.trim_offset = f.trim_offset;
            sheet.regions.push_back(r);
        }
        sheet.pending_image = ImageData{};
        return {};
    }
};

class AnimGraphLoader final : public AssetLoader<AnimGraph> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExtensions[] = {".anim.json"};
        return kExtensions;
    }

    Result<AnimGraph> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        Result<AnimGraph> g = parse_anim_graph(as_text(bytes), ctx.path());
        if (!g) return without_path_prefix(g.error(), ctx.path());
        if (!g->sheet_path.empty()) g->sheet = ctx.depend<SpriteSheet>(g->sheet_path);
        return g;
    }

    Status finalize(AnimGraph &g, FinalizeContext &ctx) override {
        if (!g.sheet) return {};
        const SpriteSheet *s = ctx.server.get<SpriteSheet>(g.sheet);
        if (s == nullptr) {
            return make_error(ErrorCode::NotFound, "sheet '" + g.sheet_path + "' is not loaded");
        }
        return check_anim_graph(g, *s);
    }
};

} // namespace

namespace detail {
std::unique_ptr<AssetLoader<SpriteSheet>> make_sprite_sheet_loader() {
    return std::make_unique<SpriteSheetLoader>();
}
std::unique_ptr<AssetLoader<AnimGraph>> make_anim_graph_loader() {
    return std::make_unique<AnimGraphLoader>();
}
} // namespace detail

void AnimationPlugin::build(App &app) {
    app.require_plugin("lam.assets", name());
    app.register_component<Animator>();
    app.register_component<AnimParams>();
    app.register_component<MotorAnimBridge>();
    app.register_component<Tween>();
    app.register_component<FrameTween>();
    app.add_event<AnimationEvent>(EventScope::World, 1024);
    app.add_event<TweenFinished>(EventScope::World, 256);
    detail::register_bridge_param_names();
    (void)intern("none");
    // Loaders now if AssetsPlugin came first (startup systems and later builds may load sheets);
    // otherwise finish() registers them once every plugin is built.
    if (AssetServer *assets = app.try_res<AssetServer>()) {
        assets->register_loader<SpriteSheet>(detail::make_sprite_sheet_loader());
        assets->register_loader<AnimGraph>(detail::make_anim_graph_loader());
        loaders_registered_ = true;
    }
    const auto add = [&](Stage stage, SystemFn fn, std::string_view n, int order) {
        SystemDesc d;
        d.stage = stage;
        d.fn = fn;
        d.name = n;
        d.order = order;
        d.scope = SystemScope::PerWorld;
        app.add_system(d);
    };
    add(Stage::Animate, &detail::motor_anim_bridge_system, "motor_anim_bridge", -1500);
    add(Stage::Animate, &detail::animators_system, "animators", -1400);
    add(Stage::Animate, &detail::tweens_system, "tweens", -1300);
    add(Stage::Update, &detail::frame_tweens_system, "frame_tweens", -1500);
}

void AnimationPlugin::finish(App &app) {
    if (loaders_registered_) return;
    if (AssetServer *assets = app.try_res<AssetServer>()) {
        assets->register_loader<SpriteSheet>(detail::make_sprite_sheet_loader());
        assets->register_loader<AnimGraph>(detail::make_anim_graph_loader());
        loaders_registered_ = true;
        LAM_LOG_WARN("lam.animation: AssetsPlugin was added after AnimationPlugin; sheets and "
                     "graphs load only after every plugin is built (add AssetsPlugin first)");
    }
}

} // namespace lam
