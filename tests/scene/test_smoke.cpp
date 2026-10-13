// Smoke test: the public lam/scene headers compile and ScenePlugin runs a few headless frames.
#include <lam/assets/asset_server.h>
#include <lam/render/render_plugin.h>
#include <lam/scene/save.h>
#include <lam/scene/scene.h>
#include <lam/test/support.h>

TEST_CASE("scene smoke: header-only helpers are usable") {
    lam::Transition t;
    CHECK(t.kind == lam::TransitionKind::None);
}

TEST_CASE("scene smoke: ScenePlugin builds headless and runs frames") {
    lam::test::HeadlessApp h;
    h.add<lam::AssetsPlugin>();
    h.add<lam::RenderPlugin>();
    h.add<lam::ScenePlugin>();
    CHECK(h.app().has_plugin("lam.assets"));
    CHECK(h.app().has_plugin("lam.render"));
    CHECK(h.app().has_plugin("lam.scene"));
    h.frames(3);
}
