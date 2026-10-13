// Animation graphs: .anim.json parsing, checks against the sheet, and playback.
#include <string>
#include <vector>

#include <lam/animation/animator.h>

#include "anim_test_util.h"

using anim_test::contains;
using anim_test::make_sheet;
using anim_test::Rig;
using lam::AnimDirection;
using lam::str_id;

namespace {

const char *kGraph = R"({
  "sheet": "hero.json",
  "start": "idle",
  "clips": [
    {"name": "idle"},
    {"name": "run", "tag": "run"},
    {"name": "walk", "tags": ["walk_side", "walk_down", "", "walk_up"]},
    {"name": "attack", "tags": ["attack"], "mirror_left": false}
  ],
  "transitions": [
    {"from": "idle", "to": "run", "param": "speed", "above": 10},
    {"from": "run", "to": "idle", "param": "speed", "below": 10},
    {"from": "*", "to": "attack", "trigger": "attack"},
    {"from": "attack", "to": "idle", "on_finish": true}
  ],
  "events": [{"tag": "attack", "frame": 1, "name": "hit"},
             {"tag": "walk", "frame": 1, "name": "step"},
             {"tag": "run", "frame": 0, "name": "run_start"}]
})";

// idle 0-1, run 2-5, attack 6-7 (once), walk_side 8-9, walk_down 10-11, walk_up 12-13.
lam::SpriteSheet hero_sheet() {
    return make_sheet({100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100},
                      {{"idle", 0, 1},
                       {"run", 2, 5},
                       {"attack", 6, 7, AnimDirection::Forward, 1},
                       {"walk_side", 8, 9},
                       {"walk_down", 10, 11},
                       {"walk_up", 12, 13}});
}

lam::AnimGraph graph_of(const std::string &text) {
    lam::Result<lam::AnimGraph> g = lam::parse_anim_graph(text, "g.anim.json");
    REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
    return std::move(g.value());
}

std::string error_of(const std::string &text) {
    lam::Result<lam::AnimGraph> g = lam::parse_anim_graph(text, "g.anim.json");
    CHECK_FALSE(g.ok());
    return g.ok() ? std::string() : g.error().message;
}

Rig hero_rig(const std::string &graph = kGraph) {
    Rig r;
    r.sheet = hero_sheet();
    r.graph = graph_of(graph);
    r.use_graph = true;
    return r;
}

std::size_t count(const std::vector<lam::AnimationEvent> &evs, lam::StrId name) {
    std::size_t n = 0;
    for (const lam::AnimationEvent &e : evs) n += e.name == name ? 1 : 0;
    return n;
}

} // namespace

TEST_CASE("anim graph: a full graph parses") {
    const lam::AnimGraph g = graph_of(kGraph);
    CHECK(g.sheet_path == "hero.json");
    CHECK_FALSE(g.sheet);
    REQUIRE(g.clips.size() == 4);
    CHECK(g.start == 0);
    CHECK(g.clips[0].name == str_id("idle"));
    CHECK(g.clips[0].tags[0] == str_id("idle")); // a clip without tags plays its own name
    CHECK(g.clips[0].dir_count == 1);
    CHECK(g.clips[1].tags[0] == str_id("run"));
    CHECK(g.clips[2].dir_count == 4);
    CHECK(g.clips[2].tags[1] == str_id("walk_down"));
    CHECK_FALSE(g.clips[2].tags[2]);
    CHECK(g.clips[2].mirror_left);
    CHECK_FALSE(g.clips[3].mirror_left);
    CHECK(g.find_clip(str_id("walk")) == 2);
    CHECK(g.find_clip(str_id("swim")) == -1);
    CHECK(g.facing_dirs() == 4);
    REQUIRE(g.transitions.size() == 4);
    CHECK(g.transitions[0].from == 0);
    CHECK(g.transitions[0].to == 1);
    CHECK(g.transitions[0].param == str_id("speed"));
    CHECK(g.transitions[0].threshold == 10.0f);
    CHECK(g.transitions[0].greater);
    CHECK_FALSE(g.transitions[1].greater);
    CHECK(g.transitions[2].from == lam::kAnyClip);
    CHECK(g.transitions[2].trigger == str_id("attack"));
    CHECK(g.transitions[3].on_finish);
    CHECK_FALSE(g.transitions[3].param);
    REQUIRE(g.frame_events.size() == 3);
    CHECK(g.frame_events[0].tag == str_id("attack"));
    CHECK(g.frame_events[0].frame == 1);
    CHECK(g.frame_events[0].name == str_id("hit"));
    CHECK(lam::id_name(g.frame_events[0].name) == "hit");

    SUBCASE("threshold and greater may be written out") {
        const lam::AnimGraph t = graph_of(R"({"clips": [{"name": "a"}, {"name": "b"}],
            "transitions": [{"from": "a", "to": "b", "param": "grounded", "threshold": 0.5,
                             "greater": false}, {"from": "b", "to": "a", "param": "x"}]})");
        CHECK(t.transitions[0].threshold == 0.5f);
        CHECK_FALSE(t.transitions[0].greater);
        CHECK(t.transitions[1].threshold == 0.5f); // default: a boolean param > 0.5
        CHECK(t.transitions[1].greater);
        CHECK(t.sheet_path.empty());
    }
}

TEST_CASE("anim graph: parse errors name the file and the JSON path") {
    CHECK(contains(error_of(R"({"clips": [{"name": "a"}], "bogus": 1})"),
                   "g.anim.json: /: unknown key \"bogus\""));
    CHECK(contains(error_of(R"({"clips": []})"), "g.anim.json: /clips: must not be empty"));
    CHECK(contains(error_of(R"({})"), "/clips: missing required key"));
    CHECK(contains(error_of(R"({"clips": [{"name": "a"}, {"name": "a"}]})"),
                   "/clips/1: duplicate clip name \"a\""));
    CHECK(contains(error_of(R"({"clips": [{"name": "a", "tags": ["x", "y", "z"]}]})"),
                   "/clips/0/tags: must list 1, 2, 4 or 8 tags (one per facing), not 3"));
    CHECK(contains(error_of(R"({"clips": [{"name": "a", "tag": "x", "tags": ["x"]}]})"),
                   "/clips/0: give either \"tag\" or \"tags\""));
    CHECK(contains(error_of(R"({"clips": [{"name": "a", "tags": ["", "x"]}]})"),
                   "/clips/0: the first facing (right) needs a tag"));
    CHECK(contains(error_of(R"({"clips": [{"name": "a", "speed": 2}]})"),
                   "/clips/0: unknown key \"speed\""));
    CHECK(contains(error_of(R"({"clips": [{"name": "a"}], "start": "b"})"),
                   "g.anim.json: /: start \"b\" is not a clip of this graph"));
    const auto tr = [](const std::string &t) {
        return R"({"clips": [{"name": "a"}, {"name": "b"}], "transitions": [)" + t + "]}";
    };
    CHECK(contains(error_of(tr(R"({"from": "nope", "to": "b", "trigger": "t"})")),
                   "/transitions/0: from \"nope\" is not a clip of this graph"));
    CHECK(contains(error_of(tr(R"({"from": "a", "to": "*", "trigger": "t"})")),
                   "/transitions/0: to \"*\" is not a clip of this graph"));
    CHECK(contains(error_of(tr(R"({"from": "a", "to": "b", "param": "s", "above": 1, "below": 2})")),
                   "/transitions/0: give one of \"above\", \"below\" or \"threshold\""));
    CHECK(contains(error_of(tr(R"({"from": "a", "to": "b", "above": 1})")),
                   "/transitions/0: a threshold needs a \"param\""));
    CHECK(contains(error_of(tr(R"({"from": "any", "to": "b"})")),
                   "/transitions/0: a transition from any clip needs a param"));
    CHECK(contains(error_of(tr(R"({"from": "a", "to": "b", "param": "s", "above": "x"})")),
                   "/transitions/0/above: expected a number"));
    CHECK(contains(error_of(R"({"clips": [{"name": "a"}], "events": [{"tag": "a", "frame": 0}]})"),
                   "/events/0/name: missing required key"));
    CHECK(contains(error_of(R"({"clips": [{"name": "a"}], "events": [{"tag": "a", "frame": -1,
                               "name": "e"}]})"),
                   "/events/0/frame: integer -1 is outside"));
    CHECK(contains(error_of("not json"), "g.anim.json: "));
}

TEST_CASE("anim graph: check_anim_graph validates against the sheet") {
    const lam::SpriteSheet sheet = hero_sheet();
    CHECK(lam::check_anim_graph(graph_of(kGraph), sheet).ok());
    const auto err = [&](const std::string &text) {
        const lam::Status s = lam::check_anim_graph(graph_of(text), sheet);
        CHECK_FALSE(s.ok());
        return s.ok() ? std::string() : s.error().message;
    };
    CHECK(contains(err(R"({"clips": [{"name": "swim"}]})"),
                   "clip \"swim\": tag \"swim\" is not in the sheet"));
    CHECK(contains(err(R"({"clips": [{"name": "w", "tags": ["walk_side", "walk_downn"]}]})"),
                   "clip \"w\": tag \"walk_downn\" is not in the sheet"));
    CHECK(contains(err(R"({"clips": [{"name": "idle"}], "events": [{"tag": "idle", "frame": 2,
                          "name": "e"}]})"),
                   "event \"e\": frame 2 is past the end of \"idle\" (2 frames)"));
    CHECK(contains(err(R"({"clips": [{"name": "idle"}], "events": [{"tag": "jump", "frame": 0,
                          "name": "e"}]})"),
                   "\"jump\" is neither a sheet tag nor a clip"));
    // A clip name works for events; its frame must fit the clip's longest tag.
    CHECK(lam::check_anim_graph(graph_of(R"({"clips": [{"name": "w", "tags": ["walk_side"]}],
                                   "events": [{"tag": "w", "frame": 1, "name": "e"}]})"),
                                sheet)
              .ok());
    lam::AnimGraph empty;
    CHECK_FALSE(lam::check_anim_graph(empty, sheet).ok());
}

TEST_CASE("anim graph: the start clip plays; params switch clips on the same tick") {
    Rig r = hero_rig();
    r.tick();
    CHECK(r.a.clip == str_id("idle"));
    CHECK(r.a.tag == 0);
    CHECK(r.a.frame == 0);
    r.tick(6);
    CHECK(r.a.frame == 1);
    lam::set_param(r.params, str_id("speed"), 10); // not above 10
    r.tick();
    CHECK(r.a.clip == str_id("idle"));
    lam::set_param(r.params, str_id("speed"), 10.5f);
    r.tick();
    CHECK(r.a.clip == str_id("run"));
    CHECK(r.a.frame == 2);
    CHECK(count(r.events, str_id("run_start")) == 1); // the first frame's event fires on start
    r.tick(6 * 4);                                     // a full loop re-enters frame 0
    CHECK(count(r.events, str_id("run_start")) == 2);
    lam::set_param(r.params, str_id("speed"), 3);
    r.tick();
    CHECK(r.a.clip == str_id("idle"));
    CHECK(r.a.frame == 0);
}

TEST_CASE("anim graph: triggers fire once, are consumed and are cleared every tick") {
    Rig r = hero_rig();
    r.tick(3);
    lam::fire_trigger(r.params, str_id("attack"));
    r.tick();
    CHECK(r.a.clip == str_id("attack"));
    CHECK(r.a.frame == 6);
    CHECK_FALSE(r.params.triggers[0]);
    // The hit event fires when frame 1 of the tag is entered, 6 ticks later.
    r.tick(5);
    CHECK(count(r.events, str_id("hit")) == 0);
    r.tick();
    CHECK(r.a.frame == 7);
    REQUIRE(count(r.events, str_id("hit")) == 1);
    CHECK(r.events.back().entity == lam::Entity{7, 1});
    CHECK(r.events.back().frame == 1);
    // Attack plays once; idle starts on the very tick its last frame's time is up.
    r.tick(5);
    CHECK(r.a.clip == str_id("attack"));
    CHECK(r.a.frame == 7);
    r.tick();
    CHECK(r.a.clip == str_id("idle"));
    CHECK(r.a.frame == 0);
    CHECK_FALSE(r.a.finished);

    SUBCASE("a trigger that matches nothing is dropped, not kept for later") {
        Rig j = hero_rig(R"({"clips": [{"name": "idle"}, {"name": "run"}, {"name": "attack"}],
            "transitions": [{"from": "idle", "to": "run", "param": "speed", "above": 1},
                            {"from": "run", "to": "attack", "trigger": "attack"}]})");
        j.tick();
        lam::fire_trigger(j.params, str_id("attack")); // idle has no attack transition
        j.tick();
        CHECK(j.a.clip == str_id("idle"));
        CHECK_FALSE(j.params.triggers[0]);
        lam::set_param(j.params, str_id("speed"), 5);
        j.tick(3);
        CHECK(j.a.clip == str_id("run"));
    }
    SUBCASE("a trigger to the current clip restarts it") {
        r.tick(); // idle
        lam::fire_trigger(r.params, str_id("attack"));
        r.tick();
        r.tick(8);
        CHECK(r.a.frame == 7);
        lam::fire_trigger(r.params, str_id("attack"));
        r.tick();
        CHECK(r.a.clip == str_id("attack"));
        CHECK(r.a.frame == 6);
    }
}

TEST_CASE("anim graph: any-state param transitions do not restart their own target") {
    Rig r = hero_rig(R"({"clips": [{"name": "idle"}, {"name": "run"}],
        "transitions": [{"from": "*", "to": "run", "param": "vy", "above": 0},
                        {"from": "run", "to": "idle", "param": "vy", "below": 0}]})");
    r.tick();
    lam::set_param(r.params, str_id("vy"), 1);
    r.tick();
    CHECK(r.a.clip == str_id("run"));
    r.tick(13);
    CHECK(r.a.frame == 4); // kept playing: never restarted by the any-state transition
}

TEST_CASE("anim graph: the first matching transition wins") {
    Rig r = hero_rig(R"({"clips": [{"name": "idle"}, {"name": "run"}, {"name": "attack"}],
        "transitions": [{"from": "idle", "to": "attack", "param": "speed", "above": 1},
                        {"from": "idle", "to": "run", "param": "speed", "above": 1}]})");
    r.tick();
    lam::set_param(r.params, str_id("speed"), 2);
    r.tick();
    CHECK(r.a.clip == str_id("attack"));
}

TEST_CASE("anim graph: frame events name a tag or a clip; facings keep the clip's events") {
    Rig r = hero_rig();
    r.a.clip = str_id("walk");
    r.a.facing = 1; // down
    r.tick();
    CHECK(r.a.frame == 10);
    r.tick(6);
    CHECK(r.a.frame == 11);
    CHECK(count(r.events, str_id("step")) == 1);
    r.a.facing = 0; // right: same clip, side tag, position kept
    r.tick(6);
    CHECK(r.a.frame == 8); // 11 -> 9 (remap), then the loop back to 8
    r.tick(6);
    CHECK(r.a.frame == 9);
    CHECK(count(r.events, str_id("step")) == 2);
}

TEST_CASE("anim graph: a sheet tag that is not a clip plays directly; only any-state applies") {
    Rig r = hero_rig();
    r.tick();
    lam::play(r.a, r.sheet, str_id("walk_up"));
    r.tick();
    CHECK(r.a.frame == 12);
    lam::set_param(r.params, str_id("speed"), 50); // idle -> run does not apply
    r.tick(3);
    CHECK(r.a.clip == str_id("walk_up"));
    lam::fire_trigger(r.params, str_id("attack")); // any -> attack does
    r.tick();
    CHECK(r.a.clip == str_id("attack"));
}

TEST_CASE("anim graph: a graph without transitions or sheet still plays its start clip") {
    Rig r = hero_rig(R"({"clips": [{"name": "idle"}, {"name": "run"}], "start": "run"})");
    r.tick(7);
    CHECK(r.a.clip == str_id("run"));
    CHECK(r.a.frame == 3);
}
