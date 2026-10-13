#pragma once
// Allocation counting. Real counts only when the executable links the OBJECT library
// lam_alloc_hook (lam_add_test does, unless LAM_SANITIZE). Counts operator new on this thread;
// malloc calls from C libraries (raylib, stb) are not counted.
#include <cstdint>
namespace lam {
bool allocation_counting_enabled();
std::uint64_t allocation_count();
// AllocationScope s; ...; CHECK(s.count() == 0);
class AllocationScope {
public:
    AllocationScope();
    std::uint64_t count() const;
private:
    std::uint64_t start_ = 0;
};
namespace detail {
void note_allocation();                                         // called by lam_alloc_hook
void set_allocation_hook_linked();
}
}
