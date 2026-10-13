// UiPlugin and its systems. Every other module's resource is optional (try_res), so headless
// tests can install only what they need.
#include <cmath>
#include <cstdint>
#include <span>

#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/core/app.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/input/keys.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include <lam/render/sprite.h>
#include <lam/render/virtual_screen.h>
#include <lam/ui/dialogue.h>
#include <lam/ui/menu_scenes.h>
#include <lam/ui/toasts.h>
#include <lam/ui/ui.h>
#include <lam/ui/ui_draw.h>
#include <lam/ui/ui_plugin.h>

#include "detail/menu_fit.h"

namespace lam {
namespace {

Rect screen_rect(const Ctx &ctx) {
    if (const VirtualLayout *vl = ctx.try_res<VirtualLayout>(); vl != nullptr && vl->virtual_size.x > 0)
        return {0, 0, static_cast<float>(vl->virtual_size.x), static_cast<float>(vl->virtual_size.y)};
    const IVec2 v = ctx.app().config().virtual_size;
    return {0, 0, static_cast<float>(v.x), static_cast<float>(v.y)};
}

const UiTheme &resolve_theme(const Ctx &ctx, AssetId<UiTheme> id) {
    if (id) {
        if (const AssetServer *assets = ctx.try_res<AssetServer>()) {
            if (const UiTheme *t = assets->get(id)) return *t;
        }
    }
    return default_ui_theme();
}

// The theme's font, else RenderDefaults::font; null without either (layout uses 6x8 metrics).
const Font *resolve_font(const Ctx &ctx, const UiTheme &theme) {
    const AssetServer *assets = ctx.try_res<AssetServer>();
    if (assets == nullptr) return nullptr;
    if (theme.font) {
        if (const Font *f = assets->get(theme.font)) return f;
    }
    if (const RenderDefaults *d = ctx.try_res<RenderDefaults>(); d != nullptr && d->font) return assets->get(d->font);
    return nullptr;
}

void play_ui_sound(Audio *audio, StrId group) {
    if (audio == nullptr || !group) return;
    PlayParams p;
    p.bus = audio->bus("ui");
    audio->play(group, p);
}

void ui_update(Ctx &ctx) {
    UiTree &tree = ctx.local<UiTree>();
    tree.clear_events();
    if (tree.size() == 0) return;
    const UiTheme &theme = resolve_theme(ctx, tree.theme());
    const Font *font = resolve_font(ctx, theme);
    const Rect screen = screen_rect(ctx);
    tree.layout(screen, theme, font);
    // A menu taller than the screen gives the overflow back before input sees it;
    // the focused item is scrolled into the smaller list.
    if (const MenuState *menu = ctx.try_local<MenuState>(); menu != nullptr && detail::fit_menu(tree, *menu, screen)) {
        tree.layout(screen, theme, font);
        tree.scroll_into_view(tree.focused());
        if (tree.layout_dirty()) tree.layout(screen, theme, font);
    }

    UiInput in;
    const FrameInput &frame = frame_input(ctx);                 // neutral unless this world is focused
    if (const UiNav *nav = ctx.try_res<UiNav>()) {
        in = ui_input_from(frame, *nav);
    } else {
        in = ui_input_from(frame, UiNav{});
    }
    InputSystem *input = ctx.try_res<InputSystem>();
    if (input != nullptr && ctx.focused()) in.typed = input->text_typed();
    if (const ActionMap *map = ctx.try_res<ActionMap>(); map != nullptr && map->capturing()) {
        in = UiInput{};                                         // a key is being rebound: UI ignores input
    }
    tree.handle_input(in);
    if (tree.layout_dirty()) tree.layout(screen, theme, font);

    Audio *audio = ctx.try_res<Audio>();
    for (const UiEvent &e : tree.events()) {
        ctx.emit(e);
        switch (e.kind) {
        case UiEvent::Kind::Focused: play_ui_sound(audio, theme.focus_sound); break;
        case UiEvent::Kind::Back: break;
        default: play_ui_sound(audio, theme.click_sound); break;
        }
        // UI never changes simulation state directly: node intents go through the input.
        if (e.kind == UiEvent::Kind::Clicked || e.kind == UiEvent::Kind::Changed ||
            e.kind == UiEvent::Kind::Submitted) {
            const UiNode *n = tree.get(e.node);
            if (n != nullptr && n->intent && input != nullptr)
                input->queue_intent(Intent{n->intent, n->intent_a, static_cast<std::int64_t>(std::llround(e.value))});
        }
    }
}

void draw_ui(Ctx &ctx) {
    UiTree *tree = ctx.try_local<UiTree>();
    RenderQueue *queue = ctx.try_res<RenderQueue>();
    if (tree == nullptr || queue == nullptr || tree->size() == 0) return;
    const UiTheme &theme = resolve_theme(ctx, tree->theme());
    const Font *font = resolve_font(ctx, theme);
    if (tree->layout_dirty()) tree->layout(screen_rect(ctx), theme, font);   // edited after ui_update
    draw_ui_tree(*queue, *tree, theme, font, kLayerUi);
}

void toasts_update(Ctx &ctx) {
    if (ToastQueue *toasts = ctx.try_res<ToastQueue>()) toasts->update(ctx.time().frame_dt);
}

void draw_toasts(Ctx &ctx) {
    ToastQueue *toasts = ctx.try_res<ToastQueue>();
    RenderQueue *queue = ctx.try_res<RenderQueue>();
    if (toasts == nullptr || queue == nullptr || toasts->toasts().empty()) return;
    const UiTree *tree = ctx.try_local<UiTree>();
    const UiTheme &theme = resolve_theme(ctx, tree != nullptr ? tree->theme() : AssetId<UiTheme>{});
    draw_toast_list(*queue, *toasts, theme, resolve_font(ctx, theme), screen_rect(ctx), kLayerOverlay);
}

UiNav default_nav(ActionMap &map) {
    using AM = ActionMap;
    UiNav nav;
    nav.up = map.button("ui.up", {AM::key(Key::Up), AM::key(Key::W), AM::pad(PadButton::DUp),
                                  AM::pad_axis(PadAxis::LeftY, false)});
    nav.down = map.button("ui.down", {AM::key(Key::Down), AM::key(Key::S), AM::pad(PadButton::DDown),
                                      AM::pad_axis(PadAxis::LeftY, true)});
    nav.left = map.button("ui.left", {AM::key(Key::Left), AM::key(Key::A), AM::pad(PadButton::DLeft),
                                      AM::pad_axis(PadAxis::LeftX, false)});
    nav.right = map.button("ui.right", {AM::key(Key::Right), AM::key(Key::D), AM::pad(PadButton::DRight),
                                        AM::pad_axis(PadAxis::LeftX, true)});
    nav.confirm = map.button("ui.confirm", {AM::key(Key::Enter), AM::key(Key::Space), AM::key(Key::KpEnter),
                                            AM::pad(PadButton::South)});
    nav.back = map.button("ui.back", {AM::key(Key::Escape), AM::pad(PadButton::East)});
    nav.erase = map.button("ui.erase", {AM::key(Key::Backspace)});
    // Held directions repeat: 18 ticks delay, then every 4 ticks (real time in FrameInput).
    for (ButtonAction a : {nav.up, nav.down, nav.left, nav.right, nav.erase})
        if (a) map.set_repeat(a, 18, 4);
    return nav;
}

} // namespace

void UiPlugin::build(App &app) {
    app.add_event<UiEvent>(EventScope::World, 64);
    ActionMap *map = app.try_res<ActionMap>();
    app.insert_resource<UiNav>(map != nullptr ? default_nav(*map) : UiNav{});
    app.insert_resource<ToastQueue>();
    if (AssetServer *assets = app.try_res<AssetServer>()) {
        assets->register_loader<UiTheme>(make_ui_theme_loader());
        assets->register_loader<DialogueScript>(make_dialogue_loader());
        loaders_ = true;
    }
    SystemDesc d;
    d.stage = Stage::Update;
    d.fn = ui_update;
    d.name = "ui_update";
    d.order = -800;
    d.scope = SystemScope::PerWorld;
    d.run_if = has_local<UiTree>;
    app.add_system(d);

    d = SystemDesc{};
    d.stage = Stage::Update;
    d.fn = toasts_update;
    d.name = "toasts_update";
    d.order = 500;
    d.scope = SystemScope::Once;
    app.add_system(d);

    d = SystemDesc{};
    d.stage = Stage::RenderUI;
    d.fn = draw_ui;
    d.name = "draw_ui";
    d.order = -1000;
    d.scope = SystemScope::PerWorld;
    d.run_if = has_local<UiTree>;
    app.add_system(d);

    d = SystemDesc{};
    d.stage = Stage::RenderUI;
    d.fn = draw_toasts;
    d.name = "draw_toasts";
    d.order = 500;
    d.scope = SystemScope::Once;
    app.add_system(d);
}

void UiPlugin::init_world(App &, World &world) {
    if (!world.resources().has<UiTree>()) world.resources().insert<UiTree>();
}

void UiPlugin::finish(App &app) {
    if (!loaders_) {
        if (AssetServer *assets = app.try_res<AssetServer>()) {
            assets->register_loader<UiTheme>(make_ui_theme_loader());
            assets->register_loader<DialogueScript>(make_dialogue_loader());
            loaders_ = true;
        }
    }
    UiNav &nav = app.res<UiNav>();
    const bool unset = !nav.up && !nav.down && !nav.left && !nav.right && !nav.confirm && !nav.back && !nav.erase;
    if (ActionMap *map = app.try_res<ActionMap>(); map != nullptr && unset) nav = default_nav(*map);
}

} // namespace lam
