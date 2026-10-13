// Smoke test: the public lam/tilemap headers compile and TilemapPlugin runs a few headless frames.
#include <lam/assets/asset_server.h>
#include <lam/physics/physics_plugin.h>
#include <lam/render/render_plugin.h>
#include <lam/test/support.h>
#include <lam/tilemap/autotile.h>
#include <lam/tilemap/fov.h>
#include <lam/tilemap/grid_maps.h>
#include <lam/tilemap/grid_mover.h>
#include <lam/tilemap/grid_path.h>
#include <lam/tilemap/path_follow.h>
#include <lam/tilemap/procgen.h>
#include <lam/tilemap/tilemap.h>
#include <lam/tilemap/tilemap_plugin.h>
#include <lam/tilemap/tileset.h>

TEST_CASE("tilemap smoke: header-only helpers are usable") {
    CHECK((lam::kTileIdMask & lam::kTileFlipH) == 0u);
}

TEST_CASE("tilemap smoke: TilemapPlugin builds headless and runs frames") {
    lam::test::HeadlessApp h;
    h.add<lam::AssetsPlugin>();
    h.add<lam::RenderPlugin>();
    h.add<lam::PhysicsPlugin>();
    h.add<lam::TilemapPlugin>();
    CHECK(h.app().has_plugin("lam.assets"));
    CHECK(h.app().has_plugin("lam.render"));
    CHECK(h.app().has_plugin("lam.physics"));
    CHECK(h.app().has_plugin("lam.tilemap"));
    h.frames(3);
}
