// World serialisation. A world saved midway and loaded into a fresh App must continue with the
// same state hash as the uninterrupted run.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/data.h>
#include <lam/core/fixed_string.h>
#include <lam/core/fs.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/scene/save.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>

namespace wst {
enum class Mood : std::uint8_t { Calm, Angry };
struct Stats {
    std::int32_t hp = 10;
    float speed = 1.5f;
};
struct Mover {
    lam::Vec2 vel;
    std::int32_t bounces = 0;
    lam::Entity target;
    lam::StrId tag;
    Mood mood = Mood::Calm;
    Stats stats;
    std::array<std::int16_t, 3> hist{};
    lam::FixedString<16> label;
};
struct Enemy {};
struct Score {
    std::int64_t points = 0;
    std::uint32_t spawned = 0;
};
struct Note {
    lam::AssetId<lam::TextAsset> text;
};
struct Tint {
    lam::Color color;
};
struct Scratch {
    int x = 0;
};
} // namespace wst
LAM_REFLECT_ENUM(wst::Mood, "calm", "angry")
LAM_REFLECT_STRUCT(wst::Stats, hp, speed)
LAM_REFLECT(wst::Mover, vel, bounces, target, tag, mood, stats, hist, label)
LAM_REFLECT_TAG(wst::Enemy)
LAM_REFLECT(wst::Score, points, spawned)
LAM_REFLECT(wst::Note, text)
LAM_REFLECT_FLAGS(wst::Tint, lam::kSerialize | lam::kSnapshot, color)

using namespace lam;
using namespace lam::literals;

namespace {

// A deterministic simulation with churn: moves and bounces, destroys and spawns through commands,
// draws from two RNG streams and keeps a snapshot resource.
void sim(Ctx &ctx) {
    World &w = ctx.world();
    Rng &rng = ctx.rng("wst.sim"_id);
    wst::Score &score = ctx.local<wst::Score>();
    for (auto [e, t, m] : w.view<Transform, wst::Mover>()) {
        t.position += m.vel;
        if (t.position.x < 0 || t.position.x > 200) {
            m.vel.x = -m.vel.x;
            ++m.bounces;
            score.points += m.stats.hp;
        }
        m.hist[static_cast<std::size_t>(w.tick() % 3)] = static_cast<std::int16_t>(t.position.x);
    }
    const std::size_t n = w.count<wst::Mover>();
    if (n > 4 && rng.below(3) == 0) {
        const auto victim = w.view<wst::Mover>().size_hint();
        (void)victim;
        Entity pick;
        std::uint32_t k = rng.below(static_cast<std::uint32_t>(n));
        for (auto [e, m] : w.view<wst::Mover>())
            if (k-- == 0) pick = e;
        ctx.commands().destroy(pick);
    }
    if (rng.below(4) == 0) {
        Rng &aux = ctx.rng("wst.aux"_id);
        const Entity e = ctx.commands().spawn();
        ctx.commands().add(e, Transform{{static_cast<float>(aux.between(10, 190)), 5}});
        wst::Mover m;
        m.vel = {aux.unit() * 4 - 2, 0.5f};
        m.stats.hp = aux.between(1, 20);
        m.mood = aux.below(2) ? wst::Mood::Angry : wst::Mood::Calm;
        m.label = "spawned";
        ctx.commands().add(e, m);
        if (aux.below(2)) ctx.commands().add(e, wst::Enemy{});
        ++score.spawned;
    }
}

void populate(Ctx &ctx) {
    World &w = ctx.world();
    Rng &rng = ctx.rng("wst.setup"_id);
    Entity prev;
    for (int i = 0; i < 12; ++i) {
        const Entity e = w.create();
        w.add<Transform>(e, Transform{{static_cast<float>(rng.between(0, 200)), 10.0f * i}});
        wst::Mover m;
        m.vel = {rng.unit() * 6 - 3, 0};
        m.target = prev;
        m.tag = intern(i % 2 ? "odd" : "even");
        m.stats.speed = rng.unit();
        w.add<wst::Mover>(e, m);
        if (i % 3 == 0) w.add<wst::Enemy>(e);
        w.add<wst::Tint>(e, wst::Tint{Color{static_cast<std::uint8_t>(i * 10), 0, 0, 255}});
        w.add<wst::Scratch>(e, wst::Scratch{i});
        prev = e;
    }
    // A hierarchy and a destroyed entity, so the free list is not empty.
    const Entity child = w.create();
    set_parent(w, child, prev, Transform{{0, -4}});
    const Entity gone = w.create();
    w.destroy(gone);
}

void setup(App &app, bool with_population) {
    app.add_system(Stage::Simulate, sim, "wst.sim");
    World &w = app.main_world();
    w.resources().insert<wst::Score>();
    w.snapshot_resource<wst::Score>();
    if (with_population) app.add_startup(populate, "wst.populate");
}

std::string save_text(const World &w, const SerializeContext &ctx = {}) {
    Result<std::string> t = serialize_world_text(w, ctx);
    REQUIRE(t);
    return *t;
}

// Replaces the first occurrence of `from` (which must exist).
std::string patched(std::string s, std::string_view from, std::string_view to) {
    const std::size_t at = s.find(from);
    REQUIRE_MESSAGE(at != std::string::npos, "pattern not found: " << from);
    s.replace(at, from.size(), to);
    return s;
}

} // namespace

TEST_CASE("world save: save midway, load into a fresh App, continue: same hash") {
    constexpr int kHalf = 150;
    test::HeadlessApp continuous;
    setup(continuous.app(), true);
    continuous.ticks(2 * kHalf);
    const std::uint64_t expected = continuous.app().main_world().state_hash();

    test::HeadlessApp first;
    setup(first.app(), true);
    first.ticks(kHalf);
    const World &mid = first.app().main_world();
    CHECK(mid.tick() == kHalf);
    const std::string text = save_text(mid);
    const std::uint64_t mid_hash = mid.state_hash();

    test::HeadlessApp second;                                   // different seed: load must override it
    second.app().set_seed(0xBADC0FFEEull);
    setup(second.app(), false);
    World &w = second.app().main_world();
    const Status st = deserialize_world_text(w, text, second.app().serialize_context());
    REQUIRE_MESSAGE(st, (st ? std::string() : st.error().message));
    CHECK(w.state_hash() == mid_hash);
    CHECK(w.tick() == kHalf);
    second.ticks(kHalf);
    CHECK(w.state_hash() == expected);
    // And the loaded world saves to the same bytes as the continuous run at the same tick.
    CHECK(save_text(w) == save_text(continuous.app().main_world()));
}

TEST_CASE("world save: exact layout round trip") {
    test::HeadlessApp a;
    setup(a.app(), true);
    a.ticks(20);
    World &src = a.app().main_world();
    const std::string text = save_text(src);
    CHECK(text.find(R"("layout":1)") != std::string::npos);
    CHECK(text.find(R"("component":"Scratch")") == std::string::npos);   // runtime: not saved
    CHECK(text.find(R"("component":"PrevTransform")") == std::string::npos);   // kSnapshot only
    CHECK(text.find(R"("component":"Tint")") != std::string::npos);   // presentation: saved
    CHECK(text.find(R"("tag":"odd")") != std::string::npos);        // StrId as its name
    CHECK(text.find(R"("mood":"calm")") != std::string::npos);      // enum as its name
    CHECK(text.find(R"("resource":"Score")") != std::string::npos);

    WorldSnapshot before;
    src.snapshot_into(before);
    test::HeadlessApp b;
    setup(b.app(), false);
    World &dst = b.app().main_world();
    // Pre-existing junk in the destination is replaced entirely.
    for (int i = 0; i < 40; ++i)
        dst.add<wst::Scratch>(dst.create(), wst::Scratch{i});
    REQUIRE(deserialize_world_text(dst, text, {}));
    WorldSnapshot after;
    dst.snapshot_into(after);
    CHECK(std::vector<std::uint32_t>(after.generations().begin(), after.generations().end()) ==
          std::vector<std::uint32_t>(before.generations().begin(), before.generations().end()));
    CHECK(std::vector<std::uint32_t>(after.free_list().begin(), after.free_list().end()) ==
          std::vector<std::uint32_t>(before.free_list().begin(), before.free_list().end()));
    CHECK(after.rng_streams().size() == before.rng_streams().size());
    CHECK(dst.seed() == src.seed());
    CHECK(dst.entity_count() == src.entity_count());
    CHECK(dst.count<wst::Scratch>() == 0);                      // runtime pool pruned
    CHECK(dst.state_hash() == src.state_hash());
    // Dense order per pool is reproduced exactly.
    std::vector<Entity> order_src, order_dst;
    for (auto [e, m] : src.view<wst::Mover>())
        order_src.push_back(e);
    for (auto [e, m] : dst.view<wst::Mover>())
        order_dst.push_back(e);
    CHECK(order_src == order_dst);
    // Entity fields are raw handles: still pointing at the same (alive) entities.
    for (auto [e, m] : dst.view<wst::Mover>()) {
        const wst::Mover &o = src.get<wst::Mover>(e);
        CHECK(m.target == o.target);
        CHECK(m.label.view() == o.label.view());
        CHECK(m.hist == o.hist);
        CHECK(m.stats.speed == o.stats.speed);
    }
    // Restore hooks ran: Children rebuilt, PrevTransform present and equal to Transform.
    std::size_t parents = 0;
    for (auto [e, p] : dst.view<Parent>()) {
        ++parents;
        REQUIRE(dst.has<Children>(p.entity));
        CHECK(dst.get<Children>(p.entity).count == 1);
    }
    CHECK(parents == 1);
    for (auto [e, t, pt] : dst.view<Transform, PrevTransform>())
        CHECK(pt.position == t.position);
    CHECK(dst.count<PrevTransform>() == dst.count<Transform>());
    CHECK(dst.resources().get<wst::Score>().spawned == src.resources().get<wst::Score>().spawned);
    // WorldRestored is emitted.
    REQUIRE(dst.events().try_get<WorldRestored>() != nullptr);
    CHECK(dst.events().try_get<WorldRestored>()->all().size() >= 1);
    // Saving again gives the same bytes.
    CHECK(save_text(dst) == text);
    // Unknown StrIds round-trip as "#<16 hex>".
    const Entity raw = dst.create();
    wst::Mover m;
    m.tag = StrId{0x0123456789abcdefull};
    dst.add<wst::Mover>(raw, m);
    const std::string t2 = save_text(dst);
    CHECK(t2.find(R"("tag":"#0123456789abcdef")") != std::string::npos);
    test::HeadlessApp c;
    setup(c.app(), false);
    REQUIRE(deserialize_world_text(c.app().main_world(), t2, {}));
    CHECK(c.app().main_world().get<wst::Mover>(raw).tag.value == 0x0123456789abcdefull);
}

TEST_CASE("world save: hand-written data loads; missing fields keep defaults") {
    const std::string base = R"({"layout":1,"tick":5,"seed":9,
        "entities":{"generations":[1,2,1],"states":[1,0,1],"free":[1]},
        "rng":[{"stream":"wst.sim","state":12345}],
        "pools":[{"component":"Mover","entities":[[0,1],[2,1]],"values":[{"bounces":1},{"bounces":2,"stats":{"hp":3}}]},
                 {"component":"Enemy","entities":[[2,1]]}],
        "resources":[{"resource":"Score","value":{"points":77}}]})";
    test::HeadlessApp a;
    setup(a.app(), false);
    World &w = a.app().main_world();
    const Status st = deserialize_world_text(w, base, {});
    REQUIRE_MESSAGE(st, (st ? std::string() : st.error().message));
    CHECK(w.tick() == 5);
    CHECK(w.seed() == 9);
    CHECK(w.entity_count() == 2);
    CHECK(w.get<wst::Mover>(Entity{0, 1}).bounces == 1);
    CHECK(w.get<wst::Mover>(Entity{0, 1}).stats.hp == 10);      // default kept
    CHECK(w.get<wst::Mover>(Entity{2, 1}).stats.hp == 3);
    CHECK(w.has<wst::Enemy>(Entity{2, 1}));
    CHECK(w.resources().get<wst::Score>().points == 77);
    CHECK(w.resources().get<wst::Score>().spawned == 0);
    // The free list is honoured: the next entity reuses index 1, one generation after the stored
    // one (a free slot stores its last occupant's generation).
    const Entity next = w.create();
    CHECK(next.index == 1);
    CHECK(next.generation == 3);
}

TEST_CASE("world save: a failed load changes nothing") {
    const std::string base = R"({"layout":1,"tick":5,"seed":9,
        "entities":{"generations":[1,2,1],"states":[1,0,1],"free":[1]},
        "rng":[],
        "pools":[{"component":"Mover","entities":[[0,1],[2,1]],"values":[{"bounces":1},{"bounces":2}]},
                 {"component":"Enemy","entities":[[2,1]]}],
        "resources":[{"resource":"Score","value":{"points":77}}]})";
    struct Case {
        const char *what;
        std::string_view from, to;
        ErrorCode code = ErrorCode::Invalid;
    };
    const Case cases[] = {
        {"unknown component", R"("component":"Enemy")", R"("component":"Enemy2")"},
        {"not serialisable", R"("component":"Enemy")", R"("component":"PrevTransform")"},
        {"dead entity in a pool", R"([[2,1]]})", R"([[1,2]]})"},
        {"stale generation", R"([[2,1]]})", R"([[2,7]]})"},
        {"index out of range", R"([[2,1]]})", R"([[9,1]]})"},
        {"repeated entity", R"("entities":[[0,1],[2,1]])", R"("entities":[[0,1],[0,1]])"},
        {"values count", R"(,{"bounces":2}])", R"(])"},
        {"unknown field", R"({"bounces":2})", R"({"bounces":2,"speed":1})"},
        {"wrong field type", R"({"bounces":2})", R"({"bounces":"two"})"},
        {"bad entity shape", R"([[2,1]]})", R"([[2]]})"},
        {"layout", R"("layout":1)", R"("layout":2)", ErrorCode::Unsupported},
        {"free list", R"("free":[1])", R"("free":[])"},
        {"free list repeats", R"("free":[1])", R"("free":[1,1])"},
        {"states size", R"("states":[1,0,1])", R"("states":[1,0])"},
        {"bad state", R"("states":[1,0,1])", R"("states":[1,0,3])"},
        {"zero generation", R"("generations":[1,2,1])", R"("generations":[0,2,1])"},
        {"unknown resource", R"("resource":"Score")", R"("resource":"Nope")"},
        {"resource not in world", R"("resource":"Score")", R"("resource":"Mover")"},
        {"repeated pool", R"("component":"Enemy")", R"("component":"Mover")"},
        {"unknown top-level key", R"("tick":5)", R"("tick":5,"extra":1)"},
        {"missing tick", R"("tick":5,)", R"()"},
        {"negative seed", R"("seed":9)", R"("seed":-9)"},
        {"truncated", R"(]})", R"(])", ErrorCode::Parse},
    };
    test::HeadlessApp a;
    setup(a.app(), true);
    a.ticks(10);
    World &w = a.app().main_world();
    REQUIRE(deserialize_world_text(w, base, {}));               // the unpatched data is valid
    a.ticks(3);
    const std::uint64_t hash = w.state_hash();
    const std::string before = save_text(w);
    const std::size_t restored = w.events().get<WorldRestored>().all().size();
    for (const Case &c : cases) {
        CAPTURE(c.what);
        const Status st = deserialize_world_text(w, patched(base, c.from, c.to), {});
        REQUIRE_FALSE(st);
        CHECK(st.error().code == c.code);
        CHECK(w.state_hash() == hash);
        CHECK(w.events().get<WorldRestored>().all().size() == restored);
    }
    CHECK(save_text(w) == before);
    // The DataReader entry point fails the same way.
    Result<DataDocument> doc = DataDocument::parse(patched(base, R"("component":"Enemy")", R"("component":"X")"), "t");
    REQUIRE(doc);
    DataReader root = doc->root();
    CHECK_FALSE(deserialize_world(w, root, {}));
    CHECK(w.state_hash() == hash);
}

TEST_CASE("world save: nested in a game document and inside a save envelope") {
    test::HeadlessApp a;
    setup(a.app(), true);
    a.ticks(30);
    World &src = a.app().main_world();
    DataWriter out;
    out.begin_object().key("level").value(3).key("world");
    REQUIRE(serialize_world(src, out, {}));
    out.end_object();
    const Result<std::string> doc_text = out.finish();
    REQUIRE(doc_text);

    test::TempDir dir;
    SaveSlots slots(dir.path(), "wst", 1, 2);
    REQUIRE(slots.save(1, SaveEnvelope{.summary_json = R"({"tick":30})", .data_json = *doc_text}));
    const Result<SaveEnvelope> loaded = slots.load(1);
    REQUIRE(loaded);

    test::HeadlessApp b;
    setup(b.app(), false);
    Result<DataDocument> doc = DataDocument::parse(loaded->data_json, "slot1");
    REQUIRE(doc);
    DataReader root = doc->root();
    CHECK(root.integer("level", 0, 10) == 3);
    DataReader wr = root.object("world");
    REQUIRE(deserialize_world(b.app().main_world(), wr, {}));
    REQUIRE(doc->finish());
    CHECK(b.app().main_world().state_hash() == src.state_hash());
    b.ticks(30);
    a.ticks(30);
    CHECK(b.app().main_world().state_hash() == src.state_hash());
}

TEST_CASE("world save: AssetId fields are saved as paths and resolved on load") {
    test::TempDir dir;
    REQUIRE(write_file_atomic(dir.file("note.txt"), "hello"));
    REQUIRE(write_file_atomic(dir.file("other.txt"), "x"));
    test::HeadlessApp a;
    a.add<AssetsPlugin>(AssetsConfig{.root = dir.path()});
    World &src = a.app().main_world();
    AssetServer &sa = a.app().res<AssetServer>();
    (void)sa.load<TextAsset>("other.txt");                      // shifts ids in this App only
    const Entity e = src.create();
    src.add<wst::Note>(e, wst::Note{sa.load<TextAsset>("note.txt")});
    const Entity none = src.create();
    src.add<wst::Note>(none);
    const std::string text = save_text(src, a.app().serialize_context());
    CHECK(text.find(R"({"text":"note.txt"})") != std::string::npos);
    CHECK(text.find(R"({"text":null})") != std::string::npos);

    test::HeadlessApp b;
    b.add<AssetsPlugin>(AssetsConfig{.root = dir.path()});
    World &dst = b.app().main_world();
    // Without a resolver the asset path cannot be loaded: an error, and nothing changes.
    CHECK_FALSE(deserialize_world_text(dst, text, {}));
    CHECK(dst.entity_count() == 0);
    REQUIRE(deserialize_world_text(dst, text, b.app().serialize_context()));
    AssetServer &sb = b.app().res<AssetServer>();
    const AssetId<TextAsset> id = dst.get<wst::Note>(e).text;
    REQUIRE(id);
    CHECK(sb.path_of(id) == "note.txt");
    REQUIRE(sb.get(id) != nullptr);
    CHECK(sb.get(id)->text == "hello");
    CHECK_FALSE(dst.get<wst::Note>(none).text);
}

TEST_CASE("world save: an empty world round-trips") {
    World a(WorldConfig{.seed = 3});
    const std::string text = save_text(a);
    World b(WorldConfig{.seed = 99});
    const Entity junk = b.create();
    (void)junk;
    REQUIRE(deserialize_world_text(b, text, {}));
    CHECK(b.entity_count() == 0);
    CHECK(b.seed() == 3);
    CHECK(b.state_hash() == a.state_hash());
}
