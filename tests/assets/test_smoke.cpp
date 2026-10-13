// Smoke test: the public lam/assets headers compile and AssetsPlugin runs a few headless frames.
#include <lam/assets/asset_server.h>
#include <lam/test/support.h>

TEST_CASE("assets smoke: header-only helpers are usable") {
    CHECK(lam::kRootScope == 0u);
}

TEST_CASE("assets smoke: AssetsPlugin builds headless and runs frames") {
    lam::test::HeadlessApp h;
    h.add<lam::AssetsPlugin>();
    CHECK(h.app().has_plugin("lam.assets"));
    h.frames(3);
}
