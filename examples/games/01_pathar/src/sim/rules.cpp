// The pure rules (see rules.h). Move history and counters are kept by the ECS board (board.cpp),
// which uses History.
#include "sim/rules.h"

#include <algorithm>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>

namespace pathar {

namespace {

std::size_t at(int c) { return static_cast<std::size_t>(c); }

}  // namespace

lam::IVec2 dir_vec(Dir d) {
    switch (d) {
    case Dir::Up: return {0, -1};
    case Dir::Down: return {0, 1};
    case Dir::Left: return {-1, 0};
    case Dir::Right: return {1, 0};
    }
    return {};
}

Dir opposite(Dir d) {
    switch (d) {
    case Dir::Up: return Dir::Down;
    case Dir::Down: return Dir::Up;
    case Dir::Left: return Dir::Right;
    case Dir::Right: return Dir::Left;
    }
    return d;
}

int Level::step(int c, Dir d) const {
    const lam::IVec2 p = pos(c) + dir_vec(d);
    return inside(p) ? cell(p) : -1;
}

int Level::goal_count() const { return static_cast<int>(std::count(goal.begin(), goal.end(), 1)); }

int Level::water_index(int c) const {
    const auto it = std::find(water.begin(), water.end(), c);
    return it == water.end() ? -1 : static_cast<int>(it - water.begin());
}

lam::Result<Level> Level::parse(const std::vector<std::string> &rows, std::string name) {
    auto fail = [&](const std::string &why) { return lam::make_error(lam::ErrorCode::Parse, name + ": " + why); };
    Level l;
    l.height = static_cast<int>(rows.size());
    for (const std::string &r : rows) l.width = std::max(l.width, static_cast<int>(r.size()));
    if (l.width < 3 || l.height < 3 || l.width > 32 || l.height > 32) return fail("a level is 3 to 32 cells each way");
    const std::size_t n = static_cast<std::size_t>(l.width * l.height);
    l.tiles.assign(n, Tile::Outside);
    l.goal.assign(n, 0);
    l.bridge.assign(n, 0);
    std::vector<std::uint8_t> blocked(n, 1);
    int players = 0;
    for (int y = 0; y < l.height; ++y) {
        const std::string &row = rows[static_cast<std::size_t>(y)];
        for (int x = 0; x < l.width; ++x) {
            const char ch = x < static_cast<int>(row.size()) ? row[static_cast<std::size_t>(x)] : ' ';
            const int c = l.cell(x, y);
            const std::size_t i = at(c);
            switch (ch) {
            case '#': l.tiles[i] = Tile::Wall; break;
            case ' ': case '-': case '_': blocked[i] = 0; break;
            case '.': l.goal[i] = 1; blocked[i] = 0; break;
            case '$': l.boxes.push_back(c); blocked[i] = 0; break;
            case '*': l.boxes.push_back(c); l.goal[i] = 1; blocked[i] = 0; break;
            case '@': l.player = c; ++players; blocked[i] = 0; break;
            case '+': l.player = c; l.goal[i] = 1; ++players; blocked[i] = 0; break;
            case '~': l.tiles[i] = Tile::Water; blocked[i] = 0; break;
            case '=': l.bridge[i] = 1; blocked[i] = 0; break;
            default:
                return fail("unknown cell '" + std::string(1, ch) + "' at " + std::to_string(x) + "," + std::to_string(y));
            }
        }
    }
    if (players != 1) return fail("needs exactly one player");
    // Inside is what the player could reach if nothing but walls were in the way.
    std::deque<int> queue{l.player};
    std::vector<std::uint8_t> seen(n, 0);
    seen[at(l.player)] = 1;
    while (!queue.empty()) {
        const int c = queue.front();
        queue.pop_front();
        if (l.tiles[at(c)] == Tile::Outside) l.tiles[at(c)] = Tile::Ground;
        for (Dir d : kDirs) {
            const int nb = l.step(c, d);
            if (nb < 0 || seen[at(nb)] || blocked[at(nb)]) continue;
            seen[at(nb)] = 1;
            queue.push_back(nb);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (l.tiles[i] == Tile::Water) {
            if (!seen[i]) return fail("water outside the walls");
            l.water.push_back(static_cast<int>(i));
        }
        const bool stone = std::find(l.boxes.begin(), l.boxes.end(), static_cast<int>(i)) != l.boxes.end();
        if ((l.goal[i] || stone) && !seen[i]) return fail("a stone or slab outside the walls");
    }
    if (l.water.size() > 64) return fail("at most 64 water cells");
    const int goals = l.goal_count();
    if (goals == 0 || static_cast<int>(l.boxes.size()) < goals) return fail("needs slabs, and a stone for each");
    if (l.boxes.size() > 12) return fail("at most 12 stones");
    l.name = std::move(name);
    return l;
}

State start_state(const Level &level) {
    State s;
    s.player = level.player;
    s.boxes = level.boxes;
    return s;
}

int box_at(const State &s, int c) {
    for (std::size_t i = 0; i < s.boxes.size(); ++i) {
        if (s.boxes[i] == c) return static_cast<int>(i);
    }
    return -1;
}

bool open_water(const Level &l, const State &s, int c) {
    const int w = l.water_index(c);
    return w >= 0 && ((s.filled >> w) & 1u) == 0;
}

bool floor_at(const Level &l, const State &s, int c) {
    if (c < 0) return false;
    const Tile t = l.tile(c);
    return t == Tile::Ground || (t == Tile::Water && !open_water(l, s, c));
}

int on_goals(const Level &l, const State &s) {
    int n = 0;
    for (int b : s.boxes) n += (b >= 0 && l.is_goal(b)) ? 1 : 0;
    return n;
}

bool solved(const Level &l, const State &s) { return on_goals(l, s) == l.goal_count(); }

bool stuck(const Level &l, const State &s, const std::vector<std::uint8_t> &dead) {
    int alive = 0;
    for (int b : s.boxes) {
        if (b < 0) continue;
        ++alive;
        if (!l.is_goal(b) && dead[at(b)]) return true;
    }
    return alive < l.goal_count();
}

MoveResult apply_move(const Level &l, State &s, Dir d) {
    MoveResult r;
    const int next = l.step(s.player, d);
    if (next < 0 || !floor_at(l, s, next)) return r;
    const int b = box_at(s, next);
    if (b >= 0) {
        const int beyond = l.step(next, d);
        if (beyond < 0 || box_at(s, beyond) >= 0) return r;
        const bool sinks = open_water(l, s, beyond);
        if (!sinks && !floor_at(l, s, beyond)) return r;
        r.pushed = true;
        r.box = b;
        r.from = next;
        r.to = beyond;
        r.off_goal = l.is_goal(next);
        if (sinks) {
            r.sank = true;
            s.boxes[at(b)] = -1;
            s.filled |= std::uint64_t{1} << l.water_index(beyond);
        } else {
            s.boxes[at(b)] = beyond;
            r.onto_goal = l.is_goal(beyond);
        }
    }
    s.player = next;
    r.moved = true;
    return r;
}

std::vector<std::uint8_t> dead_squares(const Level &l) {
    const std::size_t n = l.tiles.size();
    auto passable = [&](int c) { return c >= 0 && (l.tile(c) == Tile::Ground || l.tile(c) == Tile::Water); };
    // Pull every slab (and every water cell: sinking a stone opens a way) backwards: a stone at
    // `from` is alive if it can be pushed towards an alive cell with the player standing behind.
    std::vector<std::uint8_t> alive(n, 0);
    std::deque<int> queue;
    for (std::size_t i = 0; i < n; ++i) {
        if (l.goal[i] || l.tiles[i] == Tile::Water) {
            alive[i] = 1;
            queue.push_back(static_cast<int>(i));
        }
    }
    while (!queue.empty()) {
        const int c = queue.front();
        queue.pop_front();
        for (Dir d : kDirs) {
            const int from = l.step(c, d);
            const int behind = from < 0 ? -1 : l.step(from, d);
            if (!passable(from) || !passable(behind) || alive[at(from)]) continue;
            alive[at(from)] = 1;
            queue.push_back(from);
        }
    }
    std::vector<std::uint8_t> dead(n, 0);
    for (std::size_t i = 0; i < n; ++i) dead[i] = (l.tiles[i] == Tile::Ground && !alive[i]) ? 1 : 0;
    return dead;
}

int stars(int moves, int par) {
    if (par <= 0 || moves <= par) return 3;
    if (moves * 10 <= par * 13 + 40) return 2;
    return 1;
}

std::vector<Dir> walk_path(const Level &l, const State &s, int target) {
    std::vector<Dir> path;
    if (target < 0 || target == s.player) return path;
    const std::size_t n = l.tiles.size();
    std::vector<int> from(n, -2);
    std::vector<Dir> via(n, Dir::Up);
    std::deque<int> queue{s.player};
    from[at(s.player)] = -1;
    while (!queue.empty()) {
        const int c = queue.front();
        queue.pop_front();
        if (c == target) break;
        for (Dir d : kDirs) {
            const int nb = l.step(c, d);
            if (nb < 0 || from[at(nb)] != -2 || !floor_at(l, s, nb) || box_at(s, nb) >= 0) continue;
            from[at(nb)] = c;
            via[at(nb)] = d;
            queue.push_back(nb);
        }
    }
    if (from[at(target)] == -2) return path;
    for (int c = target; from[at(c)] != -1; c = from[at(c)]) path.push_back(via[at(c)]);
    std::reverse(path.begin(), path.end());
    return path;
}

// ---- the solver ---------------------------------------------------------------------------------

namespace {

struct Search {
    const Level &l;
    std::vector<std::uint8_t> dead;

    bool walkable(const State &s, int c, const std::vector<int> &box_of) const {
        if (c < 0) return false;
        const Tile t = l.tile(c);
        if (t == Tile::Wall || t == Tile::Outside) return false;
        if (t == Tile::Water && ((s.filled >> l.water_index(c)) & 1u) == 0) return false;
        return box_of[at(c)] < 0;
    }

    // Marks the cells the player can walk to; returns the smallest (the position's name).
    int reach(const State &s, const std::vector<int> &box_of, std::vector<int> &mark, int stamp) const {
        std::deque<int> queue{s.player};
        mark[at(s.player)] = stamp;
        int lowest = s.player;
        while (!queue.empty()) {
            const int c = queue.front();
            queue.pop_front();
            lowest = std::min(lowest, c);
            for (Dir d : kDirs) {
                const int nb = l.step(c, d);
                if (nb < 0 || mark[at(nb)] == stamp || !walkable(s, nb, box_of)) continue;
                mark[at(nb)] = stamp;
                queue.push_back(nb);
            }
        }
        return lowest;
    }

    std::string key(const State &s, int normal) const {
        std::vector<int> b;
        for (int x : s.boxes) {
            if (x >= 0) b.push_back(x);
        }
        std::sort(b.begin(), b.end());
        std::string k;
        k.reserve(2 + b.size() * 2 + 8);
        k.push_back(static_cast<char>(normal & 0xff));
        k.push_back(static_cast<char>(normal >> 8));
        for (int x : b) {
            k.push_back(static_cast<char>(x & 0xff));
            k.push_back(static_cast<char>(x >> 8));
        }
        for (int i = 0; i < 8; ++i) k.push_back(static_cast<char>((s.filled >> (8 * i)) & 0xff));
        return k;
    }
};

// The walk from the player to `target` in `s`, appended to `out`; false if unreachable.
bool walk(const Level &l, const State &s, int target, std::vector<Dir> &out) {
    if (target == s.player) return true;
    const std::vector<Dir> path = walk_path(l, s, target);
    if (path.empty()) return false;
    out.insert(out.end(), path.begin(), path.end());
    return true;
}

}  // namespace

Solution solve(const Level &l, const State &from, std::size_t max_states) {
    Solution sol;
    Search sr{l, dead_squares(l)};
    const std::size_t n = l.tiles.size();
    struct Node {
        State state;
        int parent;
        int box;  // the stone pushed to get here
        Dir dir;
    };
    std::vector<Node> nodes;
    std::unordered_map<std::string, int> seen;
    std::vector<int> here(n, 0), there(n, 0);  // reach marks of the position, and of a child
    int here_stamp = 0, there_stamp = 0;
    std::vector<int> box_of(n, -1);
    auto fill_boxes = [&](const State &st) {
        std::fill(box_of.begin(), box_of.end(), -1);
        for (std::size_t i = 0; i < st.boxes.size(); ++i) {
            if (st.boxes[i] >= 0) box_of[at(st.boxes[i])] = static_cast<int>(i);
        }
    };
    fill_boxes(from);
    const int start_norm = sr.reach(from, box_of, there, ++there_stamp);
    nodes.push_back({from, -1, -1, Dir::Up});
    seen.emplace(sr.key(from, start_norm), 0);
    int goal_node = solved(l, from) ? 0 : -1;
    for (std::size_t head = 0; head < nodes.size() && goal_node < 0; ++head) {
        if (nodes.size() > max_states) {
            sol.gave_up = true;
            break;
        }
        const State s = nodes[head].state;
        fill_boxes(s);
        sr.reach(s, box_of, here, ++here_stamp);
        int alive = 0;
        for (int b : s.boxes) alive += b >= 0 ? 1 : 0;
        for (std::size_t bi = 0; bi < s.boxes.size() && goal_node < 0; ++bi) {
            const int b = s.boxes[bi];
            if (b < 0) continue;
            for (Dir d : kDirs) {
                const int to = l.step(b, d);
                const int behind = l.step(b, opposite(d));  // the player stands on the other side
                if (to < 0 || behind < 0 || here[at(behind)] != here_stamp) continue;
                const Tile t = l.tile(to);
                if (t == Tile::Wall || t == Tile::Outside || box_of[at(to)] >= 0) continue;
                const int wi = t == Tile::Water ? l.water_index(to) : -1;
                const bool sinks = wi >= 0 && ((s.filled >> wi) & 1u) == 0;
                if (!sinks && !l.is_goal(to) && sr.dead[at(to)]) continue;
                if (sinks && alive - 1 < l.goal_count()) continue;
                State next = s;
                next.player = b;
                if (sinks) {
                    next.boxes[bi] = -1;
                    next.filled |= std::uint64_t{1} << wi;
                } else {
                    next.boxes[bi] = to;
                }
                fill_boxes(next);
                const int norm = sr.reach(next, box_of, there, ++there_stamp);
                fill_boxes(s);
                std::string k = sr.key(next, norm);
                if (seen.count(k)) continue;
                seen.emplace(std::move(k), static_cast<int>(nodes.size()));
                nodes.push_back({next, static_cast<int>(head), static_cast<int>(bi), d});
                if (solved(l, next)) {
                    goal_node = static_cast<int>(nodes.size()) - 1;
                    break;
                }
            }
        }
    }
    sol.states = nodes.size();
    if (goal_node < 0) return sol;
    // The pushes, then the walks between them.
    std::vector<int> chain;
    for (int i = goal_node; i > 0; i = nodes[at(i)].parent) chain.push_back(i);
    std::reverse(chain.begin(), chain.end());
    State s = from;
    for (int i : chain) {
        const Node &node = nodes[at(i)];
        const int b = s.boxes[at(node.box)];
        if (!walk(l, s, l.step(b, opposite(node.dir)), sol.moves)) return sol;
        sol.moves.push_back(node.dir);
        s = node.state;
    }
    sol.found = true;
    sol.pushes = static_cast<int>(chain.size());
    return sol;
}

}  // namespace pathar
