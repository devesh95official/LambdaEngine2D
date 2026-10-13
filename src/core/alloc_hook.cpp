// lam_alloc_hook (OBJECT library): replaces the global operator new/delete family so every
// allocation on a thread is counted. Linked by test executables and benches only.
#include <lam/core/alloc.h>

#include <cstdlib>
#include <new>

namespace {
struct HookLinked {
    HookLinked() { lam::detail::set_allocation_hook_linked(); }
} g_hook_linked;

void *allocate(std::size_t size) {
    lam::detail::note_allocation();
    if (size == 0) size = 1;
    void *p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void *allocate_aligned(std::size_t size, std::align_val_t align) {
    lam::detail::note_allocation();
    if (size == 0) size = 1;
    std::size_t a = static_cast<std::size_t>(align);
    if (a < sizeof(void *)) a = sizeof(void *);
    void *p = nullptr;
    if (posix_memalign(&p, a, size) != 0) throw std::bad_alloc();
    return p;
}
} // namespace

void *operator new(std::size_t size) { return allocate(size); }
void *operator new[](std::size_t size) { return allocate(size); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void *operator new(std::size_t size, std::align_val_t a) { return allocate_aligned(size, a); }
void *operator new[](std::size_t size, std::align_val_t a) { return allocate_aligned(size, a); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }
