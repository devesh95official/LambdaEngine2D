// Smoke test: the public lam/animation headers compile and AnimationPlugin runs a few headless
// frames.
#include <lam/animation/animation_plugin.h>
#include <lam/animation/animator.h>
#include <lam/animation/sprite_sheet.h>
#include <lam/animation/tween.h>
#include <lam/assets/asset_server.h>
#include <lam/physics/physics_plugin.h>
#include <lam/render/render_plugin.h>
#include <lam/test/support.h>

TEST_CASE("animation smoke: header-only helpers are usable") {
    lam::Animator a;
    CHECK(a.tag == -1);
}

TEST_CASE("animation smoke: AnimationPlugin builds headless and runs frames") {
    lam::test::HeadlessApp h;
    h.add<lam::AssetsPlugin>();
    h.add<lam::RenderPlugin>();
    h.add<lam::PhysicsPlugin>();
    h.add<lam::AnimationPlugin>();
    CHECK(h.app().has_plugin("lam.assets"));
    CHECK(h.app().has_plugin("lam.render"));
    CHECK(h.app().has_plugin("lam.physics"));
    CHECK(h.app().has_plugin("lam.animation"));
    h.frames(3);
}
