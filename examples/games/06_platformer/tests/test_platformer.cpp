// Platformer rule tests. Pure checks (bot decisions, scoring) plus whole-level runs on the real
// engine (headless, synchronous assets): the map conventions, every rule (coins, spikes, pits,
// checkpoint, stomp vs side contact, game over), ladder and lift, hero mirroring, the bot reaching
// the goal, undo and save round trips, and no allocation per tick.
#include <cmath>
#include <lam/animation/animator.h>
#include <lam/audio/audio.h>
#include <lam/core/data.h>
#include <lam/core/default_plugins.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/input/driver.h>
#include <lam/physics/components.h>
#include <lam/physics/motors.h>
#include <lam/physics/tile_world.h>
#include <lam/render/sprite.h>
#include <lam/scene/save.h>
#include <lam/test/support.h>
#include "platformer/bot.h"
#include "platformer/rules.h"

using namespace lam;
using namespace lam::literals;
using namespace plat;

namespace {
// 20 x 10 tiles of 16 px with a solid floor on row 8 (feet at y = 128).
TileWorld flat_world() {
    TileWorld tw;
    tw.resize(20, 10, 16, {0, 0});
    for (int x = 0; x < 20; ++x) {
        tw.set({x, 8}, TileShape::Solid, 0);
        tw.set({x, 9}, TileShape::Solid, 0);
    }
    return tw;
}
BotSenses hero_at(float x) {
    BotSenses s;
    s.feet = {x, 128};
    s.grounded = true;
    s.velocity = {110, 0};
    return s;
}

// A level app on the main world: engine (headless), the game plugin, prefabs, the sound bank and
// the map. The level's objects spawn on the first frame (tilemap_sync, FrameBegin).
struct LevelHarness {
    test::HeadlessApp h;
    LevelHarness() {
        App &app = h.app();
        app.add_plugins(DefaultPlugins{.headless = true, .assets = {.root = PLATFORMER_ASSET_DIR}});
        app.add_plugin<PlatformerPlugin>();
        REQUIRE(app.res<PrefabLibrary>().load("prefabs.json", app.serialize_context()).ok());
        bank = app.res<AssetServer>().load<SoundBank>("sfx/platformer.sounds.json");
        app.res<Audio>().add_bank(bank);
        spawn_level(app, app.main_world());
    }
    World &world() { return h.app().main_world(); }
    const Level &level() { return world().resources().get<Level>(); }
    template <class Tag> Entity first() {
        for (auto [e, tf] : world().view<Transform>(With<Tag>{})) return e;
        return {};
    }
    Entity hero() { return first<Player>(); }
    Entity slime() {
        for (auto [e, p] : world().view<Patrol>()) return e;
        return {};
    }
    Vec2 feet(Entity e) { return world().get<Transform>(e).position; }
    // Move the hero without interpolation and with no speed left.
    void place_hero(Vec2 at) {
        world().teleport(hero(), at);
        world().get<Velocity>(hero()).linear = {};
    }
    AssetId<SoundBank> bank;
};

// Holds the move axis every tick, like a player keeping keys down. jump(n): press jump, then
// hold it for n ticks in total (a full-height jump needs it held, or jump_cut caps the rise).
class HoldDriver final : public InputDriver {
public:
    Controls controls;
    Vec2 move;
    void jump(int ticks) { jump_ticks_ = ticks, jump_pressed_ = false; }
    void fill(const Ctx &, std::span<InputFrame, kMaxPlayers> frames) override {
        frames[0].clear_devices();
        frames[0].set_axis(controls.move, move);
        if (jump_ticks_ > 0) {
            if (!jump_pressed_) frames[0].press(controls.jump);
            else frames[0].hold(controls.jump);
            jump_pressed_ = true;
            --jump_ticks_;
        }
    }
private:
    int jump_ticks_ = 0;
    bool jump_pressed_ = false;
};
}  // namespace

TEST_CASE("bot: runs right on flat ground and jumps only for a reason") {
    const TileWorld tw = flat_world();
    const BotDecision d = bot_decide(tw, hero_at(40));
    CHECK(d.move_x == 1.0f);
    CHECK(d.jump == BotReason::None);
    BotSenses air = hero_at(40);
    air.grounded = false;
    CHECK(bot_decide(tw, air).jump == BotReason::None);         // never decides mid-air
}

TEST_CASE("bot: jumps at gaps, two-tile walls, hazards, enemies and when stuck") {
    SUBCASE("gap") {
        TileWorld tw = flat_world();
        for (int y = 8; y < 10; ++y) tw.set({4, y}, TileShape::Empty, 0);
        CHECK(bot_decide(tw, hero_at(56)).jump == BotReason::Gap);   // front edge 61, gap at 64
        CHECK(bot_decide(tw, hero_at(20)).jump == BotReason::None);  // still far away
    }
    SUBCASE("wall two tiles high") {
        TileWorld tw = flat_world();
        tw.set({5, 7}, TileShape::Solid, 0);
        tw.set({5, 6}, TileShape::Solid, 0);
        CHECK(bot_decide(tw, hero_at(70)).jump == BotReason::Wall);
    }
    SUBCASE("slope is not a wall") {
        TileWorld tw = flat_world();
        tw.set({5, 7}, TileShape::SlopeUpRight, 0);
        CHECK(bot_decide(tw, hero_at(70)).jump == BotReason::None);
    }
    SUBCASE("spikes") {
        TileWorld tw = flat_world();
        tw.set({5, 7}, TileShape::Empty, kTileHazard);
        CHECK(bot_decide(tw, hero_at(70)).jump == BotReason::Hazard);
    }
    SUBCASE("enemy ahead, not behind") {
        const TileWorld tw = flat_world();
        const Vec2 ahead[] = {{100, 128}};
        BotSenses s = hero_at(70);
        s.enemies = ahead;
        CHECK(bot_decide(tw, s).jump == BotReason::Enemy);
        s.feet.x = 120;
        CHECK(bot_decide(tw, s).jump == BotReason::None);
    }
    SUBCASE("stuck") {
        const TileWorld tw = flat_world();
        CHECK(bot_decide(tw, hero_at(40), 30).jump == BotReason::Stuck);
    }
}

TEST_CASE("score: coins, stomps and a goal bonus per life left") {
    Level lv;
    lv.coins = 3;
    lv.stomps = 1;
    CHECK(score_of(lv) == 550);
    lv.complete = true;
    lv.lives = 2;
    CHECK(score_of(lv) == 550 + 1000 + 400);
}

// Pins the Tiled conventions the level relies on: tile properties become
// TileWorld shapes/flags, point objects become prefabs at their point, a Collider without Velocity
// or motor is a Static solid.
TEST_CASE("the map builds TileWorld and spawns every object") {
    LevelHarness t;
    t.h.ticks(5);
    REQUIRE(t.level().started);
    World &w = t.world();
    const TileWorld &tw = w.resources().get<TileWorld>();
    CHECK(tw.shapes.width() == 60);
    CHECK(tw.shapes.height() == 17);
    CHECK(tw.at({1, 14}) == TileShape::Solid);                  // grass floor
    CHECK(tw.at({18, 13}) == TileShape::SlopeUpRight);          // hill
    CHECK(tw.at({22, 12}) == TileShape::SlopeUpLeft);
    CHECK(tw.at({13, 10}) == TileShape::OneWay);                // coin plank
    CHECK((tw.flags_at({52, 10}) & kTileLadder) != 0);
    CHECK((tw.flags_at({26, 13}) & kTileHazard) != 0);          // floor spikes: hazard, no shape
    CHECK(tw.at({26, 13}) == TileShape::Empty);
    CHECK(w.count<Coin>() == 12);
    CHECK(w.count<Goal>() == 1);
    CHECK(w.count<Patrol>() == 1);
    CHECK(w.count<Checkpoint>() == 1);
    CHECK(w.count<PathFollower>() == 1);
    CHECK(t.level().respawn.x == doctest::Approx(56));          // hero point object (3.5 tiles)
    int statics = 0;                                            // the crate stack: Static entity solid
    for (auto [e, col, tf] : w.view<Collider, Transform>())
        if (!col.trigger && !w.has<Velocity>(e) && !w.has<PlatformerMotor>(e) && std::fabs(tf.position.x - 264) < 1)
            ++statics;
    CHECK(statics == 1);
    for (auto [e, pf] : w.view<PathFollower>()) {               // lift_setup: Tiled "travel_y" -> points
        CHECK(pf.count == 2);
        CHECK(pf.points[0].x == doctest::Approx(904));
        CHECK(pf.points[0].y == doctest::Approx(180));
        CHECK(pf.points[1].y == doctest::Approx(100));
    }
}

// The engine formats are strict: the graph (and its sheet, relative to the graph file) and the
// sound bank (groups keyed by id, files relative to the bank) must load, or the hero never
// animates and every PlaySound warns.
TEST_CASE("the hero's graph and sheet and the sound bank load") {
    LevelHarness t;
    t.h.ticks(2);
    const AssetServer &assets = t.h.app().res<AssetServer>();
    const Animator &anim = t.world().get<Animator>(t.hero());
    CHECK(assets.state(anim.graph) == LoadState::Ready);
    CHECK(assets.state(t.bank) == LoadState::Ready);
    const Audio &audio = t.h.app().res<Audio>();
    for (const char *g : {"coin", "jump", "stomp", "hurt", "checkpoint", "win"}) {
        INFO(g);
        CHECK(audio.has_group(str_id(g)));
    }
}

TEST_CASE("the hero runs left mirrored and right unmirrored") {
    LevelHarness t;
    t.h.ticks(5);
    HoldDriver drive;
    drive.controls = t.h.app().res<Controls>();
    t.h.app().res<InputSystem>().set_driver(&drive);
    drive.move = {-1, 0};
    t.h.ticks(20);
    const Entity hero = t.hero();
    CHECK(t.world().get<Velocity>(hero).linear.x < -50);
    CHECK((t.world().get<Sprite>(hero).flags & kFlipX) != 0);
    CHECK(t.world().get<Animator>(hero).clip == "run"_id);
    drive.move = {1, 0};
    t.h.ticks(20);
    CHECK((t.world().get<Sprite>(hero).flags & kFlipX) == 0);
    drive.move = {};
    t.h.ticks(40);
    CHECK(t.world().get<Animator>(hero).clip == "idle"_id);
    t.h.app().res<InputSystem>().set_driver(nullptr);
}

TEST_CASE("floor spikes cost a life and respawn the hero at the start") {
    LevelHarness t;
    t.h.ticks(5);
    const Vec2 start = t.level().respawn;
    t.place_hero({424, 223});                                   // on the floor spikes (cols 26-27)
    t.h.ticks(3);
    CHECK(t.level().deaths == 1);
    CHECK(t.level().lives == 2);
    CHECK(t.feet(t.hero()).x == doctest::Approx(start.x));
    CHECK(t.world().get<PlayerState>(t.hero()).invuln_ticks > 0);
    t.place_hero({424, 223});                                   // invulnerable: no second loss
    t.h.ticks(3);
    CHECK(t.level().deaths == 1);
}

TEST_CASE("falling into a pit costs a life") {
    LevelHarness t;
    t.h.ticks(5);
    t.place_hero({168, 200});                                   // above the first pit (cols 10-11)
    for (int i = 0; i < 60 && t.level().deaths == 0; ++i) t.h.ticks(1);
    CHECK(t.level().deaths == 1);
    t.h.ticks(2);
    CHECK(t.feet(t.hero()).y < 224);                            // back on the start floor
}

TEST_CASE("a touched checkpoint becomes the respawn point and switches on") {
    LevelHarness t;
    t.h.ticks(5);
    Entity cp;
    for (auto [e, c] : t.world().view<Checkpoint>()) cp = e;
    REQUIRE(cp);
    t.place_hero({488, 223});
    t.h.ticks(3);
    CHECK(t.world().get<Checkpoint>(cp).active);
    CHECK(t.level().respawn.x == doctest::Approx(488));
    CHECK(t.world().get<Animator>(cp).clip == "on"_id);
    t.place_hero({424, 223});                                   // spikes
    t.h.ticks(3);
    CHECK(t.level().deaths == 1);
    CHECK(t.feet(t.hero()).x == doctest::Approx(488));
}

TEST_CASE("landing on the slime stomps it; walking into it hurts") {
    SUBCASE("stomp") {
        LevelHarness t;
        t.h.ticks(5);
        const Vec2 s = t.feet(t.slime());
        t.place_hero({s.x, s.y - 30});
        for (int i = 0; i < 40 && t.level().stomps == 0; ++i) t.h.ticks(1);
        CHECK(t.level().stomps == 1);
        CHECK(t.level().deaths == 0);
        t.h.ticks(1);
        CHECK(t.world().count<Patrol>() == 0);
        CHECK(t.world().get<Velocity>(t.hero()).linear.y < 0);  // bounced
    }
    SUBCASE("side contact") {
        LevelHarness t;
        t.h.ticks(5);
        const Vec2 s = t.feet(t.slime());
        t.place_hero({s.x - 13, s.y});                          // the slime walks left into the hero
        for (int i = 0; i < 60 && t.level().deaths == 0; ++i) t.h.ticks(1);
        CHECK(t.level().deaths == 1);
        CHECK(t.level().stomps == 0);
        CHECK(t.world().count<Patrol>() == 1);
    }
}

TEST_CASE("three losses end the run") {
    LevelHarness t;
    t.h.ticks(5);
    for (int i = 0; i < 3; ++i) {
        t.place_hero({424, 223});
        t.h.ticks(kInvulnTicks + 2);
    }
    CHECK(t.level().deaths == 3);
    CHECK(t.level().lives == 0);
    CHECK(t.level().game_over);
    CHECK_FALSE(t.level().complete);
    CHECK(t.level().finish_tick > 0);
}

TEST_CASE("the ladder and the lift reach the high coins") {
    SUBCASE("ladder to the plank") {
        LevelHarness t;
        t.h.ticks(5);
        HoldDriver drive;
        drive.controls = t.h.app().res<Controls>();
        drive.move = {0, -1};                                   // up (+y is down)
        t.h.app().res<InputSystem>().set_driver(&drive);
        t.place_hero({840, 223});                               // ladder column 52
        t.h.ticks(60);
        CHECK(t.world().get<PlatformerMotor>(t.hero()).on_ladder);
        CHECK(t.feet(t.hero()).y < 223 - 40);
        t.h.ticks(100);                                         // to the top (row 8, y 128)
        drive.move = {1, -1};                                   // step off onto the one-way plank
        t.h.ticks(20);
        drive.move = {1, 0};
        t.h.ticks(6);
        drive.move = {};                                        // stop before its end (x 880)
        t.h.ticks(30);
        t.h.app().res<InputSystem>().set_driver(nullptr);
        CHECK(t.feet(t.hero()).y == doctest::Approx(128));
        CHECK(t.level().coins == 2);                            // (53, 7) and (54, 7)
    }
    SUBCASE("lift") {
        LevelHarness t;
        t.h.ticks(5);
        Entity lift;
        for (auto [e, pf] : t.world().view<PathFollower>()) lift = e;
        REQUIRE(lift);
        const Vec2 at = t.feet(lift);                           // centre; the top is 4 px above
        CHECK(at.y < 180);                                      // already rising
        t.place_hero({at.x, at.y - 5});                         // feet 1 px above the top
        t.h.ticks(60);
        const Entity hero = t.hero();
        CHECK(t.feet(hero).y < at.y - 5 - 20);                  // 32 px/s for one second
        CHECK(t.feet(hero).y == doctest::Approx(t.feet(lift).y - 4).epsilon(0.02));
        CHECK(t.world().get<PlatformerMotor>(hero).on_ground);
        t.h.ticks(80);                                          // near the top of the path (y 100)
        CHECK(t.feet(lift).y < 104);
        HoldDriver drive;
        drive.controls = t.h.app().res<Controls>();
        drive.move = {1, 0};
        drive.jump(18);
        t.h.app().res<InputSystem>().set_driver(&drive);
        t.h.ticks(50);                                          // onto the high plank (row 5, y 80)
        t.h.app().res<InputSystem>().set_driver(nullptr);
        CHECK(t.feet(hero).y == doctest::Approx(80));
        CHECK(t.level().coins == 2);                            // (57, 4) and (58, 4)
    }
}

TEST_CASE("summary line has the documented shape") {
    LevelHarness t;
    const std::string line = summary_line(t.world());
    CHECK(line.rfind("platformer summary: ticks=", 0) == 0);
    CHECK(line.find(" hash=") != std::string::npos);
}

TEST_CASE("bot reaches the goal headless with the summary's numbers") {
    LevelHarness t;
    PlatformerBot bot;
    bot.set_controls(t.h.app().res<Controls>());
    t.h.app().res<InputSystem>().set_driver(&bot);
    for (int i = 0; i < 60 && !t.level().complete; ++i) t.h.ticks(50);   // <= 3000 ticks
    t.h.app().res<InputSystem>().set_driver(nullptr);
    INFO(summary_line(t.world()));
    CHECK(t.level().started);
    CHECK(t.level().coins_total == 12);
    CHECK(t.level().coins >= 3);                                // the three coins on the start floor
    CHECK(t.level().complete);
    CHECK_FALSE(t.level().game_over);
    CHECK(t.level().stomps == 1);
    CHECK(t.level().deaths == 0);
    CHECK(bot.jumps() > 0);
}

TEST_CASE("a level tick allocates nothing after warm-up") {
    LevelHarness t;
    PlatformerBot bot;
    bot.set_controls(t.h.app().res<Controls>());
    t.h.app().res<InputSystem>().set_driver(&bot);
    t.h.ticks(120);
    CHECK_NO_ALLOC(t.h.ticks(240));                             // coins, slime, lift, jumps
    t.h.app().res<InputSystem>().set_driver(nullptr);
}

TEST_CASE("undo restores the level exactly and replays identically") {
    LevelHarness t;
    t.h.ticks(30);                                              // map synced, hero spawned
    REQUIRE(t.level().started);
    History history;
    history.push(t.world());
    const std::uint64_t at_push = t.world().state_hash();
    t.h.ticks(120);                                             // slime patrols, lift moves
    const std::uint64_t after = t.world().state_hash();
    CHECK(after != at_push);
    t.world().resources().get<Level>().coins = 99;              // the Level resource is snapshotted too
    REQUIRE(history.undo(t.world(), RestoreOptions{.tick = true}));
    CHECK(t.world().state_hash() == at_push);
    CHECK(t.level().coins == 0);
    t.h.ticks(120);
    CHECK(t.world().state_hash() == after);                     // same inputs (none) -> same state
}

TEST_CASE("save round trip keeps the state hash") {
    LevelHarness t;
    t.h.ticks(90);
    REQUIRE(t.level().started);
    const std::uint64_t before = t.world().state_hash();
    DataWriter out;
    REQUIRE(serialize_world(t.world(), out, t.h.app().serialize_context()).ok());
    Result<std::string> text = out.finish();
    REQUIRE(text.ok());
    t.h.ticks(60);                                              // move on, then load the save back
    Result<DataDocument> doc = DataDocument::parse(text.value(), "save.json");
    REQUIRE(doc.ok());
    DataReader root = doc.value().root();
    REQUIRE(deserialize_world(t.world(), root, t.h.app().serialize_context()).ok());
    CHECK(t.world().state_hash() == before);
}


// Engine regression, without the game: a PlatformerMotor body that falls onto a rising one-way
// MovingPlatform used to drop through it when its feet were within the platform's rise for that
// tick.
TEST_CASE("engine: a motor body lands on a rising one-way platform") {
    test::HeadlessApp h;
    h.app().add_plugins(DefaultPlugins{.headless = true});
    World &w = h.app().main_world();
    const Entity lift = w.create();
    w.add<Transform>(lift).position = {100, 100};
    w.add<Velocity>(lift);
    w.add<Collider>(lift, Collider{.half = {24, 4}, .one_way = true});
    w.add<MovingPlatform>(lift);
    PathFollower path;
    path.points[0] = {100, 100};
    path.points[1] = {100, 0};
    path.count = 2;
    path.speed = 32;
    w.add<PathFollower>(lift, path);
    const Entity body = w.create();
    w.add<Transform>(body).position = {100, 95};               // feet 1 px above the top (y 96)
    w.add<Velocity>(body);
    w.add<Collider>(body, Collider{.offset = {0, -7}, .half = {5, 7}});
    w.add<PlatformerMotor>(body);
    h.ticks(60);
    INFO("body feet y " << w.get<Transform>(body).position.y << ", lift top y " << w.get<Transform>(lift).position.y - 4);
    CHECK(w.get<PlatformerMotor>(body).on_ground);
    CHECK(w.get<Transform>(body).position.y == doctest::Approx(w.get<Transform>(lift).position.y - 4));
}

// The same as a sweep: 120 drop heights (1 to 60.5 px above the rising lift's top, 0.5 px apart),
// no fall-throughs (before the fix, 19 of them fell).
TEST_CASE("engine: no drop height falls through a rising lift") {
    int fell = 0;
    for (int k = 0; k < 120; ++k) {
        test::HeadlessApp h;
        h.app().add_plugins(DefaultPlugins{.headless = true});
        World &w = h.app().main_world();
        const Entity lift = w.create();
        w.add<Transform>(lift).position = {100, 200};
        w.add<Velocity>(lift);
        w.add<Collider>(lift, Collider{.half = {24, 4}, .one_way = true});
        w.add<MovingPlatform>(lift);
        PathFollower path;
        path.points[0] = {100, 200};
        path.points[1] = {100, 0};
        path.count = 2;
        path.speed = 32;
        w.add<PathFollower>(lift, path);
        const Entity body = w.create();
        w.add<Transform>(body).position = {100, 196.0f - 1.0f - 0.5f * static_cast<float>(k)};
        w.add<Velocity>(body);
        w.add<Collider>(body, Collider{.offset = {0, -7}, .half = {5, 7}});
        w.add<PlatformerMotor>(body);
        h.ticks(150);
        if (w.get<Transform>(body).position.y > w.get<Transform>(lift).position.y) ++fell;
    }
    CHECK(fell == 0);
}
