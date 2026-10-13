// Engine menu scenes. ui.menu_frame turns clicks into intents and ui.menu_tick pops the scene on
// the menu world's tick, so menu choices are recorded and replay.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lam/audio/audio.h>
#include <lam/core/app.h>
#include <lam/core/debug.h>
#include <lam/core/log.h>
#include <lam/core/settings.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/render/virtual_screen.h>
#include <lam/scene/scene.h>
#include <lam/ui/menu_scenes.h>
#include <lam/ui/ui.h>

#include "detail/menu_fit.h"

namespace lam {
namespace {

Vec2 virtual_screen_size(App &app) {
    if (const VirtualLayout *vl = app.try_res<VirtualLayout>(); vl != nullptr && vl->virtual_size.x > 0)
        return {static_cast<float>(vl->virtual_size.x), static_cast<float>(vl->virtual_size.y)};
    const IVec2 v = app.config().virtual_size;
    return {static_cast<float>(v.x), static_cast<float>(v.y)};
}

UiId add_item(UiTree &tree, UiId list, const MenuItem &item) {
    UiId id = 0;
    switch (item.kind) {
    case UiKind::Button:
        id = tree.button(list, item.id, item.label.view());
        if (UiNode *n = tree.get(id)) n->value = item.value;    // free payload (slot index...)
        break;
    case UiKind::Toggle:
        id = tree.toggle(list, item.id, item.label.view(), item.value >= 0.5f);
        break;
    case UiKind::Slider: {
        const float step = item.step > 0.0f ? item.step : (item.max - item.min) / 10.0f;
        id = tree.slider(list, item.id, item.label.view(), item.value, item.min, item.max, step);
        break;
    }
    case UiKind::Choice: {
        if (item.choice_count > 0) {
            std::array<std::string_view, kMaxUiChoices> views{};
            const std::size_t count = std::min<std::size_t>(item.choice_count, kMaxUiChoices);
            for (std::size_t k = 0; k < count; ++k) views[k] = item.choices[k].view();
            id = tree.choice(list, item.id, item.label.view(), std::span<const std::string_view>(views.data(), count),
                             static_cast<int>(std::lround(item.value)));
        } else {
            UiNode n;                                           // integer stepper min..max
            n.kind = UiKind::Choice;
            n.id = item.id;
            n.text = item.label.view();
            n.min = item.min;
            n.max = item.max;
            n.value = item.value;
            id = tree.add(list, n);
        }
        break;
    }
    case UiKind::TextInput:
        id = tree.text_input(list, item.id, item.label.view());
        break;
    default:
        id = tree.label(list, item.label.view());
        break;
    }
    if (UiNode *n = tree.get(id)) n->enabled = item.enabled;
    return id;
}

void menu_frame_system(Ctx &ctx) {
    MenuState *st = ctx.try_local<MenuState>();
    UiTree *tree = ctx.try_local<UiTree>();
    if (st == nullptr || tree == nullptr) return;
    App &app = ctx.app();
    InputSystem *input = ctx.try_res<InputSystem>();
    for (const UiEvent &e : tree->events()) {
        const int idx = menu_item_index(*st, e.node);
        if (e.kind == UiEvent::Kind::Changed && idx >= 0 && st->on_change != nullptr)
            st->on_change(app, *st, static_cast<std::size_t>(idx), e.value);
        if (e.kind == UiEvent::Kind::Clicked && idx >= 0 && st->on_click != nullptr &&
            st->on_click(app, *st, static_cast<std::size_t>(idx)))
            continue;
        if (const std::optional<Intent> in = menu_event_intent(*st, *tree, e)) {
            if (input != nullptr) input->queue_intent(*in);
            else if (st->pending_count < kMaxIntents) st->pending[st->pending_count++] = *in;
        }
    }
    if (st->on_frame != nullptr) st->on_frame(app, *st, *tree, frame_input(ctx));
}

void menu_tick_system(Ctx &ctx) {
    MenuState *st = ctx.try_local<MenuState>();
    if (st == nullptr) return;
    SceneStack *stack = ctx.try_res<SceneStack>();
    const auto handle = [&](const Intent &in) {
        if (const std::optional<SceneMessage> msg = menu_intent_message(*st, in)) {
            if (stack != nullptr) stack->pop(*msg);
        }
    };
    for (const Intent &in : input_frame(ctx).intent_list()) handle(in);
    for (std::uint8_t k = 0; k < st->pending_count; ++k) handle(st->pending[k]);
    st->pending_count = 0;
}

// ---- settings ------------------------------------------------------------------------------

struct BusEntry {
    std::string_view bus, key, label;
};
constexpr BusEntry kBuses[] = {{"master", "audio.master.volume", "Master volume"},
                               {"music", "audio.music.volume", "Music"},
                               {"sfx", "audio.sfx.volume", "Sounds"},
                               {"ambience", "audio.ambience.volume", "Ambience"}};
constexpr std::string_view kFullscreenKey = "display.fullscreen";
constexpr std::string_view kShowFpsKey = "debug.show_fps";

std::string_view bus_of_key(std::string_view key) {
    for (const BusEntry &b : kBuses)
        if (b.key == key) return b.bus;
    return {};
}

void rebind_label(const ActionMap &map, ButtonAction a, FixedString<48> &out, bool capturing) {
    const std::string_view name = map.name_of(a);
    if (capturing) {
        format_to(out, "{}: press a key...", name);
        return;
    }
    const std::span<const Binding> b = map.bindings(a, 0);
    if (b.empty()) format_to(out, "{}: -", name);
    else format_to(out, "{}: {}", name, binding_to_string(b.front()));
}

void settings_change(App &app, MenuState &st, std::size_t item, float value) {
    const MenuItem &it = st.spec.items[item];
    const std::string_view key = id_name(it.id);
    SettingsStore *settings = app.try_res<SettingsStore>();
    if (const std::string_view bus = bus_of_key(key); !bus.empty()) {
        if (settings != nullptr) {
            if (!settings->defined(key)) settings->define_float(key, 1.0f, 0.0f, 1.0f);
            settings->set_float(key, value);
        }
        if (Audio *audio = app.try_res<Audio>()) audio->set_bus_volume(audio->bus(bus), value);
        return;
    }
    if (key == kFullscreenKey) {
        if (settings != nullptr) {
            if (!settings->defined(key)) settings->define_bool(key, false);
            settings->set_bool(key, value >= 0.5f);             // the platform applies it next frame
        }
        return;
    }
    if (key == kShowFpsKey) {
        if (DebugOverlay *overlay = app.try_res<DebugOverlay>()) overlay->visible = value >= 0.5f;
        return;
    }
    menu_settings_change(app, st, item, value);                 // extra (game) items
}

bool settings_click(App &app, MenuState &st, std::size_t item) {
    const MenuItem &it = st.spec.items[item];
    if (it.id != kMenuRebindItem) return false;
    ActionMap *map = app.try_res<ActionMap>();
    if (map == nullptr) return true;
    const ButtonAction a{static_cast<std::uint16_t>(std::lround(it.value))};
    map->begin_capture(a, 0, 0);
    st.hook_state = static_cast<std::int64_t>(item) + 1;        // capturing this item
    return true;
}

void settings_frame(App &app, MenuState &st, UiTree &tree, const FrameInput &) {
    ActionMap *map = app.try_res<ActionMap>();
    if (map == nullptr || st.hook_state == 0) return;
    const std::size_t item = static_cast<std::size_t>(st.hook_state - 1);
    if (item >= st.nodes.size()) {
        st.hook_state = 0;
        return;
    }
    const ButtonAction a{static_cast<std::uint16_t>(std::lround(st.spec.items[item].value))};
    const bool capturing = map->capturing();
    FixedString<48> label;
    rebind_label(*map, a, label, capturing);
    tree.set_text(st.nodes[item], label.view());
    if (capturing) return;
    st.hook_state = 0;                                          // captured (or cancelled): final label
    if (SettingsStore *settings = app.try_res<SettingsStore>()) {
        if (Status s = map->save(*settings); !s)
            LAM_LOG_WARN("settings: key bindings not saved: {}", s.error().message);
    }
}

class SettingsScene final : public MenuScene {
public:
    explicit SettingsScene(std::vector<MenuItem> extra) : MenuScene(MenuSpec{}), extra_(std::move(extra)) {}
    std::string_view name() const override { return "lam.settings"; }
    void enter(App &app, World &world) override {
        spec_.title = "Settings";
        spec_.items.clear();
        SettingsStore *settings = app.try_res<SettingsStore>();
        Audio *audio = app.try_res<Audio>();
        for (const BusEntry &b : kBuses) {
            MenuItem m;
            m.id = intern(b.key);
            m.label = b.label;
            m.kind = UiKind::Slider;
            m.min = 0;
            m.max = 1;
            m.step = 0.1f;
            if (settings != nullptr && settings->defined(b.key)) m.value = settings->get_float(b.key);
            else m.value = audio != nullptr ? audio->bus_volume(audio->bus(b.bus)) : 1.0f;
            spec_.items.push_back(m);
        }
        {
            MenuItem m;
            m.id = intern(kFullscreenKey);
            m.label = "Fullscreen";
            m.kind = UiKind::Toggle;
            m.value = settings != nullptr && settings->defined(kFullscreenKey) && settings->get_bool(kFullscreenKey)
                          ? 1.0f
                          : 0.0f;
            spec_.items.push_back(m);
        }
#if LAM_DEV
        {
            MenuItem m;
            m.id = intern(kShowFpsKey);
            m.label = "Show FPS";
            m.kind = UiKind::Toggle;
            const DebugOverlay *overlay = app.try_res<DebugOverlay>();
            m.value = overlay != nullptr && overlay->visible ? 1.0f : 0.0f;
            spec_.items.push_back(m);
        }
#endif
        if (const ActionMap *map = app.try_res<ActionMap>()) {
            bool heading = false;
            for (std::uint16_t a = 1; a < kMaxButtonActions; ++a) {
                const std::string_view action = map->name_of(ButtonAction{a});
                if (action.empty() || action.starts_with("ui.")) continue;
                if (!heading) {
                    MenuItem h;
                    h.kind = UiKind::Label;
                    h.label = "Controls";
                    spec_.items.push_back(h);
                    heading = true;
                }
                MenuItem m;
                m.id = kMenuRebindItem;
                m.kind = UiKind::Button;
                m.value = static_cast<float>(a);
                rebind_label(*map, ButtonAction{a}, m.label, false);
                spec_.items.push_back(m);
            }
        }
        for (MenuItem m : extra_) {
            // Extra items backed by a settings key show its current value.
            const std::string_view key = id_name(m.id);
            if (settings != nullptr && !key.empty() && settings->defined(key)) {
                if (m.kind == UiKind::Toggle) m.value = settings->get_bool(key) ? 1.0f : 0.0f;
                else if (m.kind == UiKind::Slider) m.value = settings->get_float(key);
                else if (m.kind == UiKind::Choice) m.value = static_cast<float>(settings->get_int(key));
            }
            spec_.items.push_back(m);
        }
        MenuItem back;
        back.id = str_id("back");
        back.label = "Back";
        spec_.items.push_back(back);
        MenuScene::enter(app, world);
        MenuState &st = world.resources().get<MenuState>();
        st.on_change = settings_change;
        st.on_click = settings_click;
        st.on_frame = settings_frame;
    }
private:
    std::vector<MenuItem> extra_;
};

// ---- text ----------------------------------------------------------------------------------

void text_frame(App &app, MenuState &st, UiTree &tree, const FrameInput &fi) {
    const UiNav *nav = app.try_res<UiNav>();
    if (nav == nullptr || st.body == 0) return;
    if (fi.is_repeated(nav->down) || fi.is_pressed(nav->down)) tree.scroll_by(st.body, 1);
    if (fi.is_repeated(nav->up) || fi.is_pressed(nav->up)) tree.scroll_by(st.body, -1);
}

class TextScene final : public MenuScene {
public:
    explicit TextScene(MenuSpec spec) : MenuScene(std::move(spec)) {}
    std::string_view name() const override { return "lam.text"; }
    void enter(App &app, World &world) override {
        MenuScene::enter(app, world);
        MenuState &st = world.resources().get<MenuState>();
        st.on_frame = text_frame;
    }
};

} // namespace

// ---- MenuScene -----------------------------------------------------------------------------

MenuScene::MenuScene(MenuSpec spec) : spec_(std::move(spec)) {}

std::string_view MenuScene::name() const {
    return "lam.menu";
}

bool MenuScene::overlay() const {
    return spec_.overlay;
}

const MenuSpec &MenuScene::spec() const {
    return spec_;
}

void MenuScene::enter(App &app, World &world) {
    UiTree *tree = world.resources().try_get<UiTree>();
    if (tree == nullptr) tree = &world.resources().insert<UiTree>();
    if (world.resources().has<MenuState>()) world.resources().remove<MenuState>();
    MenuState &st = world.resources().insert<MenuState>(build_menu(*tree, spec_, virtual_screen_size(app)));
    (void)st;
    // Plain function pointers on this world only; removed with the world.
    SystemDesc frame;
    frame.stage = Stage::Update;
    frame.fn = menu_frame_system;
    frame.name = "ui.menu_frame";
    frame.order = -700;
    frame.only_world = world.id();
    app.add_system(frame);
    SystemDesc tick;
    tick.stage = Stage::Input;
    tick.fn = menu_tick_system;
    tick.name = "ui.menu_tick";
    tick.order = -1000;
    tick.only_world = world.id();
    app.add_system(tick);
}

MenuState build_menu(UiTree &tree, MenuSpec spec, Vec2 screen) {
    MenuState st;
    tree.set_theme(spec.theme);
    // Full-screen layer: dims the scenes below and keeps the pointer inside this menu.
    UiNode root;
    root.kind = UiKind::Panel;
    root.fill = kUiFillX | kUiFillY;
    root.layout = UiLayout::None;
    root.style.theme_class = str_id("plain");
    root.style.padding = {0, 0};
    root.color = spec.overlay ? spec.dim : kTransparent;
    st.root = tree.add(0, root);
    UiNode panel;
    panel.kind = UiKind::Panel;
    panel.layout = UiLayout::Vertical;
    panel.anchor = panel.pivot = {0.5f, 0.5f};
    panel.rect.w = spec.width;
    panel.style.padding = {8, 6};
    panel.style.spacing = 4;
    st.panel = tree.add(st.root, panel);
    if (!spec.title.empty()) {
        const UiId title = tree.label(st.panel, spec.title.view());
        tree.get(title)->style.theme_class = str_id("title");
    }
    if (!spec.body.empty()) {
        const float w = spec.width > 0.0f ? 0.0f : std::clamp(screen.x - 48.0f, 64.0f, 280.0f);
        st.body = tree.text_block(st.panel, {0, 0, w, 0}, spec.body);
        if (UiNode *b = tree.get(st.body)) {
            b->style.padding = {0, 0};
            if (screen.y > 96.0f) b->max_size.y = std::floor(screen.y * 0.6f);
        }
    }
    UiNode list;
    list.kind = UiKind::List;
    list.layout = UiLayout::Vertical;
    list.style.theme_class = str_id("plain");
    list.style.padding = {0, 0};
    list.style.spacing = 2;
    if (screen.y > 64.0f) list.max_size.y = screen.y - 48.0f;
    st.list = tree.add(st.panel, list);
    st.nodes.reserve(spec.items.size());
    for (const MenuItem &item : spec.items) st.nodes.push_back(add_item(tree, st.list, item));
    // Initial focus: the requested item if it can take focus, else the first enabled item.
    UiId focus = 0;
    for (std::size_t i = 0; i < spec.items.size() && focus == 0; ++i) {
        const UiNode *n = tree.get(st.nodes[i]);
        if (spec.initial_focus && spec.items[i].id == spec.initial_focus && n && n->focusable && n->enabled)
            focus = st.nodes[i];
    }
    for (std::size_t i = 0; i < spec.items.size() && focus == 0; ++i) {
        const UiNode *n = tree.get(st.nodes[i]);
        if (n && n->focusable && n->enabled) focus = st.nodes[i];
    }
    if (focus != 0) tree.focus(focus);
    st.spec = std::move(spec);
    st.on_change = menu_settings_change;
    return st;
}

bool detail::fit_menu(UiTree &tree, const MenuState &menu, Rect screen) {
    if (menu.panel == 0 || !tree.shown(menu.panel)) return false;
    // The title, body and font are known only after layout (themes load late), so the list and
    // body caps set by build_menu can add up to more than the screen: give the overflow back.
    float over = tree.screen_rect(menu.panel).h - (screen.h - 2.0f * kMenuScreenMargin);
    bool changed = false;
    const auto shrink = [&](UiId id, float least) {
        if (!(over >= 1.0f) || id == 0 || !tree.shown(id)) return;
        const float h = tree.screen_rect(id).h;
        const float to = std::max(least, std::floor(h - over));
        if (!(to < h)) return;
        tree.get(id)->max_size.y = to;
        over -= h - to;
        changed = true;
    };
    shrink(menu.list, 16.0f);                                   // about one row
    shrink(menu.body, 8.0f);                                    // one line of the built-in font
    return changed;
}

int menu_item_index(const MenuState &state, UiId node) {
    if (node == 0) return -1;
    for (std::size_t i = 0; i < state.nodes.size(); ++i)
        if (state.nodes[i] == node) return static_cast<int>(i);
    return -1;
}

std::optional<Intent> menu_event_intent(const MenuState &state, const UiTree &tree, const UiEvent &e) {
    if (state.closed) return std::nullopt;
    switch (e.kind) {
    case UiEvent::Kind::Clicked: {
        const int idx = menu_item_index(state, e.node);
        if (idx < 0) return std::nullopt;
        return Intent{kMenuSelectIntent, idx, static_cast<std::int64_t>(std::llround(e.value))};
    }
    case UiEvent::Kind::Submitted: {
        const int idx = menu_item_index(state, e.node);
        if (idx < 0) return std::nullopt;
        const UiNode *n = tree.get(e.node);
        const StrId text = (n != nullptr && !n->text.empty()) ? intern(n->text.view()) : StrId{};
        return Intent{kMenuSelectIntent, idx, static_cast<std::int64_t>(text.value)};
    }
    case UiEvent::Kind::Back:
        if (!state.spec.back_pops) return std::nullopt;
        return Intent{kMenuBackIntent, 0, 0};
    default:
        return std::nullopt;
    }
}

std::optional<SceneMessage> menu_intent_message(MenuState &state, const Intent &intent) {
    if (state.closed) return std::nullopt;
    SceneMessage msg;
    if (intent.kind == kMenuSelectIntent) {
        if (intent.a < 0 || intent.a >= static_cast<std::int64_t>(state.spec.items.size())) return std::nullopt;
        const MenuItem &item = state.spec.items[static_cast<std::size_t>(intent.a)];
        if (item.kind != UiKind::Button && item.kind != UiKind::TextInput) return std::nullopt;
        msg.kind = item.id;
        if (item.kind == UiKind::TextInput) msg.detail = StrId{static_cast<std::uint64_t>(intent.b)};
        else msg.value = intent.b;
    } else if (intent.kind == kMenuBackIntent && state.spec.back_pops) {
        msg.kind = state.spec.back_kind;
    } else {
        return std::nullopt;
    }
    state.closed = true;
    state.result = msg;
    return msg;
}

void menu_settings_change(App &app, MenuState &state, std::size_t item, float value) {
    if (item >= state.spec.items.size()) return;
    const MenuItem &it = state.spec.items[item];
    SettingsStore *settings = app.try_res<SettingsStore>();
    const std::string_view key = id_name(it.id);
    if (settings == nullptr || key.empty() || !settings->defined(key)) return;
    switch (it.kind) {
    case UiKind::Toggle: settings->set_bool(key, value >= 0.5f); break;
    case UiKind::Slider: settings->set_float(key, value); break;
    case UiKind::Choice: settings->set_int(key, static_cast<int>(std::lround(value))); break;
    default: break;
    }
}

std::unique_ptr<Scene> make_confirm_scene(std::string_view question, StrId yes_kind, StrId no_kind) {
    MenuSpec spec;
    if (question.size() <= FixedString<48>::capacity()) spec.title = question;
    else spec.body = std::string(question);
    MenuItem yes;
    yes.id = yes_kind;
    yes.label = "Yes";
    MenuItem no;
    no.id = no_kind;
    no.label = "No";
    spec.items = {yes, no};
    spec.initial_focus = no_kind;                               // the safe answer
    spec.back_kind = no_kind;
    return std::make_unique<MenuScene>(std::move(spec));
}

std::unique_ptr<Scene> make_settings_scene(std::span<const MenuItem> extra) {
    return std::make_unique<SettingsScene>(std::vector<MenuItem>(extra.begin(), extra.end()));
}

std::unique_ptr<Scene> make_text_scene(std::string_view title, std::string body) {
    // '#' heading markers of the old text screens are dropped (one text style per block).
    std::string text;
    text.reserve(body.size());
    bool line_start = true;
    for (char c : body) {
        if (line_start && c == '#') {
            line_start = false;
            continue;
        }
        line_start = c == '\n';
        text.push_back(c);
    }
    MenuSpec spec;
    spec.title = title;
    spec.body = std::move(text);
    MenuItem close;
    close.id = str_id("close");
    close.label = "Close";
    spec.items = {close};
    return std::make_unique<TextScene>(std::move(spec));
}

} // namespace lam
