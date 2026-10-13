#pragma once
// The levels by chapter (assets/data/levels.json, read with lam::DataReader: strict, every key
// checked, errors name the JSON path) and what the player has done with them: solved or not, the
// fewest moves, the stars, which levels are open. Pure; progress is written with DataWriter.
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <lam/core/result.h>
#include "sim/rules.h"

namespace pathar {

struct LevelInfo {
    std::string name;
    std::string english;
    Level level;
    int pushes = 0;  // the solver's
};

struct ChapterInfo {
    std::string id;
    std::string name;
    std::string english;
    std::string theme;  // the look: village, ghat, pahad
    std::string music;
    std::vector<LevelInfo> levels;
};

struct Catalogue {
    std::vector<ChapterInfo> chapters;

    // Every level parsed and checked; Error names the file and JSON path.
    static lam::Result<Catalogue> parse(std::string_view json_text, std::string_view file_name = "levels.json");
    int total() const;
    std::pair<int, int> locate(int global) const;  // (chapter, level in chapter)
    int global(int chapter, int level) const;
    const LevelInfo &at(int global) const;
    const ChapterInfo &chapter_of(int global) const;
};

struct Record {
    bool solved = false;
    int best_moves = 0;  // 0 = solved only with hints
    int stars = 0;
};

class Progress {
public:
    explicit Progress(int levels = 0) : records_(static_cast<std::size_t>(levels)) {}

    // Two levels may be skipped: level n is open once n - 2 levels are solved.
    bool unlocked(int global) const;
    // Records a solve; true if it beat the best (moves) or was the first. A solve helped by hints
    // earns one star and does not count as a best.
    bool finish(int global, int moves, int par, bool helped = false);
    const Record &record(int global) const { return records_[static_cast<std::size_t>(global)]; }
    int solved_count() const;
    int total_stars() const;
    int count() const { return static_cast<int>(records_.size()); }
    int next_unsolved() const;  // the first level not solved yet (or the last), to continue from
    int last_played = 0;

    // {"levels": {"<name>": {"moves": m, "stars": s}}, "last": n}; keyed by level name, so a level
    // list that changes keeps what still matches.
    std::string to_json(const Catalogue &catalogue) const;
    // Forgiving: unknown names are ignored, bad text gives empty progress.
    static Progress from_json(std::string_view json, const Catalogue &catalogue);

private:
    std::vector<Record> records_;
};

}  // namespace pathar
