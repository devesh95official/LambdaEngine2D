// Camera maths (camera_math.h), driven through a small stateful wrapper. make_camera_view rounds
// the translation with floor(t + 0.5), so -79.5 shows as -80.
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>

#include <lam/render/camera.h>
#include <lam/render/camera_math.h>
#include <lam/test/support.h>

namespace {

using lam::Vec2;

struct Config {
    float follow_half_life, lookahead_seconds, lookahead_max, lookahead_half_life;
};
struct Bounds {
    float view_width, view_height, map_width, map_height;
};
struct Target {
    float x, y, vx, vy;
};

const Config kConfig{0.08f, 0.25f, 24.0f, 0.2f};
const Bounds kBounds{480.0f, 270.0f, 768.0f, 480.0f};
const Bounds kOpen{480.0f, 270.0f, 100000.0f, 100000.0f};
constexpr float kDt = 1.0f / 60.0f;

Target at(float x, float y, float vx = 0.0f, float vy = 0.0f) {
    return {x, y, vx, vy};
}

lam::CameraFollow follow_of(const Config &c) {
    lam::CameraFollow f;
    f.half_life = c.follow_half_life;
    f.lookahead_seconds = c.lookahead_seconds;
    f.lookahead_max = c.lookahead_max;
    f.lookahead_half_life = c.lookahead_half_life;
    return f;
}

bool usable(const Target &t) {
    return std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.vx) && std::isfinite(t.vy);
}

// Calls the free functions in the same order as the camera_follow system.
class OldCamera {
public:
    void snap(const Target &t, const Bounds &b) {
        if (!usable(t)) return;
        look_ = {};
        center_ = clamp(Vec2{t.x, t.y}, b);
        placed_ = true;
        view_ = b;
    }
    void update(const Target &t, const Bounds &b, const Config &c, float dt) {
        if (!placed_) {
            snap(t, b);
            return;
        }
        view_ = b;
        if (!usable(t) || !(dt > 0.0f) || !std::isfinite(dt)) return;
        const lam::CameraFollow f = follow_of(c);
        look_ = lam::camera_lookahead_step(look_, {t.vx, t.vy}, f, dt);
        center_ = lam::camera_follow_point(center_, Vec2{t.x, t.y} + look_, f, dt);
        center_ = clamp(center_, b);
    }
    // Whole-pixel top-left corner as the view draws it.
    int left() const { return -static_cast<int>(view().world_to_screen.tx); }
    int top() const { return -static_cast<int>(view().world_to_screen.ty); }
    float smooth_left() const { return center_.x - view_.view_width * 0.5f; }
    float smooth_top() const { return center_.y - view_.view_height * 0.5f; }
    Vec2 center() const { return center_; }

private:
    static Vec2 clamp(Vec2 c, const Bounds &b) {
        return lam::clamp_to_bounds(c, {b.view_width, b.view_height},
                                    {0, 0, b.map_width, b.map_height});
    }
    lam::CameraView view() const {
        lam::Camera2D cam;
        cam.position = center_;
        return lam::make_camera_view(
            cam, {static_cast<int>(view_.view_width), static_cast<int>(view_.view_height)}, {},
            true);
    }
    Vec2 center_, look_;
    Bounds view_{};
    bool placed_ = false;
};

float centre_x(const OldCamera &c) {
    return c.center().x;
}
float centre_y(const OldCamera &c) {
    return c.center().y;
}

// The view's rounding rule: translation = floor(half - centre + 0.5).
void check_on_pixels(const OldCamera &c, const Bounds &b) {
    CHECK(static_cast<float>(-c.left()) == std::floor(b.view_width * 0.5f - c.center().x + 0.5f));
    CHECK(static_cast<float>(-c.top()) == std::floor(b.view_height * 0.5f - c.center().y + 0.5f));
}

void check_inside(const OldCamera &c, const Bounds &b) {
    CHECK(c.left() >= 0);
    CHECK(c.top() >= 0);
    CHECK(c.left() + static_cast<int>(b.view_width) <= static_cast<int>(b.map_width));
    CHECK(c.top() + static_cast<int>(b.view_height) <= static_cast<int>(b.map_height));
}

} // namespace

TEST_CASE("camera: snap centres the view on the target") {
    OldCamera camera;
    camera.snap(at(384.0f, 240.0f), kBounds);
    CHECK(camera.left() == 144);
    CHECK(camera.top() == 105);
    CHECK(camera.smooth_left() == 144.0f);
    CHECK(camera.smooth_top() == 105.0f);
}

TEST_CASE("camera: snap ignores velocity (no look-ahead) and drops an earlier look-ahead") {
    OldCamera camera;
    camera.snap(at(384.0f, 240.0f, 90.0f, 90.0f), kBounds);
    CHECK(camera.left() == 144);
    CHECK(camera.top() == 105);

    OldCamera moving;
    moving.snap(at(5000.0f, 5000.0f), kOpen);
    for (int i = 0; i < 120; ++i)
        moving.update(at(5000.0f, 5000.0f, 90.0f, 0.0f), kOpen, kConfig, kDt);
    REQUIRE(centre_x(moving) > 5010.0f);
    moving.snap(at(6000.0f, 5000.0f), kOpen);
    CHECK(centre_x(moving) == 6000.0f);
    moving.update(at(6000.0f, 5000.0f), kOpen, kConfig, kDt);
    CHECK(centre_x(moving) == 6000.0f);
}

TEST_CASE("camera: the first update snaps if snap was never called") {
    OldCamera camera;
    camera.update(at(384.0f, 240.0f, 90.0f, 0.0f), kBounds, kConfig, kDt);
    CHECK(camera.left() == 144);
    CHECK(camera.top() == 105);
}

TEST_CASE("camera: the view never leaves the map") {
    SUBCASE("snap near each corner") {
        OldCamera camera;
        camera.snap(at(10.0f, 10.0f), kBounds);
        CHECK(camera.left() == 0);
        CHECK(camera.top() == 0);
        camera.snap(at(760.0f, 470.0f), kBounds);
        CHECK(camera.left() == 768 - 480);
        CHECK(camera.top() == 480 - 270);
        camera.snap(at(-500.0f, 9000.0f), kBounds);
        CHECK(camera.left() == 0);
        CHECK(camera.top() == 210);
    }
    SUBCASE("running into every edge and corner at speed, look-ahead pushing outward") {
        OldCamera camera;
        camera.snap(at(384.0f, 240.0f), kBounds);
        std::uint32_t state = 12345u;
        float x = 384.0f, y = 240.0f, vx = 0.0f, vy = 0.0f;
        for (int frame = 0; frame < 60 * 90; ++frame) {
            if (frame % 90 == 0) {
                state = state * 1664525u + 1013904223u;
                const int direction = static_cast<int>((state >> 24) % 9u);
                vx = static_cast<float>(direction % 3 - 1) * 300.0f;
                vy = static_cast<float>(direction / 3 - 1) * 300.0f;
            }
            x += vx * kDt;
            y += vy * kDt;
            if (x < -200.0f || x > 968.0f) vx = -vx;
            if (y < -200.0f || y > 680.0f) vy = -vy;
            camera.update(at(x, y, vx, vy), kBounds, kConfig, kDt);
            check_inside(camera, kBounds);
            check_on_pixels(camera, kBounds);
        }
    }
    SUBCASE("held against an edge it rests exactly on the edge") {
        OldCamera camera;
        camera.snap(at(384.0f, 240.0f), kBounds);
        for (int i = 0; i < 600; ++i)
            camera.update(at(760.0f, 240.0f, 90.0f, 0.0f), kBounds, kConfig, kDt);
        CHECK(camera.left() == 288);
        CHECK(camera.smooth_left() == 288.0f);
    }
}

TEST_CASE("camera: after an edge the view waits for the target, then follows without a jump") {
    OldCamera camera;
    camera.snap(at(760.0f, 240.0f), kBounds);
    REQUIRE(camera.left() == 288);
    float x = 760.0f;
    int previous = camera.left();
    for (int i = 0; i < 600 && x > 300.0f; ++i) {
        x -= 90.0f * kDt;
        camera.update(at(x, 240.0f, -90.0f, 0.0f), kBounds, kConfig, kDt);
        if (x > 528.0f + 24.0f) CHECK(camera.left() == 288);
        CHECK(camera.left() <= previous);
        CHECK(previous - camera.left() <= 3);
        previous = camera.left();
    }
    CHECK(camera.left() < 150);
}

TEST_CASE("camera: a map smaller than the view is centred and does not move") {
    SUBCASE("smaller on both axes") {
        const Bounds small{480.0f, 270.0f, 320.0f, 160.0f};
        OldCamera camera;
        camera.snap(at(10.0f, 10.0f), small);
        CHECK(camera.left() == -80);
        CHECK(camera.top() == -55);
        CHECK(-camera.left() == camera.left() + 480 - 320);
        CHECK(-camera.top() == camera.top() + 270 - 160);
        for (int i = 0; i < 300; ++i) {
            camera.update(at(10.0f + static_cast<float>(i), 150.0f, 60.0f, -200.0f), small, kConfig,
                          kDt);
            CHECK(camera.left() == -80);
            CHECK(camera.top() == -55);
        }
    }
    SUBCASE("smaller on one axis only: that axis is centred, the other follows and clamps") {
        const Bounds wide{480.0f, 270.0f, 1024.0f, 160.0f};
        OldCamera camera;
        camera.snap(at(600.0f, 20.0f), wide);
        CHECK(camera.left() == 360);
        CHECK(camera.top() == -55);
        camera.snap(at(2000.0f, 150.0f), wide);
        CHECK(camera.left() == 1024 - 480);
        CHECK(camera.top() == -55);
        const Bounds tall{480.0f, 270.0f, 320.0f, 800.0f};
        camera.snap(at(300.0f, 400.0f), tall);
        CHECK(camera.left() == -80);
        CHECK(camera.top() == 265);
    }
    SUBCASE("an odd difference still gives one constant whole-pixel position") {
        const Bounds odd{480.0f, 270.0f, 321.0f, 161.0f};
        OldCamera camera;
        camera.snap(at(0.0f, 0.0f), odd);
        const int left = camera.left();
        const int top = camera.top();
        CHECK(left == -80); // -79.5: the view rounds its translation (79.5 + 0.5 -> 80)
        CHECK(top == -55);  // -54.5 likewise
        for (int i = 0; i < 200; ++i) {
            camera.update(at(static_cast<float>(i * 2), static_cast<float>(i), 120.0f, 60.0f), odd,
                          kConfig, kDt);
            CHECK(camera.left() == left);
            CHECK(camera.top() == top);
        }
    }
    SUBCASE("a map exactly as large as the view sits at 0") {
        const Bounds exact{480.0f, 270.0f, 480.0f, 270.0f};
        OldCamera camera;
        camera.snap(at(400.0f, 30.0f), exact);
        CHECK(camera.left() == 0);
        CHECK(camera.top() == 0);
    }
}

TEST_CASE("camera: glides to a resting target, halving the distance every half-life") {
    OldCamera camera;
    camera.snap(at(5000.0f, 5000.0f), kOpen);
    const Target target = at(5100.0f, 5060.0f);
    for (int i = 0; i < 4; ++i)
        camera.update(target, kOpen, kConfig, 0.02f);
    CHECK(centre_x(camera) == doctest::Approx(5050.0f).epsilon(0.00001));
    CHECK(centre_y(camera) == doctest::Approx(5030.0f).epsilon(0.00001));
    for (int i = 0; i < 4; ++i)
        camera.update(target, kOpen, kConfig, 0.02f);
    CHECK(centre_x(camera) == doctest::Approx(5075.0f).epsilon(0.00001));
    CHECK(centre_y(camera) == doctest::Approx(5045.0f).epsilon(0.00001));
}

TEST_CASE("camera: converges on the target and never overshoots it") {
    for (const float dt : {1.0f / 240.0f, 1.0f / 120.0f, 1.0f / 60.0f, 1.0f / 30.0f, 0.25f, 5.0f}) {
        CAPTURE(dt);
        OldCamera camera;
        camera.snap(at(300.0f, 300.0f), kOpen);
        const Target target = at(420.0f, 230.0f);
        float px = centre_x(camera), py = centre_y(camera);
        int pleft = camera.left();
        for (int i = 0; i < 2000; ++i) {
            camera.update(target, kOpen, kConfig, dt);
            const float x = centre_x(camera), y = centre_y(camera);
            CHECK(x >= px);
            CHECK(x <= 420.0f);
            CHECK(y <= py);
            CHECK(y >= 230.0f);
            CHECK(camera.left() >= pleft);
            px = x;
            py = y;
            pleft = camera.left();
        }
        CHECK(camera.left() == 420 - 240);
        CHECK(camera.top() == 230 - 135);
        CHECK(centre_x(camera) == doctest::Approx(420.0f));
        CHECK(centre_y(camera) == doctest::Approx(230.0f));
    }
}

TEST_CASE("camera: the glide is the same at 30, 60 and 120 frames per second") {
    const Target target = at(5200.0f, 4900.0f);
    float results[3] = {};
    const int steps[3] = {30, 60, 120};
    for (int k = 0; k < 3; ++k) {
        OldCamera camera;
        camera.snap(at(5000.0f, 5000.0f), kOpen);
        for (int i = 0; i < steps[k] / 5; ++i)
            camera.update(target, kOpen, kConfig, 1.0f / static_cast<float>(steps[k]));
        results[k] = centre_x(camera);
    }
    CHECK(results[0] == doctest::Approx(results[1]).epsilon(0.00001));
    CHECK(results[1] == doctest::Approx(results[2]).epsilon(0.00001));
    CHECK(results[1] == doctest::Approx(5164.645f).epsilon(0.00001));
}

TEST_CASE("camera: a half-life of 0 locks the view to the target") {
    Config locked = kConfig;
    locked.follow_half_life = 0.0f;
    locked.lookahead_seconds = 0.0f;
    OldCamera camera;
    camera.snap(at(5000.0f, 5000.0f), kOpen);
    camera.update(at(5321.0f, 4876.0f, 90.0f, 0.0f), kOpen, locked, kDt);
    CHECK(centre_x(camera) == 5321.0f);
    CHECK(centre_y(camera) == 4876.0f);
}

TEST_CASE("camera: look-ahead leads in the direction of travel, swings on a turn, and settles") {
    Config config = kConfig;
    config.follow_half_life = 0.0f;
    OldCamera camera;
    float x = 50000.0f;
    const float y = 50000.0f;
    camera.snap(at(x, y), kOpen);
    const auto lead = [&] { return centre_x(camera) - x; };
    const auto walk = [&](float velocity, int frames, auto &&each_frame) {
        for (int i = 0; i < frames; ++i) {
            const float before = lead();
            x += velocity * kDt;
            camera.update(at(x, y, velocity, 0.0f), kOpen, config, kDt);
            each_frame(before, lead());
        }
    };
    constexpr float kSlack = 0.02f;
    walk(90.0f, 240, [&](float before, float after) {
        CHECK(after >= before - kSlack);
        CHECK(after <= 22.5f + kSlack);
        CHECK(after >= -kSlack);
    });
    CHECK(lead() == doctest::Approx(22.5f).epsilon(0.002));
    CHECK(centre_y(camera) == doctest::Approx(y));

    OldCamera fresh;
    fresh.snap(at(50000.0f, y), kOpen);
    float fresh_x = 50000.0f;
    for (int i = 0; i < 12; ++i) {
        fresh_x += 90.0f * kDt;
        fresh.update(at(fresh_x, y, 90.0f, 0.0f), kOpen, config, kDt);
    }
    CHECK(centre_x(fresh) - fresh_x == doctest::Approx(11.25f).epsilon(0.005));

    walk(-90.0f, 240, [&](float before, float after) {
        CHECK(after <= before + kSlack);
        CHECK(before - after < 3.0f);
        CHECK(after >= -22.5f - kSlack);
    });
    CHECK(lead() == doctest::Approx(-22.5f).epsilon(0.002));

    walk(0.0f, 600, [&](float before, float after) {
        CHECK(after >= before - kSlack);
        CHECK(after <= kSlack);
    });
    CHECK(lead() == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(camera.left() == 50000 - 240);
    const int rest = camera.left();
    walk(0.0f, 120, [&](float, float) { CHECK(camera.left() == rest); });
}

TEST_CASE("camera: look-ahead is limited in length, along the direction of travel") {
    Config config = kConfig;
    config.follow_half_life = 0.0f;
    OldCamera camera;
    camera.snap(at(5000.0f, 5000.0f), kOpen);
    for (int i = 0; i < 600; ++i) {
        camera.update(at(5000.0f, 5000.0f, 300.0f, -400.0f), kOpen, config, kDt);
        const float dx = centre_x(camera) - 5000.0f, dy = centre_y(camera) - 5000.0f;
        CHECK(std::sqrt(dx * dx + dy * dy) <= 24.0f + 0.01f);
    }
    CHECK(centre_x(camera) - 5000.0f == doctest::Approx(14.4f).epsilon(0.001));
    CHECK(centre_y(camera) - 5000.0f == doctest::Approx(-19.2f).epsilon(0.001));

    SUBCASE("lookahead_max 0 or lookahead_seconds 0 switch it off") {
        for (int which = 0; which < 2; ++which) {
            Config off = config;
            (which == 0 ? off.lookahead_max : off.lookahead_seconds) = 0.0f;
            OldCamera plain;
            plain.snap(at(5000.0f, 5000.0f), kOpen);
            for (int i = 0; i < 60; ++i)
                plain.update(at(5000.0f, 5000.0f, 300.0f, -400.0f), kOpen, off, kDt);
            CHECK(centre_x(plain) == 5000.0f);
            CHECK(centre_y(plain) == 5000.0f);
        }
    }
}

TEST_CASE("camera: with typical numbers the view ends centred on a player who stopped") {
    OldCamera camera;
    float x = 300.0f;
    camera.snap(at(x, 240.0f), kBounds);
    for (int i = 0; i < 90; ++i) {
        x += 90.0f * kDt;
        camera.update(at(x, 240.0f, 90.0f, 0.0f), kBounds, kConfig, kDt);
    }
    CHECK(camera.smooth_left() + 240.0f > x + 5.0f);
    for (int i = 0; i < 600; ++i)
        camera.update(at(x, 240.0f), kBounds, kConfig, kDt);
    CHECK(camera.smooth_left() + 240.0f == doctest::Approx(x).epsilon(0.0001));
    CHECK(static_cast<float>(-camera.left()) == std::floor(240.0f - camera.center().x + 0.5f));
}

TEST_CASE("camera: the position is on whole pixels and does not jitter at sub-pixel speeds") {
    for (const float speed : {3.0f, 7.0f, 20.0f}) {
        for (const float dt : {1.0f / 120.0f, 1.0f / 60.0f}) {
            for (const float sign : {1.0f, -1.0f}) {
                CAPTURE(speed);
                CAPTURE(dt);
                CAPTURE(sign);
                OldCamera camera;
                float x = 4000.3f, y = 4000.7f;
                camera.snap(at(x, y), kOpen);
                int pl = camera.left(), pt = camera.top(), moved = 0;
                const int frames = static_cast<int>(10.0f / dt);
                for (int i = 0; i < frames; ++i) {
                    x += sign * speed * dt;
                    y -= sign * speed * dt;
                    camera.update(at(x, y, sign * speed, -sign * speed), kOpen, kConfig, dt);
                    check_on_pixels(camera, kOpen);
                    const int sx = camera.left() - pl, sy = camera.top() - pt;
                    CHECK(static_cast<float>(sx) * sign >= 0.0f);
                    CHECK(static_cast<float>(sy) * sign <= 0.0f);
                    CHECK(std::abs(sx) <= 1);
                    CHECK(std::abs(sy) <= 1);
                    moved += std::abs(sx);
                    pl = camera.left();
                    pt = camera.top();
                }
                CHECK(static_cast<float>(moved) == doctest::Approx(speed * 10.0f).epsilon(0.15));
            }
        }
    }
}

TEST_CASE("camera: a player standing still gives a picture that stands still") {
    OldCamera camera;
    camera.snap(at(400.4f, 300.6f), kBounds);
    const int left = camera.left(), top = camera.top();
    for (int i = 0; i < 600; ++i) {
        camera.update(at(400.4f, 300.6f), kBounds, kConfig, kDt);
        CHECK(camera.left() == left);
        CHECK(camera.top() == top);
    }
}

TEST_CASE("camera: after the player stops, the picture comes to rest without wobbling") {
    OldCamera camera;
    float x = 4000.0f;
    camera.snap(at(x, 4000.0f), kOpen);
    for (int i = 0; i < 120; ++i) {
        x += 90.0f * kDt;
        camera.update(at(x, 4000.0f, 90.0f, 0.0f), kOpen, kConfig, kDt);
    }
    int previous = camera.left(), reversals = 0, direction = 0;
    for (int i = 0; i < 600; ++i) {
        camera.update(at(x, 4000.0f), kOpen, kConfig, kDt);
        const int step = camera.left() - previous;
        CHECK(std::abs(step) <= 1);
        if (step != 0) {
            if (direction != 0 && step != direction) ++reversals;
            direction = step;
        }
        previous = camera.left();
    }
    CHECK(reversals <= 1);
    CHECK(static_cast<float>(-camera.left()) == std::floor(240.0f - x + 0.5f));
}

TEST_CASE("camera: dt of zero, negative or NaN, and a NaN target, leave the camera unchanged") {
    OldCamera camera;
    camera.snap(at(384.0f, 240.0f), kBounds);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    camera.update(at(500.0f, 300.0f), kBounds, kConfig, 0.0f);
    camera.update(at(500.0f, 300.0f), kBounds, kConfig, -1.0f);
    camera.update(at(500.0f, 300.0f), kBounds, kConfig, nan);
    camera.update(at(nan, 300.0f), kBounds, kConfig, kDt);
    camera.update(at(500.0f, 300.0f, nan, 0.0f), kBounds, kConfig, kDt);
    camera.snap(at(nan, nan), kBounds);
    CHECK(camera.left() == 144);
    CHECK(camera.top() == 105);
    CHECK(camera.smooth_left() == 144.0f);
    camera.update(at(500.0f, 300.0f), kBounds, kConfig, kDt);
    CHECK(camera.smooth_left() > 144.0f);
    // camera_follow_step refuses the same inputs.
    const lam::CameraFollow f = follow_of(kConfig);
    CHECK(lam::camera_follow_step({1, 2}, {nan, 0}, {}, f, kDt) == Vec2{1, 2});
    CHECK(lam::camera_follow_step({1, 2}, {5, 5}, {nan, 0}, f, kDt) == Vec2{1, 2});
    CHECK(lam::camera_follow_step({1, 2}, {5, 5}, {}, f, 0.0f) == Vec2{1, 2});
}

TEST_CASE("camera: a change of view size takes effect on the next call") {
    OldCamera camera;
    camera.snap(at(700.0f, 240.0f), kBounds);
    REQUIRE(camera.left() == 288);
    const Bounds phone{600.0f, 270.0f, 768.0f, 480.0f};
    camera.update(at(700.0f, 240.0f), phone, kConfig, kDt);
    CHECK(camera.left() == 168);
    camera.snap(at(384.0f, 240.0f), phone);
    CHECK(camera.left() == 84);
}

TEST_CASE("camera: a map exactly as large as the view keeps the camera still at a fractional "
          "origin") {
    // lo + view/2 rounds to 240.300003 and lo + size - view/2 to 240.299988, so the clamp bounds
    // are inverted. The result must not depend on the target.
    const lam::Rect bounds{0.3f, 0.0f, 480.0f, 270.0f};
    const Vec2 view{480.0f, 270.0f};
    const Vec2 left = lam::clamp_to_bounds({100.0f, 135.0f}, view, bounds);
    const Vec2 right = lam::clamp_to_bounds({300.0f, 135.0f}, view, bounds);
    CHECK(left.x == right.x);
    CHECK(left.x == 0.3f + 480.0f * 0.5f);
    CHECK(left.y == 135.0f);
    CHECK(right.y == 135.0f);
}
