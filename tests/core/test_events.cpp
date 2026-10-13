// EventBus and Events<T>: shared sequence numbers, read windows, trimming and capacity.
#include <doctest/doctest.h>

#include <lam/core/events.h>
#include <lam/test/support.h>

using namespace lam;

namespace {
struct Hit {
    int damage = 0;
};
struct Spawned {
    int id = 0;
};
struct Unused {};
} // namespace

TEST_CASE("events: windows select by sequence, in emission order") {
    EventBus bus;
    CHECK(bus.sequence() == 1);
    Events<Hit> &hits = bus.add<Hit>(8);
    hits.emit({1});
    hits.emit({2});
    hits.emit({3});
    CHECK(bus.sequence() == 4);
    auto all = hits.window(1, 4);
    REQUIRE(all.size() == 3);
    CHECK(all[0].damage == 1);
    CHECK(all[2].damage == 3);
    auto tail = hits.window(2, 4);
    REQUIRE(tail.size() == 2);
    CHECK(tail[0].damage == 2);
    CHECK(hits.window(4, 10).empty());
    CHECK(hits.window(3, 2).empty());
    CHECK(hits.window(0, 1).empty());
    CHECK(hits.all().size() == 3);
}

TEST_CASE("events: the sequence is shared by every channel of a bus") {
    EventBus bus;
    Events<Hit> &hits = bus.add<Hit>();
    Events<Spawned> &spawns = bus.add<Spawned>();
    const std::uint32_t mark0 = bus.sequence();
    hits.emit({10});    // seq 1
    spawns.emit({100}); // seq 2
    const std::uint32_t mark1 = bus.sequence();
    hits.emit({20}); // seq 3
    CHECK(hits.window(mark0, mark1).size() == 1);
    CHECK(hits.window(mark1, bus.sequence()).size() == 1);
    CHECK(hits.window(mark1, bus.sequence())[0].damage == 20);
    CHECK(spawns.window(mark0, mark1).size() == 1);
    CHECK(spawns.window(mark1, bus.sequence()).empty());
    CHECK(bus.retained() == 3);
    // Exactly once: consecutive windows cover every event without overlap.
    int seen = 0;
    std::uint32_t from = 1;
    for (std::uint32_t to : {2u, 2u, 4u}) {
        seen += static_cast<int>(hits.window(from, to).size());
        from = to;
    }
    CHECK(seen == 2);
}

TEST_CASE("events: trim drops old events and keeps windows correct") {
    EventBus bus;
    Events<Hit> &hits = bus.add<Hit>(4);
    Events<Spawned> &spawns = bus.add<Spawned>(4);
    for (int i = 1; i <= 4; ++i) {
        hits.emit({i});
        spawns.emit({i});
    }
    CHECK(bus.retained() == 8);
    bus.trim(5); // drops seq 1..4
    CHECK(bus.retained() == 4);
    CHECK(hits.size() == 2);
    CHECK(hits.window(1, 5).empty());
    auto rest = hits.window(1, bus.sequence());
    REQUIRE(rest.size() == 2);
    CHECK(rest[0].damage == 3);
    bus.trim(1); // nothing older
    CHECK(bus.retained() == 4);
    bus.trim(bus.sequence());
    CHECK(bus.retained() == 0);
    CHECK(bus.sequence() == 9); // trimming never rewinds the sequence
}

TEST_CASE("events: add is idempotent; try_get finds registered channels only") {
    EventBus bus;
    Events<Hit> &a = bus.add<Hit>(16);
    Events<Hit> &b = bus.add<Hit>(99);
    CHECK(&a == &b);
    CHECK(a.capacity() == 16);
    CHECK(&bus.get<Hit>() == &a);
    CHECK(bus.try_get<Hit>() == &a);
    CHECK(bus.try_get<Unused>() == nullptr);
    const EventBus &cbus = bus;
    CHECK(cbus.try_get<Hit>() == &a);
    CHECK(cbus.try_get<Unused>() == nullptr);
}

TEST_CASE("events: emitting within capacity does not allocate; growing is counted") {
    EventBus bus;
    Events<Hit> &hits = bus.add<Hit>(32);
    const std::uint64_t grown = detail::event_grow_count();
    CHECK_NO_ALLOC(for (int i = 0; i < 32; ++i) hits.emit({i}));
    CHECK(detail::event_grow_count() == grown);
    for (int i = 0; i < 64; ++i)
        hits.emit({i});
    CHECK(detail::event_grow_count() > grown);
    CHECK(hits.size() == 96);
    // After a trim the storage is reused.
    bus.trim(bus.sequence());
    CHECK_NO_ALLOC(for (int i = 0; i < 32; ++i) hits.emit({i}));
    CHECK_NO_ALLOC((void)hits.window(1, bus.sequence()));
}

TEST_CASE("events: reserve raises capacity and never shrinks; emitting up to it does not allocate") {
    EventBus bus;
    Events<Hit> &hits = bus.add<Hit>(4);
    hits.emit({1});
    hits.reserve(64);
    CHECK(hits.capacity() == 64);
    hits.reserve(8); // never shrinks
    CHECK(hits.capacity() == 64);
    CHECK(hits.size() == 1); // retained events are kept
    CHECK(hits.all()[0].damage == 1);
    const std::uint64_t grown = detail::event_grow_count();
    CHECK_NO_ALLOC(for (int i = 0; i < 63; ++i) hits.emit({i}));
    CHECK(detail::event_grow_count() == grown);
    CHECK(hits.window(0, bus.sequence()).size() == 64);
}
