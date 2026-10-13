#pragma once
// Scene stack. Each scene owns a World and an asset scope. Operations queue up and apply at
// FrameBegin/TickBegin -2000, so ones issued from tick systems replay exactly.
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/id.h>
#include <lam/math/color.h>
namespace lam {
// Result handed to the scene below by pop(), via resumed(). Copied, so the sender may be gone.
struct SceneMessage { StrId kind; std::int64_t value = 0; StrId detail; };
// One screen of a game (title, level, pause menu). Callbacks run inside SceneStack::apply, never
// while the scene's world is being iterated.
class Scene {
public:
    virtual ~Scene() = default;
    // Names the scene's World (App::create_world); non-empty, static storage recommended.
    virtual std::string_view name() const = 0;
    // Spawn entities, load assets (into asset_scope()) and add systems here.
    virtual void enter(App &app, World &world) = 0;
    // Leaving the stack. The world still exists here and is destroyed right after.
    virtual void exit(App &app, World &world) { (void)app; (void)world; }
    virtual void covered(App &app) { (void)app; }               // a scene was pushed above
    // On top again after the scene above popped. `msg` is the pop() result (or null), valid only
    // during the call.
    virtual void resumed(App &app, const SceneMessage *msg) { (void)app; (void)msg; }
    virtual bool overlay() const { return false; }              // lower scenes stay visible
    virtual bool blocks_ticks() const { return true; }          // lower scenes stop ticking
    virtual bool blocks_input() const { return true; }          // lower scenes lose WorldEntry::input
    World &world() const;                                       // asserts entered()
    // Loads made during enter() land here; pass it explicitly to load into it from systems.
    AssetScope asset_scope() const;
    bool entered() const { return world_ != nullptr; }          // between enter() and exit()
    // System that runs only on this scene's world and goes away with it. `name` must be static
    // and unique per stage. FrameEnd systems only run while this world has focus.
    void add_system(App &app, Stage stage, SystemFn fn, std::string_view name, int order = 0) const;
private:
    friend class SceneStack;
    World *world_ = nullptr;
    AssetScope scope_ = kRootScope;
};
enum class TransitionKind : std::uint8_t { None, Fade };
// Fade out over `ticks`, run the operation at full cover, wait for assets if asked, fade back in.
// No world gets input while it runs. ticks == 0 is the same as None.
struct Transition {
    TransitionKind kind = TransitionKind::None;
    std::uint16_t ticks = 18;                                   // fade out + fade in, each
    Color color = kBlack;
    bool wait_for_assets = true;                                // the new world ticks once its scope is Ready
};
// push/pop/replace/reset only queue; apply() and apply_tick() run the queue in order, and a fade
// holds later operations until it finishes. Also drives the App's world stack.
class SceneStack {                                              // App resource
public:
    SceneStack();
    ~SceneStack();
    SceneStack(const SceneStack &) = delete;
    SceneStack &operator=(const SceneStack &) = delete;
    // Queues: enter `s` above the current top (covered() on the old top).
    void push(std::unique_ptr<Scene> s, Transition t = {});
    // Queues: exit the top scene, then resumed(result) on the new top. Empty stack: a warning.
    void pop(std::optional<SceneMessage> result = std::nullopt, Transition t = {});
    // Queues: exit the top scene and enter `s` in its place (no covered/resumed). Empty: push.
    void replace(std::unique_ptr<Scene> s, Transition t = {});
    // Queues: exit every scene (top first) and enter `s`; null `s` leaves the stack empty.
    void reset(std::unique_ptr<Scene> s, Transition t = {});
    // push(std::make_unique<S>(args...)) with no transition.
    template <class S, class... A> void push_new(A &&...args) { push(std::make_unique<S>(std::forward<A>(args)...)); }
    bool transitioning() const;                                 // a fade is running (any phase)
    // Operations not yet applied (an operation whose fade-out is running counts).
    std::size_t queued() const;
    Scene *top() const;                                         // null when empty
    std::size_t size() const;                                   // scenes on the stack
    float fade_alpha() const;                                   // 0..1, drawn by scene_fade
    // FrameBegin: applies queued operations up to the next fade. Also steps a fade while paused.
    void apply(App &app);
    // TickBegin: applies queued operations, then steps a running fade by one tick.
    void apply_tick(App &app);
    Scene *at(std::size_t i) const;                             // 0 = bottom; LAM_ASSERT(i < size())
    bool empty() const { return size() == 0; }
    Color fade_color() const;                                   // before fade_alpha() is applied
    // True while the top scene waits for its assets (wait_for_assets) and its world does not tick.
    bool waiting_for_assets() const;
    // The world stack entries derived at the last apply (bottom first).
    std::span<const WorldEntry> world_entries() const;
    // Exits the scene owning `w` and drops it from the stack. Called when its world is destroyed.
    void exit_world(App &app, World &w);
    // Exits every remaining scene, top first, and drops queued operations.
    void shutdown(App &app);
private:
    enum class OpKind : std::uint8_t { Push, Pop, Replace, Reset };
    enum class Phase : std::uint8_t { Idle, Out, Wait, In };
    struct Op {
        OpKind kind = OpKind::Push;
        std::unique_ptr<Scene> scene;
        std::optional<SceneMessage> message;
        Transition transition;
    };
    struct Entry {
        std::unique_ptr<Scene> scene;
        bool waiting = false;                                   // world held until assets are Ready
    };
    void enqueue(OpKind kind, std::unique_ptr<Scene> s, std::optional<SceneMessage> m, Transition t);
    void drain(App &app);
    void advance(App &app);
    void perform(App &app, Op &op);
    void enter_scene(App &app, std::unique_ptr<Scene> s, bool wait_for_assets);
    void exit_top(App &app);
    void release_scopes(App &app);
    bool assets_ready(App &app, const Scene &scene) const;     // its scope's loads
    void update_waiting(App &app);
    void sync_world_stack(App &app);
    std::deque<Op> ops_;
    std::vector<Entry> scenes_;
    std::vector<AssetScope> released_;
    Op active_;                                                 // the operation of the running fade
    bool active_has_op_ = false;                                // false once performed
    Phase phase_ = Phase::Idle;
    std::uint16_t elapsed_ = 0;
    bool applying_ = false;
    bool owns_stack_ = false;                                   // we set the App's world stack
    bool used_ = false;                                         // a scene has entered at least once
    std::array<WorldEntry, kMaxWorlds> entries_{};
    std::size_t entry_count_ = 0;
};
// SceneStack plus scene_apply (FrameBegin -2000), scene_apply_tick (TickBegin -2000) and
// scene_fade (RenderUI +1000).
class ScenePlugin final : public Plugin {
public:
    std::string_view name() const override { return "lam.scene"; }
    void build(App &app) override;
    void shutdown_world(App &app, World &world) override;       // exits the scene owning `world`
    void shutdown(App &app) override;                           // exits every scene, top first
};
}
