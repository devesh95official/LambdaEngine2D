// .particles.json loader. Keys are EmitterConfig's member names; unknown keys and out-of-range
// values are errors. A decoded "image" waits in a mutex-guarded table until finalize packs it.
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <lam/core/data.h>
#include <lam/core/log.h>
#include <lam/particles/particles.h>
#include <lam/render/atlas.h>
#include <lam/render/render_queue.h>
#include <lam/render/texture.h>

#include "detail/particles_detail.h"

namespace lam::detail {
namespace {

constexpr float kBig = 1000000.0f;

struct PendingImage {
    ImageData image;
    Rect source;                                                // texels inside the image; empty = all
    std::string group = "default";                              // atlas group or "none"
};

// What decode found besides the config itself.
struct ImageRequest {
    std::string path;
    Rect source;
    bool has_source = false;
    std::string group = "default";
};

struct LayerName {
    std::string_view name;
    std::uint8_t layer;
};
constexpr LayerName kLayers[] = {{"background", kLayerBackground}, {"tiles", kLayerTiles},
                                 {"world", kLayerWorld},           {"foreground", kLayerForeground},
                                 {"lights", kLayerLights},         {"screen_fx", kLayerScreenFx},
                                 {"ui", kLayerUi},                 {"overlay", kLayerOverlay},
                                 {"debug", kLayerDebug}};

std::uint8_t read_layer(DataReader &in, std::uint8_t fallback) {
    if (!in.has("layer")) return fallback;
    if (in.type("layer") == DataType::String) {
        const std::string name = in.string("layer");
        for (const LayerName &l : kLayers) {
            if (l.name == name) return l.layer;
        }
        in.fail("unknown layer '" + name +
                "' (background, tiles, world, foreground, lights, screen_fx, ui, overlay, debug)");
        return fallback;
    }
    return static_cast<std::uint8_t>(in.integer("layer", 0, 255));
}

// Reads [min, max) as two keys; min > max is an error at the max key.
void read_range(DataReader &in, std::string_view lo_key, std::string_view hi_key, float &lo, float &hi,
                float min, float max) {
    lo = in.number_or(lo_key, lo, min, max);
    hi = in.number_or(hi_key, hi, min, max);
    if (hi < lo) in.fail(std::string(hi_key) + " is smaller than " + std::string(lo_key));
}

EmitterConfig read_config(DataReader &in, ImageRequest &image) {
    EmitterConfig c;
    c.capacity = static_cast<std::uint32_t>(in.integer_or("capacity", static_cast<int>(c.capacity), 1,
                                                          static_cast<int>(kMaxParticlesPerPool)));
    c.rate = in.number_or("rate", c.rate, 0.0f, kBig);
    c.burst = static_cast<std::uint16_t>(in.integer_or("burst", c.burst, 0, 65535));
    read_range(in, "speed_min", "speed_max", c.speed_min, c.speed_max, 0.0f, 100000.0f);
    c.angle = in.number_or("angle", c.angle, -1000.0f, 1000.0f);
    c.spread = in.number_or("spread", c.spread, 0.0f, 7.0f); // sanitize() clamps to tau
    read_range(in, "life_min", "life_max", c.life_min, c.life_max, 0.001f, 3600.0f);
    c.size_start = in.number_or("size_start", c.size_start, 0.0f, 4096.0f);
    c.size_end = in.number_or("size_end", c.size_end, 0.0f, 4096.0f);
    if (in.has("color_start")) c.color_start = in.color("color_start");
    if (in.has("color_end")) c.color_end = in.color("color_end");
    if (in.has("gravity")) c.gravity = in.vec2("gravity");
    c.drag = in.number_or("drag", c.drag, 0.0f, 1000.0f);
    c.layer = read_layer(in, c.layer);
    c.material = static_cast<MaterialId>(in.integer_or("material", c.material, 0, kMaxMaterials - 1));
    if (in.has("spawn_area")) c.spawn_area = in.rect("spawn_area");
    c.local_space = in.boolean_or("local_space", c.local_space);
    if (in.has("spawn_shape")) {
        const std::string shape = in.string("spawn_shape");
        if (shape == "rect") c.spawn_shape = SpawnShape::Rect;
        else if (shape == "ellipse") c.spawn_shape = SpawnShape::Ellipse;
        else if (shape == "ring") c.spawn_shape = SpawnShape::Ring;
        else in.fail("unknown spawn_shape '" + shape + "' (rect, ellipse, ring)");
    }
    read_range(in, "rotation_min", "rotation_max", c.rotation_min, c.rotation_max, -1000.0f, 1000.0f);
    read_range(in, "spin_min", "spin_max", c.spin_min, c.spin_max, -1000.0f, 1000.0f);
    c.align_to_velocity = in.boolean_or("align_to_velocity", c.align_to_velocity);
    c.stretch = in.number_or("stretch", c.stretch, 0.0f, 10.0f);
    c.sway = in.number_or("sway", c.sway, 0.0f, 10000.0f);
    c.sway_hz = in.number_or("sway_hz", c.sway_hz, 0.0f, 100.0f);
    c.jitter = in.number_or("jitter", c.jitter, 0.0f, 100000.0f);
    c.y_sort = in.boolean_or("y_sort", c.y_sort);
    c.depth = in.number_or("depth", c.depth, -524288.0f, 524288.0f);
    c.prewarm = in.number_or("prewarm", c.prewarm, 0.0f, 60.0f);
    // Look: a built-in shape on the shape page, or an image (atlas-packed at finalize).
    if (in.has("sprite") && in.has("image")) in.fail("give either \"sprite\" or \"image\", not both");
    if (in.has("sprite")) {
        const std::string sprite = in.string("sprite");
        if (sprite == "circle") c.region.source = kCircleSource;
        else if (sprite == "square") c.region.source = kWhiteSource;
        else if (sprite == "falloff") c.region.source = kFalloffSource;
        else in.fail("unknown sprite '" + sprite + "' (circle, square, falloff)");
    }
    if (in.has("image")) {
        image.path = in.string("image");
        if (in.has("source")) {
            image.source = in.rect("source");
            image.has_source = true;
            if (image.source.empty()) in.fail("source must have a positive size");
        }
        if (in.has("atlas")) image.group = in.string("atlas");
    } else {
        if (in.has("source")) in.fail("\"source\" needs an \"image\"");
        if (in.has("atlas")) in.fail("\"atlas\" needs an \"image\"");
    }
    return c;
}

// Lower-case extension of a path ("spark.PNG" -> ".png").
std::string extension_of(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = file.find_last_of('.');
    std::string ext = dot == std::string_view::npos ? std::string() : std::string(file.substr(dot));
    for (char &ch : ext) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return ext;
}

class EmitterLoader final : public AssetLoader<EmitterConfig> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExtensions[] = {".particles.json"};
        return kExtensions;
    }

    Result<EmitterConfig> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        ImageRequest request;
        Result<EmitterConfig> config = parse_data<EmitterConfig>(
            text, ctx.path(), [&request](DataReader &in) { return read_config(in, request); });
        if (!config) return config.error();
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(std::string(ctx.path()));
        if (request.path.empty()) return config;
        Result<std::vector<std::byte>> file = ctx.read(request.path);
        if (!file) return make_error(file.error().code, std::string(ctx.path()) + ": image: " + file.error().message);
        Result<ImageData> image = decode_image(file.value(), extension_of(request.path));
        if (!image) {
            return make_error(image.error().code,
                              std::string(ctx.path()) + ": image '" + request.path + "': " + image.error().message);
        }
        const Rect whole{0, 0, static_cast<float>(image->width), static_cast<float>(image->height)};
        if (request.has_source && intersection(request.source, whole) != request.source) {
            return make_error(ErrorCode::Parse, std::string(ctx.path()) + ": /source: outside the " +
                                                    std::to_string(image->width) + "x" +
                                                    std::to_string(image->height) + " image");
        }
        PendingImage pending;
        pending.image = std::move(image.value());
        pending.source = request.has_source ? request.source : whole;
        pending.group = std::move(request.group);
        pending_.insert_or_assign(std::string(ctx.path()), std::move(pending));
        return config;
    }

    Status finalize(EmitterConfig &config, FinalizeContext &ctx) override {
        PendingImage pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pending_.find(std::string(ctx.path));
            if (it == pending_.end()) {
                config = sanitize(config);
                return {};
            }
            pending = std::move(it->second);
            pending_.erase(it);
        }
        TextureRegion page_region;
        AtlasSet *atlas = ctx.app.try_res<AtlasSet>();
        if (atlas != nullptr && pending.group != "none") {
            Result<TextureRegion> packed = atlas->add(ctx.server, pending.image, str_id(pending.group));
            if (packed) {
                page_region = packed.value();
            } else {
                LAM_LOG_WARN("{}: image not packed ({}); using a standalone texture", ctx.path,
                             packed.error().message);
            }
        }
        if (!page_region.texture) {
            const int w = pending.image.width, h = pending.image.height;
            Texture t;
            t.width = w;
            t.height = h;
            t.pixels = std::move(pending.image);
            page_region.texture = ctx.server.add<Texture>(std::move(t), std::string(ctx.path) + "#image");
            page_region.source = {0, 0, static_cast<float>(w), static_cast<float>(h)};
        }
        // The configured source is relative to the image; the image sits at page_region.source.
        config.region.texture = page_region.texture;
        config.region.source = pending.source.translated(page_region.source.min());
        config.region.size = pending.source.size();
        config.region.trim_offset = {};
        config = sanitize(config);
        return {};
    }

private:
    std::mutex mutex_;
    std::map<std::string, PendingImage, std::less<>> pending_;
};

} // namespace

std::unique_ptr<AssetLoader<EmitterConfig>> make_emitter_loader() {
    return std::make_unique<EmitterLoader>();
}

} // namespace lam::detail
