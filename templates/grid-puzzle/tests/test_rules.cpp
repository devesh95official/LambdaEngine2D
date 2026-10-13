// The rules are plain functions over plain data, so they are tested without an App or a window.
// TODO(tests): once your rules grow, also drive a lam::test::HeadlessApp (RECIPES.md 1.6) with
// the bot for a few thousand ticks and check that two runs give the same state_hash.
#include <string>
#include <lam/core/fs.h>
#include <lam/test/support.h>
#include "grid_puzzle/rules.h"

using namespace grid_puzzle;

TEST_CASE("grid_puzzle rules: walk, push, block, solve, and every shipped level has a solution") {
    const lam::IVec2 left{-1, 0}, right{1, 0};
    auto parsed = parse_levels("#######\n"
                               "#@ $ .#\n"
                               "#  $$.#\n"
                               "#    .#\n"
                               "#######\n");
    REQUIRE(parsed);
    const Level &level = parsed->front();
    State s = level.start;
    CHECK(try_move(level, s, left) == Move::Blocked);           // a wall
    CHECK(s == level.start);                                    // a blocked move changes nothing
    CHECK(try_move(level, s, right) == Move::Walk);
    CHECK(try_move(level, s, right) == Move::Push);             // the crate goes 3 -> 4
    CHECK(try_move(level, s, right) == Move::Push);             // and onto the goal at 5
    CHECK(try_move(level, s, right) == Move::Blocked);          // never into a wall
    CHECK_FALSE(solved(level, s));                              // two crates are still off their goals
    State row = level.start;
    row.player = {2, 2};
    CHECK(try_move(level, row, right) == Move::Blocked);        // two crates in a row do not move

    auto simple = parse_levels("; a comment\n#####\n#@$.#\n#####\n\n#####\n#@ $#\n#  .#\n#####\n");
    REQUIRE(simple);
    REQUIRE(simple->size() == 2);
    CHECK(solve((*simple)[0], (*simple)[0].start) == std::vector<lam::IVec2>{right});
    CHECK(solve((*simple)[1], (*simple)[1].start).empty());     // crate stuck in a corner: no solution
    CHECK_FALSE(parse_levels("#####\n#@$x#\n#####\n"));         // unknown character
    CHECK_FALSE(parse_levels("#####\n# $.#\n#####\n"));         // no player

    // Every level in assets/levels.txt parses and can be solved, so a broken level edit fails here.
    const lam::Result<std::string> text = lam::read_file(GRID_PUZZLE_ASSETS "/levels.txt");
    REQUIRE(text);
    auto shipped = parse_levels(*text);
    REQUIRE(shipped);
    for (const Level &l : *shipped) {
        State play = l.start;
        for (lam::IVec2 d : solve(l, l.start)) CHECK(try_move(l, play, d) != Move::Blocked);
        CHECK(solved(l, play));
    }
}
