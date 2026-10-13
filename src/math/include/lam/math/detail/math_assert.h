#pragma once
// lam_math sits below lam_core and can't use LAM_ASSERT, so it has its own. Same
// LAM_ENABLE_ASSERTS switch.
#include <cstdio>
#include <cstdlib>

#if defined(LAM_ENABLE_ASSERTS) && LAM_ENABLE_ASSERTS
#define LAM_MATH_ASSERT(cond, msg)                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: assertion failed: %s (%s)\n", __FILE__, __LINE__, #cond,   \
                         msg);                                                                     \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)
#else
#define LAM_MATH_ASSERT(cond, msg) ((void)0)
#endif
