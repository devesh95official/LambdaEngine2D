#pragma once
// Apple Trail presentation. Board, apples and trail are drawn straight into the RenderQueue from
// interpolated Transforms; the traveller is an ordinary Sprite + Animator.
#include <cstdint>
#include <string_view>
#include <lam/animation/sprite_sheet.h>
#include <lam/core/app.h>
#include <lam/math/color.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include "trail_map.h"

namespace game {

inline constexpr lam::IVec2 kVirtual{480, 270};                 // pixel-art target (the view size)
inline constexpr int kHudHeight = 22;                            // top bar, screen space
inline constexpr lam::Color kHudBg{40, 32, 28, 255};
inline constexpr lam::Color kHudInk{246, 232, 196, 255};
inline constexpr lam::Color kGold{240, 196, 64, 255};
inline constexpr lam::Color kBump{255, 120, 100, 255};

// Loaded once at startup (root asset scope), shared by every run (App resource).
struct GameArt {
    trail::Orchard orchard;
    lam::AssetId<lam::SpriteSheet> traveller;
};

// App resource kept across runs: best score and the headless summary numbers.
struct Session {
    int runs = 0;
    int best = 0;
    int last_score = 0;
    int last_apples = 0;                                        // apples eaten in the last run
    int apples = 0;                                             // over all finished runs
    int crashes = 0;                                            // finished runs
    int total_score = 0;                                        // over all finished runs
    std::uint64_t last_hash = 0;                                // play world state_hash, last tick
    std::uint64_t ticks = 0;                                    // app ticks seen by the play world
};

// Draws `text` with the engine's built-in font, centred on x when `centred`. No allocation.
void draw_label(const lam::Ctx &ctx, std::string_view text, lam::Vec2 pos, float size, lam::Color color,
                bool centred = false);
// A dim rectangle over the whole screen below the HUD (overlay scenes).
void draw_dim(const lam::Ctx &ctx, lam::Color color);

// Frame and tick systems of the play world, gated with run_if = has_local<TrailState>.
class PresentPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "game.apple_trail.present"; }
    void build(lam::App &app) override;
};

// Gives the traveller (the run's head entity) its sprite, shadow and animator, and spawns the
// camera centred on the board below the HUD. Called by the play scene after trail::start_run.
void dress_run(lam::App &app, lam::World &w);

} // namespace game
