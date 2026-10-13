#pragma once
// Missile Command rules. State is the Enemy/Counter/Blast components plus the Run resource.
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>
#include <lam/ecs/component.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/math/vec2.h>
#include <lam/particles/components.h>
namespace game {

inline constexpr float kW = 480, kH = 270;                       // screen and world pixels match
inline constexpr float kGround = 252;                            // top of the ground strip
inline constexpr int kCities = 6, kBases = 3;
inline constexpr std::array<float, kCities> kCityX = {92, 134, 176, 304, 346, 388};
inline constexpr std::array<float, kBases> kBaseX = {36, 240, 444};
inline constexpr float kBaseY = 240;                             // muzzle of each base
inline constexpr std::int32_t kAmmoPerBase = 10;

enum Phase : std::uint8_t { kTitle = 0, kPlay = 1, kBonus = 2, kOver = 3 };

// Incoming warhead, moving from `from` to `to` at `speed` px per tick.
struct Enemy {
    lam::Vec2 from, pos, to;
    float speed = 0.5f;
    float split_y = 0;                                           // 0 = never splits
};
// Counter-missile from a base to the crosshair; becomes a Blast on arrival.
struct Counter {
    lam::Vec2 from, pos, to;
};
// Grows then shrinks. Player blasts destroy warheads inside them.
struct Blast {
    lam::Vec2 pos;
    std::uint16_t age = 0;
    bool hostile = false;                                        // enemy impact: red, kills nothing
};

struct Run {
    std::uint8_t phase = kTitle;
    std::int32_t score = 0, best = 0, wave = 0;
    std::uint32_t cities = 0x3F;                                 // bit i = city i alive
    std::int32_t ammo0 = 0, ammo1 = 0, ammo2 = 0;               // 0 = base empty or destroyed
    std::int32_t to_spawn = 0;                                   // warheads left this wave
    std::uint32_t timer = 0;                                     // spawn / phase timer (ticks)
    std::int32_t bonus = 0;                                      // last wave bonus (HUD)
    std::uint32_t kills = 0;
    lam::Vec2 cross{240, 130}, last_pointer{-1, -1};
};

inline constexpr std::uint16_t kBlastGrow = 22, kBlastHold = 14, kBlastShrink = 22;
inline constexpr float kBlastMax = 22;
float blast_radius(const Blast &b);
std::int32_t &ammo(Run &r, int base);
std::int32_t ammo(const Run &r, int base);
int city_count(const Run &r);

struct Controls {
    lam::ButtonAction fire, b1, b2, b3;
    lam::AxisAction move;
};
struct Fx { lam::AssetId<lam::EmitterConfig> boom, debris; };    // App resource, filled by main.cpp

class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "missile.rules"; }
    void build(lam::App &app) override;
};
void start(lam::World &w);                                       // adds Run on the title

class Bot final : public lam::InputDriver {
public:
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
};
}
LAM_REFLECT(game::Enemy, from, pos, to, speed, split_y)
LAM_REFLECT(game::Counter, from, pos, to)
LAM_REFLECT(game::Blast, pos, age, hostile)
LAM_REFLECT(game::Run, phase, score, best, wave, cities, ammo0, ammo1, ammo2, to_spawn, timer,
            bonus, kills, cross, last_pointer)
