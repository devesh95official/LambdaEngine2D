#pragma once
// Private AssetServer state shared by the lam_assets .cpp files. Main thread only, except members
// marked "guarded", which the worker also touches under Impl::mutex. The registry hands out ids and
// generations; the slot table holds values, states and refcounts.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <lam/assets/asset_root.h>
#include <lam/assets/asset_server.h>

namespace lam {
namespace detail {

// AssetEvent<T>::Kind values (asset_server.h), as stored by the type-erased emitter.
inline constexpr std::uint8_t kAssetLoaded = 0;
inline constexpr std::uint8_t kAssetFailed = 1;
inline constexpr std::uint8_t kAssetModified = 2;
inline constexpr std::uint8_t kAssetRemoved = 3;
// A slot whose generation reaches this value is retired, never reused.
inline constexpr std::uint32_t kRetiredGeneration = 0xFFFFFFFFu;
// Current scope during a loader's finalize(). What it adds or loads there is retained by the
// asset being finalized, as a dependency.
inline constexpr AssetScope kFinalizeScope = 0xFFFFFFFFu;

// A freed registry index gets the next generation. True if the index may be reused; false once
// the generation reaches kRetiredGeneration, which is never handed out (the slot is retired).
constexpr bool advance_generation(std::uint32_t &generation) {
    if (generation == kRetiredGeneration) return false;
    return ++generation != kRetiredGeneration;
}

constexpr std::uint64_t asset_bits(std::uint32_t index, std::uint32_t generation) {
    return index | (std::uint64_t(generation) << 32);
}
constexpr std::uint32_t asset_bits_index(std::uint64_t bits) {
    return std::uint32_t(bits & 0xFFFFFFFFu);
}
constexpr std::uint32_t asset_bits_generation(std::uint64_t bits) {
    return std::uint32_t(bits >> 32);
}

// ---- paths (asset_paths.cpp; cold path, allocate) -----------------------------------------------
// ASCII lower case.
std::string asset_to_lower(std::string_view s);
// "dir/file.png#16" -> "dir/file.png".
std::string_view asset_strip_fragment(std::string_view path);
// "/x" or "C:..." (drive letter).
bool asset_is_absolute(std::string_view path);
// Lexical normalisation: '\' -> '/', drops "." and empty components, folds "..", keeps a leading
// '/' and the "#fragment". "a/./b/../c.png#2" -> "a/c.png#2".
std::string normalize_asset_path(std::string_view path);
// `relative` next to `parent` (a root-relative asset path), normalised; absolute stays absolute.
std::string sibling_asset_path(std::string_view parent, std::string_view relative);

// ---- records ------------------------------------------------------------------------------------

// (type, index) of a slot. The generation is checked separately where it matters.
struct AssetRef {
    TypeId type = 0;
    std::uint32_t index = 0;
    bool operator==(const AssetRef &) const = default;
};

// One dependency declared by LoadContext::depend during a decode (any thread). The registry
// entry is pinned until the main thread converts the pin into the parent's retain (or drops it).
struct DepRecord {
    AssetRef ref;
    std::uint32_t generation = 0;
    std::string path;                       // normalised key
    AssetLoaderBase *loader = nullptr;      // the loader the registry chose for `path`
};

// A file a decode read (the asset itself, then LoadContext::read calls), watched for hot reload.
struct ReadFile {
    std::string absolute;
    std::int64_t mtime = -1;                // -1 = missing or unknown
};

// Outcome of a job that someone waits for (AssetServer::reload).
struct JobResult {
    Status status;
    bool done = false;
};

// One decode (load or reload) of one slot, owned by Slot::job until it finalizes or fails. The main
// thread fills the request, the decoding thread the result; both hand-offs go through the mutex.
struct Job {
    // request (main thread, before queueing)
    AssetRef ref;
    std::uint32_t generation = 0;
    std::string path;                       // normalised, root-relative (or absolute)
    AssetLoaderBase *loader = nullptr;
    bool reload = false;                    // the slot keeps serving its old value meanwhile
    JobResult *result = nullptr;            // reload(): filled when the job completes
    bool watch = false;                     // record file mtimes (hot reload on)
    // result (decoding thread)
    void *value = nullptr;                  // owned until finalize moves it into the slot
    std::optional<Error> error;
    std::optional<Error> dep_error;         // first depend() without a loader
    std::vector<DepRecord> dep_records;     // declaration order, unique, each pinned once
    std::vector<ReadFile> files;            // when `watch`
    // guarded: the worker took it (a queued job still reads the newest file)
    bool started = false;
    // main thread, after the decode came back
    bool decoded = false;
    std::vector<AssetRef> deps;             // retained once each (declaration order)
};

// Registry entry (guarded).
struct RegEntry {
    std::uint32_t generation = 1;           // live ids start at 1
    std::uint32_t pins = 0;                 // depend() results not yet seen by the main thread
    bool live = false;                      // allocated (by load, add or depend)
};

// Main-thread state of one asset.
struct Slot {
    std::uint32_t generation = 1;           // mirrors the registry while materialised
    LoadState state = LoadState::None;      // None = not materialised / free
    bool runtime = false;                   // made by add(); no loader
    bool visiting = false;                  // finalize DFS (cycle detection)
    bool reload_requested = false;          // a reload arrived while a job was in flight
    void *value = nullptr;                  // owned; visible only when Ready
    AssetServer::DestroyFn destroy = nullptr;   // runtime values
    AssetLoaderBase *loader = nullptr;      // loaded values
    std::string path;                       // normalised key; add()'s name
    std::uint32_t refs = 0;                 // scopes + handles + parents
    std::uint32_t version = 0;
    Error error;                            // valid when Failed
    std::vector<AssetRef> deps;             // retained dependencies of the live value
    std::vector<AssetScope> scopes;         // scopes holding a reference
    std::unique_ptr<Job> job;               // in flight (decode or finalize pending)
};

struct LoaderEntry {
    std::string extension;                  // lower case, with the dot
    AssetLoaderBase *loader = nullptr;
};

struct TypeTable {
    // main thread
    AssetServer::EmitFn emit = nullptr;
    std::deque<Slot> slots;                 // [0] unused; stable addresses
    // guarded by Impl::mutex
    std::vector<RegEntry> reg;              // [0] unused
    std::vector<std::uint32_t> free;        // reusable indices (LIFO)
    std::map<std::string, std::uint32_t, std::less<>> by_path;
    std::vector<LoaderEntry> loaders;       // longest extension first
};

struct ScopeRecord {
    AssetScope id = kRootScope;
    std::vector<AssetRef> entries;
};

// An entry of the finalize order: top-level loads and reloads in call order.
struct RootEntry {
    AssetRef ref;
    std::uint32_t generation = 0;
};

// A watched file of a Ready or Failed asset (hot reload, LAM_DEV, asynchronous mode).
struct Watch {
    AssetRef ref;
    std::uint32_t generation = 0;
    std::filesystem::path file;
    std::int64_t mtime = -1;
};

// Result of a lookup in the registry (guarded section).
struct Lookup {
    bool ok = false;                        // false: no loader for this (type, extension)
    bool created = false;                   // a new registry entry
    AssetRef ref;
    std::uint32_t generation = 0;
    AssetLoaderBase *loader = nullptr;
};

// Finalize budget of one update() (finalize_budget_ms) or unlimited (wait_all, load_now).
struct FinalizeBudget {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    double seconds = 0.0;
    bool unlimited = true;
    int finalized = 0;                      // at least one finalize per update always runs
    bool exhausted() const {
        if (unlimited || finalized == 0) return false;
        const std::chrono::duration<double> used = std::chrono::steady_clock::now() - start;
        return used.count() >= seconds;
    }
};

// Outcome of finalize_tree.
enum class TreeState : std::uint8_t { Done, Blocked, OutOfBudget, Cycle };

// mtime of a file in nanoseconds since the file clock epoch; -1 if missing. Allocation-free for a
// prepared path (the hot-reload poll runs every frame).
std::int64_t file_mtime_ns(const std::filesystem::path &file);

// "<path>: <message>", unless the loader's message already starts with the path (parsers that
// name their file): the file is named once.
inline std::string name_file(const std::string &path, const std::string &message) {
    if (message.size() > path.size() && message.compare(0, path.size(), path) == 0 && message[path.size()] == ':')
        return message;
    return path + ": " + message;
}

} // namespace detail

struct AssetServer::Impl {
    using AssetRef = detail::AssetRef;
    using Slot = detail::Slot;
    using Job = detail::Job;
    using TypeTable = detail::TypeTable;

    App &app;
    AssetServer &self;
    AssetsConfig config;
    std::string root;
    AssetRootSource root_source = AssetRootSource::Config;
    bool synchronous = true;
    bool hot_reload = false;
    std::thread::id main_thread = std::this_thread::get_id();

    std::vector<std::unique_ptr<AssetLoaderBase>> loaders;  // owned forever; jobs point into them
    std::vector<std::unique_ptr<TypeTable>> types;          // by TypeId; grown under `mutex`
    std::vector<detail::ScopeRecord> scopes;                // [0] = root
    AssetScope next_scope = 1;
    AssetScope current = kRootScope;
    std::size_t in_flight = 0;                              // slots with a job
    bool gc_pending = false;
    int finalize_depth = 0;                                 // >0 while loader callbacks run
    Job *finalizing = nullptr;                              // its finalize() runs (kFinalizeScope)
    std::vector<detail::RootEntry> roots;                   // finalize order (asynchronous mode)
    std::size_t roots_head = 0;

    // ---- worker (asynchronous mode) ----
    mutable std::mutex mutex;                               // guards everything marked "guarded"
    std::condition_variable work_cv;                        // worker: queued work or stop
    std::condition_variable done_cv;                        // main: a job came back or idle
    std::deque<Job *> dep_queue;                            // guarded; dependencies and waits first
    std::deque<Job *> root_queue;                           // guarded; top-level loads, FIFO
    std::vector<Job *> done;                                // guarded; decoded, not yet seen
    std::vector<Job *> drained;                             // main thread swap buffer
    Job *current_job = nullptr;                             // guarded; being decoded
    bool stop = false;                                      // guarded
    bool worker_running = false;                            // main thread
    std::atomic<bool> done_ready{false};                    // `done` non-empty (cheap poll)
    std::thread worker;

    // ---- hot reload ----
    std::vector<detail::Watch> watches;
    std::size_t watch_cursor = 0;
    double watch_carry = 0.0;
    std::chrono::steady_clock::time_point last_poll{};
    bool have_last_poll = false;

    Impl(App &a, AssetServer &s, AssetsConfig c);

    bool on_main_thread() const { return std::this_thread::get_id() == main_thread; }

    // ---- registry and slots (asset_registry.cpp) ----
    TypeTable &table(TypeId type);                          // creates (under mutex)
    TypeTable *table_if(TypeId type) const;                 // main thread or under mutex
    Slot *find(TypeId type, std::uint64_t bits);
    const Slot *find(TypeId type, std::uint64_t bits) const;
    Slot &at(AssetRef r) { return types[r.type]->slots[r.index]; }
    const Slot &at(AssetRef r) const { return types[r.type]->slots[r.index]; }
    std::uint64_t bits_of(AssetRef r) const { return detail::asset_bits(r.index, at(r).generation); }
    bool materialised(AssetRef r, std::uint32_t generation) const;
    Slot &slot_storage(AssetRef r);                         // grows the slot table to r.index
    // Guarded section: call with `mutex` held.
    AssetLoaderBase *find_loader_locked(const TypeTable &t, std::string_view path) const;
    std::uint32_t alloc_index_locked(TypeTable &t);
    detail::Lookup lookup_locked(TypeId type, const std::string &key, bool allow_create, bool pin);
    // Turns a registry entry into a materialised Queued slot (no job yet).
    Slot &materialise(AssetRef r, std::uint32_t generation, const std::string &path,
                      AssetLoaderBase *loader);
    // Frees a materialised slot's index: by_path entry, registry entry, new generation.
    void release_index(AssetRef r);
    void emit(TypeId type, std::uint8_t kind, std::uint64_t bits);

    // ---- refcounts and scopes (asset_registry.cpp) ----
    detail::ScopeRecord *scope_record(AssetScope scope);
    void retain(AssetRef r) { ++at(r).refs; }
    void release(AssetRef r);
    void retain_in_scope(AssetRef r, AssetScope scope);
    void end_scope(AssetScope scope);

    // ---- loading (asset_pipeline.cpp) ----
    std::uint64_t load(TypeId type, std::string_view path, AssetScope scope);
    Job &make_job(AssetRef r, bool reload);
    void submit(Job &job, bool priority);                   // asynchronous: to the worker
    void on_decoded(Job &job);                              // main: pins -> retains, dep jobs
    void load_inline(AssetRef r);                           // synchronous: decode, deps, finalize
    detail::TreeState finalize_tree(AssetRef r, bool as_dep, detail::FinalizeBudget &budget);
    void finalize_job(AssetRef r);                          // every dependency is Ready
    bool depends_on(AssetRef from, AssetRef target) const;  // through live dependencies
    void fail_job(AssetRef r, Error error);
    void succeed_job(AssetRef r);
    void after_job(AssetRef r);                             // coalesced reload requests
    void release_job_deps(std::vector<AssetRef> &deps);
    void discard_unloaded(AssetRef r);                      // Queued, never decoded, unreferenced
    void request_reload(AssetRef r);
    Status reload_now(AssetRef r);
    void reload_dependants(AssetRef r);
    void unload_slot(AssetRef r);
    void collect();
    void destroy_all();

    // ---- decode (asset_decode.cpp; any thread) ----
    void decode_job(Job &job);
    std::uint64_t depend(Job &job, TypeId type, const std::string &path);
    static std::uint64_t depend_thunk(void *impl, Job &job, TypeId type, const std::string &path);

    // ---- worker and waiting (asset_worker.cpp) ----
    void start_worker();
    void stop_worker();                                     // joins; cancels queued jobs
    void worker_main();
    void enqueue_locked(Job *job, bool priority);
    bool drain();                                           // done jobs -> on_decoded
    void process_roots(detail::FinalizeBudget &budget);
    bool wait_for_progress();                               // false: nothing can progress
    void prioritise(AssetRef r);
    void wait_for(AssetRef r, const detail::JobResult *result);
    void wait_all();

    // ---- hot reload (asset_hot_reload.cpp) ----
    void watch(AssetRef r, const std::vector<detail::ReadFile> &files);
    void unwatch(AssetRef r);
    void poll_watches();
};

} // namespace lam
