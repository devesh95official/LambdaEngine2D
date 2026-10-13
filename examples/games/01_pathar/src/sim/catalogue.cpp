// Catalogue and Progress (see catalogue.h). DataReader pattern: typed reads mark keys as read and
// record sticky errors with the JSON path, so the build code needs no per-read checks; the
// document's finish() (inside parse_data) reports unknown keys.
#include "sim/catalogue.h"

#include <algorithm>
#include <lam/core/data.h>

namespace pathar {

lam::Result<Catalogue> Catalogue::parse(std::string_view json_text, std::string_view file_name) {
    return lam::parse_data<Catalogue>(json_text, file_name, [](lam::DataReader &root) {
        Catalogue cat;
        lam::DataArray chapters = root.array("chapters");
        for (std::size_t ci = 0; ci < chapters.size(); ++ci) {
            lam::DataReader c = chapters.object(ci);
            ChapterInfo ch;
            ch.id = c.string("id");
            ch.name = c.string("name");
            ch.english = c.string("english");
            ch.theme = c.string("theme");
            ch.music = c.string("music");
            lam::DataArray levels = c.array("levels");
            for (std::size_t li = 0; li < levels.size(); ++li) {
                lam::DataReader l = levels.object(li);
                LevelInfo info;
                info.name = l.string("name");
                info.english = l.string("english");
                std::vector<std::string> rows;
                lam::DataArray r = l.array("rows");
                for (std::size_t ri = 0; ri < r.size(); ++ri) rows.push_back(r.text(ri));
                lam::Result<Level> level = Level::parse(rows, info.name);
                if (!level) {
                    l.fail(level.error().message);  // sticky; parse_data returns it with the path
                    continue;
                }
                info.level = std::move(level.value());
                info.level.par = l.integer("par", 1, 100000);
                info.pushes = l.integer("pushes", 1, 100000);
                ch.levels.push_back(std::move(info));
            }
            if (ch.levels.empty()) c.fail("no levels");
            cat.chapters.push_back(std::move(ch));
        }
        if (cat.chapters.empty()) root.fail("no chapters");
        return cat;
    });
}

int Catalogue::total() const {
    int n = 0;
    for (const ChapterInfo &c : chapters) n += static_cast<int>(c.levels.size());
    return n;
}

std::pair<int, int> Catalogue::locate(int global) const {
    for (std::size_t c = 0; c < chapters.size(); ++c) {
        const int n = static_cast<int>(chapters[c].levels.size());
        if (global < n) return {static_cast<int>(c), global};
        global -= n;
    }
    return {static_cast<int>(chapters.size()) - 1, static_cast<int>(chapters.back().levels.size()) - 1};
}

int Catalogue::global(int chapter, int level) const {
    int g = 0;
    for (int c = 0; c < chapter; ++c) g += static_cast<int>(chapters[static_cast<std::size_t>(c)].levels.size());
    return g + level;
}

const LevelInfo &Catalogue::at(int global) const {
    const auto [c, l] = locate(global);
    return chapters[static_cast<std::size_t>(c)].levels[static_cast<std::size_t>(l)];
}

const ChapterInfo &Catalogue::chapter_of(int global) const {
    return chapters[static_cast<std::size_t>(locate(global).first)];
}

bool Progress::unlocked(int global) const { return global >= 0 && global < count() && global <= solved_count() + 2; }

bool Progress::finish(int global, int moves, int par, bool helped) {
    if (global < 0 || global >= count()) return false;
    Record &r = records_[static_cast<std::size_t>(global)];
    if (helped) {
        if (r.solved) return false;
        r.solved = true;
        r.best_moves = 0;  // no best of its own
        r.stars = 1;
        return true;
    }
    const bool better = !r.solved || r.best_moves == 0 || moves < r.best_moves;
    if (better) {
        r.solved = true;
        r.best_moves = moves;
        r.stars = std::max(r.stars, stars(moves, par));
    }
    return better;
}

int Progress::solved_count() const {
    return static_cast<int>(std::count_if(records_.begin(), records_.end(), [](const Record &r) { return r.solved; }));
}

int Progress::total_stars() const {
    int n = 0;
    for (const Record &r : records_) n += r.stars;
    return n;
}

int Progress::next_unsolved() const {
    for (int i = 0; i < count(); ++i) {
        if (!records_[static_cast<std::size_t>(i)].solved) return i;
    }
    return std::max(0, count() - 1);
}

std::string Progress::to_json(const Catalogue &cat) const {
    lam::DataWriter w;
    w.begin_object().key("levels").begin_object();
    for (int i = 0; i < count(); ++i) {
        const Record &r = records_[static_cast<std::size_t>(i)];
        if (!r.solved) continue;
        w.key(cat.at(i).name).begin_object().key("moves").value(r.best_moves).key("stars").value(r.stars).end_object();
    }
    w.end_object().key("last").value(last_played).end_object();
    lam::Result<std::string> out = w.finish();
    return out ? out.value() : std::string("{}");
}

Progress Progress::from_json(std::string_view json, const Catalogue &cat) {
    Progress p(cat.total());
    lam::Result<lam::DataDocument> doc = lam::DataDocument::parse(json, "progress");
    if (!doc) return p;
    lam::DataReader root = doc->root();
    if (root.type("levels") == lam::DataType::Object) {
        lam::DataReader levels = root.object("levels");
        for (int i = 0; i < p.count(); ++i) {
            const std::string &name = cat.at(i).name;
            if (levels.type(name) != lam::DataType::Object) continue;
            lam::DataReader r = levels.object(name);
            Record &rec = p.records_[static_cast<std::size_t>(i)];
            rec.solved = true;
            rec.best_moves = r.integer_or("moves", 0, 0, 1000000);
            rec.stars = r.integer_or("stars", 1, 1, 3);
            r.ignore_unknown();
        }
        levels.ignore_unknown();  // names of levels that no longer exist
    }
    p.last_played = std::clamp(root.integer_or("last", 0, 0, 1000000), 0, std::max(0, p.count() - 1));
    root.ignore_unknown();
    return p;
}

}  // namespace pathar
