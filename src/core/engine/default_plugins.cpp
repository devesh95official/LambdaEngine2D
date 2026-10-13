// Build order is dependency order. PlatformPlugin goes first so the window and audio device
// outlive every backend's GPU and audio resources. Headless runs skip all the raylib plugins.
#include <lam/core/default_plugins.h>

#include <lam/animation/animation_plugin.h>
#include <lam/audio/audio_plugin.h>
#include <lam/input/input_plugin.h>
#include <lam/particles/particles.h>
#include <lam/scene/scene.h>
#include <lam/tilemap/tilemap_plugin.h>
#include <lam/ui/ui_plugin.h>

namespace lam {

void DefaultPlugins::build(App &app) const {
    const bool no_backends = headless || app.options().headless;
    if (!no_backends) app.add_plugin<PlatformPlugin>(platform);
    app.add_plugin<AssetsPlugin>(assets);
    app.add_plugin<RenderPlugin>(render);
    if (!no_backends) app.add_plugin<RaylibRenderPlugin>();
    app.add_plugin<InputPlugin>();
    if (!no_backends) app.add_plugin<RaylibInputPlugin>();
    app.add_plugin<AudioPlugin>();
    if (!no_backends) app.add_plugin<RaylibAudioPlugin>();
    app.add_plugin<PhysicsPlugin>(physics);
    app.add_plugin<TilemapPlugin>();
    app.add_plugin<AnimationPlugin>();
    app.add_plugin<ParticlesPlugin>();
    // The menu scenes are factories in lam_ui; they only need the SceneStack that ScenePlugin adds.
    if (scenes) app.add_plugin<ScenePlugin>();
    app.add_plugin<UiPlugin>();
}

} // namespace lam
