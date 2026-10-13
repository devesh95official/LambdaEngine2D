#pragma once
// Private declarations shared by lam_render and its raylib backend.
#include <cstddef>
#include <cstdint>
#include <lam/assets/asset_server.h>
#include <lam/core/result.h>
#include <lam/render/atlas.h>
#include <lam/render/font.h>
#include <lam/render/shader.h>
#include <lam/render/texture.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace lam::detail {
// ---- built-in 6x8 ASCII font (code points 32..127, 16 columns x 6 rows of 6x8 cells) ----------
inline constexpr int kBuiltinFontCellW = 6;
inline constexpr int kBuiltinFontCellH = 8;
inline constexpr int kBuiltinFontColumns = 16;
inline constexpr int kBuiltinFontCount = 96;
inline constexpr IVec2 kBuiltinFontImageSize{96, 48};
// White glyphs on transparent.
ImageData builtin_font_image();
// Reserves the font's atlas space (the backend blits the pixels) and adds the Font asset.
AssetId<Font> add_builtin_font(AssetServer &assets, AtlasSet &atlas);
// Allocates transparent pixels with the built-in block if the page has none.
void ensure_page_pixels(Texture &page);
// Clipped to the page.
void blit_into(Texture &page, const ImageData &image, IVec2 at);

// ---- font loading helpers ----------------------------------------------------------------------
// AngelCode BMFont text format, single page. The page file name goes to *page_file.
Result<Font> parse_bmfont(std::string_view text, std::string_view file_name,
                          std::string *page_file);
// "name.ttf#16" -> 16, clamped to 4..256.
int font_size_from_path(std::string_view path, int fallback = 16);
// Packs the font image into the atlas, or a standalone texture if it's bigger than a page.
Status place_font_image(Font &font, ImageData image, FinalizeContext &ctx);

// Values decoded on a worker and picked up at finalize, keyed by asset path.
template <class T>
class PendingByPath {
public:
    void put(std::string_view path, T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &p : items_) {
            if (p.first == path) {
                p.second = std::move(value);
                return;
            }
        }
        items_.emplace_back(std::string(path), std::move(value));
    }
    bool take(std::string_view path, T *out) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].first == path) {
                *out = std::move(items_[i].second);
                items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        return false;
    }

private:
    std::mutex mutex_;
    std::vector<std::pair<std::string, T>> items_;
};

// ---- loaders registered by RenderPlugin (the backend replaces the .ttf and shader ones) --------
std::unique_ptr<AssetLoader<Font>> make_bmfont_loader();
std::unique_ptr<AssetLoader<Font>> make_placeholder_ttf_loader();
std::unique_ptr<AssetLoader<Shader>> make_placeholder_shader_loader();
// {"vs": "path", "fs": "path"}, both optional.
Result<Shader> parse_shader_json(std::string_view text, std::string_view file_name);
} // namespace lam::detail
