// The IO/decode worker and the main-thread side of it: queues, the done list, finalize of decoded
// roots, and the blocking waits (wait_all, load_now, reload).
//
// The worker only touches jobs it was handed, plus the registry under `mutex`. Jobs cross threads
// only via root_queue/dep_queue and `done`, under `mutex`. `done_ready` is a hint to skip the lock.
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <lam/core/assert.h>
#include <lam/core/log.h>

#include "detail/asset_internal.h"

namespace lam {

using detail::AssetRef;
using detail::FinalizeBudget;
using detail::Job;
using detail::Slot;
using detail::TreeState;

void AssetServer::Impl::start_worker() {
    if (synchronous || worker_running) return;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        stop = false;
    }
    drained.reserve(64);
    {
        const std::lock_guard<std::mutex> lock(mutex);
        done.reserve(64);
    }
    worker = std::thread([this] { worker_main(); });
    worker_running = true;
}

void AssetServer::Impl::worker_main() {
    for (;;) {
        Job *job = nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex);
            work_cv.wait(lock, [&] { return stop || !dep_queue.empty() || !root_queue.empty(); });
            if (stop) return;
            std::deque<Job *> &q = !dep_queue.empty() ? dep_queue : root_queue;
            job = q.front();
            q.pop_front();
            job->started = true;
            current_job = job;
        }
        decode_job(*job);
        {
            const std::lock_guard<std::mutex> lock(mutex);
            current_job = nullptr;
            done.push_back(job);
            done_ready.store(true, std::memory_order_release);
        }
        done_cv.notify_all();
    }
}

// Join the worker after its current decode and cancel the rest: queued jobs are dropped, decoded
// ones destroyed unfinalized and their slots marked Failed for GC. The server is synchronous after.
void AssetServer::Impl::stop_worker() {
    if (!worker_running) return;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        stop = true;
    }
    work_cv.notify_all();
    worker.join();
    worker_running = false;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        dep_queue.clear();
        root_queue.clear();
        done.clear();
        done_ready.store(false, std::memory_order_relaxed);
        // Pins of decodes that never reach on_decoded: nothing will convert them any more.
        for (std::unique_ptr<detail::TypeTable> &t : types) {
            if (!t) continue;
            for (detail::RegEntry &e : t->reg)
                e.pins = 0;
        }
    }
    for (TypeId type = 0; type < types.size(); ++type) {
        if (!types[type]) continue;
        for (std::uint32_t index = 1; index < types[type]->slots.size(); ++index) {
            Slot &s = types[type]->slots[index];
            if (!s.job) continue;
            std::unique_ptr<Job> job = std::move(s.job);
            --in_flight;
            if (job->value != nullptr) job->loader->destroy_erased(job->value);
            for (const AssetRef &d : job->deps)
                release({d.type, d.index});
            if (job->result != nullptr) {
                job->result->status = make_error(ErrorCode::Busy, "asset server stopped");
                job->result->done = true;
            }
            if (!job->reload) {
                s.state = LoadState::Failed;
                s.error = make_error(ErrorCode::Busy, s.path + ": cancelled (asset server stopped)");
            }
        }
    }
    roots.clear();
    roots_head = 0;
    synchronous = true; // later loads (shutdown code) run inline
    hot_reload = false;
}

void AssetServer::Impl::enqueue_locked(Job *job, bool priority) {
    if (priority) {
        dep_queue.push_back(job);
    } else {
        root_queue.push_back(job);
    }
}

bool AssetServer::Impl::drain() {
    if (!done_ready.load(std::memory_order_acquire)) return false;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        drained.swap(done);
        done_ready.store(false, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < drained.size(); ++i)
        on_decoded(*drained[i]);
    const bool any = !drained.empty();
    drained.clear();
    return any;
}

void AssetServer::Impl::process_roots(FinalizeBudget &budget) {
    while (roots_head < roots.size()) {
        const detail::RootEntry e = roots[roots_head];
        if (!materialised(e.ref, e.generation)) {
            ++roots_head;
            continue;
        }
        const TreeState t = finalize_tree(e.ref, false, budget);
        if (t != TreeState::Done && t != TreeState::Cycle) break; // strict order
        ++roots_head;
    }
    if (roots_head == roots.size()) {
        roots.clear();
        roots_head = 0;
    } else if (roots_head >= 64 && roots_head * 2 >= roots.size()) {
        roots.erase(roots.begin(), roots.begin() + static_cast<std::ptrdiff_t>(roots_head));
        roots_head = 0;
    }
}

// Blocks until the worker hands back a job. False when nothing can arrive: the worker is idle
// with empty queues (or stopped), so a caller that is still blocked would wait forever.
bool AssetServer::Impl::wait_for_progress() {
    if (synchronous || !worker_running) return false;
    std::unique_lock<std::mutex> lock(mutex);
    done_cv.wait(lock, [&] {
        return done_ready.load(std::memory_order_relaxed) || stop ||
               (current_job == nullptr && dep_queue.empty() && root_queue.empty());
    });
    return done_ready.load(std::memory_order_relaxed);
}

// Moves every undecoded job of r's dependency tree to the front of the worker's queue, in
// depth-first order, so a blocking wait does not sit behind unrelated loads.
void AssetServer::Impl::prioritise(AssetRef r) {
    std::vector<Job *> wanted;
    std::vector<AssetRef> stack{r};
    std::vector<AssetRef> seen;
    while (!stack.empty()) {
        const AssetRef a = stack.back();
        stack.pop_back();
        if (std::find(seen.begin(), seen.end(), a) != seen.end()) continue;
        seen.push_back(a);
        Slot &s = at(a);
        if (!s.job) continue;
        if (!s.job->decoded) {
            wanted.push_back(s.job.get());
            continue;
        }
        for (auto it = s.job->deps.rbegin(); it != s.job->deps.rend(); ++it)
            stack.push_back(*it);
    }
    if (wanted.empty()) return;
    const std::lock_guard<std::mutex> lock(mutex);
    for (auto it = wanted.rbegin(); it != wanted.rend(); ++it) {
        Job *job = *it;
        bool queued = false;
        for (std::deque<Job *> *q : {&dep_queue, &root_queue}) {
            const auto pos = std::find(q->begin(), q->end(), job);
            if (pos != q->end()) {
                q->erase(pos);
                queued = true;
            }
        }
        if (queued) dep_queue.push_front(job);
    }
}

// Main thread, asynchronous mode: until r's job (or, with `result`, that job) completes. Finalizes
// r's tree out of the root order, without budget: the caller asked for the asset now.
void AssetServer::Impl::wait_for(AssetRef r, const detail::JobResult *result) {
    LAM_ASSERT(on_main_thread(), "AssetServer: blocking waits are main-thread only");
    const std::uint32_t generation = at(r).generation;
    for (;;) {
        drain();
        if (!materialised(r, generation)) return;
        if (result != nullptr ? result->done : !at(r).job) return;
        FinalizeBudget budget;
        const TreeState t = finalize_tree(r, false, budget);
        if (!materialised(r, generation)) return;
        if (result != nullptr ? result->done : !at(r).job) return;
        if (t == TreeState::Done) continue; // a new job (coalesced reload) started
        prioritise(r);
        if (!wait_for_progress() && !done_ready.load(std::memory_order_acquire)) {
            LAM_LOG_ERROR("assets: '{}' cannot finish loading (worker idle)", at(r).path);
            return;
        }
    }
}

void AssetServer::Impl::wait_all() {
    LAM_ASSERT(on_main_thread(), "AssetServer::wait_all is main-thread only");
    LAM_ASSERT(finalize_depth == 0, "AssetServer::wait_all called from a loader callback");
    if (synchronous) return;
    while (in_flight > 0) {
        drain();
        FinalizeBudget budget;
        process_roots(budget);
        if (in_flight == 0) break;
        if (!wait_for_progress() && !done_ready.load(std::memory_order_acquire)) {
            LAM_LOG_ERROR("assets: wait_all: {} load(s) cannot progress", in_flight);
            break;
        }
    }
}

} // namespace lam
