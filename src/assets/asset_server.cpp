// AssetServer public API and AssetsPlugin. Pipeline in asset_pipeline.cpp, worker in
// asset_worker.cpp, decoding in asset_decode.cpp, hot reload in asset_hot_reload.cpp.
// Ids are dense per type from 1; "#fragment" suffixes (font.ttf#16) are part of the cache key.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lam/assets/asset_root.h>
#include <lam/assets/asset_server.h>
#include <lam/core/assert.h>
#include <lam/core/fs.h>
#include <lam/core/log.h>

#include "detail/asset_internal.h"

namespace lam {

using detail::AssetRef;
using detail::Slot;

namespace {

// ".txt" -> TextAsset: the bytes as a string.
class TextLoader final : public AssetLoader<TextAsset> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExtensions[] = {".txt"};
        return kExtensions;
    }
    Result<TextAsset> decode(std::span<const std::byte> bytes, LoadContext &) override {
        TextAsset t;
        t.text.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        return t;
    }
};

void asset_update(Ctx &ctx) {
    ctx.res<AssetServer>().update();
}

// Sync mode also collects at every tick start, so GC follows the tick count and a recorded run
// (several ticks per frame) unloads the same assets on the same tick as its headless replay.
void asset_tick_collect(Ctx &ctx) {
    AssetServer &server = ctx.res<AssetServer>();
    if (server.synchronous()) server.update();
}

} // namespace

// ---- AssetServer -------------------------------------------------------------------------------

AssetServer::AssetServer(App &app, AssetsConfig config)
    : app_(&app), impl_(std::make_unique<Impl>(app, *this, std::move(config))) {
    Impl &m = *impl_;
    m.synchronous = m.config.synchronous || app.options().deterministic();
    m.config.synchronous = m.synchronous;
    if (m.synchronous || LAM_DEV == 0) m.config.hot_reload = false; // replay determinism
    m.hot_reload = m.config.hot_reload;
    AssetRootResult root = resolve_asset_root(m.config.root);
    m.root = std::move(root.path);
    m.root_source = root.source;
    m.scopes.push_back(detail::ScopeRecord{kRootScope, {}});
    m.start_worker();
    LAM_LOG_INFO("assets: root '{}' ({}), {}{}", m.root, asset_root_source_name(m.root_source),
                 m.synchronous ? "synchronous" : "asynchronous", m.hot_reload ? ", hot reload" : "");
}

AssetServer::~AssetServer() {
    impl_->stop_worker();
    impl_->destroy_all();
}

AssetScope AssetServer::begin_scope() {
    Impl &m = *impl_;
    const AssetScope id = m.next_scope++;
    m.scopes.push_back(detail::ScopeRecord{id, {}});
    return id;
}

// Ending kRootScope is shutdown (AssetsPlugin::shutdown): it stops the worker (loads still in
// flight are cancelled, not finalized; later loads run inline) and ends every other scope first.
void AssetServer::end_scope(AssetScope scope) {
    Impl &m = *impl_;
    if (scope == kRootScope) {
        m.stop_worker();
        while (m.scopes.size() > 1)
            m.end_scope(m.scopes.back().id);
    }
    m.end_scope(scope);
}

void AssetServer::set_current_scope(AssetScope scope) {
    Impl &m = *impl_;
    LAM_ASSERT(m.scope_record(scope) != nullptr, "AssetServer::set_current_scope: unknown scope");
    m.current = m.scope_record(scope) != nullptr ? scope : kRootScope;
}

AssetScope AssetServer::current_scope() const {
    return impl_->current;
}

bool AssetServer::all_ready() const {
    return impl_->in_flight == 0;
}

std::size_t AssetServer::pending() const {
    return impl_->in_flight;
}

// Walk the scope's entries and, transitively, the deps of live values and decoded in-flight jobs,
// each slot once. An undecoded job counts itself. Cold path.
std::size_t AssetServer::pending(AssetScope scope) const {
    const Impl &m = *impl_;
    if (m.in_flight == 0) return 0;
    const detail::ScopeRecord *rec = nullptr;
    for (const detail::ScopeRecord &r : m.scopes)
        if (r.id == scope) rec = &r;
    if (rec == nullptr) return 0;
    std::vector<detail::AssetRef> todo(rec->entries.begin(), rec->entries.end());
    std::vector<detail::AssetRef> seen;
    std::size_t count = 0;
    while (!todo.empty()) {
        const detail::AssetRef r = todo.back();
        todo.pop_back();
        if (std::find(seen.begin(), seen.end(), r) != seen.end()) continue;
        seen.push_back(r);
        const detail::Slot &s = m.at(r);
        if (s.job) {
            if (s.state != LoadState::Ready && s.state != LoadState::Failed) ++count;
            if (s.job->decoded) todo.insert(todo.end(), s.job->deps.begin(), s.job->deps.end());
        }
        todo.insert(todo.end(), s.deps.begin(), s.deps.end());
    }
    return count;
}

void AssetServer::wait_all() {
    impl_->wait_all();
}

std::string AssetServer::resolve_path(std::string_view relative) const {
    const std::string clean = detail::normalize_asset_path(detail::asset_strip_fragment(relative));
    if (detail::asset_is_absolute(clean)) return clean;
    return join_path(impl_->root, clean);
}

const std::string &AssetServer::root() const {
    return impl_->root;
}

bool AssetServer::synchronous() const {
    return impl_->synchronous;
}

void AssetServer::collect_garbage() {
    LAM_ASSERT(impl_->finalize_depth == 0, "collect_garbage called from a loader callback");
    impl_->collect();
}

void AssetServer::update() {
    Impl &m = *impl_;
    LAM_ASSERT(m.on_main_thread(), "AssetServer::update is main-thread only");
    if (!m.synchronous) {
        m.drain();
        detail::FinalizeBudget budget;
        budget.unlimited = false;
        budget.seconds = static_cast<double>(m.config.finalize_budget_ms) / 1000.0;
        m.process_roots(budget);
    }
    if (m.hot_reload) m.poll_watches();
    if (m.gc_pending) m.collect();
}

std::uint64_t AssetServer::resolve(TypeId type, std::string_view path) {
    return impl_->load(type, path, impl_->current);
}

// Normalised path or add() name; "" for unnamed runtime assets and unknown ids. A save naming a
// runtime asset only loads if that asset was made first.
std::string_view AssetServer::path_of(TypeId type, std::uint64_t bits) const {
    const Slot *s = impl_->find(type, bits);
    return s != nullptr ? std::string_view(s->path) : std::string_view{};
}

Result<std::string> AssetServer::read_text(std::string_view path) const {
    Result<std::string> text = read_file(resolve_path(path));
    if (!text)
        return make_error(text.error().code, std::string(path) + ": " + text.error().message);
    return text;
}

void AssetServer::register_type(TypeId type, EmitFn emit) {
    impl_->table(type).emit = emit;
}

void AssetServer::register_erased(std::unique_ptr<AssetLoaderBase> loader) {
    LAM_ASSERT(loader != nullptr, "AssetServer::register_loader: null loader");
    if (!loader) return;
    Impl &m = *impl_;
    detail::TypeTable &t = m.table(loader->type());
    {
        const std::lock_guard<std::mutex> lock(m.mutex);
        for (std::string_view ext : loader->extensions()) {
            LAM_ASSERT(ext.size() >= 2 && ext.front() == '.',
                       "asset loader extensions start with '.'");
            std::string lower = detail::asset_to_lower(ext);
            auto it = std::find_if(t.loaders.begin(), t.loaders.end(),
                                   [&](const detail::LoaderEntry &e) { return e.extension == lower; });
            if (it != t.loaders.end()) {
                it->loader = loader.get(); // last registration wins
            } else {
                t.loaders.push_back(detail::LoaderEntry{std::move(lower), loader.get()});
            }
        }
        std::stable_sort(t.loaders.begin(), t.loaders.end(),
                         [](const detail::LoaderEntry &a, const detail::LoaderEntry &b) {
                             return a.extension.size() > b.extension.size();
                         });
    }
    m.loaders.push_back(std::move(loader));
}

std::uint64_t AssetServer::load_erased(TypeId type, std::string_view path, AssetScope scope) {
    return impl_->load(type, path, scope);
}

Result<std::uint64_t> AssetServer::load_now_erased(TypeId type, std::string_view path,
                                                   AssetScope scope) {
    Impl &m = *impl_;
    const std::uint64_t bits = m.load(type, path, scope);
    if (bits == 0) {
        return make_error(ErrorCode::Unsupported,
                          "no loader for '" + std::string(path) + "' as this asset type");
    }
    Slot *s = m.find(type, bits);
    if (s != nullptr && s->job && !s->job->reload && !m.synchronous) {
        LAM_ASSERT(m.finalize_depth == 0, "AssetServer::load_now (asynchronous) called from a "
                                          "loader callback; use load()");
        m.wait_for(AssetRef{type, detail::asset_bits_index(bits)}, nullptr);
        s = m.find(type, bits);
    }
    if (s == nullptr) {
        return make_error(ErrorCode::NotFound, "'" + std::string(path) + "' was unloaded");
    }
    if (s->state == LoadState::Failed) return s->error;
    if (s->state != LoadState::Ready) {
        return make_error(ErrorCode::Busy,
                          "'" + s->path + "' is still loading (dependency cycle?)");
    }
    return bits;
}

std::uint64_t AssetServer::add_erased(TypeId type, void *value, DestroyFn destroy,
                                      std::string_view name, AssetScope scope) {
    Impl &m = *impl_;
    LAM_ASSERT(m.on_main_thread(), "AssetServer::add is main-thread only");
    detail::TypeTable &t = m.table(type);
    std::string key = name.empty() ? std::string{} : detail::normalize_asset_path(name);
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    {
        const std::lock_guard<std::mutex> lock(m.mutex);
        index = m.alloc_index_locked(t);
        generation = t.reg[index].generation;
        // A named runtime asset is found by load<T>(name), depend() and resolve() while it lives
        // (atlas pages, generated textures); path_of() reports the name.
        if (!key.empty()) t.by_path.insert_or_assign(key, index);
    }
    const AssetRef r{type, index};
    Slot &s = m.slot_storage(r);
    s.generation = generation;
    s.runtime = true;
    s.value = value;
    s.destroy = destroy;
    s.loader = nullptr;
    s.state = LoadState::Ready;
    s.version = 1;
    s.refs = 0;
    s.error = {};
    s.path = std::move(key);
    m.retain_in_scope(r, scope);
    const std::uint64_t bits = detail::asset_bits(index, generation);
    m.emit(type, detail::kAssetLoaded, bits);
    return bits;
}

const void *AssetServer::get_erased(TypeId type, std::uint64_t bits) const {
    const Slot *s = impl_->find(type, bits);
    return (s != nullptr && s->state == LoadState::Ready) ? s->value : nullptr;
}

void *AssetServer::get_mut_erased(TypeId type, std::uint64_t bits) {
    Slot *s = impl_->find(type, bits);
    return (s != nullptr && s->state == LoadState::Ready) ? s->value : nullptr;
}

void AssetServer::mark_modified_erased(TypeId type, std::uint64_t bits) {
    Slot *s = impl_->find(type, bits);
    if (s == nullptr || s->state != LoadState::Ready) return;
    ++s->version;
    impl_->emit(type, detail::kAssetModified, bits);
}

LoadState AssetServer::state_erased(TypeId type, std::uint64_t bits) const {
    const Slot *s = impl_->find(type, bits);
    return s != nullptr ? s->state : LoadState::None;
}

const Error *AssetServer::error_erased(TypeId type, std::uint64_t bits) const {
    const Slot *s = impl_->find(type, bits);
    return (s != nullptr && s->state == LoadState::Failed) ? &s->error : nullptr;
}

std::uint32_t AssetServer::version_erased(TypeId type, std::uint64_t bits) const {
    const Slot *s = impl_->find(type, bits);
    return s != nullptr ? s->version : 0;
}

// Decode, deps and finalize a new value, then swap it in and reload dependants. On failure a
// Ready asset keeps its old value. Blocks in async mode.
Status AssetServer::reload_erased(TypeId type, std::uint64_t bits) {
    Impl &m = *impl_;
    Slot *s = m.find(type, bits);
    if (s == nullptr) return make_error(ErrorCode::Invalid, "reload: unknown or unloaded asset id");
    LAM_ASSERT(m.finalize_depth == 0, "AssetServer::reload called from a loader callback");
    return m.reload_now(AssetRef{type, detail::asset_bits_index(bits)});
}

void AssetServer::retain_erased(TypeId type, std::uint64_t bits) {
    Slot *s = impl_->find(type, bits);
    LAM_ASSERT(s != nullptr, "AssetServer::retain: unknown or unloaded asset id");
    if (s != nullptr) ++s->refs;
}

void AssetServer::release_erased(TypeId type, std::uint64_t bits) {
    Slot *s = impl_->find(type, bits);
    if (s == nullptr) return; // already unloaded at shutdown
    impl_->release({type, detail::asset_bits_index(bits)});
}

// ---- AssetsPlugin ------------------------------------------------------------------------------

AssetsPlugin::AssetsPlugin(AssetsConfig c) : config_(std::move(c)) {}

void AssetsPlugin::build(App &app) {
    AssetServer &server = app.insert_resource<AssetServer>(app, config_);
    app.set_asset_resolver(&server);
    server.register_loader<TextAsset>(std::make_unique<TextLoader>());
    SystemDesc update;
    update.stage = Stage::FrameBegin;
    update.fn = &asset_update;
    update.name = "asset_update";
    update.order = -3000;
    update.scope = SystemScope::Once;
    app.add_system(update);
    if (server.synchronous()) {
        SystemDesc tick;
        tick.stage = Stage::TickBegin;
        tick.fn = &asset_tick_collect;
        tick.name = "asset_tick_collect";
        tick.order = -3000; // before scene_apply_tick (-2000) and the input systems (-1500..-1300)
        tick.scope = SystemScope::Once;
        app.add_system(tick);
    }
    // The startup tick hold is lam_app's: it reads AssetResolver::pending() after the
    // FrameBegin Once pass, so ticks wait until the startup loads have finalized.
}

void AssetsPlugin::shutdown(App &app) {
    AssetServer *server = app.try_res<AssetServer>();
    if (server == nullptr) return;
    server->end_scope(kRootScope); // stops the worker (pending loads cancelled), ends every scope
    server->collect_garbage();     // unload callbacks run here
    if (app.asset_resolver() == server) app.set_asset_resolver(nullptr);
}

} // namespace lam
