#pragma once
// RenderPlugin (lam_render) and RaylibRenderPlugin (lam_render_rl). Systems are listed in the README.
//   app.add_plugin<AssetsPlugin>();
//   app.add_plugin<RenderPlugin>(RenderConfig{.clear_color = Color::hex(0x202030ff)});
//   app.add_plugin<RaylibRenderPlugin>();                       // windowed builds only
#include <cstddef>
#include <string_view>
#include <lam/core/app.h>
#include <lam/math/color.h>
#include <lam/render/virtual_screen.h>
namespace lam {
// Also inserted as an App resource.
struct RenderConfig {
    ScaleMode scale_mode = ScaleMode::PixelPerfect;
    Color clear_color = kBlack;
    std::size_t quad_capacity = 65536;                          // quads reserved per frame
    bool cull = true;
    int atlas_page_size = 2048;
    // Creates the light target up front. Lighting::enabled is what actually turns lights on.
    bool lighting = false;
};
// Works headless: collects and plans the frame without drawing it. Add after AssetsPlugin.
class RenderPlugin final : public Plugin {
public:
    explicit RenderPlugin(RenderConfig c = {});                 // quad_capacity 0 -> 1024
    std::string_view name() const override { return "lam.render"; }
    void build(App &app) override;
    void init_world(App &app, World &world) override;
private:
    RenderConfig config_;
};
struct RaylibRenderConfig {
    // Use rlgl's own batch instead of the VBO ring, for drivers that choke on it.
    // LAM_RENDER_RLGL_BATCH=1 does the same.
    bool rlgl_batch = false;
};
// GPU backend. Add after RenderPlugin; without a GPU it only installs the decoder and loaders.
class RaylibRenderPlugin final : public Plugin {
public:
    explicit RaylibRenderPlugin(RaylibRenderConfig c = {}) : config_(c) {}
    std::string_view name() const override { return "lam.render.raylib"; }
    void build(App &app) override;
    void shutdown(App &app) override;
private:
    RaylibRenderConfig config_;
};
}
