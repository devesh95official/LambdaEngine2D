// Save envelope and slots. Summary and data travel as
// JSON text and are re-emitted canonically through DataWriter.
#include <lam/scene/save.h>

#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <utility>

#include <lam/core/fs.h>

namespace lam {
namespace {

constexpr std::string_view kLegacyFormat = "lamda-save"; // the old engine's envelope tag

// Re-emits the object `in` canonically into `out` (which expects a value).
void copy_object(DataReader &in, DataWriter &out) {
    out.begin_object();
    out.copy_rest(in);
    out.end_object();
}

// Writes `json` (a JSON object, or "" for {}) canonically as the next value of `out`.
Status write_object_text(DataWriter &out, std::string_view json, std::string_view what) {
    bool blank = true;
    for (const char c : json)
        blank = blank && (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    if (blank) {
        out.begin_object().end_object();
        return {};
    }
    Result<DataDocument> doc = DataDocument::parse(json, what);
    if (!doc) return make_error(ErrorCode::Invalid, "write_save: " + doc.error().message);
    DataReader root = doc->root();
    if (!root.valid())
        return make_error(ErrorCode::Invalid, "write_save: " + std::string(what) + " must be a JSON object");
    copy_object(root, out);
    return {};
}

std::string slot_file_name(int slot) {
    return "slot" + std::to_string(slot) + ".json";
}

} // namespace

// ---- envelope ----------------------------------------------------------------------------------

Result<std::string> write_save(const SaveEnvelope &s) {
    if (s.game.empty()) return make_error(ErrorCode::Invalid, "write_save: the game id is empty");
    if (s.version < 1)
        return make_error(ErrorCode::Invalid,
                          "write_save: version " + std::to_string(s.version) + " is below 1");
    DataWriter w;
    w.begin_object();
    w.key("format").value(kSaveFormat);
    w.key("game").value(s.game);
    w.key("version").value(s.version);
    w.key("summary");
    LAM_TRY(write_object_text(w, s.summary_json, "summary"));
    w.key("data");
    LAM_TRY(write_object_text(w, s.data_json, "data"));
    w.end_object();
    Result<std::string> text = w.finish();
    if (!text) return make_error(ErrorCode::Invalid, "write_save: " + text.error().message);
    return text;
}

namespace {

// One migration step on canonical data text: from -> from + 1.
Result<std::string> migrate_step(const std::string &data, int from, SaveMigration step) {
    const std::string label = "save data v" + std::to_string(from);
    Result<DataDocument> doc = DataDocument::parse(data, label);
    if (!doc) return make_error(ErrorCode::MigrationFailed, doc.error().message);
    DataReader in = doc->root();
    DataWriter out;
    out.begin_object();
    Status s;
    try {
        s = step(in, out, from);
    } catch (const std::exception &e) {
        s = make_error(ErrorCode::MigrationFailed, std::string("exception: ") + e.what());
    } catch (...) {
        s = make_error(ErrorCode::MigrationFailed, "exception");
    }
    const std::string step_name =
        "migration " + std::to_string(from) + " -> " + std::to_string(from + 1);
    if (!s) return make_error(ErrorCode::MigrationFailed, step_name + ": " + s.error().message);
    if (Status fin = doc->finish(); !fin)
        return make_error(ErrorCode::MigrationFailed,
                          step_name + " left data unread (copy_rest or ignore it): " +
                              fin.error().message);
    out.end_object();
    Result<std::string> text = out.finish();
    if (!text) return make_error(ErrorCode::MigrationFailed, step_name + ": " + text.error().message);
    return text;
}

} // namespace

Result<SaveEnvelope> read_save(std::string_view text, std::string_view game, int current_version,
                               std::span<const SaveMigration> migrations) {
    if (current_version < 1)
        return make_error(ErrorCode::Invalid, "read_save: current_version must be at least 1");
    Result<DataDocument> doc = DataDocument::parse(text, "save");
    if (!doc) return make_error(ErrorCode::Parse, "the save is not valid JSON: " + doc.error().message);
    DataReader root = doc->root();
    if (!root.valid()) return make_error(ErrorCode::Invalid, "not a save file (root is not an object)");
    if (root.type("format") != DataType::String)
        return make_error(ErrorCode::Invalid, "not a save file (no \"format\")");
    const std::string format = root.text("format");
    if (format != kSaveFormat && format != kLegacyFormat)
        return make_error(ErrorCode::Invalid, "not a save file (format \"" + format + "\")");
    if (root.type("game") != DataType::String)
        return make_error(ErrorCode::Invalid, "the save names no game");
    std::string save_game = root.text("game");
    if (save_game != game)
        return make_error(ErrorCode::Invalid, "a save of another game (\"" + save_game +
                                                  "\", expected \"" + std::string(game) + "\")");
    if (root.type("version") != DataType::Number)
        return make_error(ErrorCode::Invalid, "the save has no version");
    const std::int64_t version = root.integer64("version", std::numeric_limits<std::int32_t>::min(),
                                                std::numeric_limits<std::int32_t>::max());
    if (root.failed()) return make_error(ErrorCode::Invalid, "the save version is not an integer");
    if (version < 1)
        return make_error(ErrorCode::Invalid,
                          "the save version " + std::to_string(version) + " is below 1");
    if (version > current_version)
        return make_error(ErrorCode::VersionTooNew,
                          "the save is version " + std::to_string(version) +
                              ", newer than this build (" + std::to_string(current_version) + ")");
    SaveEnvelope out;
    out.game = std::move(save_game);
    {
        DataWriter w;
        const DataType summary_type = root.type("summary");
        if (summary_type == DataType::Object) {
            DataReader s = root.object("summary");
            copy_object(s, w);
        } else if (summary_type == DataType::Missing || summary_type == DataType::Null) {
            // The old engine wrote "summary": null when a caller passed a null summary.
            if (summary_type == DataType::Null) root.ignore("summary");
            w.begin_object().end_object();
        } else {
            return make_error(ErrorCode::Invalid, "the save summary is not an object");
        }
        Result<std::string> t = w.finish();
        if (!t) return make_error(ErrorCode::Invalid, t.error().message);
        out.summary_json = std::move(*t);
    }
    {
        if (root.type("data") != DataType::Object)
            return make_error(ErrorCode::Invalid, "the save has no data object");
        DataReader d = root.object("data");
        DataWriter w;
        copy_object(d, w);
        Result<std::string> t = w.finish();
        if (!t) return make_error(ErrorCode::Invalid, t.error().message);
        out.data_json = std::move(*t);
    }
    if (Status fin = doc->finish(); !fin)
        return make_error(ErrorCode::Invalid, "the save envelope: " + fin.error().message);
    // Stepwise upgrade: migrations[v - 1] takes v to v + 1.
    for (int v = static_cast<int>(version); v < current_version; ++v) {
        const auto idx = static_cast<std::size_t>(v - 1);
        if (idx >= migrations.size() || migrations[idx] == nullptr)
            return make_error(ErrorCode::MigrationFailed,
                              "the save is version " + std::to_string(v) +
                                  " and no migration to " + std::to_string(v + 1) + " was given");
        Result<std::string> next = migrate_step(out.data_json, v, migrations[idx]);
        if (!next) return next.error();
        out.data_json = std::move(*next);
    }
    out.version = current_version;
    return out;
}

// ---- slots -------------------------------------------------------------------------------------

SaveSlots::SaveSlots(std::string dir, std::string game, int version, int slot_count)
    : dir_(std::move(dir)), game_(std::move(game)), version_(version < 1 ? 1 : version),
      count_(slot_count < 1 ? 1 : slot_count) {}

std::string SaveSlots::slot_path(int slot) const {
    if (slot < 1 || slot > count_) return {};
    return join_path(dir_, slot_file_name(slot));
}

bool SaveSlots::exists(int slot) const {
    const std::string p = slot_path(slot);
    return !p.empty() && file_exists(p);
}

Status SaveSlots::save(int slot, const SaveEnvelope &s) {
    if (slot < 1 || slot > count_)
        return make_error(ErrorCode::OutOfRange, "save slot " + std::to_string(slot) +
                                                     " is outside [1, " + std::to_string(count_) + "]");
    SaveEnvelope e;
    e.game = game_;
    e.version = version_;
    e.summary_json = s.summary_json;
    e.data_json = s.data_json;
    Result<std::string> text = write_save(e);
    if (!text) return text.error();
    LAM_TRY(make_directories(dir_));
    return write_file_atomic(slot_path(slot), *text);
}

Result<SaveEnvelope> SaveSlots::load(int slot, std::span<const SaveMigration> migrations) const {
    if (slot < 1 || slot > count_)
        return make_error(ErrorCode::OutOfRange, "save slot " + std::to_string(slot) +
                                                     " is outside [1, " + std::to_string(count_) + "]");
    const std::string path = slot_path(slot);
    if (!file_exists(path))
        return make_error(ErrorCode::NotFound, "save slot " + std::to_string(slot) + " is empty");
    Result<std::string> text = read_file(path);
    if (!text) return text.error();
    Result<SaveEnvelope> r = read_save(*text, game_, version_, migrations);
    if (!r) return make_error(r.error().code, path + ": " + r.error().message);
    return r;
}

Status SaveSlots::erase(int slot) {
    if (slot < 1 || slot > count_)
        return make_error(ErrorCode::OutOfRange, "save slot " + std::to_string(slot) +
                                                     " is outside [1, " + std::to_string(count_) + "]");
    return remove_file(slot_path(slot));
}

int SaveSlots::latest() const {
    int best = 0;
    std::int64_t best_time = 0;
    for (int slot = 1; slot <= count_; ++slot) {
        const std::string p = slot_path(slot);
        if (!file_exists(p)) continue;
        Result<std::int64_t> t = file_mtime(p);
        if (!t) continue;
        if (best == 0 || *t > best_time) {
            best = slot;
            best_time = *t;
        }
    }
    return best;
}

std::vector<std::string> SaveSlots::summaries() const {
    std::vector<std::string> list(static_cast<std::size_t>(count_));
    for (int slot = 1; slot <= count_; ++slot) {
        const std::string p = slot_path(slot);
        if (!file_exists(p)) continue;
        Result<std::string> text = read_file(p);
        if (!text) continue;
        Result<DataDocument> doc = DataDocument::parse(*text, p);
        if (!doc) continue;
        DataReader root = doc->root();
        if (!root.valid() || root.type("game") != DataType::String || root.text("game") != game_ ||
            root.type("summary") != DataType::Object)
            continue;
        DataReader s = root.object("summary");
        DataWriter w;
        copy_object(s, w);
        if (Result<std::string> t = w.finish()) list[static_cast<std::size_t>(slot - 1)] = std::move(*t);
    }
    return list;
}

} // namespace lam
