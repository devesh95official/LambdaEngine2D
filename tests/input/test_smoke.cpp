// Smoke test: the public lam/input headers compile and InputPlugin runs a few headless frames.
#include <lam/assets/asset_server.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/input/input_plugin.h>
#include <lam/input/keys.h>
#include <lam/input/raw_input.h>
#include <lam/render/render_plugin.h>
#include <lam/test/support.h>

TEST_CASE("input smoke: header-only helpers are usable") {
    lam::RawInput raw;
    raw.set_key(lam::Key::Space, true);
    CHECK(raw.key(lam::Key::Space));
}

TEST_CASE("input smoke: InputPlugin builds headless and runs frames") {
    lam::test::HeadlessApp h;
    h.add<lam::AssetsPlugin>();
    h.add<lam::RenderPlugin>();
    h.add<lam::InputPlugin>();
    CHECK(h.app().has_plugin("lam.assets"));
    CHECK(h.app().has_plugin("lam.render"));
    CHECK(h.app().has_plugin("lam.input"));
    h.frames(3);
}
