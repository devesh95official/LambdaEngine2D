#pragma once
// The default plugin bundle (target lam_engine). Headless runs skip the raylib backends.
// Add game plugins after it; to enlarge an engine event channel use App::reserve_event<T>.
#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/raylib_platform.h>
#include <lam/physics/physics_plugin.h>
#include <lam/render/render_plugin.h>
namespace lam {
struct DefaultPlugins {
    bool headless = false;                                      // OR'd with app.options().headless
    bool scenes = true;                                         // ScenePlugin + menu scenes
    PlatformConfig platform{};
    AssetsConfig assets{};
    RenderConfig render{};
    PhysicsConfig physics{};
    void build(App &app) const;
};
}
