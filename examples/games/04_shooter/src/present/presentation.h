#pragma once
// Shooter presentation (executable only): fx and sound assets, HUD, camera, sprite looks. Reads sim
// state and events; the only way back into the rules is an intent.
#include <cstdint>
#include <string_view>
#include <vector>
#include <lam/audio/components.h>
#include <lam/core/app.h>
#include <lam/ecs/world.h>
#include <lam/particles/components.h>
#include <lam/ui/ui.h>
#include <shooter/arena.h>
namespace shooter_app {

// Game flags (App resource), read from LaunchOptions::extra.
struct GameFlags {
    shooter::ArenaConfig arena;                                 // --enemies N, --god
    bool bot = false;                                           // --bot: ShooterBot drives player 0
};
GameFlags parse_flags(const lam::LaunchOptions &options);

// App resource: the arena scene's world (set in ArenaScene::enter), for the exit summary.
struct Session {
    lam::World *arena = nullptr;
};

// App resource: --bench frame stats (real frame time, draw calls, quads, live enemies), printed at
// exit. The first kBenchWarmup frames are skipped.
struct Bench {
    static constexpr std::uint64_t kBenchWarmup = 120;
    bool on = false;
    std::vector<float> frame_ms;                                // reserved at startup (cold path)
    double last_real = -1;                                      // Time::real_seconds at the last sample
    double enemies_sum = 0, draw_calls_sum = 0, quads_sum = 0;
    std::size_t enemies_max = 0;
    double busy_ms_sum = 0;                                     // frames with enemies alive
    std::size_t busy_frames = 0;
};
// One "shooter bench:" line (frames, avg/p50/p99/max ms, fps, enemies, draw calls, quads) and
// busy_* = the frames with enemies alive (real-time runs spend their first frames in the
// intermission before wave 1).
void print_bench(lam::App &app);

// App resource: fx state the rules never see.
struct PresentFx {
    lam::AssetId<lam::EmitterConfig> sparkle;                   // continuous emitter on pickups
    lam::VoiceHandle alarm;                                     // looping low-HP heartbeat voice
};

// World resource: HUD label ids in this world's UiTree.
struct Hud {
    lam::UiId score = 0, wave = 0, hp = 0, enemies = 0, power = 0;
};
// Builds the HUD panels in `w`'s UiTree and inserts Hud.
void build_hud(lam::World &w);

// FX emitter configs, the sound bank, and every presentation/flow system.
class ShooterPresentationPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "game.shooter.present"; }
    void build(lam::App &app) override;
};
}
