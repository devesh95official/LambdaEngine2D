// RenderPlugin: components, resources, loaders and systems. The GPU side is in
// backend/raylib_render_plugin.cpp.
#include <lam/render/render_plugin.h>

#include <lam/assets/asset_server.h>
#include <lam/core/assert.h>
#include <lam/core/log.h>
#include <lam/core/platform.h>
#include <lam/ecs/component.h>
#include <lam/ecs/transform.h>
#include <lam/render/atlas.h>
#include <lam/render/camera.h>
#include <lam/render/camera_math.h>
#include <lam/render/font.h>
#include <lam/render/lighting.h>
#include <lam/render/passes.h>
#include <lam/render/render_plan.h>
#include <lam/render/render_queue.h>
#include <lam/render/shader.h>
#include <lam/render/sprite.h>
#include <lam/render/texture.h>
#include <lam/render/virtual_screen.h>

#include "detail/render_internal.h"
#include "detail/systems.h"

namespace lam {
namespace {

void add(App &app, Stage stage, SystemFn fn, std::string_view name, int order, SystemScope scope,
         RunIf run_if = nullptr) {
    SystemDesc d;
    d.stage = stage;
    d.fn = fn;
    d.name = name;
    d.order = order;
    d.scope = scope;
    d.run_if = run_if;
    app.add_system(d);
}

} // namespace

RenderPlugin::RenderPlugin(RenderConfig c) : config_(c) {}

void RenderPlugin::build(App &app) {
    app.require_plugin("lam.assets", name());
    AssetServer *assets = app.try_res<AssetServer>();
    LAM_CHECK(assets != nullptr, "RenderPlugin: add AssetsPlugin before RenderPlugin");

    // Components
    app.register_component<Sprite>();
    app.register_component<SpriteFade>();
    app.register_component<RenderOffset>();
    app.register_component<Shadow>();
    app.register_component<FadeOccluder>();
    app.register_component<FadeRevealer>();
    app.register_component<Camera2D>();
    app.register_component<CameraFollow>();
    app.register_component<CameraBounds>();
    app.register_component<CameraShake>();
    app.register_component<Light2D>();
    app.register_component<Text>();
    ComponentRegistry::global().register_type<CameraFollowState>(kSnapshot);

    // App resources
    if (config_.quad_capacity == 0) config_.quad_capacity = 1024;
    app.insert_resource<RenderConfig>(config_);
    app.insert_resource<RenderQueue>(config_.quad_capacity);
    app.insert_resource<RenderStats>();
    RenderPlan &plan = app.insert_resource<RenderPlan>();
    plan.batches.reserve(256);
    plan.staging.reserve(config_.quad_capacity * 4);
    plan.scratch.a.reserve(config_.quad_capacity);
    plan.scratch.b.reserve(config_.quad_capacity);
    app.insert_resource<ScreenCapture>();
    app.insert_resource<VirtualLayout>(compute_virtual_layout(
        app.platform().window().framebuffer, app.config().virtual_size, config_.scale_mode));
    AtlasSet &atlas = app.insert_resource<AtlasSet>(config_.atlas_page_size);
    app.insert_resource<Materials>();
    RenderPasses &passes = app.insert_resource<RenderPasses>();
    if (config_.lighting) ensure_light_pass(passes, 0.5f);
    app.insert_resource<detail::RenderRuntime>();
    // World event: simulation code shakes the camera without touching CameraShake.
    app.add_event<ShakeAt>(EventScope::World, 64);

    // Loaders; the backend replaces the decoder, .ttf/.otf and .shader.json ones.
    assets->register_loader<Texture>(make_texture_loader());
    assets->register_loader<Shader>(detail::make_placeholder_shader_loader());
    assets->register_loader<Font>(detail::make_bmfont_loader());
    assets->register_loader<Font>(detail::make_placeholder_ttf_loader());

    // Pack the built-in font first so it shares the shape page and batches with shapes.
    RenderDefaults &defaults = app.insert_resource<RenderDefaults>();
    defaults.font = detail::add_builtin_font(*assets, atlas);
    app.res<RenderQueue>().set_shape_texture(atlas.shape_page());

    // Systems
    using namespace detail;
    add(app, Stage::FrameBegin, sys_virtual_layout_update, "virtual_layout_update", -9000,
        SystemScope::Once);
    add(app, Stage::FrameBegin, sys_atlas_shape_page, "atlas_shape_page", -2400, SystemScope::Once);
    add(app, Stage::Update, sys_fade_occluders, "fade_occluders", -1400, SystemScope::PerWorld);
    add(app, Stage::Update, sys_sprite_fades, "sprite_fades", -1400, SystemScope::PerWorld);
    add(app, Stage::Update, sys_camera_follow, "camera_follow", -1300, SystemScope::PerWorld);
    add(app, Stage::Update, sys_camera_shake, "camera_shake", -1200, SystemScope::PerWorld);
    add(app, Stage::Update, sys_camera_view, "camera_view", -1100, SystemScope::PerWorld);
    add(app, Stage::Render, sys_begin_view, "begin_view", -2000, SystemScope::PerWorld);
    add(app, Stage::Render, sys_collect_sprites, "collect_sprites", -900, SystemScope::PerWorld);
    add(app, Stage::Render, sys_draw_lights, "draw_lights", -700, SystemScope::PerWorld,
        lighting_enabled);
    add(app, Stage::RenderUI, sys_resume_view, "resume_view", -2000, SystemScope::PerWorld);
    add(app, Stage::RenderUI, sys_draw_debug, "draw_debug", 2000, SystemScope::PerWorld);
    add(app, Stage::RenderUI, sys_draw_overlay, "draw_overlay", 2000, SystemScope::Once);
    add(app, Stage::FrameEnd, sys_render_plan, "render_plan", -1000, SystemScope::Once);
    add(app, Stage::FrameEnd, sys_render_clear, "render_clear", 9000, SystemScope::Once);
    app.align_pools<Transform, PrevTransform, Sprite>();
}

void RenderPlugin::init_world(App &app, World &world) {
    (void)app;
    if (!world.resources().has<CameraView>()) world.resources().insert<CameraView>();
    if (!world.resources().has<Lighting>()) world.resources().insert<Lighting>();
}

} // namespace lam
