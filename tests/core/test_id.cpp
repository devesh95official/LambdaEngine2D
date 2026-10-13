// Ids: str_id (FNV-1a 64), the name table, id_display, type ids and AssetId packing.
#include <doctest/doctest.h>

#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <lam/core/asset_id.h>
#include <lam/core/id.h>

using namespace lam;
using namespace lam::literals;

// FNV-1a 64 reference values; "" is the none id.
static_assert(str_id("") == StrId{});
static_assert(str_id("a").value == 0xaf63dc4c8601ec8cULL);
static_assert(str_id("foobar").value == 0x85944171f73967e8ULL);
static_assert("jump"_id == str_id("jump"));
static_assert("jump"_id != "Jump"_id);
static_assert(!StrId{});
static_assert(static_cast<bool>("x"_id));

TEST_CASE("id: intern registers the name; id_name and id_display read it back") {
    const StrId goblin = intern("test_id.goblin");
    CHECK(goblin == str_id("test_id.goblin"));
    CHECK(id_name(goblin) == "test_id.goblin");
    CHECK(id_display(goblin) == "test_id.goblin");
    // The view stays valid and stable.
    const std::string_view v = id_name(goblin);
    for (int i = 0; i < 200; ++i)
        intern("test_id.filler." + std::to_string(i));
    CHECK(v == "test_id.goblin");
    CHECK(id_name(goblin).data() == v.data());
}

TEST_CASE("id: unknown ids display as #<16 hex digits>") {
    CHECK(id_name(StrId{0x1234}).empty());
    CHECK(id_display(StrId{0x1234}) == "#0000000000001234");
    CHECK(id_display(StrId{0xFEDCBA9876543210ULL}) == "#fedcba9876543210");
    CHECK(id_name(StrId{}).empty());
    const StrId unnamed = "test_id.never_interned"_id;
    CHECK(id_display(unnamed).size() == 17);
    CHECK(id_display(unnamed)[0] == '#');
}

TEST_CASE("id: register_id_name keeps the first name; none is never registered") {
    const StrId id{0xABCDEF0001ULL};
    register_id_name(id, "first");
    register_id_name(id, "second");
    CHECK(id_name(id) == "first");
    register_id_name(StrId{}, "nothing");
    CHECK(id_name(StrId{}).empty());
}

TEST_CASE("id: interning from several threads is safe") {
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 200; ++i)
                intern("test_id.thread." + std::to_string(t * 1000 + i));
        });
    }
    for (std::thread &th : threads)
        th.join();
    for (int t = 0; t < 4; ++t) {
        for (int i = 0; i < 200; i += 37) {
            const std::string name = "test_id.thread." + std::to_string(t * 1000 + i);
            REQUIRE(id_name(str_id(name)) == name);
        }
    }
}

TEST_CASE("id: type ids are dense, non-zero and stable per type") {
    struct A {};
    struct B {};
    const TypeId a = type_id<A>();
    const TypeId b = type_id<B>();
    CHECK(a != 0);
    CHECK(b != 0);
    CHECK(a != b);
    CHECK(type_id<A>() == a);
    CHECK(type_id<int>() != type_id<float>());
}

TEST_CASE("id: StrId hashes by value") {
    std::unordered_set<StrId> set{"a"_id, "b"_id, "a"_id};
    CHECK(set.size() == 2);
    CHECK(std::hash<StrId>{}(StrId{42}) == 42);
}

TEST_CASE("asset id: none is zero; bits round-trip") {
    struct Tex {};
    constexpr AssetId<Tex> none{};
    static_assert(!none);
    constexpr AssetId<Tex> id{7, 3};
    static_assert(static_cast<bool>(id));
    static_assert(id.bits() == (7ULL | (3ULL << 32)));
    static_assert(AssetId<Tex>::from_bits(id.bits()) == id);
    CHECK(AssetId<Tex>::from_bits(0) == none);
}

// "..."_id is in the inline namespace lam::literals, so `using namespace lam;` is enough to use it.
static_assert(lam::operator""_id("jump", 4) == lam::str_id("jump"));
static_assert(lam::literals::operator""_id("jump", 4) == lam::str_id("jump"));

TEST_CASE("id: the _id literal is reachable through lam and lam::literals") {
    CHECK(lam::operator""_id("jump", 4) == "jump"_id);
}
