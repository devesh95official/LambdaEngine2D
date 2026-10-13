// Smoke test: the public lam/particles headers compile and ParticlesPlugin runs a few headless
// frames.
#include <lam/assets/asset_server.h>
#include <lam/particles/components.h>
#include <lam/particles/particles.h>
#include <lam/render/render_plugin.h>
#include <lam/test/support.h>

TEST_CASE("particles smoke: header-only helpers are usable") {
    CHECK_FALSE(static_cast<bool>(lam::ParticlePoolId{}));
}

TEST_CASE("particles smoke: ParticlesPlugin builds headless and runs frames") {
    lam::test::HeadlessApp h;
    h.add<lam::AssetsPlugin>();
    h.add<lam::RenderPlugin>();
    h.add<lam::ParticlesPlugin>();
    CHECK(h.app().has_plugin("lam.assets"));
    CHECK(h.app().has_plugin("lam.render"));
    CHECK(h.app().has_plugin("lam.particles"));
    h.frames(3);
}
