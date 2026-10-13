#pragma once
// Input plugins. System order is listed in src/input/README.md.
#include <memory>
#include <string_view>
#include <lam/core/app.h>
#include <lam/input/driver.h>
namespace lam {
class InputPlugin final : public Plugin {
public:
    std::string_view name() const override { return "lam.input"; }
    // Handles --replay here (applies its seed); a bad replay file quits with code 1.
    void build(App &app) override;
    // Loads bindings from settings and opens the --record-input file.
    void finish(App &app) override;
    void shutdown(App &app) override;
private:
    std::unique_ptr<ReplayDriver> replay_;
    std::unique_ptr<InputRecorder> recorder_;
};
class RaylibInputPlugin final : public Plugin {
public:
    std::string_view name() const override { return "lam.input.raylib"; }
    // Polls raylib devices into InputSystem every frame. Add after InputPlugin.
    void build(App &app) override;
};
}
