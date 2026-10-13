#pragma once
// raylib-backed image decoder and loaders.
#include <lam/assets/asset_server.h>
#include <lam/render/font.h>
#include <lam/render/shader.h>
#include <lam/render/texture.h>
#include <memory>
namespace lam::detail {
ImageDecoder raylib_image_decoder();                         // stb_image via raylib; RGBA8
std::unique_ptr<AssetLoader<Font>> make_raylib_ttf_loader(); // ".ttf"/".otf" baked at load
std::unique_ptr<AssetLoader<Shader>>
make_raylib_shader_loader(); // ".shader.json", compiled at finalize
} // namespace lam::detail
