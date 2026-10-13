// Decoder hook, PNG header reader, the built-in image and the .png loader. Headless runs only
// read PNG sizes, which is enough to pack the same atlas layout as a real run.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <lam/assets/asset_server.h>
#include <lam/core/result.h>
#include <lam/render/texture.h>

namespace lam {
namespace {

constexpr std::array<std::uint8_t, 8> kPngSignature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

std::uint32_t read_be32(std::span<const std::byte> b, std::size_t at) {
    return (std::uint32_t(b[at]) << 24) | (std::uint32_t(b[at + 1]) << 16) |
           (std::uint32_t(b[at + 2]) << 8) | std::uint32_t(b[at + 3]);
}

bool has_png_signature(std::span<const std::byte> bytes) {
    if (bytes.size() < kPngSignature.size()) return false;
    for (std::size_t i = 0; i < kPngSignature.size(); ++i) {
        if (static_cast<std::uint8_t>(bytes[i]) != kPngSignature[i]) return false;
    }
    return true;
}

// Size only; real decoding needs the backend.
Result<ImageData> header_only_decoder(std::span<const std::byte> bytes,
                                      std::string_view extension) {
    if (!has_png_signature(bytes)) {
        return make_error(ErrorCode::Unsupported,
                          "no image decoder for '" + std::string(extension) +
                              "' (without the raylib backend only PNG headers are read)");
    }
    Result<IVec2> size = read_png_size(bytes);
    if (!size) return size.error();
    ImageData image;
    image.width = size->x;
    image.height = size->y;
    return image;
}

std::atomic<ImageDecoder> g_decoder{&header_only_decoder};

void put_texel(std::vector<std::uint8_t> &rgba, int x, int y, std::uint8_t alpha) {
    const std::size_t i =
        (static_cast<std::size_t>(y) * kBuiltinImageSize + static_cast<std::size_t>(x)) * 4;
    rgba[i + 0] = 255;
    rgba[i + 1] = 255;
    rgba[i + 2] = 255;
    rgba[i + 3] = alpha;
}

std::uint8_t to_alpha(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// White block at (0, 0), circle at (0, 32), light falloff at (32, 32). White with straight
// alpha so tints multiply cleanly.
ImageData make_builtin_image() {
    ImageData image;
    image.width = kBuiltinImageSize;
    image.height = kBuiltinImageSize;
    image.rgba.assign(static_cast<std::size_t>(kBuiltinImageSize * kBuiltinImageSize * 4), 0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x)
            put_texel(image.rgba, x, y, 255);
    }
    constexpr float kRadius = 16.0f;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - kRadius;
            const float dy = static_cast<float>(y) + 0.5f - kRadius;
            const float d = std::sqrt(dx * dx + dy * dy);
            // One texel of anti-aliasing at the rim.
            put_texel(image.rgba, x, 32 + y, to_alpha(kRadius - d + 0.5f));
            float v = std::clamp(1.0f - d / kRadius, 0.0f, 1.0f);
            v = v * v * (3.0f - 2.0f * v);
            put_texel(image.rgba, 32 + x, 32 + y, to_alpha(v));
        }
    }
    return image;
}

// Lower case, with the dot.
std::string last_extension(std::string_view path) {
    const std::size_t hash = path.find('#');
    if (hash != std::string_view::npos) path = path.substr(0, hash);
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

// CPU side only; the backend uploads it.
class TextureLoader final : public AssetLoader<Texture> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExtensions[] = {".png"};
        return kExtensions;
    }
    Result<Texture> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        Result<ImageData> image = decode_image(bytes, last_extension(ctx.path()));
        if (!image) {
            return make_error(image.error().code,
                              std::string(ctx.path()) + ": " + image.error().message);
        }
        Texture t;
        t.width = image->width;
        t.height = image->height;
        t.pixels = std::move(image.value());
        return t;
    }
};

} // namespace

void set_image_decoder(ImageDecoder decoder) {
    g_decoder.store(decoder != nullptr ? decoder : &header_only_decoder, std::memory_order_release);
}

Result<ImageData> decode_image(std::span<const std::byte> bytes, std::string_view extension) {
    const ImageDecoder decoder = g_decoder.load(std::memory_order_acquire);
    Result<ImageData> r = decoder(bytes, extension);
    if (!r) return r;
    const ImageData &image = r.value();
    if (image.width <= 0 || image.height <= 0) {
        return make_error(ErrorCode::Parse, "image decoder returned an empty image");
    }
    if (image.has_pixels() && image.rgba.size() != static_cast<std::size_t>(image.width) *
                                                       static_cast<std::size_t>(image.height) * 4) {
        return make_error(ErrorCode::Parse,
                          "image decoder returned a pixel buffer of the wrong size");
    }
    return r;
}

Result<IVec2> read_png_size(std::span<const std::byte> bytes) {
    // Signature (8), then the IHDR chunk: length (4) = 13, "IHDR" (4), width (4), height (4).
    if (!has_png_signature(bytes))
        return make_error(ErrorCode::Parse, "not a PNG file (bad signature)");
    if (bytes.size() < 24) return make_error(ErrorCode::Parse, "PNG file truncated before IHDR");
    const bool ihdr = static_cast<char>(bytes[12]) == 'I' && static_cast<char>(bytes[13]) == 'H' &&
                      static_cast<char>(bytes[14]) == 'D' && static_cast<char>(bytes[15]) == 'R';
    if (!ihdr || read_be32(bytes, 8) != 13) {
        return make_error(ErrorCode::Parse, "PNG file does not start with an IHDR chunk");
    }
    const std::uint32_t w = read_be32(bytes, 16);
    const std::uint32_t h = read_be32(bytes, 20);
    if (w == 0 || h == 0 || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu) {
        return make_error(ErrorCode::Parse, "PNG header has an invalid size");
    }
    return IVec2{static_cast<int>(w), static_cast<int>(h)};
}

const ImageData &builtin_image() {
    static const ImageData image = make_builtin_image();
    return image;
}

std::unique_ptr<AssetLoader<Texture>> make_texture_loader() {
    return std::make_unique<TextureLoader>();
}

} // namespace lam
