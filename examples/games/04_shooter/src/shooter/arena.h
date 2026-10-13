#pragma once
// Arena layout, per-world game state and setup. The rules build TileWorld from the rows below;
// tools/gen_assets.py parses them to write the Tiled map the executable only draws.
#include <array>
#include <cstdint>
#include <string_view>
#include <lam/core/app.h>
#include <lam/ecs/component.h>
#include <lam/input/input_frame.h>
#include <lam/math/vec2.h>
namespace shooter {

inline constexpr int kTileSize = 16;
inline constexpr int kArenaW = 48;
inline constexpr int kArenaH = 30;
// '#' wall, '.' floor, 'S' enemy spawn pad (floor), 'P' player start (floor).
// ARENA-LAYOUT-BEGIN (parsed by tools/gen_assets.py)
inline constexpr std::array<std::string_view, kArenaH> kArenaRows = {
    "################################################",
    "#..............................................#",
    "#.S..........................................S.#",
    "#..............................................#",
    "#.......###..........................###.......#",
    "#.......###..........................###.......#",
    "#..............................................#",
    "#..............................................#",
    "#...................##......##.................#",
    "#...................##......##.................#",
    "#..............................................#",
    "#.....#....................................#...#",
    "#.....#....................................#...#",
    "#.....#....................................#...#",
    "#.S...#..............P.....................#.S.#",
    "#.....#....................................#...#",
    "#.....#....................................#...#",
    "#.....#....................................#...#",
    "#..............................................#",
    "#...................##......##.................#",
    "#...................##......##.................#",
    "#..............................................#",
    "#..............................................#",
    "#.......###..........................###.......#",
    "#.......###..........................###.......#",
    "#..............................................#",
    "#..............................................#",
    "#.S..........................S...............S.#",
    "#..............................................#",
    "################################################",
};
// ARENA-LAYOUT-END

// Game phase. Intermission counts down to the next wave; GameOver freezes the rules until the
// "shooter.restart" intent arrives.
enum class Phase : std::uint8_t { Intermission, Fighting, GameOver };

// Per-world game state, snapshotted (undo, saves, state hash).
struct Arena {
    Phase phase = Phase::Intermission;
    std::uint32_t wave = 0;
    std::uint32_t phase_ticks = 90;                             // ticks left in this phase step
    std::uint32_t to_spawn = 0;                                 // enemies of this wave to spawn
    std::uint32_t spawn_cursor = 0;                             // round-robin spawn pad index
    std::int64_t score = 0;
    std::uint32_t kills = 0;
    // Session counters: kept across restarts.
    std::uint32_t runs = 1;
    std::uint32_t deaths = 0;
    std::int64_t best = 0;
    std::uint32_t total_kills = 0;
    // Config copied in at setup (so saves and replays carry it).
    std::uint32_t stress_enemies = 0;                           // --enemies N: every wave has N
    bool god = false;                                           // stress mode: cannot die
};

// Launch-time configuration (executable flags; tests set it directly).
struct ArenaConfig {
    std::uint32_t stress_enemies = 0;                           // 0 = normal waves
    bool god = false;
};

// Action ids, resolved once from the ActionMap in ShooterSimPlugin::build (App resource).
struct Controls {
    lam::AxisAction move;                                       // WASD / left stick
    lam::AxisAction aim;                                        // arrows / right stick (twin-stick)
    lam::ButtonAction fire;                                     // mouse left / Space / RB
    lam::ButtonAction pause;                                    // Escape / Start (exe flow)
};

// Intent kinds (UI -> simulation, delivered in the tick InputFrame and recorded).
inline constexpr lam::StrId kIntentRestart = lam::str_id("shooter.restart");

// Wave size for wave `w` (1-based); `stress` overrides it.
std::uint32_t wave_size(std::uint32_t w, std::uint32_t stress);
// Pixel centre of tile (x, y).
lam::Vec2 tile_center(int x, int y);
// The 'P' cell centre.
lam::Vec2 player_start();
// Fills the world's TileWorld (shapes, opaque flags, path costs) from kArenaRows.
void build_arena_tiles(lam::World &w);
// Inserts Arena (snapshotted), Crowd and the TileWorld, and spawns the "player" prefab. Call once
// per world. Returns the player.
lam::Entity setup_arena(lam::App &app, lam::World &w, const ArenaConfig &config);
// The player entity, or null.
lam::Entity find_player(lam::World &w);
}
LAM_REFLECT_ENUM(shooter::Phase, "intermission", "fighting", "game_over")
LAM_REFLECT(shooter::Arena, phase, wave, phase_ticks, to_spawn, spawn_cursor, score, kills, runs,
            deaths, best, total_kills, stress_enemies, god)
