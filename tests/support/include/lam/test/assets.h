#pragma once
// Asset fixtures for tests. Textures and fonts are added as runtime assets, so tests don't need
// PNG decoding (headless runs have none).
#include <string>
#include <string_view>
#include <lam/assets/asset_server.h>
#include <lam/math/color.h>
#include <lam/render/font.h>
#include <lam/render/texture.h>
#include <lam/test/support.h>
namespace lam::test {
// A texture filled with `fill`, added to the current scope and Ready at once.
AssetId<Texture> add_test_texture(AssetServer &assets, IVec2 size, Color fill = kWhite);
// 6x8 monospace font (code points 32..127) on a 96x48 white texture, added to the current scope.
AssetId<Font> add_test_font(AssetServer &assets);
// Writes dir/name, creating folders, and returns the absolute path. Aborts if the write fails.
std::string write_test_file(const TempDir &dir, std::string_view name, std::string_view content);
}
