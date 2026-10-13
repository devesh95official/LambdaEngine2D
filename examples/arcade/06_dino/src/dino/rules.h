#pragma once
// Dino Runner rules. Jump cacti and low birds, duck under mid birds; the scroll speeds up every
// tick. State is the DinoState resource plus the Runner and Obstacle components.
#include <cstdint>
#include <span>
#include <string_view>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/math/rect.h>
#include <lam/particles/particles.h>

namespace dino {

enum class Phase : std::uint8_t { Title, Play, Over };
enum class Kind : std::uint8_t { SmallCactus, BigCactus, Cluster, BirdLow, BirdMid, BirdHigh };

struct Tune {
    float ground_y = 214;                                       // world y of the ground line
    float runner_x = 72;
    float gravity = 0.55f;                                      // px / tick^2
    float jump_v = 8.8f;                                        // px / tick
    float cut_v = 3.2f;                                         // rise cap when jump is let go early
    float fast_fall = 0.9f;                                     // extra gravity while Down is held in the air
    float start_speed = 4.0f;
    float max_speed = 10.5f;
    float accel = 0.0012f;
    std::int32_t restart_delay = 36;                            // ticks on the game-over screen before restart
    std::int32_t birds_from = 250;                              // score at which birds start
};
inline constexpr Tune kTune{};
inline constexpr float kVirtualW = 480, kVirtualH = 270;

struct Runner {                                                 // on the runner entity (Transform = feet)
    float vy = 0;
    bool grounded = true;
    bool ducking = false;
    bool jump_held = false;
    bool fast_fall = false;
    std::int32_t air_ticks = 0;
};
struct Obstacle {                                               // Transform = bottom centre
    Kind kind = Kind::SmallCactus;
    float w = 10, h = 20;
    std::int32_t variant = 0;                                   // cactus arm layout, drawing only
};

struct DinoState {                                              // snapshotted World resource
    Phase phase = Phase::Title;
    lam::Entity runner;
    float speed = kTune.start_speed;
    float distance = 0;
    float next_spawn = 300;
    std::int32_t score = 0, best = 0, last_score = 0, runs = 0;
    std::int32_t ticks_in_phase = 0;
    std::uint32_t play_ticks = 0;
};

struct Controls { lam::ButtonAction jump, duck, start; };      // App resource
struct Fx { lam::AssetId<lam::EmitterConfig> dust, crash, kick; };   // App resource, filled by main.cpp

lam::Rect runner_box(lam::Vec2 feet, const Runner &r);         // world rect
lam::Rect obstacle_box(lam::Vec2 bottom, const Obstacle &o);

void setup_world(lam::World &w);                               // DinoState and the runner

// --bot: bot_decide picks the input, DinoBot presses it.
struct BotChoice { bool jump = false, duck = false, start = false; };
BotChoice bot_decide(lam::World &w);
class DinoBot final : public lam::InputDriver {
public:
    Controls controls;
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
private:
    bool was_jump_ = false, was_start_ = false, was_duck_ = false;
};

class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "game.dino.rules"; }
    void build(lam::App &app) override;
};

} // namespace dino

LAM_REFLECT_ENUM(dino::Phase, "title", "play", "over")
LAM_REFLECT_ENUM(dino::Kind, "small_cactus", "big_cactus", "cluster", "bird_low", "bird_mid", "bird_high")
LAM_REFLECT(dino::Runner, vy, grounded, ducking, jump_held, fast_fall, air_ticks)
LAM_REFLECT(dino::Obstacle, kind, w, h, variant)
LAM_REFLECT_RESOURCE(dino::DinoState, phase, runner, speed, distance, next_spawn, score, best, last_score, runs,
                     ticks_in_phase, play_ticks)
