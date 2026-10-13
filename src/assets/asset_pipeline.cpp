// Load pipeline (main thread): load -> job -> decode -> on_decoded -> finalize -> Ready/Failed.
// Both modes finalize top-level loads in call order, each after its deps. Async mode stops at the
// first tree that isn't decoded yet, so event order and atlas packing never depend on IO timing.
#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lam/core/assert.h>
#include <lam/core/log.h>

#include "detail/asset_internal.h"

namespace lam {

using detail::AssetRef;
using detail::FinalizeBudget;
using detail::Job;
using detail::Lookup;
using detail::Slot;
using detail::TreeState;
using detail::TypeTable;

std::uint64_t AssetServer::Impl::load(TypeId type, std::string_view path, AssetScope scope) {
    LAM_ASSERT(on_main_thread(),
               "AssetServer::load is main-thread only (decode code uses LoadContext::depend)");
    const std::string key = detail::normalize_asset_path(path);
    if (key.empty()) {
        LAM_LOG_WARN("assets: load of an empty path");
        return 0;
    }
    table(type);
    Lookup lk;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        lk = lookup_locked(type, key, true, false);
    }
    if (!lk.ok) {
        LAM_LOG_WARN("assets: no loader for '{}' as this asset type", key);
        return 0;
    }
    bool fresh = false;
    if (!materialised(lk.ref, lk.generation)) {
        materialise(lk.ref, lk.generation, key, lk.loader);
        Job &job = make_job(lk.ref, false);
        fresh = true;
        if (!synchronous) {
            submit(job, false);
            roots.push_back({lk.ref, lk.generation});
        }
    } else if (!synchronous && at(lk.ref).job) {
        roots.push_back({lk.ref, lk.generation}); // stays reachable if its parent fails
    }
    retain_in_scope(lk.ref, scope);
    if (fresh && synchronous) load_inline(lk.ref);
    return detail::asset_bits(lk.ref.index, lk.generation);
}

Job &AssetServer::Impl::make_job(AssetRef r, bool reload) {
    Slot &s = at(r);
    LAM_ASSERT(!s.job, "AssetServer: a second job for one asset");
    s.job = std::make_unique<Job>();
    Job &job = *s.job;
    job.ref = r;
    job.generation = s.generation;
    job.path = s.path;
    job.loader = s.loader;
    job.reload = reload;
    job.watch = hot_reload;
    if (reload) { // last registration wins, also for reloads
        const std::lock_guard<std::mutex> lock(mutex);
        if (AssetLoaderBase *l = find_loader_locked(*types[r.type], s.path)) job.loader = l;
    }
    ++in_flight;
    return job;
}

void AssetServer::Impl::submit(Job &job, bool priority) {
    if (synchronous) return;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        enqueue_locked(&job, priority);
    }
    work_cv.notify_one();
}

void AssetServer::Impl::on_decoded(Job &job) {
    job.decoded = true;
    Slot &s = at(job.ref);
    if (!job.reload) s.state = LoadState::Loading;
    std::vector<Job *> fresh;
    if (!job.error) {
        for (const detail::DepRecord &rec : job.dep_records) {
            if (!materialised(rec.ref, rec.generation)) {
                materialise(rec.ref, rec.generation, rec.path, rec.loader);
                fresh.push_back(&make_job(rec.ref, false));
            }
            retain(rec.ref);
            job.deps.push_back(rec.ref);
        }
    }
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (const detail::DepRecord &rec : job.dep_records) {
            TypeTable &t = *types[rec.ref.type];
            detail::RegEntry &e = t.reg[rec.ref.index];
            if (e.pins > 0) --e.pins;
            // A dependency only this failed decode asked for: never loaded, so forget it.
            if (job.error && e.pins == 0 && e.live && !materialised(rec.ref, rec.generation)) {
                if (const auto it = t.by_path.find(rec.path);
                    it != t.by_path.end() && it->second == rec.ref.index) {
                    t.by_path.erase(it);
                }
                e.live = false;
                if (detail::advance_generation(e.generation)) t.free.push_back(rec.ref.index);
            }
        }
        if (!synchronous) {
            for (Job *j : fresh)
                enqueue_locked(j, true);
        }
    }
    if (!synchronous && !fresh.empty()) work_cv.notify_one();
    job.dep_records.clear();
}

void AssetServer::Impl::load_inline(AssetRef r) {
    Job &job = *at(r).job;
    if (!job.reload) at(r).state = LoadState::Loading;
    decode_job(job);
    on_decoded(job);
    if (job.error) {
        Error e = *job.error;
        fail_job(r, std::move(e));
        return;
    }
    // Dependencies depth first, in declaration order. The first one not Ready (failed, or on the
    // stack: a cycle) fails the parent. `visiting` catches cycles even while r serves an old value.
    at(r).visiting = true;
    for (std::size_t i = 0; i < job.deps.size(); ++i) {
        const AssetRef d = job.deps[i];
        if (at(d).job && !at(d).job->decoded) load_inline(d);
        if (at(d).state != LoadState::Ready || at(d).visiting) break;
    }
    at(r).visiting = false;
    finalize_job(r);
}

TreeState AssetServer::Impl::finalize_tree(AssetRef r, bool as_dep, FinalizeBudget &budget) {
    Slot &s = at(r);
    if (!s.job) return TreeState::Done;
    // Up the stack: a cycle, also when the asset on the stack is a Ready one being reloaded (a
    // reload whose new content depends on its own dependant would otherwise retain itself).
    if (s.visiting) return TreeState::Cycle;
    if (as_dep && s.state == LoadState::Ready) return TreeState::Done; // reloading: old value serves
    Job &job = *s.job;
    if (!job.decoded) return TreeState::Blocked;
    if (job.error) {
        Error e = *job.error;
        fail_job(r, std::move(e));
        return TreeState::Done;
    }
    s.visiting = true;
    for (std::size_t i = 0; i < job.deps.size(); ++i) {
        const AssetRef d = job.deps[i];
        const TreeState t = finalize_tree(d, true, budget);
        if (t == TreeState::Cycle) {
            s.visiting = false;
            fail_job(r, make_error(ErrorCode::Invalid,
                                   job.path + ": dependency cycle through '" + at(d).path + "'"));
            return TreeState::Done;
        }
        if (t != TreeState::Done) {
            s.visiting = false;
            return t;
        }
        // A failed dependency fails the parent at once (finalize_job reports it), as in
        // synchronous mode: the parent does not wait for its later dependencies to decode.
        if (at(d).state == LoadState::Failed) break;
    }
    s.visiting = false;
    if (budget.exhausted()) return TreeState::OutOfBudget;
    finalize_job(r);
    ++budget.finalized;
    return TreeState::Done;
}

void AssetServer::Impl::finalize_job(AssetRef r) {
    Slot &s = at(r);
    Job &job = *s.job;
    for (const AssetRef &d : job.deps) {
        const Slot &ds = at(d);
        if (ds.visiting) { // up the stack (synchronous mode; asynchronous mode found it earlier)
            fail_job(r, make_error(ErrorCode::Invalid,
                                   job.path + ": dependency cycle through '" + ds.path + "'"));
            return;
        }
        if (ds.state == LoadState::Failed) {
            fail_job(r, make_error(ErrorCode::NotFound, job.path + ": dependency '" + ds.path +
                                                            "' failed: " + ds.error.message));
            return;
        }
        if (ds.state != LoadState::Ready) {
            fail_job(r, make_error(ErrorCode::Invalid,
                                   job.path + ": dependency cycle through '" + ds.path + "'"));
            return;
        }
        // A reload may depend on an asset that already depends on r: finalizing it would close a
        // cycle of Ready assets (never collected, and reload_dependants would ping-pong).
        if (job.reload && depends_on(d, r)) {
            fail_job(r, make_error(ErrorCode::Invalid,
                                   job.path + ": dependency cycle through '" + ds.path + "'"));
            return;
        }
    }
    FinalizeContext fc{app, self, s.path};
    // Runtime assets the loader makes (add<T>(value) into the current scope) and loads it starts
    // are retained by r, as dependencies (retain_in_scope, kFinalizeScope).
    Job *const outer_job = finalizing;
    const AssetScope outer_scope = current;
    finalizing = &job;
    current = detail::kFinalizeScope;
    ++finalize_depth;
    Status st = job.loader->finalize_erased(job.value, fc);
    --finalize_depth;
    finalizing = outer_job;
    const bool outer_alive =
        outer_scope == detail::kFinalizeScope || scope_record(outer_scope) != nullptr;
    current = outer_alive ? outer_scope : kRootScope; // the root if the loader ended that scope
    if (!st) {
        Error e = st.error();
        e.message = detail::name_file(job.path, e.message);
        fail_job(r, std::move(e));
        return;
    }
    succeed_job(r);
}

// True if `target` is `from` or reachable through live dependencies. Cold path.
// The graph stays acyclic because reloads are checked here before they finalize.
bool AssetServer::Impl::depends_on(AssetRef from, AssetRef target) const {
    std::vector<AssetRef> stack{from};
    std::vector<AssetRef> seen;
    while (!stack.empty()) {
        const AssetRef a = stack.back();
        stack.pop_back();
        if (a == target) return true;
        if (std::find(seen.begin(), seen.end(), a) != seen.end()) continue;
        seen.push_back(a);
        for (const AssetRef &d : at(a).deps)
            stack.push_back(d);
    }
    return false;
}

void AssetServer::Impl::fail_job(AssetRef r, Error error) {
    Slot &s = at(r);
    std::unique_ptr<Job> job = std::move(s.job);
    --in_flight;
    if (job->value != nullptr) job->loader->destroy_erased(job->value);
    job->value = nullptr;
    if (hot_reload) {
        // Failed deps are released and collected below, so watch their files for r too:
        // fixing a broken dependency then reloads r.
        for (const AssetRef &d : job->deps) {
            if (at(d).state == LoadState::Ready) continue;
            for (const detail::Watch &w : watches)
                if (w.ref == d) job->files.push_back({w.file.string(), w.mtime});
        }
    }
    release_job_deps(job->deps);
    if (job->result != nullptr) {
        job->result->status = error;
        job->result->done = true;
    }
    if (hot_reload) watch(r, job->files);
    if (job->reload) {
        LAM_LOG_ERROR("assets: reload failed: {}", error.message);
        if (s.state != LoadState::Ready) { // a Ready asset keeps serving its old value
            s.state = LoadState::Failed;
            s.error = std::move(error);
            emit(r.type, detail::kAssetFailed, bits_of(r));
        }
    } else {
        s.state = LoadState::Failed;
        s.error = std::move(error);
        LAM_LOG_ERROR("assets: {}", s.error.message);
        emit(r.type, detail::kAssetFailed, bits_of(r));
    }
    after_job(r);
}

void AssetServer::Impl::succeed_job(AssetRef r) {
    Slot &s = at(r);
    std::unique_ptr<Job> job = std::move(s.job);
    --in_flight;
    const std::uint64_t bits = bits_of(r);
    if (job->reload) {
        const LoadState before = s.state;
        if (s.value != nullptr) { // the old value goes first
            FinalizeContext fc{app, self, s.path};
            ++finalize_depth;
            s.loader->unload_erased(s.value, fc);
            s.loader->destroy_erased(s.value);
            --finalize_depth;
        }
        Slot &slot = at(r);
        std::vector<AssetRef> old_deps = std::move(slot.deps);
        slot.value = job->value;
        slot.loader = job->loader;
        slot.deps = std::move(job->deps);
        slot.state = LoadState::Ready;
        slot.error = {};
        ++slot.version;
        job->value = nullptr;
        for (const AssetRef &d : old_deps)
            release(d);
        if (job->result != nullptr) job->result->done = true;
        if (hot_reload) watch(r, job->files);
        emit(r.type, before == LoadState::Ready ? detail::kAssetModified : detail::kAssetLoaded,
             bits);
        reload_dependants(r);
    } else {
        s.value = job->value;
        s.deps = std::move(job->deps);
        s.state = LoadState::Ready;
        s.version = 1;
        job->value = nullptr;
        if (hot_reload) watch(r, job->files);
        emit(r.type, detail::kAssetLoaded, bits);
    }
    after_job(r);
}

void AssetServer::Impl::after_job(AssetRef r) {
    Slot &s = at(r);
    // collect() skips slots with a job in flight; one whose last reference went while it was
    // loading (its scope ended mid-load) is collected at the next update() instead.
    if (s.refs == 0 && !s.job) gc_pending = true;
    if (s.reload_requested && !s.job && s.state != LoadState::None) {
        s.reload_requested = false;
        request_reload(r);
    }
}

// Release a finished or failed job's dependencies. Sync mode drops one only this job queued, so
// pending() can't get stuck; async mode lets it finish and GC takes it.
void AssetServer::Impl::release_job_deps(std::vector<AssetRef> &deps) {
    for (const AssetRef &d : deps) {
        release(d);
        Slot &ds = at(d);
        if (!ds.job) continue;
        if (synchronous) {
            if (ds.refs == 0 && !ds.job->decoded && !ds.job->reload) discard_unloaded(d);
        } else {
            roots.push_back({d, ds.generation});
        }
    }
    deps.clear();
}

void AssetServer::Impl::discard_unloaded(AssetRef r) {
    Slot &s = at(r);
    s.job.reset();
    --in_flight;
    release_index(r);
    Slot &slot = at(r);
    slot.state = LoadState::None;
    slot.loader = nullptr;
    slot.path.clear();
    slot.scopes.clear();
    slot.deps.clear();
    slot.refs = 0;
    slot.version = 0;
    slot.error = {};
    slot.reload_requested = false;
}

void AssetServer::Impl::request_reload(AssetRef r) {
    Slot &s = at(r);
    if (s.state == LoadState::None || s.runtime) return;
    if (s.job) {
        if (!synchronous && !s.job->decoded) {
            const std::lock_guard<std::mutex> lock(mutex);
            if (!s.job->started) return; // still queued: its decode will read the newest file
        }
        s.reload_requested = true; // coalesce: one more reload once the job in flight completes
        return;
    }
    Job &job = make_job(r, true);
    if (synchronous) {
        load_inline(r);
        return;
    }
    submit(job, false);
    roots.push_back({r, s.generation});
}

Status AssetServer::Impl::reload_now(AssetRef r) {
    Slot &s = at(r);
    if (s.runtime) {
        return make_error(ErrorCode::Unsupported, "reload: '" + s.path + "' was made at runtime");
    }
    if (s.job && !s.job->reload) {
        return make_error(ErrorCode::Busy, "reload: '" + s.path + "' is loading");
    }
    if (s.job) { // a hot reload in flight: let it land, this reload supersedes queued requests
        s.reload_requested = false;
        wait_for(r, nullptr);
        if (at(r).job) return make_error(ErrorCode::Busy, "reload: '" + s.path + "' is loading");
    }
    detail::JobResult result;
    Job &job = make_job(r, true);
    job.result = &result;
    if (synchronous) {
        load_inline(r);
    } else {
        submit(job, true);
        roots.push_back({r, at(r).generation});
        wait_for(r, &result);
    }
    if (!result.done) {
        return make_error(ErrorCode::Busy,
                          "reload: '" + at(r).path + "' could not complete (server stopping)");
    }
    return result.status;
}

// Dependants re-finalize by reloading (decode + finalize against the new dependency), so a
// loader never sees finalize twice on one value. Ready parents only; type and index order.
void AssetServer::Impl::reload_dependants(AssetRef r) {
    std::vector<AssetRef> parents;
    for (TypeId type = 0; type < types.size(); ++type) {
        if (!types[type]) continue;
        const TypeTable &t = *types[type];
        for (std::uint32_t index = 1; index < t.slots.size(); ++index) {
            const Slot &p = t.slots[index];
            if (p.state != LoadState::Ready || p.runtime) continue;
            if (std::find(p.deps.begin(), p.deps.end(), r) != p.deps.end())
                parents.push_back({type, index});
        }
    }
    for (const AssetRef &p : parents) {
        if (at(p).state == LoadState::Ready) request_reload(p);
    }
}

// ---- garbage collection ------------------------------------------------------------------------

void AssetServer::Impl::unload_slot(AssetRef r) {
    Slot &s = at(r);
    const std::uint64_t bits = bits_of(r);
    if (s.value != nullptr) {
        ++finalize_depth;
        if (s.runtime) {
            if (s.destroy != nullptr) s.destroy(s.value);
        } else {
            FinalizeContext fc{app, self, s.path};
            s.loader->unload_erased(s.value, fc);
            s.loader->destroy_erased(s.value);
        }
        --finalize_depth;
    }
    Slot &slot = at(r);
    slot.value = nullptr;
    std::vector<AssetRef> deps = std::move(slot.deps);
    if (hot_reload) unwatch(r);
    emit(r.type, detail::kAssetRemoved, bits);
    Slot &cleared = at(r); // emit may have run game code
    cleared.destroy = nullptr;
    cleared.loader = nullptr;
    cleared.runtime = false;
    cleared.state = LoadState::None;
    cleared.error = {};
    cleared.deps.clear();
    cleared.scopes.clear();
    cleared.refs = 0;
    cleared.version = 0;
    cleared.reload_requested = false;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        detail::RegEntry &e = types[r.type]->reg[r.index];
        e.live = false;
        e.pins = 0;
        if (detail::advance_generation(e.generation)) // else exhausted: retired
            types[r.type]->free.push_back(r.index);
        cleared.generation = e.generation;
    }
    cleared.path.clear();
    for (const AssetRef &d : deps)
        release(d);
}

void AssetServer::Impl::collect() {
    gc_pending = false;
    // Repeat until stable: unloading a parent can drop its dependencies to zero.
    for (bool changed = true; changed;) {
        changed = false;
        for (TypeId type = 0; type < types.size(); ++type) {
            if (!types[type]) continue;
            for (std::uint32_t index = 1; index < types[type]->slots.size(); ++index) {
                const Slot &s = types[type]->slots[index];
                if (s.refs != 0 || s.state == LoadState::None || s.job) continue;
                {
                    // Atomically with the worker's lookups: a pinned entry (a decode just got it
                    // from depend) stays; otherwise no lookup can find it from here on.
                    const std::lock_guard<std::mutex> lock(mutex);
                    TypeTable &t = *types[type];
                    detail::RegEntry &e = t.reg[index];
                    if (e.pins > 0) {
                        gc_pending = true; // retry at the next update()
                        continue;
                    }
                    if (const auto it = t.by_path.find(s.path);
                        it != t.by_path.end() && it->second == index) {
                        t.by_path.erase(it);
                    }
                }
                unload_slot({type, index});
                changed = true;
            }
        }
    }
}

// Destroys every remaining value without unload callbacks (the App may be half torn down). The
// worker has been joined, so no job is shared any more.
void AssetServer::Impl::destroy_all() {
    for (std::unique_ptr<TypeTable> &t : types) {
        if (!t) continue;
        for (Slot &s : t->slots) {
            if (s.job) {
                if (s.job->value != nullptr && s.job->loader != nullptr)
                    s.job->loader->destroy_erased(s.job->value);
                s.job.reset();
            }
            if (s.value == nullptr) continue;
            if (s.runtime) {
                if (s.destroy != nullptr) s.destroy(s.value);
            } else if (s.loader != nullptr) {
                s.loader->destroy_erased(s.value);
            }
            s.value = nullptr;
        }
    }
}

} // namespace lam
