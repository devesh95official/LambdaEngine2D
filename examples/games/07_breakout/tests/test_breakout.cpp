// Breakout rules, headless (NullPlatform): bricks score, lost balls cost lives, game over,
// paddle angles, and the bot run is deterministic.
#include <lam/test/support.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/driver.h>
#include <lam/input/input_plugin.h>
#include <lam/physics/components.h>
#include <lam/physics/physics_plugin.h>
#include <breakout/breakout.h>

using namespace lam;
using namespace breakout;

namespace {
struct Game {
    lam::test::HeadlessApp h{AppConfig{.seed = 7}};
    Game() {
        h.add<InputPlugin>();
        h.add<PhysicsPlugin>();
        h.add<RulesPlugin>();
        start_game(world());
    }
    World &world() { return h.app().main_world(); }
    Run &run() { return world().resources().get<Run>(); }
    Entity ball() {
        Entity b;
        for (auto [e, tf] : world().view<Transform>(With<Ball>{})) b = e;
        return b;
    }
    // Puts the ball in play at `at` with velocity `v` (no serve).
    void throw_ball(Vec2 at, Vec2 v) {
        run().serving = false;
        world().get<Transform>(ball()).position = at;
        world().get<Velocity>(ball()).linear = v;
    }
    int bricks() {
        int n = 0;
        for (auto [e, b] : world().view<Brick>()) {
            (void)b;
            ++n;
        }
        return n;
    }
};
}

TEST_CASE("a new game has a full wall, three lives and a served ball") {
    Game g;
    g.h.ticks(30);
    CHECK(g.bricks() == kCols * kRows);
    CHECK(g.run().bricks_left == kCols * kRows);
    CHECK(g.run().lives == kStartLives);
    CHECK(g.run().serving);
    const Vec2 p = g.world().get<Transform>(g.ball()).position;
    CHECK(p.x == doctest::Approx(kFieldW * 0.5f));
    CHECK(p.y < kPaddleY);
}

TEST_CASE("a brick hit scores its row's points, breaks the brick and bounces the ball") {
    Game g;
    g.h.ticks(1);
    // Straight up under column 0 into the bottom row (row 4: 10 points).
    const float x = kWall + 2.0f + kBrickPitchX * 0.5f;
    g.throw_ball({x, 120}, {0, -150});
    g.h.ticks(40);
    CHECK(g.run().score == 10);
    CHECK(g.run().bricks_left == kCols * kRows - 1);
    CHECK(g.run().bricks_broken == 1);
    CHECK(g.bricks() == kCols * kRows - 1);
    CHECK(g.world().get<Velocity>(g.ball()).linear.y > 0);    // now falling
}

TEST_CASE("the side walls reflect the ball") {
    Game g;
    g.h.ticks(1);
    g.throw_ball({40, 150}, {-150, -60});
    g.h.ticks(20);
    CHECK(g.world().get<Velocity>(g.ball()).linear.x > 0);
    CHECK(g.world().get<Transform>(g.ball()).position.x > kWall);
}

TEST_CASE("the paddle sends the ball up, angled by where it hits") {
    Game g;
    g.h.ticks(1);
    g.throw_ball({kFieldW * 0.5f + 14, 190}, {0, 150});      // right half of the paddle
    g.h.ticks(20);
    const Vec2 v = g.world().get<Velocity>(g.ball()).linear;
    CHECK(v.y < 0);
    CHECK(v.x > 0);
    CHECK(g.run().paddle_hits == 1);
}

TEST_CASE("a paddle-edge hit beside a side wall keeps the ball in the field") {
    Game g;
    g.h.ticks(1);
    for (auto [e, tf] : g.world().view<Transform>(With<Paddle>{})) tf.position.x = kFieldW - kWall - kPaddleHalf.x;
    // Next tick the ball touches the right wall and the paddle's right edge at once (both
    // CollisionStarted in one tick; seen in a bot run at tick 7303, seed 3).
    g.throw_ball({307.4f, 213.2f}, {116.77f, 116.58f});
    g.h.ticks(120);
    const Vec2 p = g.world().get<Transform>(g.ball()).position;
    CHECK(p.x < kFieldW - kWall);
    CHECK(p.x > kWall);
    CHECK(p.y > 0);
}

TEST_CASE("a lost ball costs a life; the last one ends the game") {
    Game g;
    g.h.ticks(1);
    for (int life = kStartLives; life > 0; --life) {
        g.throw_ball({20, 200}, {0, 200});                      // far left of the paddle: missed
        g.h.ticks(30);
        CHECK(g.run().lives == life - 1);
    }
    CHECK(g.run().over);
    CHECK(g.run().balls_lost == kStartLives);
    CHECK_FALSE(g.run().serving);
    g.h.ticks(30);
    CHECK(g.run().lives == 0);                                  // nothing changes after game over
}

TEST_CASE("the bot breaks bricks, and the same seed gives the same state hash") {
    const auto play = [](int ticks) {
        Game g;
        Bot bot;
        bot.controls = g.h.app().res<Controls>();
        g.h.app().res<InputSystem>().set_driver(&bot);
        g.h.ticks(ticks);
        g.h.app().res<InputSystem>().set_driver(nullptr);
        return std::pair{g.world().state_hash(), g.run().bricks_broken + g.world().resources().get<Stats>().total_bricks};
    };
    const auto a = play(3600);
    const auto b = play(3600);
    CHECK(a.first == b.first);
    CHECK(a.second > 20);
}
