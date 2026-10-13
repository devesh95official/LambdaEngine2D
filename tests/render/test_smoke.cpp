// Smoke test: the public lam/render headers compile and RenderPlugin runs a few headless frames.
#include <lam/assets/asset_server.h>
#include <lam/render/atlas.h>
#include <lam/render/camera.h>
#include <lam/render/camera_math.h>
#include <lam/render/font.h>
#include <lam/render/lighting.h>
#include <lam/render/passes.h>
#include <lam/render/render_plugin.h>
#include <lam/render/render_queue.h>
#include <lam/render/shader.h>
#include <lam/render/sprite.h>
#include <lam/render/texture.h>
#include <lam/render/virtual_screen.h>
#include <lam/test/support.h>

TEST_CASE("render smoke: header-only helpers are usable") {
    CHECK(lam::is_screen_layer(lam::kLayerUi));
    CHECK_FALSE(lam::is_screen_layer(lam::kLayerWorld));
}

TEST_CASE("render smoke: RenderPlugin builds headless and runs frames") {
    lam::test::HeadlessApp h;
    h.add<lam::AssetsPlugin>();
    h.add<lam::RenderPlugin>();
    CHECK(h.app().has_plugin("lam.assets"));
    CHECK(h.app().has_plugin("lam.render"));
    h.frames(3);
}
