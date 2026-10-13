#pragma once
// lam_ecs test fixtures: small reflected components and a fork-based check for LAM_CHECK aborts.
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/vec2.h>
#include <lam/test/support.h>
namespace ecs_test {
struct Pos { lam::Vec2 v; };
struct Vel { lam::Vec2 v; };
struct Health { std::int32_t hp = 10; std::int32_t max = 10; };
struct Frozen {};                                               // tag
struct Score { std::int32_t points = 0; std::uint32_t moves = 0; };   // world resource
struct RuntimeHandle { std::uint32_t handle = 0; };             // unreflected: runtime only
// Runs f in a forked child and reports whether it aborted (SIGABRT), i.e. a LAM_CHECK fired.
template <class F> bool aborts(F &&f) {
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        std::signal(SIGABRT, SIG_DFL);                          // bypass the test runner's crash handler
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        f();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
}
LAM_REFLECT(ecs_test::Pos, v)
LAM_REFLECT(ecs_test::Vel, v)
LAM_REFLECT(ecs_test::Health, hp, max)
LAM_REFLECT_TAG(ecs_test::Frozen)
LAM_REFLECT(ecs_test::Score, points, moves)
