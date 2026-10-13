// Asset fixtures: runtime textures and fonts, and a file writer for tests that load from disk.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <lam/core/assert.h>
#include <lam/core/fs.h>
#include <lam/test/assets.h>

namespace lam::test {

AssetId<Texture> add_test_texture(AssetServer &assets, IVec2 size, Color fill) {
    LAM_CHECK(size.x > 0 && size.y > 0, "add_test_texture: size must be positive");
    Texture t;
    t.width = size.x;
    t.height = size.y;
    t.pixels.width = size.x;
    t.pixels.height = size.y;
    const std::size_t texels = static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y);
    t.pixels.rgba.resize(texels * 4);
    for (std::size_t i = 0; i < texels; ++i) {
        t.pixels.rgba[i * 4 + 0] = fill.r;
        t.pixels.rgba[i * 4 + 1] = fill.g;
        t.pixels.rgba[i * 4 + 2] = fill.b;
        t.pixels.rgba[i * 4 + 3] = fill.a;
    }
    return assets.add<Texture>(std::move(t));
}

AssetId<Font> add_test_font(AssetServer &assets) {
    constexpr int kCellW = 6, kCellH = 8, kColumns = 16, kCount = 96;
    constexpr int kRows = (kCount + kColumns - 1) / kColumns;
    const AssetId<Texture> page = add_test_texture(assets, {kColumns * kCellW, kRows * kCellH});
    return assets.add<Font>(
        make_monospace_font(page, {0, 0}, kCellW, kCellH, kColumns, 32, kCount));
}

std::string write_test_file(const TempDir &dir, std::string_view name, std::string_view content) {
    const std::string path = dir.file(name);
    const std::string parent = parent_path(path);
    if (!parent.empty()) {
        const Status made = make_directories(parent);
        LAM_CHECK(made.ok(), "write_test_file: cannot create the parent folder");
    }
    const Status written = write_file_atomic(path, content);
    LAM_CHECK(written.ok(), "write_test_file: cannot write the file");
    return path;
}

} // namespace lam::test
