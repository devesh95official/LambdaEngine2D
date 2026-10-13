// Smoke test: the public lam/ui headers compile and UiPlugin runs a few headless frames.
#include <lam/assets/asset_server.h>
#include <lam/audio/audio_plugin.h>
#include <lam/input/input_plugin.h>
#include <lam/render/render_plugin.h>
#include <lam/scene/scene.h>
#include <lam/test/support.h>
#include <lam/ui/dialogue.h>
#include <lam/ui/menu_scenes.h>
#include <lam/ui/message_log.h>
#include <lam/ui/toasts.h>
#include <lam/ui/ui.h>
#include <lam/ui/ui_plugin.h>

TEST_CASE("ui smoke: header-only helpers are usable") {
    lam::UiNode node;
    CHECK(node.kind == lam::UiKind::Panel);
}

TEST_CASE("ui smoke: UiPlugin builds headless and runs frames") {
    lam::test::HeadlessApp h;
    h.add<lam::AssetsPlugin>();
    h.add<lam::RenderPlugin>();
    h.add<lam::InputPlugin>();
    h.add<lam::AudioPlugin>();
    h.add<lam::ScenePlugin>();
    h.add<lam::UiPlugin>();
    CHECK(h.app().has_plugin("lam.assets"));
    CHECK(h.app().has_plugin("lam.render"));
    CHECK(h.app().has_plugin("lam.input"));
    CHECK(h.app().has_plugin("lam.audio"));
    CHECK(h.app().has_plugin("lam.scene"));
    CHECK(h.app().has_plugin("lam.ui"));
    h.frames(3);
}
