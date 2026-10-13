// Smoke test: the public lam/physics headers compile and PhysicsPlugin runs a few headless frames.
#include <iterator>
#include <sstream>
#include <string>

#include <lam/physics/components.h>
#include <lam/physics/events.h>
#include <lam/physics/motors.h>
#include <lam/physics/physics_plugin.h>
#include <lam/physics/queries.h>
#include <lam/physics/steering.h>
#include <lam/physics/tile_world.h>
#include <lam/test/support.h>

TEST_CASE("physics smoke: header-only helpers are usable") {
    lam::TileWorld tiles;
    tiles.resize(4, 3, 16, {});
    CHECK(tiles.at({0, 0}) == lam::TileShape::Empty);
    CHECK(tiles.at({-1, 0}) == lam::TileShape::Solid);
}

TEST_CASE("physics smoke: PhysicsPlugin builds headless and runs frames") {
    lam::test::HeadlessApp h;
    h.add<lam::PhysicsPlugin>();
    CHECK(h.app().has_plugin("lam.physics"));
    h.frames(3);
}

TEST_CASE("physics smoke: every system sits at its documented slot") {
    lam::test::HeadlessApp h;
    h.add<lam::PhysicsPlugin>();
    const std::string text = h.app().schedule_text();
    struct Slot {
        const char *stage;
        int order;
        const char *name;
    };
    const Slot slots[] = {
        {"Physics", -1800, "path_followers"}, {"Physics", -1800, "moving_platforms"},
        {"Physics", -1700, "static_index"},   {"Physics", -1600, "carry_riders"},
        {"Physics", -1500, "motors"},         {"Physics", -1400, "integrate"},
        {"Physics", -1300, "projectiles"},    {"Physics", -1200, "broadphase"},
        {"Physics", -1100, "narrowphase_solve"}, {"Physics", -1000, "contact_events"},
        {"RenderUI", 1500, "draw_colliders"},
    };
    std::size_t found = 0;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        std::istringstream words(line);
        std::string stage, scope, name, plugin;
        int order = 0;
        if (!(words >> stage >> scope >> order >> name >> plugin) || plugin != "[lam.physics]") continue;
        CAPTURE(line);
        CHECK(scope == "PerWorld");
        bool known = false;
        for (const Slot &s : slots) known = known || (stage == s.stage && order == s.order && name == s.name);
        CHECK(known);
        ++found;
    }
    CHECK(found == std::size(slots));
}
