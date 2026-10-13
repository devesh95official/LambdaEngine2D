#pragma once
// Apple Trail simulation data: components, the run resource and events. Rule state is reflected or
// in the snapshotted TrailState, so the hash, undo and saves cover a run.
#include <array>
#include <cstdint>
#include <lam/core/asset_id.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/vec2.h>
#include <lam/particles/components.h>

namespace trail {

enum class Dir : std::uint8_t { Up, Down, Left, Right };
enum class AppleKind : std::uint8_t { Red, Golden };

// The pace and the points (constants, so not part of the state).
struct Rules {
    int start_step_ticks = 9;                                   // ticks per tile at the start
    int min_step_ticks = 4;
    int apples_per_speedup = 4;
    int start_length = 3;                                       // trail apples at the start
    int golden_every = 5;                                       // a golden apple after every N red ones
    int golden_ticks = 480;                                     // how long it stays (8 s at 60 Hz)
    int golden_warn_ticks = 120;                                // presentation blinks it in the last 2 s
    int red_points = 10;
    int golden_points = 50;
    int golden_growth = 2;
};
inline constexpr Rules kRules{};
inline constexpr int kTileSize = 16;                            // pixels per orchard tile

// One piece of the trail. The traveller is index 0 (the head); `behind` links to the next piece
// toward the tail, so a step walks the list head -> tail without sorting a pool.
struct Segment {
    std::uint16_t index = 0;                                    // 0 = traveller, 1.. = trail apples
    lam::Entity behind;                                         // null at the tail
};
// An apple lying in the orchard (has GridPosition{blocks = false}).
struct Apple {
    AppleKind kind = AppleKind::Red;
    std::int32_t ticks_left = 0;                                // golden apples rot at 0; red: 0 = forever
};

// The run (World resource, snapshotted).
struct TrailState {
    lam::Entity head;                                           // Segment 0
    lam::Entity tail;                                           // last Segment
    Dir heading = Dir::Right;
    std::array<Dir, 2> queued{};                                // two-deep turn buffer
    std::uint8_t queued_count = 0;
    std::int32_t step_ticks = kRules.start_step_ticks;          // ticks per tile now
    std::int32_t tick_in_step = 0;
    std::int32_t grow = 0;                                      // pieces still to add at the tail
    std::int32_t reds_since_golden = 0;
    std::int32_t score = 0;
    std::int32_t eaten = 0;
    std::int32_t length = 0;                                    // trail apples (segments - 1)
    std::uint32_t steps = 0;                                    // tiles walked this run
    bool alive = true;
};

// What happened on a tick, for presentation and scene flow (World events, emitted in Simulate).
struct AppleEaten { lam::IVec2 cell; AppleKind kind = AppleKind::Red; };
struct TrailCrashed { lam::IVec2 cell; };
struct GoldenAppeared { lam::IVec2 cell; };
// Camera shake is the engine's lam::ShakeAt (render/camera_math.h), emitted like BurstAt.

// Particle configs for BurstAt (App resource, inserted by the presentation layer). The rules emit
// BurstAt only when this resource exists, so headless tests need no particles plugin.
struct TrailFx {
    lam::AssetId<lam::EmitterConfig> red_burst;
    lam::AssetId<lam::EmitterConfig> golden_burst;
    lam::AssetId<lam::EmitterConfig> dust;
};

} // namespace trail

LAM_REFLECT_ENUM(trail::Dir, "up", "down", "left", "right")
LAM_REFLECT_ENUM(trail::AppleKind, "red", "golden")
LAM_REFLECT(trail::Segment, index, behind)
LAM_REFLECT(trail::Apple, kind, ticks_left)
LAM_REFLECT_RESOURCE(trail::TrailState, head, tail, heading, queued, queued_count, step_ticks, tick_in_step, grow,
            reds_since_golden, score, eaten, length, steps, alive)
