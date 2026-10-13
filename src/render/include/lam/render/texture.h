#pragma once
// Textures and CPU images. A texture's draw slot is its AssetId index, and UVs stay in texels
// until the shader. Slot 0 is the built-in 64x64 image, which every atlas page also carries so
// shapes and lights batch with sprites.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#include <lam/core/asset_id.h>
#include <lam/core/assert.h>
#include <lam/core/result.h>
#include <lam/ecs/component.h>
#include <lam/math/rect.h>
namespace lam {
enum class TextureFilter : std::uint8_t { Point, Bilinear };
struct ImageData {                                              // RGBA8
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;                             // empty headless
    bool has_pixels() const { return !rgba.empty(); }
};
struct Texture {                                                // the backend owns the GPU side
    int width = 0, height = 0;
    TextureFilter filter = TextureFilter::Point;
    ImageData pixels;                                           // uploaded when `version` changes, then freed
    std::uint32_t version = 1;                                  // bump and mark_modified after editing pixels
    bool keep_cpu = false;
};
template <class T> class AssetLoader;
std::unique_ptr<AssetLoader<Texture>> make_texture_loader();
struct TextureRegion {
    AssetId<Texture> texture;                                   // none = built-in image
    Rect source;                                                // texels
    Vec2 size;                                                  // untrimmed frame size
    Vec2 trim_offset;                                           // where `source` sits in the untrimmed frame
};
inline constexpr int kBuiltinImageSize = 64;
inline constexpr Rect kWhiteSource{1, 1, 2, 2};                 // inside the 4x4 white block at (0, 0)
inline constexpr Rect kCircleSource{0, 32, 32, 32};
inline constexpr Rect kFalloffSource{32, 32, 32, 32};           // radial light falloff
inline std::uint16_t texture_slot(AssetId<Texture> t) {
    LAM_CHECK(t.index < 65536u, "texture index exceeds the 16-bit draw slot");
    return static_cast<std::uint16_t>(t.index);
}
// The backend installs a real decoder. The default only reads the PNG size, which is enough for
// headless runs to pack the same atlas layout.
using ImageDecoder = Result<ImageData> (*)(std::span<const std::byte> bytes, std::string_view extension);
void set_image_decoder(ImageDecoder decoder);                  // only during plugin build
Result<ImageData> decode_image(std::span<const std::byte> bytes, std::string_view extension);  // thread-safe
Result<IVec2> read_png_size(std::span<const std::byte> bytes);
const ImageData &builtin_image();                              // always has pixels
}
LAM_REFLECT_ENUM(lam::TextureFilter, "point", "bilinear")
LAM_REFLECT_STRUCT(lam::TextureRegion, texture, source, size, trim_offset)
