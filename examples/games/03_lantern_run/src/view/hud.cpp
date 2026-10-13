// HUD and overlays, pushed into the RenderQueue in screen space (kLayerUi, virtual pixels) with
// the built-in font (RenderDefaults). Numbers are formatted into stack buffers, so the HUD does
// not allocate per frame.
#include <cstdio>
#include <string_view>
#include <lam/assets/asset_server.h>
#include <lam/ecs/world.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include <lam/render/virtual_screen.h>

#include "sim/components.h"
#include "sim/tuning.h"
#include "view/view.h"

namespace lantern {

namespace {

constexpr lam::Color kInk{246, 232, 196, 255};
constexpr lam::Color kGold{240, 196, 64, 255};
constexpr lam::Color kBar{16, 18, 30, 200};
constexpr lam::Color kDim{0, 0, 0, 130};

struct Painter {
    lam::RenderQueue &q;
    const lam::Font &font;
    float width;

    void text(std::string_view s, float x, float y, lam::Color c, float size = 0) const {
        lam::TextStyle style;
        style.color = c;
        style.size = size;
        style.layer = lam::kLayerUi;
        lam::draw_text(q, font, s, {x, y}, style);
    }
    void centred(std::string_view s, float y, lam::Color c, float size = 0) const {
        const float w = lam::measure_text(font, s, size).x;
        text(s, (width - w) * 0.5f, y, c, size);
    }
};

void hearts(const Painter &p, int have, int max) {
    for (int i = 0; i < max; ++i) {
        const lam::Color c = i < have ? lam::Color{220, 50, 60, 255} : lam::Color{70, 50, 60, 255};
        const float x = 8.0f + static_cast<float>(i) * 12.0f;
        p.q.circle({x + 2, 6}, 3, c, lam::kLayerUi);
        p.q.circle({x + 6, 6}, 3, c, lam::kLayerUi);
        p.q.circle({x + 4, 9}, 3, c, lam::kLayerUi);           // the heart's tip, rounded
    }
}

} // namespace

void draw_hud(lam::Ctx &ctx) {
    lam::RenderQueue *q = ctx.try_res<lam::RenderQueue>();
    const lam::AssetServer *assets = ctx.try_res<lam::AssetServer>();
    const lam::RenderDefaults *defaults = ctx.try_res<lam::RenderDefaults>();
    const RunState *run = ctx.try_local<RunState>();
    if (q == nullptr || assets == nullptr || defaults == nullptr || run == nullptr) return;
    const lam::Font *font = assets->get<lam::Font>(defaults->font);
    if (font == nullptr) return;
    const lam::VirtualLayout *layout = ctx.try_res<lam::VirtualLayout>();
    const lam::IVec2 view = layout != nullptr ? layout->virtual_size : ctx.app().config().virtual_size;
    const float w = static_cast<float>(view.x);
    const float h = static_cast<float>(view.y);
    Painter p{*q, *font, w};
    const Tuning &t = ctx.res<Tuning>();
    const HudState &hud = ctx.res<HudState>();

    lam::World &world = ctx.world();
    const Player *player = world.single_or_null<Player>();
    char buf[96];

    q->rect({0, 0, w, 16}, kBar, lam::kLayerUi);
    hearts(p, player != nullptr ? player->hearts : 0, t.hearts);
    std::snprintf(buf, sizeof buf, "LANTERNS %d", run->collected);
    p.text(buf, 52, 4, kGold);
    std::snprintf(buf, sizeof buf, "WAVE %d", run->wave);
    p.text(buf, 150, 4, kInk);
    std::snprintf(buf, sizeof buf, "BEST %d", run->best);
    p.text(buf, w - 60, 4, kInk);
    // The dash: a bar that fills while it recovers.
    if (player != nullptr) {
        const float ready = static_cast<float>(t.dash_cooldown - player->dash_cooldown) / static_cast<float>(t.dash_cooldown);
        q->rect({w - 120, 5, 40, 5}, {60, 60, 70, 255}, lam::kLayerUi);
        q->rect({w - 120, 5, 40 * ready, 5},
                player->dash_cooldown == 0 ? lam::Color{120, 200, 255, 255} : lam::Color{90, 120, 150, 255},
                lam::kLayerUi);
    }
    if (hud.wave_banner > 0.0f) {
        std::snprintf(buf, sizeof buf, "WAVE %d  -  another wolf joins the hunt", run->wave);
        p.centred(buf, 30, kGold);
    }

    const bool paused = ctx.app().paused();
    if (run->phase == Phase::Playing && !paused) return;
    q->rect({0, 16, w, h}, kDim, lam::kLayerUi);
    const float cy = h * 0.5f - 24.0f;
    if (paused) {
        p.centred("PAUSED", cy, kInk, 16);
    } else if (run->phase == Phase::Title) {
        p.centred("LANTERN RUN", cy - 8, kGold, 16);
        p.centred("Gather the lanterns lit in the forest.", cy + 20, kInk);
        p.centred("Wolves glow red before they lunge: dash through!", cy + 32, kInk);
        p.centred("WASD move  -  SPACE dash  -  ENTER start", cy + 52, kInk);
    } else {
        p.centred("THE WOLVES GOT YOU", cy - 4, {255, 120, 100, 255}, 16);
        std::snprintf(buf, sizeof buf, "Lanterns %d   Wave %d", run->collected, run->wave);
        p.centred(buf, cy + 22, kInk);
        p.centred("ENTER to run again", cy + 38, kInk);
    }
}

} // namespace lantern
