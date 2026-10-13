// Hot reload (LAM_DEV, async mode only). Every file a decode read is polled by mtime, a round-robin
// slice per frame so the whole list is visited once per watch_interval_seconds. A change queues a
// reload (coalesced while one is in flight). No allocation in steady state.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <utility>
#include <vector>

#include "detail/asset_internal.h"

namespace lam {

using detail::AssetRef;

// Replace r's watch list with the files its last decode read. Keep the newer mtime: the decode
// may have seen an older one than the poll already reacted to, causing a redundant reload.
void AssetServer::Impl::watch(AssetRef r, const std::vector<detail::ReadFile> &files) {
    std::vector<detail::Watch> fresh;
    const std::uint32_t generation = at(r).generation;
    for (const detail::ReadFile &f : files) {
        const std::filesystem::path file(f.absolute);
        const bool dup = std::any_of(fresh.begin(), fresh.end(),
                                     [&](const detail::Watch &w) { return w.file == file; });
        if (dup) continue;
        std::int64_t mtime = f.mtime;
        for (const detail::Watch &w : watches) {
            if (w.ref == r && w.file == file && w.mtime > mtime) mtime = w.mtime;
        }
        fresh.push_back({r, generation, file, mtime});
    }
    unwatch(r);
    for (detail::Watch &w : fresh)
        watches.push_back(std::move(w));
}

void AssetServer::Impl::unwatch(AssetRef r) {
    const auto first = std::remove_if(watches.begin(), watches.end(),
                                      [&](const detail::Watch &w) { return w.ref == r; });
    if (first == watches.end()) return;
    watches.erase(first, watches.end());
    if (watch_cursor >= watches.size()) watch_cursor = 0;
}

void AssetServer::Impl::poll_watches() {
    const auto now = std::chrono::steady_clock::now();
    if (!have_last_poll) {
        have_last_poll = true;
        last_poll = now;
    }
    const std::chrono::duration<double> elapsed = now - last_poll;
    last_poll = now;
    if (watches.empty()) {
        watch_carry = 0.0;
        return;
    }
    const double count = static_cast<double>(watches.size());
    std::size_t n = watches.size();
    if (config.watch_interval_seconds > 0.0f) {
        watch_carry += count * elapsed.count() / static_cast<double>(config.watch_interval_seconds);
        watch_carry = std::min(watch_carry, count);
        n = static_cast<std::size_t>(std::floor(watch_carry));
        watch_carry -= static_cast<double>(n);
    }
    for (std::size_t i = 0; i < n && !watches.empty(); ++i) {
        if (watch_cursor >= watches.size()) watch_cursor = 0;
        detail::Watch &w = watches[watch_cursor++];
        const std::int64_t mtime = detail::file_mtime_ns(w.file);
        if (mtime == w.mtime) continue;
        w.mtime = mtime;
        if (mtime < 0) continue; // deleted (or mid-save): keep serving; re-creating it reloads
        if (!materialised(w.ref, w.generation)) continue;
        request_reload(w.ref); // may add or remove watches only once the reload completes
    }
}

} // namespace lam
