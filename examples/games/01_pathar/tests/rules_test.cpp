// Pathar's pure rules: moving, pushing, sinking, dead squares, the solver, every shipped level
// solved at its par, and progress. Runs without any engine module but lam_core (DataReader).
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>
#include <doctest/doctest.h>
#include "sim/catalogue.h"
#include "sim/rules.h"
#include "test_util.h"

using namespace pathar;

namespace {

Level parse(const std::vector<std::string> &rows) {
    lam::Result<Level> l = Level::parse(rows, "test");
    REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
    return l.value();
}

// The solver's recorded answers for every shipped level: moves, pushes, push positions looked at,
// and FNV-1a 64 of the move letters (u d l r). Equal numbers mean solve() still finds the very
// same solutions.
struct KnownSolution {
    int level;
    std::size_t moves;
    int pushes;
    std::size_t states;
    std::uint64_t letters_fnv;
};
constexpr KnownSolution kKnown[] = {
    {1, 3, 2, 3, 0x638d615157801683ull},  // Pehla Kadam
    {2, 19, 4, 22, 0x29c37f8cdba20b27ull},  // Do Pathar
    {3, 7, 5, 8, 0x69e58e4220b60128ull},  // Mod
    {4, 34, 6, 30, 0x49e096fcfd168a45ull},  // Tulsi Chaura
    {5, 19, 10, 88, 0x12f5ae363776f803ull},  // Kuan
    {6, 31, 9, 282, 0x8d7cf5ea896598b8ull},  // Chaupal
    {7, 42, 8, 210, 0xf6f69daf33f6d917ull},  // Gali
    {8, 56, 12, 5822, 0xac08d53401b8f99aull},  // Aangan
    {9, 9, 6, 34, 0xcef7ce6c4d47a2e2ull},  // Pehli Leher
    {10, 24, 6, 22, 0xa40f93fc4cebfc14ull},  // Seedhiyan
    {11, 22, 8, 25, 0x69d16d45ff888dffull},  // Do Kinare
    {12, 20, 9, 80, 0x8f1874f82e21996full},  // Naav
    {13, 29, 8, 949, 0x268368b904b4961dull},  // Pul
    {14, 52, 6, 699, 0xef6c40bff8bb1fc7ull},  // Kamal
    {15, 29, 9, 1047, 0x7240ec48ceb8405aull},  // Sandhya
    {16, 37, 10, 7562, 0xa67ed68344185f90ull},  // Dhara
    {17, 43, 11, 287, 0xbf3b6df288772fa0ull},  // Chadhai
    {18, 49, 10, 942, 0xd39c6d20971e46d9ull},  // Chattaan
    {19, 44, 15, 524, 0xf4cae69d0632bdbaull},  // Gufa Dwaar
    {20, 61, 16, 19473, 0xdc6c43c0ed3ab810ull},  // Mandir Seedhi
    {21, 63, 13, 834, 0x7d33944b18b68beeull},  // Shikhar
    {22, 68, 16, 23038, 0x0357549187496615ull},  // Bhool Bhulaiya
    {23, 83, 19, 2658, 0x80faa5c10db11192ull},  // Ghanti
    {24, 106, 20, 16973, 0xaa150e413e51bab4ull},  // Antim Dwaar
};

std::uint64_t letters_fnv(const std::vector<Dir> &moves) {
    std::uint64_t h = 1469598103934665603ull;
    for (Dir d : moves) {
        const char c = d == Dir::Up ? 'u' : d == Dir::Down ? 'd' : d == Dir::Left ? 'l' : 'r';
        h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    }
    return h;
}

}  // namespace

TEST_CASE("pathar rules: moving and pushing") {
    const Level l = parse({"#######", "#@ $ .#", "#######"});
    State s = start_state(l);
    CHECK(l.goal_count() == 1);
    CHECK_FALSE(apply_move(l, s, Dir::Up).moved);  // a wall
    CHECK_FALSE(apply_move(l, s, Dir::Left).moved);
    MoveResult r = apply_move(l, s, Dir::Right);
    CHECK((r.moved && !r.pushed));
    r = apply_move(l, s, Dir::Right);
    CHECK((r.pushed && r.from == l.cell(3, 1) && r.to == l.cell(4, 1) && !r.onto_goal));
    r = apply_move(l, s, Dir::Right);
    CHECK((r.pushed && r.onto_goal));
    CHECK(solved(l, s));
    CHECK_FALSE(apply_move(l, s, Dir::Right).moved);  // the stone is against the wall
    // Two stones in a row cannot be pushed.
    const Level q = parse({"########", "#@$$ ..#", "########"});
    State qs = start_state(q);
    CHECK_FALSE(apply_move(q, qs, Dir::Right).moved);
}

TEST_CASE("pathar rules: water") {
    const Level l = parse({"########", "#@$~ $.#", "########"});
    State s = start_state(l);
    CHECK(l.water.size() == 1);
    CHECK(open_water(l, s, l.cell(3, 1)));
    MoveResult r = apply_move(l, s, Dir::Right);
    CHECK((r.pushed && r.sank));
    CHECK_FALSE(open_water(l, s, l.cell(3, 1)));
    CHECK(s.boxes[0] == -1);
    CHECK((apply_move(l, s, Dir::Right).moved));  // onto the filled water
    CHECK((apply_move(l, s, Dir::Right).moved));
    r = apply_move(l, s, Dir::Right);
    CHECK((r.pushed && r.onto_goal));
    CHECK(solved(l, s));
    // The player cannot wade.
    const Level q = parse({"#######", "#@~ $.#", "#######"});
    State qs = start_state(q);
    CHECK_FALSE(apply_move(q, qs, Dir::Right).moved);
}

TEST_CASE("pathar rules: dead squares and stuck") {
    const Level l = parse({"######", "#.   #", "#  $ #", "#@   #", "######"});
    const std::vector<std::uint8_t> d = dead_squares(l);
    CHECK(d[static_cast<std::size_t>(l.cell(4, 3))] == 1);  // a corner
    CHECK(d[static_cast<std::size_t>(l.cell(4, 1))] == 1);
    CHECK(d[static_cast<std::size_t>(l.cell(2, 2))] == 0);
    CHECK(d[static_cast<std::size_t>(l.cell(1, 1))] == 0);  // the slab
    // Pushed against the bottom wall, a stone can never come up again to the slab.
    const Level p = parse({"######", "#.   #", "#  $@#", "#    #", "######"});
    const std::vector<std::uint8_t> pd = dead_squares(p);
    State s = start_state(p);
    CHECK_FALSE(stuck(p, s, pd));
    apply_move(p, s, Dir::Up);
    apply_move(p, s, Dir::Left);
    CHECK(apply_move(p, s, Dir::Down).pushed);
    CHECK(stuck(p, s, pd));
}

TEST_CASE("pathar rules: solver") {
    const Level l = parse({"#######", "#@ $ .#", "#######"});
    const Solution s = solve(l, start_state(l));
    CHECK(s.found);
    CHECK((s.moves.size() == 3 && s.pushes == 2));
    // Unsolvable: the only stone in a corner.
    const Level bad = parse({"#####", "#$ .#", "# @ #", "#####"});
    CHECK_FALSE(solve(bad, start_state(bad)).found);
    // Water: a stone must be given to the river first.
    const Level w = parse({"#########", "#@ $ ~ .#", "# $     #", "#       #", "#########"});
    const Solution ws = solve(w, start_state(w));
    REQUIRE(ws.found);
    State st = start_state(w);
    for (Dir d : ws.moves) apply_move(w, st, d);
    CHECK(solved(w, st));
    // walk_path never pushes and never wades.
    const Level path = parse({"#######", "#@ $ .#", "#######"});
    CHECK(walk_path(path, start_state(path), path.cell(2, 1)).size() == 1);
    CHECK(walk_path(path, start_state(path), path.cell(4, 1)).empty());  // behind the stone
}

TEST_CASE("pathar rules: every shipped level is solved at its par") {
    const Catalogue &cat = test::catalogue();
    CHECK(cat.chapters.size() == 3);
    CHECK(cat.total() == 24);
    for (int g = 0; g < cat.total(); ++g) {
        const LevelInfo &info = cat.at(g);
        CAPTURE(info.name);
        const Solution s = solve(info.level, start_state(info.level), 1000000);
        REQUIRE(s.found);
        CHECK(static_cast<int>(s.moves.size()) == info.level.par);
        CHECK(s.pushes == info.pushes);
        State st = start_state(info.level);
        for (Dir d : s.moves) CHECK(apply_move(info.level, st, d).moved);
        CHECK(solved(info.level, st));
        CHECK(stars(static_cast<int>(s.moves.size()), info.level.par) == 3);
        const State fresh = start_state(info.level);
        CHECK_FALSE(solved(info.level, fresh));
        CHECK_FALSE(stuck(info.level, fresh, dead_squares(info.level)));
    }
    for (int g = 0; g < cat.total(); ++g) {
        const auto [c, l] = cat.locate(g);
        CHECK(cat.global(c, l) == g);
    }
}

TEST_CASE("pathar rules: the solver finds the recorded solutions, move for move") {
    const Catalogue &cat = test::catalogue();
    REQUIRE(cat.total() == static_cast<int>(std::size(kKnown)));
    for (const KnownSolution &o : kKnown) {
        const LevelInfo &info = cat.at(o.level - 1);
        CAPTURE(info.name);
        const Solution s = solve(info.level, start_state(info.level), 1000000);
        REQUIRE(s.found);
        CHECK(s.moves.size() == o.moves);
        CHECK(s.pushes == o.pushes);
        CHECK(s.states == o.states);
        CHECK(letters_fnv(s.moves) == o.letters_fnv);
    }
}

TEST_CASE("pathar rules: a broken level names itself and the JSON path") {
    lam::Result<Catalogue> c = Catalogue::parse(
        R"({"chapters": [{"id": "a", "name": "A", "english": "a", "theme": "village", "music": "m",
            "levels": [{"name": "Oops", "english": "x", "par": 3, "pushes": 1, "rows": ["#####", "#@@.#", "#####"]}]}]})",
        "bad.json");
    REQUIRE_FALSE(c.ok());
    CHECK(c.error().message.find("Oops") != std::string::npos);
    CHECK(c.error().message.find("bad.json") != std::string::npos);
}

TEST_CASE("pathar progress: stars, unlocks and the JSON round trip") {
    const Catalogue &cat = test::catalogue();
    Progress p(cat.total());
    CHECK((p.unlocked(0) && p.unlocked(2) && !p.unlocked(3)));
    CHECK(p.finish(0, 10, 9));
    CHECK(p.record(0).stars == 2);
    CHECK_FALSE(p.finish(0, 12, 9));  // not better
    CHECK(p.finish(0, 9, 9));
    CHECK(p.record(0).stars == 3);
    CHECK(p.unlocked(3));
    CHECK((p.solved_count() == 1 && p.total_stars() == 3));
    CHECK(p.next_unsolved() == 1);
    CHECK(stars(100, 50) == 1);
    CHECK(p.finish(1, 5, 30, true));  // with hints: one star
    CHECK(p.record(1).stars == 1);
    CHECK_FALSE(p.finish(1, 4, 30, true));
    CHECK(p.finish(1, 30, 30));  // later without: three
    CHECK(p.record(1).stars == 3);
    p.last_played = 5;
    const Progress back = Progress::from_json(p.to_json(cat), cat);
    CHECK((back.record(0).solved && back.record(0).best_moves == 9 && back.record(0).stars == 3));
    CHECK((back.record(1).solved && back.record(1).stars == 3));
    CHECK_FALSE(back.record(2).solved);
    CHECK(back.last_played == 5);
    CHECK(Progress::from_json("[]", cat).solved_count() == 0);
    CHECK(Progress::from_json("not json", cat).solved_count() == 0);
    // Names that no longer exist are ignored, the rest kept.
    const Progress odd = Progress::from_json(
        R"({"levels": {"Gone": {"moves": 1, "stars": 3}, "Pehla Kadam": {"moves": 3, "stars": 3}}, "last": 99})", cat);
    CHECK(odd.solved_count() == 1);
    CHECK(odd.last_played == cat.total() - 1);
}
