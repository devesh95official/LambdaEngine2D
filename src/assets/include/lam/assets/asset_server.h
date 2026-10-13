#pragma once
// Typed asset server: loads cached by path and held by scopes, one IO/decode worker, main-thread
// finalize in load order, hot reload in LAM_DEV builds. Main thread only, except LoadContext.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>
#include <lam/core/events.h>
#include <lam/core/id.h>
#include <lam/core/result.h>
namespace lam {
class AssetServer;
using AssetScope = std::uint32_t;
inline constexpr AssetScope kRootScope = 0;

// Passed to decode(), possibly on the worker; don't touch the server or the App from there.
// Paths inside an asset file are relative to that file; paths in code are relative to the root.
class LoadContext {
public:
    // Made by the server for each decode; parent_* identify the asset being decoded.
    LoadContext(AssetServer &server, std::string_view path, std::uint64_t parent_bits, TypeId parent_type);
    std::string_view path() const;                              // root-relative
    Result<std::vector<std::byte>> read(std::string_view relative_path) const;   // next to path()
    // Queues a dependency next to path(). The parent retains it and finalizes after it is Ready;
    // if it fails, the parent fails.
    template <class U> AssetId<U> depend(std::string_view relative_path) {
        return AssetId<U>::from_bits(depend_erased(type_id<U>(), relative_path));
    }
private:
    std::uint64_t depend_erased(TypeId type, std::string_view relative_path);
    AssetServer *server_;
    std::string_view path_;
    std::uint64_t parent_bits_;
    TypeId parent_type_;
};
// Passed to finalize()/unload() on the main thread. What finalize() adds or loads into the current
// scope belongs to the asset being finalized; pass kRootScope to outlive it.
struct FinalizeContext {
    App &app;
    AssetServer &server;
    std::string_view path;
};
class AssetLoaderBase {
public:
    virtual ~AssetLoaderBase() = default;
    // Lower-case extensions with the dot, matched against the file name's suffix, longest first
    // (".sounds.json" before ".json"). Return a span over static storage.
    virtual std::span<const std::string_view> extensions() const = 0;
    virtual TypeId type() const = 0;                            // type_id<T>() of the asset type
    // Type-erased forms of AssetLoader<T>; implemented there (final).
    virtual Result<void *> decode_erased(std::span<const std::byte> bytes, LoadContext &ctx) = 0;
    virtual Status finalize_erased(void *asset, FinalizeContext &ctx) = 0;
    virtual void unload_erased(void *asset, FinalizeContext &ctx) = 0;
    virtual void destroy_erased(void *asset) = 0;
};
template <class T> class AssetLoader : public AssetLoaderBase {
public:
    virtual Result<T> decode(std::span<const std::byte> bytes, LoadContext &ctx) = 0;   // CPU only
    // Main thread, every dependency Ready (GPU uploads, atlas packing). An error fails the asset.
    virtual Status finalize(T &asset, FinalizeContext &ctx) { (void)asset; (void)ctx; return {}; }
    // Main thread, before the value is destroyed (GC, reload swap, shutdown).
    virtual void unload(T &asset, FinalizeContext &ctx) { (void)asset; (void)ctx; }
    TypeId type() const final { return type_id<T>(); }
    Result<void *> decode_erased(std::span<const std::byte> bytes, LoadContext &ctx) final {
        Result<T> r = decode(bytes, ctx);
        if (!r) return r.error();
        return static_cast<void *>(new T(std::move(r.value())));
    }
    Status finalize_erased(void *asset, FinalizeContext &ctx) final {
        return finalize(*static_cast<T *>(asset), ctx);
    }
    void unload_erased(void *asset, FinalizeContext &ctx) final { unload(*static_cast<T *>(asset), ctx); }
    void destroy_erased(void *asset) final { delete static_cast<T *>(asset); }
};
template <class T> struct AssetEvent {                          // App event; registered on first use of T
    // Loaded: Ready (first time, or after Failed); Failed; Modified: reloaded or mark_modified;
    // Removed: unloaded (the id is dead).
    enum class Kind : std::uint8_t { Loaded, Failed, Modified, Removed };
    Kind kind = Kind::Loaded;
    AssetId<T> id;
};
struct AssetsConfig {
    std::string root;                                           // "" -> default resolution order
    bool hot_reload = LAM_DEV != 0;                             // forced off in synchronous mode
    float watch_interval_seconds = 0.5f;                        // hot reload: poll period per file
    float finalize_budget_ms = 4.0f;                            // main-thread finalize time per frame
    bool synchronous = false;                                   // forced on when options().deterministic()
};
template <class T> class Handle;
class AssetServer final : public AssetResolver {               // App resource (AssetsPlugin)
public:
    AssetServer(App &app, AssetsConfig config);                // resolves the root, starts the worker
    ~AssetServer() override;                                    // no unload callbacks run here
    AssetServer(const AssetServer &) = delete;
    AssetServer &operator=(const AssetServer &) = delete;

    // Registers `loader` for (T, each of its extensions); the last registration wins.
    template <class T> void register_loader(std::unique_ptr<AssetLoader<T>> loader) {
        ensure_event<T>();
        register_erased(std::move(loader));
    }
    // Starts (or finds, cached by type + path) a load retained by the current scope. Returns at
    // once; state() becomes Ready after finalize (immediately in synchronous mode).
    template <class T> AssetId<T> load(std::string_view path) { return load<T>(path, current_scope()); }
    template <class T> AssetId<T> load(std::string_view path, AssetScope scope) {
        ensure_event<T>();
        return AssetId<T>::from_bits(load_erased(type_id<T>(), path, scope));
    }
    // Loads synchronously (decode + dependencies + finalize). Startup and tests.
    template <class T> Result<AssetId<T>> load_now(std::string_view path) {
        return load_now<T>(path, current_scope());
    }
    template <class T> Result<AssetId<T>> load_now(std::string_view path, AssetScope scope) {
        ensure_event<T>();
        Result<std::uint64_t> r = load_now_erased(type_id<T>(), path, scope);
        if (!r) return r.error();
        return AssetId<T>::from_bits(r.value());
    }
    template <class T> Handle<T> load_handle(std::string_view path);   // explicit refcount (rare)
    // Runtime-made asset, Ready at once. A non-empty `name` lets load<T>(name), prefabs and saves
    // find it while it lives, so add it before loading prefabs that use it.
    template <class T> AssetId<T> add(T value, std::string_view name = {}) {
        return add<T>(std::move(value), name, current_scope());
    }
    template <class T> AssetId<T> add(T value, std::string_view name, AssetScope scope) {
        ensure_event<T>();
        return AssetId<T>::from_bits(
            add_erased(type_id<T>(), new T(std::move(value)), &destroy_value<T>, name, scope));
    }
    // Null unless Ready. The pointer is valid until the end of the current stage.
    template <class T> const T *get(AssetId<T> id) const {
        return static_cast<const T *>(get_erased(type_id<T>(), id.bits()));
    }
    // Like get(); for in-place edits, followed by mark_modified(id).
    template <class T> T *get_mut(AssetId<T> id) {
        return static_cast<T *>(get_mut_erased(type_id<T>(), id.bits()));
    }
    // Call after editing a Ready asset through get_mut: bumps version() and emits Modified.
    template <class T> void mark_modified(AssetId<T> id) { mark_modified_erased(type_id<T>(), id.bits()); }
    // None for null, stale or unloaded ids.
    template <class T> LoadState state(AssetId<T> id) const { return state_erased(type_id<T>(), id.bits()); }
    // The error while Failed, else null.
    template <class T> const Error *error(AssetId<T> id) const { return error_erased(type_id<T>(), id.bits()); }
    // The normalised path, or the add() name; "" for unnamed runtime assets and unknown ids.
    template <class T> std::string_view path_of(AssetId<T> id) const { return path_of(type_id<T>(), id.bits()); }
    template <class T> std::uint32_t version(AssetId<T> id) const {
        return version_erased(type_id<T>(), id.bits());         // ++ on reload and mark_modified
    }
    // Blocking reload: decode a new value, swap it in, reload dependants; on error the old value
    // stays. Busy while the first load is in flight, Unsupported for add()ed assets.
    template <class T> Status reload(AssetId<T> id) { return reload_erased(type_id<T>(), id.bits()); }

    // Scopes
    AssetScope begin_scope();
    void end_scope(AssetScope scope);                           // releases its retains; GC at next update()
    // The scope that load(path) / add(value) without a scope retain into (kRootScope by default).
    void set_current_scope(AssetScope scope);
    AssetScope current_scope() const;

    bool all_ready() const;                                     // no load or reload in flight
    std::size_t pending() const override;                       // loads and reloads in flight
    // Loads held by `scope` (directly or as deps) that are neither Ready nor Failed. Reloads of
    // Ready assets don't count.
    std::size_t pending(AssetScope scope) const;
    void wait_all();                                            // main thread: until pending() == 0
    std::string resolve_path(std::string_view relative) const;  // absolute path under root()
    const std::string &root() const;                            // the resolved asset root
    bool synchronous() const;                                   // deterministic runs, AssetsConfig::synchronous
    void collect_garbage();                                     // unload assets nothing retains
    void update();                                              // engine: FrameBegin -3000

    // AssetResolver
    std::uint64_t resolve(TypeId type, std::string_view path) override;
    std::string_view path_of(TypeId type, std::uint64_t bits) const override;
    Result<std::string> read_text(std::string_view path) const override;

    // Type-erased core behind the templates above (implemented in src/assets); prefer those.
    using EmitFn = void (*)(App &app, std::uint8_t kind, std::uint64_t bits);
    using DestroyFn = void (*)(void *value);
    void register_type(TypeId type, EmitFn emit);               // idempotent
    void register_erased(std::unique_ptr<AssetLoaderBase> loader);
    std::uint64_t load_erased(TypeId type, std::string_view path, AssetScope scope);
    Result<std::uint64_t> load_now_erased(TypeId type, std::string_view path, AssetScope scope);
    std::uint64_t add_erased(TypeId type, void *value, DestroyFn destroy, std::string_view name, AssetScope scope);
    const void *get_erased(TypeId type, std::uint64_t bits) const;
    void *get_mut_erased(TypeId type, std::uint64_t bits);
    void mark_modified_erased(TypeId type, std::uint64_t bits);
    LoadState state_erased(TypeId type, std::uint64_t bits) const;
    const Error *error_erased(TypeId type, std::uint64_t bits) const;
    std::uint32_t version_erased(TypeId type, std::uint64_t bits) const;
    Status reload_erased(TypeId type, std::uint64_t bits);
    void retain_erased(TypeId type, std::uint64_t bits);
    void release_erased(TypeId type, std::uint64_t bits);
private:
    template <class T> void ensure_event() {
        if (!app_->events().try_get<AssetEvent<T>>()) app_->add_event<AssetEvent<T>>(EventScope::App);
        register_type(type_id<T>(), &emit_event<T>);
    }
    template <class T> static void emit_event(App &app, std::uint8_t kind, std::uint64_t bits) {
        app.events().get<AssetEvent<T>>().emit(
            AssetEvent<T>{static_cast<typename AssetEvent<T>::Kind>(kind), AssetId<T>::from_bits(bits)});
    }
    template <class T> static void destroy_value(void *p) { delete static_cast<T *>(p); }
    struct Impl;
    App *app_;
    std::unique_ptr<Impl> impl_;
};
// Explicit strong reference (main thread). Most code relies on scopes. The server must outlive it.
template <class T> class Handle {
public:
    Handle() = default;
    Handle(AssetServer &server, AssetId<T> id) : server_(&server), id_(id) { retain(); }
    Handle(const Handle &o) : server_(o.server_), id_(o.id_) { retain(); }
    Handle(Handle &&o) noexcept : server_(o.server_), id_(o.id_) { o.server_ = nullptr; o.id_ = {}; }
    Handle &operator=(const Handle &o) {
        if (this != &o) { release(); server_ = o.server_; id_ = o.id_; retain(); }
        return *this;
    }
    Handle &operator=(Handle &&o) noexcept {
        if (this != &o) { release(); server_ = o.server_; id_ = o.id_; o.server_ = nullptr; o.id_ = {}; }
        return *this;
    }
    ~Handle() { release(); }
    AssetId<T> id() const { return id_; }
    explicit operator bool() const { return static_cast<bool>(id_); }
private:
    void retain() { if (server_ && id_) server_->retain_erased(type_id<T>(), id_.bits()); }
    void release() { if (server_ && id_) server_->release_erased(type_id<T>(), id_.bits()); }
    AssetServer *server_ = nullptr;
    AssetId<T> id_;
};
template <class T> Handle<T> AssetServer::load_handle(std::string_view path) {
    return Handle<T>(*this, load<T>(path));
}
struct TextAsset { std::string text; };                         // ".txt", built-in loader
class AssetsPlugin final : public Plugin {
public:
    explicit AssetsPlugin(AssetsConfig c = {});
    std::string_view name() const override { return "lam.assets"; }
    // Inserts AssetServer, the TextAsset loader, asset_update (FrameBegin -3000) and, in sync
    // mode, asset_tick_collect (TickBegin -3000).
    void build(App &app) override;
    void shutdown(App &app) override;                           // unloads everything
private:
    AssetsConfig config_;
};
}
