// Catalog<T>: insertion order, stable indices, lookup by StrId and duplicate ids.
#include <doctest/doctest.h>

#include <string>

#include <lam/core/catalog.h>

#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace lam;
using namespace lam::literals;

namespace {

// Runs `f` in a child process and reports whether it aborted (SIGABRT turned into exit code 77 so
// the OS does not write a crash report). Used for LAM_ASSERT checks only.
template <class F>
bool aborts(F f) {
    const pid_t pid = fork();
    if (pid == 0) {
        std::signal(SIGABRT, [](int) { _exit(77); });
        const int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) dup2(null_fd, STDERR_FILENO);
        f();
        _exit(0);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 77;
}

struct EnemyDef {
    int hp = 0;
    std::string name;
};

} // namespace

TEST_CASE("catalog keeps insertion order and looks up by id") {
    Catalog<int> c;
    CHECK(c.add("b"_id, 2) == 0);
    CHECK(c.add("a"_id, 1) == 1);
    REQUIRE(c.size() == 2);
    CHECK(c.id_at(0) == "b"_id);
    CHECK(c.id_at(1) == "a"_id);
    CHECK(c.id_at(2) == StrId{});
    CHECK(*c.find("b"_id) == 2);
    CHECK(c.find("zzz"_id) == nullptr);
    CHECK(c.index_of("a"_id) == 1);
    CHECK(c.index_of("zzz"_id) == Catalog<int>::kNone);
    CHECK(c.at(0) == 2);
    c.at(0) = 20;
    CHECK(*c.find("b"_id) == 20);
    REQUIRE(c.items().size() == 2);
    CHECK(c.items()[0] == 20);
    CHECK(c.items()[1] == 1);
}

TEST_CASE("catalog: indices stay stable as the catalog grows") {
    Catalog<EnemyDef> c;
    const std::uint32_t goblin = c.add("goblin"_id, {5, "Goblin"});
    const std::uint32_t wolf = c.add("wolf"_id, {8, "Wolf"});
    for (int i = 0; i < 1000; ++i)
        c.add(str_id("filler" + std::to_string(i)), {i, "x"});
    CHECK(c.index_of("goblin"_id) == goblin);
    CHECK(c.index_of("wolf"_id) == wolf);
    CHECK(c.at(goblin).name == "Goblin");
    CHECK(c.at(wolf).hp == 8);
    CHECK(c.index_of(str_id("filler999")) == 1001);
    const Catalog<EnemyDef> &cc = c;
    CHECK(cc.at(wolf).name == "Wolf");
    c.clear();
    CHECK(c.size() == 0);
    CHECK(c.find("goblin"_id) == nullptr);
    CHECK(c.add("wolf"_id, {1, "again"}) == 0);
}

TEST_CASE("catalog: a duplicate id is rejected by LAM_ASSERT") {
#if defined(LAM_ENABLE_ASSERTS) && LAM_ENABLE_ASSERTS
    CHECK(aborts([] {
        Catalog<int> c;
        c.add("same"_id, 1);
        c.add("same"_id, 2);
    }));
    CHECK_FALSE(aborts([] {
        Catalog<int> c;
        c.add("one"_id, 1);
        c.add("two"_id, 2);
    }));
    CHECK(aborts([] {
        Catalog<int> c;
        (void)c.at(3);
    }));
#else
    MESSAGE("asserts are off in this build; duplicate ids are not checked");
#endif
}
