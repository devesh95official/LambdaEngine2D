// Prefabs: parsing, extends, children, local references, spawning and failed loads.
#include "ecs_test.h"
#include <algorithm>
#include <array>
#include <lam/ecs/commands.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <map>
#include <string>
#include <vector>

using namespace lam;
using namespace ecs_test;
using namespace lam::literals;

namespace game {
struct EnemyTag {};
struct Glow {
    float radius = 1;
};
struct Aim {
    Entity barrel;
};
struct Skin; // asset type
struct Look {
    AssetId<Skin> skin;
};
} // namespace game
LAM_REFLECT_TAG(game::EnemyTag)
LAM_REFLECT(game::Glow, radius)
LAM_REFLECT(game::Aim, barrel)
LAM_REFLECT(game::Look, skin)

namespace {
constexpr const char *kPrefabs = R"({"prefabs": {
  "enemy": {"tags": ["EnemyTag"], "components": {"Health": {"hp": 10, "max": 10},
                                                 "Transform": {"scale": [2, 2]}}},
  "goblin": {"extends": "enemy", "components": {"Health": {"hp": 5}},
             "children": [{"prefab": "torch", "offset": [4, -6]}]},
  "torch": {"components": {"Transform": {"position": [1, 0]}, "Glow": {"radius": 3}}},
  "turret": {"components": {"Aim": {"barrel": 1}}, "children": [{"prefab": "torch"}]},
  "skinned": {"components": {"Look": {"skin": "goblin.png"}}}
}})";

struct Files final : AssetResolver {
    std::map<std::string, std::string> files;
    std::map<std::string, std::uint64_t> ids;
    std::uint64_t resolve(TypeId, std::string_view path) override {
        auto [it, inserted] = ids.try_emplace(std::string(path), ids.size() + 1);
        return it->second;
    }
    std::string_view path_of(TypeId, std::uint64_t bits) const override {
        for (const auto &[p, b] : ids)
            if (b == bits) return p;
        return {};
    }
    Result<std::string> read_text(std::string_view path) const override {
        const auto it = files.find(std::string(path));
        if (it == files.end()) return make_error(ErrorCode::NotFound, std::string(path));
        return it->second;
    }
    std::size_t pending() const override { return 0; }
};

std::vector<Entity> children_of(const World &w, Entity parent) {
    std::vector<Entity> out;
    const Children *ch = w.try_get<Children>(parent);
    if (!ch) return out;
    for (Entity c = ch->first; c; c = w.get<Parent>(c).next_sibling)
        out.push_back(c);
    return out;
}
} // namespace

TEST_CASE("load and spawn: extends merges fields, tags, children placed by offset") {
    Files files;
    SerializeContext ctx{&files, {}};
    PrefabLibrary lib;
    const Status st = lib.load_text(kPrefabs, "prefabs.json", ctx);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    CHECK(lib.has("goblin"_id));
    CHECK(lib.names().size() == 5);
    World w;
    const Entity g = lib.spawn(w, "goblin"_id, {100, 50});
    REQUIRE(w.alive(g));
    CHECK(w.get<Health>(g).hp == 5);   // overridden
    CHECK(w.get<Health>(g).max == 10); // inherited
    CHECK(w.has<game::EnemyTag>(g));
    CHECK(w.get<Transform>(g).position == Vec2{100, 50});
    CHECK(w.get<Transform>(g).scale == Vec2{2, 2});
    CHECK(w.get<PrevTransform>(g).position == Vec2{100, 50});
    CHECK(w.has<JustSpawned>(g));
    const std::vector<Entity> kids = children_of(w, g);
    REQUIRE(kids.size() == 1);
    const Entity torch = kids[0];
    CHECK(w.get<game::Glow>(torch).radius == 3);
    CHECK(w.get<Parent>(torch).entity == g);
    CHECK(w.get<Parent>(torch).local.position == Vec2{5, -6}); // torch position + offset
    CHECK(w.get<Transform>(torch).position == Vec2{110, 38});  // parent scale 2 applies
    CHECK(w.has<JustSpawned>(torch));
    CHECK(!w.has<game::EnemyTag>(torch));
    // The base prefab is unchanged by the override.
    const Entity e = lib.spawn(w, "enemy"_id, {});
    CHECK(w.get<Health>(e).hp == 10);
    CHECK(children_of(w, e).empty());
}

TEST_CASE("prefab-local entity references resolve to the spawned instance") {
    Files files;
    PrefabLibrary lib;
    REQUIRE(lib.load_text(kPrefabs, "prefabs.json", SerializeContext{&files, {}}).ok());
    World w;
    const Entity t1 = lib.spawn(w, "turret"_id, {});
    const Entity t2 = lib.spawn(w, "turret"_id, {});
    const Entity b1 = w.get<game::Aim>(t1).barrel;
    const Entity b2 = w.get<game::Aim>(t2).barrel;
    CHECK(children_of(w, t1) == std::vector<Entity>{b1});
    CHECK(children_of(w, t2) == std::vector<Entity>{b2});
    CHECK(b1 != b2);
    CHECK(w.has<game::Glow>(b1));
}

TEST_CASE("asset fields resolve at load through the context") {
    Files files;
    PrefabLibrary lib;
    REQUIRE(lib.load_text(kPrefabs, "prefabs.json", SerializeContext{&files, {}}).ok());
    CHECK(files.ids.count("goblin.png") == 1);
    World w;
    const Entity s = lib.spawn(w, "skinned"_id, {});
    CHECK(w.get<game::Look>(s).skin.bits() == files.ids["goblin.png"]);
}

TEST_CASE("deferred spawning through Commands") {
    Files files;
    PrefabLibrary lib;
    REQUIRE(lib.load_text(kPrefabs, "prefabs.json", SerializeContext{&files, {}}).ok());
    World w;
    w.set_prefabs(&lib);
    CHECK(w.prefabs() == &lib);
    const Entity g = w.commands().spawn_prefab("goblin"_id, {10, 10});
    const Entity t = w.commands().spawn_prefab("turret"_id, {0, 0});
    CHECK(!w.alive(g));
    w.commands().apply();
    REQUIRE(w.alive(g));
    CHECK(w.get<Health>(g).hp == 5);
    CHECK(w.has<JustSpawned>(g));
    const std::vector<Entity> kids = children_of(w, g);
    REQUIRE(kids.size() == 1);
    CHECK(w.has<JustSpawned>(kids[0]));
    CHECK(w.get<Transform>(kids[0]).position == Vec2{20, -2});
    CHECK(w.get<game::Aim>(t).barrel == children_of(w, t).at(0));
    // Immediate and deferred spawns build identical component sets.
    const Entity g2 = lib.spawn(w, "goblin"_id, {10, 10});
    std::array<ComponentId, 16> a{}, b{};
    const std::size_t na = w.components_of(g, a), nb = w.components_of(g2, b);
    CHECK(na == nb);
    CHECK(std::equal(a.begin(), a.begin() + na, b.begin()));
}

TEST_CASE("load from a file through the resolver; missing files are errors") {
    Files files;
    files.files["data/p.json"] = R"({"prefabs": {"rock": {"components": {"Health": {"hp": 1}}}}})";
    PrefabLibrary lib;
    REQUIRE(lib.load("data/p.json", SerializeContext{&files, {}}).ok());
    CHECK(lib.has("rock"_id));
    CHECK(!lib.load("data/missing.json", SerializeContext{&files, {}}).ok());
}

TEST_CASE("strict loading: errors name the problem and leave the library unchanged") {
    Files files;
    SerializeContext ctx{&files, {}};
    PrefabLibrary lib;
    REQUIRE(lib.load_text(R"({"prefabs": {"rock": {"components": {"Health": {"hp": 1}}}}})",
                          "a.json", ctx)
                .ok());
    struct Case {
        const char *json;
        const char *needle;
    };
    const Case cases[] = {
        {R"({"prefabs": {"x": {"components": {"Health": {"hp": 1, "bogus": 2}}}}})", "bogus"},
        {R"({"prefabs": {"x": {"components": {"Nope": {}}}}})", "Nope"},
        {R"({"prefabs": {"x": {"components": {"PrevTransform": {}}}}})", "PrevTransform"},
        {R"({"prefabs": {"x": {"extends": "ghost"}}})", "ghost"},
        {R"({"prefabs": {"x": {"extends": "y"}, "y": {"extends": "x"}}})", "cycle"},
        {R"({"prefabs": {"x": {"children": [{"prefab": "x"}]}}})", "cycle"},
        {R"({"prefabs": {"x": {"children": [{"prefab": "ghost"}]}}})", "ghost"},
        {R"({"prefabs": {"x": {"tags": ["NotATag"]}}})", "NotATag"},
        {R"({"prefabs": {"x": {"colour": 1}}})", "colour"},
        {R"({"prefabz": {}})", "prefab"},
        {R"({"prefabs": {"x": {"components": {"Health": {"hp": "many"}}}}})", "hp"},
        {R"({"prefabs": {"rock": {"components": {"Health": {"hp": 2}}}, "bad": {"zz": 1}}})", "zz"},
        {R"(not json)", "a.json"},
    };
    for (const Case &c : cases) {
        const Status st = lib.load_text(c.json, "a.json", ctx);
        REQUIRE_MESSAGE(!st.ok(), c.json);
        CHECK_MESSAGE(st.error().message.find(c.needle) != std::string::npos, st.error().message);
    }
    CHECK(lib.names().size() == 1); // nothing committed
    World w;
    CHECK(w.get<Health>(lib.spawn(w, "rock"_id, {})).hp ==
          1); // "rock" was not replaced by the failed load
}

TEST_CASE("reloading replaces same-named prefabs in place; clear empties") {
    PrefabLibrary lib;
    const SerializeContext ctx{};
    REQUIRE(lib.load_text(R"({"prefabs": {"a": {"components": {"Health": {"hp": 1}}}, "b": {}}})",
                          "x.json", ctx)
                .ok());
    REQUIRE(
        lib.load_text(R"({"prefabs": {"a": {"components": {"Health": {"hp": 2}}}}})", "x.json", ctx)
            .ok());
    CHECK(lib.names().size() == 2);
    CHECK(lib.names()[0] == "a"_id);
    World w;
    CHECK(w.get<Health>(lib.spawn(w, "a"_id, {})).hp == 2);
    const Entity b = lib.spawn(w, "b"_id, {3, 4}); // empty prefab: still gets a Transform
    CHECK(w.get<Transform>(b).position == Vec2{3, 4});
    lib.clear();
    CHECK(!lib.has("a"_id));
    CHECK(lib.names().empty());
}

namespace game {
struct Route {
    std::array<Vec2, 4> points{};
    std::uint8_t count = 0;
};
int g_glow_replaced = 0;
void count_glow_replace(World &, Entity, void *) {
    ++g_glow_replaced;
}
} // namespace game
LAM_REFLECT(game::Route, points, count)

TEST_CASE("Commands records after spawn_prefab override the prefab's values (on_replace fires)") {
    Files files;
    PrefabLibrary lib;
    REQUIRE(lib.load_text(kPrefabs, "prefabs.json", SerializeContext{&files, {}}).ok());
    World w;
    w.set_prefabs(&lib);
    game::g_glow_replaced = 0;
    ComponentRegistry::global().set_hooks<game::Glow>(nullptr, &game::count_glow_replace, nullptr);
    Commands &cmd = w.commands();
    const Entity t = cmd.spawn_prefab("torch"_id, {0, 0});
    cmd.add<game::Glow>(t, game::Glow{7});    // the whole component is replaced
    cmd.add<Health>(t, Health{1, 1});         // a component the prefab does not have
    cmd.apply();
    ComponentRegistry::global().set_hooks<game::Glow>(nullptr, nullptr, nullptr);
    REQUIRE(w.alive(t));
    CHECK(w.get<game::Glow>(t).radius == 7);
    CHECK(game::g_glow_replaced == 1);
    CHECK(w.get<Health>(t).hp == 1);
    CHECK(w.get<Transform>(t).position == Vec2{1, 0}); // prefab values not overridden stay
}

TEST_CASE("prefab arrays may list fewer than N elements; the rest come from the base prefab") {
    Files files;
    PrefabLibrary lib;
    const Status st = lib.load_text(R"({"prefabs": {
      "patrol": {"components": {"Route": {"points": [[0, 0], [10, 0], [10, 10]], "count": 3}}},
      "patrol_moved": {"extends": "patrol", "components": {"Route": {"points": [[5, 5]]}}},
      "short": {"components": {"Route": {"points": [[1, 2], [3, 4]], "count": 2}}}
    }})",
                                    "routes.json", SerializeContext{&files, {}});
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    World w;
    const game::Route moved = w.get<game::Route>(lib.spawn(w, "patrol_moved"_id, {}));
    CHECK(moved.points == std::array<Vec2, 4>{Vec2{5, 5}, Vec2{10, 0}, Vec2{10, 10}, Vec2{}});
    CHECK(moved.count == 3);
    const game::Route brief = w.get<game::Route>(lib.spawn(w, "short"_id, {}));
    CHECK(brief.points == std::array<Vec2, 4>{Vec2{1, 2}, Vec2{3, 4}, Vec2{}, Vec2{}});
    PrefabLibrary strict;
    CHECK_FALSE(strict
                    .load_text(R"({"prefabs": {"long": {"components": {"Route":
                                   {"points": [[0, 0], [0, 0], [0, 0], [0, 0], [0, 0]]}}}}})",
                               "long.json", SerializeContext{&files, {}})
                    .ok());
}
