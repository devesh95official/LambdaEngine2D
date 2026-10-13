#pragma once
// Lantern Run presentation (executable only):
//   startup                      camera (follow + look-ahead + bounds + shake), ground map, night
//   PostPhysics 500 (tick)       game events -> BurstAt (the particles plugin draws them)
//   Update        0 (frame)      camera retarget, shake trauma, wave banner, pause
//   Update       10 (frame)      wind-up flash, hurt blink, lantern lights on/off
//   Render        0 (frame)      warm additive glows over the night (draw_glows)
//   RenderUI      0 (frame)      HUD and title / paused / game-over overlays
#include <string_view>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>

namespace lam {
struct EmitterConfig;
struct SpriteSheet;
}

namespace lantern {

struct FxAssets {                                               // App resource: runtime-made emitter configs
    lam::AssetId<lam::EmitterConfig> sparks, dust, hit;
    lam::AssetId<lam::SpriteSheet> world_sheet;                 // its "diya_glow" frame (draw_glows)
};

struct HudState {                                               // App resource, frame time (never hashed)
    float wave_banner = 0;                                      // seconds left on screen
    float time = 0;                                             // seconds of unpaused play (blinks, flashes)
};

class LanternRunViewPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "lantern_run.view"; }
    void build(lam::App &app) override;
};

void draw_hud(lam::Ctx &ctx);                                   // hud.cpp

} // namespace lantern
