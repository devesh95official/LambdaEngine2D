#pragma once
// Typed key/value settings with a schema, owned by the App. Loaded before plugins build and
// saved on quit and on Background (not in scripted runs). Keys loaded before define_*() are
// kept raw and validated once defined.
//   s.define_float("audio.music", 0.8f, 0, 1);  float v = s.get_float("audio.music");
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <lam/core/result.h>
namespace lam {
class SettingsStore {
public:
    void define_int(std::string_view key, int def, int min, int max);
    void define_float(std::string_view key, float def, float min, float max);
    void define_bool(std::string_view key, bool def);
    void define_string(std::string_view key, std::string_view def);
    bool defined(std::string_view key) const;
    // Getters LAM_ASSERT that the key is defined with that type; undefined -> 0/false/"".
    int get_int(std::string_view key) const;
    float get_float(std::string_view key) const;
    bool get_bool(std::string_view key) const;
    const std::string &get_string(std::string_view key) const;
    // Setters clamp to the schema; undefined keys are ignored (LAM_ASSERT).
    void set_int(std::string_view key, int v);
    void set_float(std::string_view key, float v);
    void set_bool(std::string_view key, bool v);
    void set_string(std::string_view key, std::string_view v);
    void reset(std::string_view key);
    // Forgiving: bad or missing values keep their defaults. A missing file is not an error.
    Status load(const std::string &path);
    // Atomic write (write_file_atomic). Unknown loaded keys are written back unchanged.
    Status save(const std::string &path) const;
    // Increments on every change (systems poll it instead of subscribing).
    std::uint32_t revision() const;
private:
    enum class Kind : std::uint8_t { Int, Float, Bool, String, Raw };
    struct Entry {
        std::string key;
        Kind kind = Kind::Raw;
        bool defined = false;
        double num = 0, def_num = 0, min = 0, max = 0;
        bool flag = false, def_flag = false;
        std::string str, def_str;
        Kind raw_kind = Kind::Raw;                              // type of a loaded-but-undefined value
        std::string raw_json;                                   // loaded {"key": value}, saved verbatim while undefined
    };
    Entry *find(std::string_view key);
    const Entry *find(std::string_view key) const;
    Entry &find_or_add(std::string_view key);
    void adopt_raw(Entry &e);
    std::vector<Entry> entries_;
    std::uint32_t revision_ = 0;
};
}
