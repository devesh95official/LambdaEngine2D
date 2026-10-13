// ui_demo: a main menu (buttons, volume slider, toggle, choice) beside a scrolling level list in
// the main world's UiTree, themed by assets/ui/demo.theme.json (a tinted nine-slice). UiEvents
// become toasts and message-log lines; "Settings" pushes the engine settings scene. Until the
// first input, an attract mode drives the tree through UiInput so --frames runs cover it.
//   ./ui_demo [--frames N] [--hidden] [--headless]
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/input/input_frame.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include <lam/scene/scene.h>
#include <lam/ui/menu_scenes.h>
#include <lam/ui/message_log.h>
#include <lam/ui/toasts.h>
#include <lam/ui/ui.h>
#include <lam/ui/ui_draw.h>
#include <lam/ui/ui_plugin.h>

using namespace lam;
using namespace lam::literals;

namespace {

struct Demo {                                                   // App resource
    MessageLog log{6};
    UiId play = 0, list = 0, status = 0;
    bool attract = true;                                        // until the first real input
    std::uint32_t clicks = 0, changes = 0;
};

// Startup: the widgets. Builders return UiIds; keep those, not pointers.
void build_ui(Ctx &ctx) {
    UiTree &ui = ctx.local<UiTree>();
    Demo &demo = ctx.res<Demo>();
    ui.set_theme(ctx.res<AssetServer>().load<UiTheme>("ui/demo.theme.json"));
    const UiId root = ui.panel(0, {}, UiLayout::Horizontal);   // auto-sized row, centred
    ui.get(root)->anchor = ui.get(root)->pivot = {0.5f, 0.5f};
    ui.get(root)->style.spacing = 6;
    const UiId menu = ui.panel(root, {0, 0, 168, 0});           // vertical stack, fixed width
    ui.get(ui.label(menu, "LambdaEngine2D"))->style.theme_class = "title"_id;
    demo.play = ui.button(menu, "play"_id, "Play");
    ui.slider(menu, "volume"_id, "Volume", 0.8f, 0.0f, 1.0f);
    ui.toggle(menu, "hints"_id, "Hints", true);
    const std::string_view modes[] = {"Easy", "Normal", "Hard"};
    ui.choice(menu, "mode"_id, "Mode", modes, 1);
    ui.button(menu, "settings"_id, "Settings");
    ui.button(menu, "quit"_id, "Quit");
    demo.status = ui.label(menu, "Arrows/mouse, Enter");
    ui.get(demo.status)->style.theme_class = "hint"_id;
    demo.list = ui.list(root, {0, 0, 88, 0});                   // scrolls once capped
    ui.get(demo.list)->max_size.y = 120;
    for (int i = 1; i <= 12; ++i) {
        FixedString<16> name;
        format_to(name, "Level {}", i);
        ui.get(ui.button(demo.list, "level"_id, name.view()))->value = static_cast<float>(i);
    }
    ui.focus(demo.play);
}

// Update (after ui_update): UI decisions. Game-state changes would go through
// InputSystem::queue_intent (or UiNode::intent) so replays see them.
void react(Ctx &ctx) {
    UiTree &ui = ctx.local<UiTree>();
    Demo &demo = ctx.res<Demo>();
    ToastQueue &toasts = ctx.res<ToastQueue>();
    for (const UiEvent &e : ui.events()) {                      // this frame's events, in order
        FixedString<64> line;
        if (e.kind == UiEvent::Kind::Clicked) {
            ++demo.clicks;
            if (e.id == "quit"_id) {
                ctx.app().quit();
            } else if (e.id == "settings"_id) {
                if (SceneStack *scenes = ctx.try_res<SceneStack>()) scenes->push(make_settings_scene());
            } else if (e.id == "level"_id) {
                format_to(line, "Level {} selected", static_cast<int>(e.value));
            } else {
                line = "Play!";
            }
        } else if (e.kind == UiEvent::Kind::Changed) {
            ++demo.changes;
            const UiNode *n = ui.get(e.node);
            if (n == nullptr) continue;
            if (n->kind == UiKind::Slider) format_to(line, "{} {}%", n->text.view(), static_cast<int>(e.value * 100 + 0.5f));
            else if (n->kind == UiKind::Choice) format_to(line, "{}: {}", n->text.view(), n->choices[static_cast<std::size_t>(e.value)].view());
            else format_to(line, "{}: {}", n->text.view(), e.value >= 0.5f ? "on" : "off");
        }
        if (!line.empty()) {
            toasts.push(line.view());
            demo.log.add(line.view(), e.kind == UiEvent::Kind::Clicked ? Color{255, 215, 94, 255} : kWhite);
        }
    }
}

// Update -790 (between ui_update and react): scripted navigation until the user takes over.
void attract(Ctx &ctx) {
    Demo &demo = ctx.res<Demo>();
    if (const FrameInput *in = ctx.try_res<FrameInput>()) {
        bool any = in->pointer.pressed != 0;
        for (std::uint64_t word : in->pressed) any = any || word != 0;
        if (any) demo.attract = false;
    }
    const std::uint64_t f = ctx.time().frame;
    if (!demo.attract || f % 20 != 10) return;
    // c = confirm, d/u = down/up, l/r = left/right. Each round starts on "Play".
    static constexpr std::string_view kScript = "cdllrdcdrrurddddddddcludd";
    const std::size_t step_index = (f / 20) % kScript.size();
    UiTree &ui = ctx.local<UiTree>();
    if (step_index == 0) ui.focus(demo.play);
    const char k = kScript[step_index];
    const UiNode *focused = ui.get(ui.focused());
    if (k == 'c' && focused != nullptr && (focused->id == "quit"_id || focused->id == "settings"_id)) return;
    UiInput step;
    step.confirm = k == 'c';
    step.down = k == 'd';
    step.up = k == 'u';
    step.left = k == 'l';
    step.right = k == 'r';
    ui.handle_input(step);                                      // appends to ui.events()
}

// RenderUI (after draw_ui): the message log in the bottom-left corner.
void draw_log(Ctx &ctx) {
    RenderQueue *q = ctx.try_res<RenderQueue>();
    const RenderDefaults *defaults = ctx.try_res<RenderDefaults>();
    const Font *font = defaults != nullptr ? ctx.res<AssetServer>().get(defaults->font) : nullptr;
    if (q == nullptr || font == nullptr) return;
    draw_message_log(*q, ctx.res<Demo>().log, *font, {4, 214, 200, 52});
}

} // namespace

int main(int argc, char **argv) {
    App app({.title = "ui_demo", .virtual_size = {480, 270}, .settings_file = ""}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    app.insert_resource<Demo>();
    app.add_startup(build_ui, "demo.build_ui");
    app.add_system(Stage::Update, attract, "demo.attract", -790);
    app.add_system(Stage::Update, react, "demo.react");
    app.add_system(Stage::RenderUI, draw_log, "demo.draw_log");
    const int code = app.run();
    const Demo &demo = app.res<Demo>();
    const UiTree &ui = app.main_world().resources().get<UiTree>();
    const UiNode *focused = ui.get(ui.focused());
    std::printf("ui_demo: %llu frames; %zu nodes; %u clicks, %u changes; focus '%s'; list scroll %u; log %zu "
                "lines\n",
                static_cast<unsigned long long>(app.time().frame), ui.size(), demo.clicks, demo.changes,
                focused != nullptr ? focused->text.c_str() : "", static_cast<unsigned>(ui.get(demo.list)->scroll),
                demo.log.size());
    return code;
}
