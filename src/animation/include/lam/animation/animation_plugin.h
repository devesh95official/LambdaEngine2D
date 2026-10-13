#pragma once
// AnimationPlugin: sprite sheet and graph loaders, the animation components and their systems.
#include <string_view>
#include <lam/core/app.h>
namespace lam {
// Systems: motor_anim_bridge (Animate -1500), animators (-1400), tweens (-1300) and frame_tweens
// (Update -1500), all PerWorld. Requires lam.assets.
class AnimationPlugin final : public Plugin {
public:
    std::string_view name() const override { return "lam.animation"; }
    // Components, events, systems, and the loaders when AssetServer already exists.
    void build(App &app) override;
    // Registers the loaders if AssetsPlugin was added after this plugin (build could not).
    void finish(App &app) override;

private:
    bool loaders_registered_ = false;
};
}
