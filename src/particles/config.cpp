// Config validation and weather presets. Every config goes through sanitize() before a pool
// stores it; the old engine trusted its inputs and a NaN could blow up the drop count.
#include <cmath>
#include <cstdint>

#include <lam/math/scalar.h>
#include <lam/particles/particles.h>
#include <lam/render/render_queue.h>

namespace lam {
namespace {

constexpr float kMaxSpeed = 100000.0f;     // px/s
constexpr float kMaxLife = 3600.0f;        // s
constexpr float kMinLife = 0.001f;         // s
constexpr float kMaxSize = 4096.0f;        // px
constexpr float kMaxRate = 1000000.0f;     // particles/s
constexpr float kMaxAccel = 1000000.0f;    // px/s^2
constexpr float kMaxDrag = 1000.0f;        // 1/s
constexpr float kMaxCoord = 1000000.0f;    // px (spawn areas, texel rects)
constexpr float kMaxAngle = 1000.0f;       // rad (angles are wrapped afterwards)
constexpr float kMaxSpin = 1000.0f;        // rad/s
constexpr float kMaxStretch = 10.0f;       // s
constexpr float kMaxSway = 10000.0f;       // px
constexpr float kMaxSwayHz = 100.0f;       // Hz
constexpr float kMaxJitter = 100000.0f;    // px/s/sqrt(s)
constexpr float kMaxDepth = 524288.0f;     // px (the sort key's depth range)
constexpr float kMaxPrewarm = 60.0f;       // s

// Finite v clamped to [lo, hi]; non-finite -> fallback (clamped too).
float fix(float v, float fallback, float lo, float hi) {
    if (!std::isfinite(v)) v = fallback;
    return clamp(v, lo, hi);
}

// Sorts a [min, max) pair after fixing both ends.
void fix_range(float &lo, float &hi, float def_lo, float def_hi, float min, float max) {
    lo = fix(lo, def_lo, min, max);
    hi = fix(hi, def_hi, min, max);
    if (hi < lo) {
        const float t = lo;
        lo = hi;
        hi = t;
    }
}

Vec2 fix_vec(Vec2 v, float limit) {
    return {fix(v.x, 0.0f, -limit, limit), fix(v.y, 0.0f, -limit, limit)};
}

// A finite rect with non-negative size (negative sizes flip to the other side).
Rect fix_rect(Rect r) {
    r.x = fix(r.x, 0.0f, -kMaxCoord, kMaxCoord);
    r.y = fix(r.y, 0.0f, -kMaxCoord, kMaxCoord);
    r.w = fix(r.w, 0.0f, -kMaxCoord, kMaxCoord);
    r.h = fix(r.h, 0.0f, -kMaxCoord, kMaxCoord);
    if (r.w < 0.0f) {
        r.x += r.w;
        r.w = -r.w;
    }
    if (r.h < 0.0f) {
        r.y += r.h;
        r.h = -r.h;
    }
    return r;
}

// Clamped preset intensity: NaN and negatives -> 0, above 1 (+inf too) -> 1.
float fix_intensity(float intensity) {
    return saturate(intensity); // clamp() maps NaN to the lower bound
}

// Non-finite or tiny screen sizes fall back to the default virtual screen.
Vec2 fix_screen(Vec2 s) {
    if (!std::isfinite(s.x) || !std::isfinite(s.y) || s.x < 16.0f || s.y < 16.0f) return {480, 270};
    return {clamp(s.x, 16.0f, 16384.0f), clamp(s.y, 16.0f, 16384.0f)};
}

} // namespace

EmitterConfig sanitize(const EmitterConfig &in) {
    const EmitterConfig def{};
    EmitterConfig c = in;
    c.capacity = c.capacity == 0 ? 1u : (c.capacity > kMaxParticlesPerPool ? kMaxParticlesPerPool : c.capacity);
    c.rate = fix(c.rate, 0.0f, 0.0f, kMaxRate);
    if (c.burst > c.capacity) c.burst = static_cast<std::uint16_t>(c.capacity);
    fix_range(c.speed_min, c.speed_max, def.speed_min, def.speed_max, 0.0f, kMaxSpeed);
    c.angle = wrap_angle(fix(c.angle, 0.0f, -kMaxAngle, kMaxAngle));
    c.spread = fix(c.spread, def.spread, 0.0f, kTau);
    fix_range(c.life_min, c.life_max, def.life_min, def.life_max, kMinLife, kMaxLife);
    c.size_start = fix(c.size_start, def.size_start, 0.0f, kMaxSize);
    c.size_end = fix(c.size_end, def.size_end, 0.0f, kMaxSize);
    c.gravity = fix_vec(c.gravity, kMaxAccel);
    c.drag = fix(c.drag, 0.0f, 0.0f, kMaxDrag);
    // Region: texel rects must be finite; a texture without a source uses its frame size, and a
    // region that still has no source falls back to the built-in circle (texture none).
    c.region.source = fix_rect(c.region.source);
    c.region.size = {fix(c.region.size.x, 0.0f, 0.0f, kMaxCoord), fix(c.region.size.y, 0.0f, 0.0f, kMaxCoord)};
    c.region.trim_offset = fix_vec(c.region.trim_offset, kMaxCoord);
    if (c.region.texture && c.region.source.empty()) {
        if (c.region.size.x > 0.0f && c.region.size.y > 0.0f)
            c.region.source = {0, 0, c.region.size.x, c.region.size.y};
        else
            c.region = {};
    }
    if (c.material >= kMaxMaterials) c.material = 0;
    c.spawn_area = fix_rect(c.spawn_area);
    if (static_cast<std::uint8_t>(c.spawn_shape) > static_cast<std::uint8_t>(SpawnShape::Ring))
        c.spawn_shape = SpawnShape::Rect;
    fix_range(c.rotation_min, c.rotation_max, 0.0f, 0.0f, -kMaxAngle, kMaxAngle);
    fix_range(c.spin_min, c.spin_max, 0.0f, 0.0f, -kMaxSpin, kMaxSpin);
    c.stretch = fix(c.stretch, 0.0f, 0.0f, kMaxStretch);
    c.sway = fix(c.sway, 0.0f, 0.0f, kMaxSway);
    c.sway_hz = fix(c.sway_hz, 0.0f, 0.0f, kMaxSwayHz);
    c.jitter = fix(c.jitter, 0.0f, 0.0f, kMaxJitter);
    c.depth = fix(c.depth, 0.0f, -kMaxDepth, kMaxDepth);
    c.prewarm = fix(c.prewarm, 0.0f, 0.0f, kMaxPrewarm);
    return c;
}

void burst(ParticleEmitter &e, std::uint16_t count) {
    const std::uint32_t sum = std::uint32_t{e.pending_burst} + count;
    e.pending_burst = static_cast<std::uint16_t>(sum > 65535u ? 65535u : sum);
}

EmitterConfig rain_preset(float intensity) {
    return rain_preset(intensity, {480, 270});
}

EmitterConfig snow_preset(float intensity) {
    return snow_preset(intensity, {480, 270});
}

EmitterConfig rain_preset(float intensity, Vec2 screen_size) {
    const float k = fix_intensity(intensity);
    const Vec2 s = fix_screen(screen_size);
    // Old RainField: 220 drops at full intensity, 260 px/s down, -40 px/s wind, 6 px streaks,
    // landing anywhere from 15% of the view height to just below it.
    constexpr float kFall = 260.0f, kWind = -40.0f, kMaxDrops = 220.0f;
    const float fall_seconds = (s.y + 24.0f) / kFall;
    EmitterConfig c;
    c.capacity = 256;
    c.life_min = fall_seconds * 0.25f;
    c.life_max = fall_seconds * 1.05f;
    // Steady state: alive = rate * mean life, so full intensity keeps kMaxDrops drops.
    c.rate = k * kMaxDrops / ((c.life_min + c.life_max) * 0.5f);
    c.speed_min = kFall * 0.92f;
    c.speed_max = kFall * 1.12f;
    c.angle = det_atan2(kFall, kWind);
    c.spread = 0.05f;
    c.size_start = c.size_end = 1;
    c.stretch = 0.02f;                                          // ~5 px streak at 260 px/s
    c.color_start = Color{170, 190, 230, 170};
    c.color_end = Color{170, 190, 230, 120};
    c.region = TextureRegion{.texture = {}, .source = kWhiteSource, .size = {}, .trim_offset = {}};
    c.layer = kLayerScreenFx;
    // Wind pushes drops sideways while they fall, so spawn across a band that reaches upwind.
    const float slack = -kWind * fall_seconds + 8.0f;
    c.spawn_area = {-8.0f, -24.0f, s.x + slack + 8.0f, 16.0f};
    c.prewarm = fall_seconds;                                   // a level that starts in the rain
    return c;
}

EmitterConfig snow_preset(float intensity, Vec2 screen_size) {
    const float k = fix_intensity(intensity);
    const Vec2 s = fix_screen(screen_size);
    constexpr float kFall = 28.0f, kMaxFlakes = 300.0f;
    const float fall_seconds = (s.y + 16.0f) / kFall;
    EmitterConfig c;
    c.capacity = 512;
    c.life_min = fall_seconds * 0.35f;
    c.life_max = fall_seconds * 1.05f;
    c.rate = k * kMaxFlakes / ((c.life_min + c.life_max) * 0.5f);
    c.speed_min = kFall * 0.85f;
    c.speed_max = kFall * 1.15f;
    c.angle = kPi * 0.5f;
    c.spread = 0.4f;
    c.size_start = 2;
    c.size_end = 1.5f;
    c.color_start = Color{255, 255, 255, 230};
    c.color_end = Color{255, 255, 255, 140};
    c.sway = 6;
    c.sway_hz = 0.3f;
    c.jitter = 1.5f;
    c.layer = kLayerScreenFx;
    c.spawn_area = {-16.0f, -12.0f, s.x + 32.0f, 8.0f};
    c.prewarm = fall_seconds;
    return c;
}

} // namespace lam
