#include <lam/core/alloc.h>

#include <atomic>

namespace lam {
namespace {
thread_local std::uint64_t t_allocations = 0;
std::atomic<bool> g_hook_linked{false};
} // namespace

namespace detail {
void note_allocation() { ++t_allocations; }
void set_allocation_hook_linked() { g_hook_linked.store(true); }
} // namespace detail

bool allocation_counting_enabled() { return g_hook_linked.load(); }

std::uint64_t allocation_count() { return t_allocations; }

AllocationScope::AllocationScope() : start_(t_allocations) {}

std::uint64_t AllocationScope::count() const { return t_allocations - start_; }

} // namespace lam
