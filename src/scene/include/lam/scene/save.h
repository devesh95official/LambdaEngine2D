#pragma once
// Save files: a versioned JSON envelope with migrations, slot files, and world serialisation that
// keeps the exact ECS layout. Cold path; does file IO on the calling thread.
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <lam/core/data.h>
#include <lam/core/result.h>
#include <lam/ecs/component.h>
#include <lam/ecs/world.h>
namespace lam {
// {"format":"lam-save","game":G,"version":V,"summary":{...},"data":{...}}
// read_save also accepts the old engine's "lamda-save" tag.
inline constexpr std::string_view kSaveFormat = "lam-save";
// The "layout" key of serialize_world. Bump it when the ECS layout changes.
inline constexpr int kWorldLayoutVersion = 1;
// JSON fields are passed as text so no JSON library leaks into the API.
struct SaveEnvelope {
    std::string game;                                           // non-empty
    int version = 1;                                            // >= 1
    std::string summary_json;                                   // shown in slot lists; a JSON object or "" ({})
    std::string data_json;                                      // a JSON object or "" ({})
};
// Upgrades from_version to from_version + 1. Every key of `in` must be read, copied or ignored,
// so a forgotten copy_rest fails instead of dropping data.
using SaveMigration = Status (*)(DataReader &in, DataWriter &out, int from_version);
// Fails on an empty game, version < 1, or summary/data that isn't a JSON object.
Result<std::string> write_save(const SaveEnvelope &s);
// Reads and migrates to current_version; migrations[i] upgrades i + 1 -> i + 2.
// Errors: Parse, Invalid (not a save or another game's), VersionTooNew, MigrationFailed.
Result<SaveEnvelope> read_save(std::string_view text, std::string_view game, int current_version,
                               std::span<const SaveMigration> migrations);
// Numbered save files <dir>/slot<n>.json, n in [1, slot_count], written atomically.
class SaveSlots {
public:
    // slot_count and version are clamped to at least 1. Doesn't touch the disk.
    SaveSlots(std::string dir, std::string game, int version, int slot_count = 3);
    // Uses this SaveSlots' game and version, not the envelope's. Creates `dir` if needed.
    Status save(int slot, const SaveEnvelope &s);               // OutOfRange outside [1, count]
    // NotFound for an empty slot, otherwise read_save errors.
    Result<SaveEnvelope> load(int slot, std::span<const SaveMigration> migrations = {}) const;
    Status erase(int slot);                                     // OutOfRange; missing file is ok
    bool exists(int slot) const;                                // false out of range
    int latest() const;                                         // 0 = none
    std::string slot_path(int slot) const;                      // "" out of range
    int count() const { return count_; }
    const std::string &dir() const { return dir_; }
    // summary_json per slot (index 0 = slot 1); "" for empty, broken or foreign slots.
    std::vector<std::string> summaries() const;
private:
    std::string dir_;
    std::string game_;
    int version_ = 1;
    int count_ = 1;
};
struct WorldSaveOptions { bool pretty = false; };              // pretty: serialize_world_text only
// Writes every kSerialize component and snapshot resource plus the entity table as one JSON
// object. Pending commands are not saved.
Status serialize_world(const World &w, DataWriter &out, const SerializeContext &ctx,
                       const WorldSaveOptions &o = {});
// Validates everything before touching `w`, so a failed load leaves it as it was. Assets load
// into the AssetServer's current scope.
Status deserialize_world(World &w, DataReader &in, const SerializeContext &ctx);
// serialize_world into a standalone JSON document.
Result<std::string> serialize_world_text(const World &w, const SerializeContext &ctx,
                                         const WorldSaveOptions &o = {});
// Like deserialize_world, but any unread key in the document is an error too.
Status deserialize_world_text(World &w, std::string_view json, const SerializeContext &ctx,
                              std::string_view file_name = "world");
}
