#pragma once
// LAM_ASSERT (debug), LAM_CHECK (always), LAM_UNREACHABLE. LAM_ENABLE_ASSERTS is a PUBLIC
// definition on lam_math/lam_core, so every target agrees.
//   LAM_ASSERT(cond, msg)  API misuse and invariants; compiled out in Release.
//   LAM_CHECK(cond, msg)   violations that would corrupt memory; always on; logs and aborts.
namespace lam::detail {
[[noreturn]] void assert_fail(const char *file, int line, const char *expr, const char *msg);
}
#if defined(LAM_ENABLE_ASSERTS) && LAM_ENABLE_ASSERTS
#define LAM_ASSERT(cond, msg)                                                                      \
    do {                                                                                           \
        if (!(cond)) ::lam::detail::assert_fail(__FILE__, __LINE__, #cond, msg);                   \
    } while (false)
#else
#define LAM_ASSERT(cond, msg) ((void)0)
#endif
#define LAM_CHECK(cond, msg)                                                                       \
    do {                                                                                           \
        if (!(cond)) ::lam::detail::assert_fail(__FILE__, __LINE__, #cond, msg);                   \
    } while (false)
#define LAM_UNREACHABLE() ::lam::detail::assert_fail(__FILE__, __LINE__, "unreachable", "")
