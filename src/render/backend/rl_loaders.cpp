// raylib-backed image decoder, .ttf/.otf loader and .shader.json loader. Load time only.
#include <algorithm>
#include <cmath>
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

#include <raylib.h>
#include <rlgl.h>

#include <lam/assets/asset_server.h>
#include <lam/core/platform.h>
#include <lam/render/atlas.h>
#include <lam/render/font.h>
#include <lam/render/shader.h>
#include <lam/render/texture.h>

#include "backend/gpu.h"
#include "backend/rl_loaders.h"
#include "detail/render_internal.h"

namespace lam::detail {
namespace {

Result<ImageData> raylib_decode(std::span<const std::byte> bytes, std::string_view extension) {
    if (bytes.empty()) return make_error(ErrorCode::Parse, "empty image file");
    const std::string type = extension.empty() ? std::string(".png") : std::string(extension);
    ::Image img =
        LoadImageFromMemory(type.c_str(), reinterpret_cast<const unsigned char *>(bytes.data()),
                            static_cast<int>(bytes.size()));
    if (img.data == nullptr || img.width <= 0 || img.height <= 0) {
        if (img.data != nullptr) UnloadImage(img);
        return make_error(ErrorCode::Parse, "cannot decode the image (" + type + ")");
    }
    if (img.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    // ImageFormat silently refuses compressed data, leaving a buffer smaller than w * h * 4.
    if (img.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) {
        UnloadImage(img);
        return make_error(ErrorCode::Unsupported,
                          "image format cannot be converted to RGBA8 (" + type + ")");
    }
    ImageData out;
    out.width = img.width;
    out.height = img.height;
    const std::size_t n =
        static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height) * 4;
    out.rgba.resize(n);
    std::memcpy(out.rgba.data(), img.data, n);
    UnloadImage(img);
    return out;
}

struct BakedFont {
    Font font;
    ImageData image;
};

// Smallest square page (256..2048) that fits all the glyphs.
std::optional<BakedFont> bake(const GlyphInfo *glyphs, int count, int size) {
    for (int page = 256; page <= 2048; page *= 2) {
        AtlasPacker packer(page, 1, {0, 0});
        std::vector<IRect> rects(static_cast<std::size_t>(count));
        bool fits = true;
        int used_h = 0;
        for (int i = 0; i < count && fits; ++i) {
            const GlyphInfo &g = glyphs[i];
            const bool has =
                g.value != 32 && g.image.data != nullptr && g.image.width > 0 && g.image.height > 0;
            if (!has) continue;
            const std::optional<AtlasPacker::Placement> p =
                packer.add({g.image.width, g.image.height});
            if (!p || p->page != 0) {
                fits = false;
                break;
            }
            rects[static_cast<std::size_t>(i)] = p->rect;
            used_h = std::max(used_h, p->rect.y + p->rect.h);
        }
        if (!fits) continue;
        BakedFont out;
        out.image.width = page;
        out.image.height = std::max(used_h, 1);
        out.image.rgba.assign(static_cast<std::size_t>(out.image.width) *
                                  static_cast<std::size_t>(out.image.height) * 4,
                              0);
        out.font.base_size = static_cast<float>(size);
        out.font.line_height = static_cast<float>(size);
        float ascent = 0.0f;
        for (int i = 0; i < count; ++i) {
            const GlyphInfo &g = glyphs[i];
            Glyph glyph;
            glyph.cp = static_cast<char32_t>(g.value);
            glyph.advance = static_cast<float>(g.advanceX);
            glyph.offset = {static_cast<float>(g.offsetX), static_cast<float>(g.offsetY)};
            const IRect r = rects[static_cast<std::size_t>(i)];
            if (r.w > 0 && r.h > 0) {
                glyph.source = to_rect(r);
                glyph.size = {static_cast<float>(r.w), static_cast<float>(r.h)};
                ascent = std::max(ascent, static_cast<float>(g.offsetY + g.image.height));
                const auto *src = static_cast<const std::uint8_t *>(g.image.data);
                for (int y = 0; y < r.h; ++y) {
                    for (int x = 0; x < r.w; ++x) {
                        const std::size_t o = (static_cast<std::size_t>(r.y + y) *
                                                   static_cast<std::size_t>(out.image.width) +
                                               static_cast<std::size_t>(r.x + x)) *
                                              4;
                        out.image.rgba[o + 0] = 255;
                        out.image.rgba[o + 1] = 255;
                        out.image.rgba[o + 2] = 255;
                        out.image.rgba[o + 3] =
                            src[static_cast<std::size_t>(y) * static_cast<std::size_t>(r.w) +
                                static_cast<std::size_t>(x)];
                    }
                }
            }
            out.font.glyphs.push_back(glyph);
        }
        out.font.ascent = std::min(ascent, static_cast<float>(size));
        std::sort(out.font.glyphs.begin(), out.font.glyphs.end(),
                  [](const Glyph &a, const Glyph &b) { return a.cp < b.cp; });
        return out;
    }
    return std::nullopt;
}

class RlTtfLoader final : public AssetLoader<Font> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExtensions[] = {".ttf", ".otf"};
        return kExtensions;
    }
    Result<Font> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        const int size = font_size_from_path(ctx.path());
        int count = 0;
        GlyphInfo *glyphs =
            LoadFontData(reinterpret_cast<const unsigned char *>(bytes.data()),
                         static_cast<int>(bytes.size()), size, nullptr, 95, FONT_DEFAULT, &count);
        if (glyphs == nullptr || count <= 0) {
            if (glyphs != nullptr) UnloadFontData(glyphs, count);
            return make_error(ErrorCode::Parse,
                              std::string(ctx.path()) + ": not a usable TrueType/OpenType font");
        }
        std::optional<BakedFont> baked = bake(glyphs, count, size);
        UnloadFontData(glyphs, count);
        if (!baked)
            return make_error(ErrorCode::OutOfRange,
                              std::string(ctx.path()) + ": glyphs do not fit a 2048 page");
        pending_.put(ctx.path(), std::move(baked->image));
        return std::move(baked->font);
    }
    Status finalize(Font &font, FinalizeContext &ctx) override {
        ImageData image;
        if (!pending_.take(ctx.path, &image))
            return make_error(ErrorCode::Invalid,
                              std::string(ctx.path) + ": baked glyphs missing at finalize");
        return place_font_image(font, std::move(image), ctx);
    }

private:
    PendingByPath<ImageData> pending_;
};

struct ShaderSources {
    std::string vs, fs;
};

class RlShaderLoader final : public AssetLoader<Shader> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExtensions[] = {".shader.json"};
        return kExtensions;
    }
    Result<Shader> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        Result<Shader> shader = parse_shader_json(text, ctx.path());
        if (!shader) return shader;
        const ShaderDialect dialect = current_shader_dialect();
        ShaderSources src;
        const auto stage = [&](const std::string &file, ShaderStage s, std::string_view engine_body,
                               std::string *out) -> Status {
            std::string body(engine_body);
            std::string name = "engine";
            if (!file.empty()) {
                Result<std::vector<std::byte>> b = ctx.read(file);
                if (!b)
                    return make_error(b.error().code,
                                      std::string(ctx.path()) + ": " + b.error().message);
                body.assign(reinterpret_cast<const char *>(b->data()), b->size());
                name = file;
            }
            Result<std::string> full = build_shader_source(dialect, s, body, name);
            if (!full) return full.error();
            *out = std::move(full.value());
            return {};
        };
        if (Status s =
                stage(shader->vs_path, ShaderStage::Vertex, engine_vertex_shader_body(), &src.vs);
            !s)
            return s.error();
        if (Status s = stage(shader->fs_path, ShaderStage::Fragment, engine_fragment_shader_body(),
                             &src.fs);
            !s)
            return s.error();
        pending_.put(ctx.path(), std::move(src));
        return shader;
    }
    Status finalize(Shader &shader, FinalizeContext &ctx) override {
        ShaderSources src;
        if (!pending_.take(ctx.path, &src))
            return make_error(ErrorCode::Invalid,
                              std::string(ctx.path) + ": shader sources missing at finalize");
        if (!ctx.app.platform().has_gpu()) return {}; // headless: fall back to the engine shader
        shader.gpu_id = GpuBatcher::compile(src.vs.c_str(), src.fs.c_str());
        if (shader.gpu_id == 0)
            return make_error(ErrorCode::Parse,
                              std::string(ctx.path) + ": the shader did not compile (see the log)");
        return {};
    }
    void unload(Shader &shader, FinalizeContext &ctx) override {
        (void)ctx;
        if (shader.gpu_id != 0) rlUnloadShaderProgram(shader.gpu_id);
        shader.gpu_id = 0;
    }

private:
    PendingByPath<ShaderSources> pending_;
};

} // namespace

ImageDecoder raylib_image_decoder() {
    return &raylib_decode;
}

std::unique_ptr<AssetLoader<Font>> make_raylib_ttf_loader() {
    return std::make_unique<RlTtfLoader>();
}

std::unique_ptr<AssetLoader<Shader>> make_raylib_shader_loader() {
    return std::make_unique<RlShaderLoader>();
}

} // namespace lam::detail
