#pragma once
// Audio plugins. PlatformPlugin opens and closes the raylib audio device itself.
//   app.add_plugin<AssetsPlugin>();  app.add_plugin<AudioPlugin>();  // + RaylibAudioPlugin (windowed)
#include <string_view>
#include <lam/core/app.h>
namespace lam {
// Audio resource, loaders, PlaySound and the audio systems. Add after AssetsPlugin.
class AudioPlugin final : public Plugin {
public:
    std::string_view name() const override { return "lam.audio"; }
    void build(App &app) override;
    void shutdown_world(App &app, World &world) override;
    void shutdown(App &app) override;
};
// Installs RaylibAudioDevice (lam_audio_rl). Add after AudioPlugin. Shutdown swaps back to a
// NullAudioDevice so raylib sounds are freed before PlatformPlugin closes the device.
class RaylibAudioPlugin final : public Plugin {
public:
    std::string_view name() const override { return "lam.audio.raylib"; }
    void build(App &app) override;
    void shutdown(App &app) override;
};
}
