// lam_core allocation counting: test executables link lam_alloc_hook, so operator new is
// counted per thread and AllocationScope measures a block.
#include <doctest/doctest.h>

#include <cstdint>
#include <new>
#include <thread>

#include <lam/core/alloc.h>
#include <lam/test/support.h>

using namespace lam;

namespace {
// Explicit calls to the replaceable allocation functions cannot be elided by the optimiser.
void allocate_and_free(int n) {
    for (int i = 0; i < n; ++i)
        ::operator delete(::operator new(16));
}
} // namespace

TEST_CASE("alloc: counting is on in test executables") {
#if defined(LAM_TEST_SANITIZE) && LAM_TEST_SANITIZE
    // Sanitizers own operator new, so lam_add_test does not link the hook.
    CHECK_FALSE(allocation_counting_enabled());
#else
    CHECK(allocation_counting_enabled());
#endif
}

TEST_CASE("alloc: AllocationScope counts the allocations made inside it") {
    if (!allocation_counting_enabled()) return;
    AllocationScope none;
    CHECK(none.count() == 0);
    AllocationScope scope;
    allocate_and_free(3);
    CHECK(scope.count() == 3);
    {
        AllocationScope inner;
        allocate_and_free(2);
        CHECK(inner.count() == 2);
    }
    CHECK(scope.count() == 5);
    const std::uint64_t before = allocation_count();
    ::operator delete[](::operator new[](8));
    ::operator delete(::operator new(32, std::align_val_t{64}), std::align_val_t{64});
    void *p = ::operator new(8, std::nothrow);
    ::operator delete(p);
    CHECK(allocation_count() - before == 3);
}

TEST_CASE("alloc: counts are per thread") {
    if (!allocation_counting_enabled()) return;
    AllocationScope scope;
    std::uint64_t other = 0;
    std::thread t([&other] {
        AllocationScope s;
        allocate_and_free(10);
        other = s.count();
    });
    t.join();
    CHECK(other == 10);
    // std::thread itself may allocate on this thread; the worker's 10 are not counted here.
    CHECK(scope.count() < 10);
}

TEST_CASE("alloc: CHECK_NO_ALLOC passes for allocation-free code") {
    int sum = 0;
    CHECK_NO_ALLOC(for (int i = 0; i < 100; ++i) sum += i);
    CHECK(sum == 4950);
}

TEST_CASE("alloc: CHECK_NO_ALLOC reports an allocation (must-fail canary)") {
    if (!allocation_counting_enabled()) return;
    MUST_FAIL(CHECK_NO_ALLOC(allocate_and_free(1)));
}
