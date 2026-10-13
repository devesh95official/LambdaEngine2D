// Resources: type-keyed singletons that never move, destroyed in reverse insertion order.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include <lam/core/resources.h>

using namespace lam;

namespace {
std::vector<std::string> g_destroyed;

template <int N>
struct Tracked {
    explicit Tracked(int v = 0) : value(v) {}
    ~Tracked() { g_destroyed.push_back("T" + std::to_string(N)); }
    Tracked(const Tracked &) = delete;
    Tracked &operator=(const Tracked &) = delete;
    int value;
};
struct Score {
    int points = 0;
    std::string name;
};
struct Never {};
} // namespace

TEST_CASE("resources: insert constructs in place; get returns the same object") {
    Resources res;
    CHECK_FALSE(res.has<Score>());
    CHECK(res.try_get<Score>() == nullptr);
    Score &s = res.insert<Score>(Score{5, "p1"});
    CHECK(res.has<Score>());
    CHECK(&res.get<Score>() == &s);
    CHECK(res.get<Score>().points == 5);
    const Resources &cres = res;
    CHECK(&cres.get<Score>() == &s);
    CHECK(cres.try_get<Score>() == &s);
    CHECK(res.get_raw(type_id<Score>()) == &s);
    CHECK(res.get_raw(type_id<Never>()) == nullptr);
    Tracked<1> &t = res.insert<Tracked<1>>(42); // non-movable types work
    CHECK(t.value == 42);
    g_destroyed.clear();
}

TEST_CASE("resources: references stay valid while other resources come and go") {
    Resources res;
    Score &s = res.insert<Score>();
    res.insert<Tracked<1>>(1);
    res.insert<Tracked<2>>(2);
    res.insert<Tracked<3>>(3);
    res.insert<Tracked<4>>(4);
    res.remove<Tracked<2>>();
    res.insert<Tracked<5>>(5);
    CHECK(&res.get<Score>() == &s);
    s.points = 9;
    CHECK(res.get<Score>().points == 9);
    CHECK(res.get<Tracked<4>>().value == 4);
    g_destroyed.clear();
}

TEST_CASE("resources: remove destroys one; removing an absent one is a no-op") {
    g_destroyed.clear();
    Resources res;
    res.insert<Tracked<1>>();
    res.insert<Tracked<2>>();
    res.remove<Tracked<1>>();
    CHECK(g_destroyed == std::vector<std::string>{"T1"});
    CHECK_FALSE(res.has<Tracked<1>>());
    CHECK(res.has<Tracked<2>>());
    res.remove<Tracked<1>>();
    res.remove<Never>();
    CHECK(g_destroyed.size() == 1);
    // Re-inserting after removal is allowed.
    res.insert<Tracked<1>>(7);
    CHECK(res.get<Tracked<1>>().value == 7);
    g_destroyed.clear();
}

TEST_CASE("resources: destruction runs in reverse insertion order") {
    g_destroyed.clear();
    {
        Resources res;
        res.insert<Tracked<1>>();
        res.insert<Tracked<2>>();
        res.insert<Tracked<3>>();
    }
    CHECK(g_destroyed == std::vector<std::string>{"T3", "T2", "T1"});

    g_destroyed.clear();
    Resources res;
    res.insert<Tracked<3>>();
    res.insert<Tracked<1>>();
    res.insert<Tracked<2>>();
    res.remove<Tracked<1>>();
    res.insert<Tracked<4>>();
    g_destroyed.clear();
    res.clear();
    CHECK(g_destroyed == std::vector<std::string>{"T4", "T2", "T3"});
    CHECK_FALSE(res.has<Tracked<3>>());
    res.insert<Tracked<3>>(); // usable after clear()
    CHECK(res.has<Tracked<3>>());
    g_destroyed.clear();
}
// Inserting a type that is already present is a LAM_ASSERT (programmer error); it aborts in
// Debug, so it is not exercised here. Resources::get<T>() on a missing T aborts in every build.
