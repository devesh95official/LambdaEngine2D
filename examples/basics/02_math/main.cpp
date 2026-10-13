// math_demo: console tour of lam_math. The same seed prints the same numbers on every run.
//   ./math_demo [seed]
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

#include <lam/math/curve.h>
#include <lam/math/ease.h>
#include <lam/math/geometry.h>
#include <lam/math/grid.h>
#include <lam/math/rng.h>
#include <lam/math/transform.h>
#include <lam/math/tween.h>
#include <lam/math/vec2.h>

using namespace lam;

namespace {

// One Rng per independent system, each derived from the world seed, so drawing more loot never
// changes what the AI rolls.
void rng_streams(std::uint64_t seed) {
    std::printf("-- rng streams (seed %llu)\n", static_cast<unsigned long long>(seed));
    Rng loot(derive_seed(seed, 1));
    Rng ai(derive_seed(seed, 2));
    std::printf("loot d6 rolls:");
    for (int i = 0; i < 6; ++i)
        std::printf(" %d", loot.between(1, 6));
    std::printf("\nai wander angle: %.3f rad, aggression %.2f\n", ai.range(-kPi, kPi), ai.unit());
    std::array<int, 8> deck{1, 2, 3, 4, 5, 6, 7, 8};
    loot.shuffle(std::span<int>(deck));
    std::printf("shuffled deck:");
    for (int card : deck)
        std::printf(" %d", card);
    const Vec2 spray = ai.in_disc(16.0f);
    std::printf("\nspray offset: (%.2f, %.2f)\n", spray.x, spray.y);
}

// Steering a point toward a target at a fixed speed, then aiming with det_* trig.
void vectors() {
    std::printf("-- vectors\n");
    Vec2 pos{0, 0};
    const Vec2 target{30, 40};
    for (int tick = 1; tick <= 3; ++tick) {
        pos = approach(pos, target, 20.0f); // 20 px per tick, never overshoots
        std::printf("tick %d: (%.1f, %.1f)\n", tick, pos.x, pos.y);
    }
    const Vec2 to_mouse = Vec2{-3, 3} - pos;
    std::printf("aim angle %.3f rad, unit (%.3f, %.3f)\n", angle_of(to_mouse),
                normalize_or(to_mouse).x, normalize_or(to_mouse).y);
    const Transform2D parent{{100, 50}, kPi * 0.5f, {2, 2}};
    const Transform2D child = combine(parent, Transform2D{{8, 0}, 0.0f, {1, 1}});
    std::printf("child world position (%.1f, %.1f)\n", child.position.x, child.position.y);
}

// A bullet path against a crate, and a rolling ball against the same crate.
void shapes() {
    std::printf("-- shapes\n");
    const Rect crate{100, 100, 32, 32};
    if (const auto hit = intersect(Segment{{50, 116}, {200, 116}}, crate))
        std::printf("bullet hits at (%.1f, %.1f), normal (%.0f, %.0f), t=%.3f\n", hit->point.x,
                    hit->point.y, hit->normal.x, hit->normal.y, hit->t);
    if (const auto hit = sweep_circle_rect({60, 60}, {116, 116}, 6.0f, crate))
        std::printf("ball touches the corner at (%.2f, %.2f)\n", hit->point.x, hit->point.y);
    std::printf("player rect overlaps crate: %s\n",
                Rect{120, 90, 16, 16}.overlaps(crate) ? "yes" : "no");
    std::printf("guard sees player: %s\n",
                point_in_cone({0, 0}, {1, 0}, 200.0f, kPi / 6, {150, 40}) ? "yes" : "no");
}

// A frame-time UI tween, a fader and the day/night ambient curve.
void tweens() {
    std::printf("-- tweens and curves\n");
    FloatTween slide;
    slide.start(-100.0f, 0.0f, 0.5f, Ease::OutBack);
    for (int frame = 0; frame < 4; ++frame) {
        slide.update(1.0f / 8.0f);
        std::printf("menu x after %d frames: %.1f\n", frame + 1, slide.value());
    }
    Fader fade;
    fade.set_target(1.0f, 2.0f);
    fade.update(0.25f);
    std::printf("fade %.2f, ease '%.*s'\n", fade.value(),
                static_cast<int>(ease_name(Ease::OutBack).size()), ease_name(Ease::OutBack).data());
    ColorCurve day;
    day.add(0.0f, Color{20, 20, 60, 255});    // midnight
    day.add(0.5f, Color{255, 250, 230, 255}); // noon
    for (float t : {0.0f, 0.25f, 0.5f, 0.9f}) {
        const Color c = day.sample(t);
        std::printf("ambient at %.2f: #%02x%02x%02x\n", t, c.r, c.g, c.b);
    }
}

// A small walkability grid; outside the map counts as a wall.
void grid() {
    std::printf("-- grid\n");
    Grid<std::uint8_t> walls(6, 3, 0);
    for (int x = 0; x < 6; ++x)
        walls.set(x, 1, x == 3 ? 0 : 1);
    for (int y = -1; y <= 3; ++y) {
        for (int x = -1; x <= 6; ++x)
            std::printf("%c", walls.get_or(x, y, 1) ? '#' : '.');
        std::printf("\n");
    }
}

} // namespace

int main(int argc, char **argv) {
    const std::uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 42;
    rng_streams(seed);
    vectors();
    shapes();
    tweens();
    grid();
    return 0;
}
