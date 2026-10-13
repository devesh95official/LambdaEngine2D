#include <lam/core/settings.h>

#include <cmath>
#include <limits>

#include <lam/core/assert.h>
#include <lam/core/data.h>
#include <lam/core/fs.h>
#include <lam/core/log.h>

namespace lam {

SettingsStore::Entry *SettingsStore::find(std::string_view key) {
    for (Entry &e : entries_)
        if (e.key == key) return &e;
    return nullptr;
}

const SettingsStore::Entry *SettingsStore::find(std::string_view key) const {
    for (const Entry &e : entries_)
        if (e.key == key) return &e;
    return nullptr;
}

SettingsStore::Entry &SettingsStore::find_or_add(std::string_view key) {
    if (Entry *e = find(key)) return *e;
    entries_.push_back(Entry{});
    entries_.back().key = std::string(key);
    return entries_.back();
}

// Validates a value loaded before its definition against the new schema; bad values keep the default.
void SettingsStore::adopt_raw(Entry &e) {
    const Kind raw = e.raw_kind;
    e.raw_kind = Kind::Raw;
    switch (e.kind) {
    case Kind::Int:
        if (raw == Kind::Float && e.num == std::floor(e.num) && e.num >= e.min && e.num <= e.max) return;
        break;
    case Kind::Float:
        if (raw == Kind::Float && std::isfinite(e.num) && e.num >= e.min && e.num <= e.max) return;
        break;
    case Kind::Bool:
        if (raw == Kind::Bool) return;
        break;
    case Kind::String:
        if (raw == Kind::String) return;
        break;
    case Kind::Raw: return;
    }
    if (raw != Kind::Raw) LAM_LOG_WARN("settings: bad value for \"{}\"; using the default", e.key);
    e.num = e.def_num;
    e.flag = e.def_flag;
    e.str = e.def_str;
}

void SettingsStore::define_int(std::string_view key, int def, int min, int max) {
    Entry &e = find_or_add(key);
    const bool had_raw = !e.defined && e.raw_kind != Kind::Raw;
    e.kind = Kind::Int;
    e.defined = true;
    e.min = min;
    e.max = max;
    e.def_num = def < min ? min : (def > max ? max : def);
    if (had_raw) adopt_raw(e);
    else e.num = e.def_num;
    ++revision_;
}

void SettingsStore::define_float(std::string_view key, float def, float min, float max) {
    Entry &e = find_or_add(key);
    const bool had_raw = !e.defined && e.raw_kind != Kind::Raw;
    e.kind = Kind::Float;
    e.defined = true;
    e.min = min;
    e.max = max;
    e.def_num = def < min ? min : (def > max ? max : def);
    if (had_raw) adopt_raw(e);
    else e.num = e.def_num;
    ++revision_;
}

void SettingsStore::define_bool(std::string_view key, bool def) {
    Entry &e = find_or_add(key);
    const bool had_raw = !e.defined && e.raw_kind != Kind::Raw;
    e.kind = Kind::Bool;
    e.defined = true;
    e.def_flag = def;
    if (had_raw) adopt_raw(e);
    else e.flag = def;
    ++revision_;
}

void SettingsStore::define_string(std::string_view key, std::string_view def) {
    Entry &e = find_or_add(key);
    const bool had_raw = !e.defined && e.raw_kind != Kind::Raw;
    e.kind = Kind::String;
    e.defined = true;
    e.def_str = std::string(def);
    if (had_raw) adopt_raw(e);
    else e.str = e.def_str;
    ++revision_;
}

bool SettingsStore::defined(std::string_view key) const {
    const Entry *e = find(key);
    return e && e->defined;
}

int SettingsStore::get_int(std::string_view key) const {
    const Entry *e = find(key);
    LAM_ASSERT(e && e->defined && e->kind == Kind::Int, "SettingsStore::get_int: key not defined as int");
    return e && e->defined && e->kind == Kind::Int ? static_cast<int>(e->num) : 0;
}

float SettingsStore::get_float(std::string_view key) const {
    const Entry *e = find(key);
    LAM_ASSERT(e && e->defined && e->kind == Kind::Float, "SettingsStore::get_float: key not defined as float");
    return e && e->defined && e->kind == Kind::Float ? static_cast<float>(e->num) : 0.0f;
}

bool SettingsStore::get_bool(std::string_view key) const {
    const Entry *e = find(key);
    LAM_ASSERT(e && e->defined && e->kind == Kind::Bool, "SettingsStore::get_bool: key not defined as bool");
    return e && e->defined && e->kind == Kind::Bool && e->flag;
}

const std::string &SettingsStore::get_string(std::string_view key) const {
    static const std::string empty;
    const Entry *e = find(key);
    LAM_ASSERT(e && e->defined && e->kind == Kind::String, "SettingsStore::get_string: key not defined as string");
    return e && e->defined && e->kind == Kind::String ? e->str : empty;
}

void SettingsStore::set_int(std::string_view key, int v) {
    Entry *e = find(key);
    LAM_ASSERT(e && e->defined && e->kind == Kind::Int, "SettingsStore::set_int: key not defined as int");
    if (!e || !e->defined || e->kind != Kind::Int) return;
    const double c = v < e->min ? e->min : (v > e->max ? e->max : v);
    if (c != e->num) {
        e->num = c;
        ++revision_;
    }
}

void SettingsStore::set_float(std::string_view key, float v) {
    Entry *e = find(key);
    LAM_ASSERT(e && e->defined && e->kind == Kind::Float, "SettingsStore::set_float: key not defined as float");
    if (!e || !e->defined || e->kind != Kind::Float || !std::isfinite(v)) return;
    const double c = v < e->min ? e->min : (v > e->max ? e->max : static_cast<double>(v));
    if (c != e->num) {
        e->num = c;
        ++revision_;
    }
}

void SettingsStore::set_bool(std::string_view key, bool v) {
    Entry *e = find(key);
    LAM_ASSERT(e && e->defined && e->kind == Kind::Bool, "SettingsStore::set_bool: key not defined as bool");
    if (!e || !e->defined || e->kind != Kind::Bool) return;
    if (e->flag != v) {
        e->flag = v;
        ++revision_;
    }
}

void SettingsStore::set_string(std::string_view key, std::string_view v) {
    Entry *e = find(key);
    LAM_ASSERT(e && e->defined && e->kind == Kind::String, "SettingsStore::set_string: key not defined as string");
    if (!e || !e->defined || e->kind != Kind::String) return;
    if (e->str != v) {
        e->str = std::string(v);
        ++revision_;
    }
}

void SettingsStore::reset(std::string_view key) {
    Entry *e = find(key);
    if (!e || !e->defined) return;
    e->num = e->def_num;
    e->flag = e->def_flag;
    e->str = e->def_str;
    ++revision_;
}

Status SettingsStore::load(const std::string &path) {
    auto text = read_file(path);
    if (!text) {
        if (text.error().code == ErrorCode::NotFound) return {};
        return text.error();
    }
    auto doc = DataDocument::parse(*text, path);
    if (!doc) {
        LAM_LOG_WARN("{}; keeping default settings", doc.error().message);
        return {};
    }
    DataReader root = doc->root();
    for (std::string_view key : root.keys()) {
        // Each value is re-parsed on its own: reader errors are sticky per document, so one bad
        // value must not zero the keys after it. The raw text is kept for save() (unknown keys,
        // including objects, arrays and null, are written back unchanged).
        DataWriter one;
        one.begin_object().copy(root, key).end_object();
        auto raw = one.finish();
        if (!raw) continue;                                     // e.g. a number that overflowed to inf
        auto sub = DataDocument::parse(*raw, path);
        if (!sub) continue;
        DataReader r = sub->root();
        Entry &e = find_or_add(key);
        e.raw_json = std::move(*raw);
        e.raw_kind = Kind::Raw;
        switch (r.type(key)) {
        case DataType::Number: {
            constexpr double big = std::numeric_limits<double>::max();
            const double d = r.number64(key, -big, big);        // integers up to 2^53 stay exact
            if (!sub->failed()) {
                e.num = d;
                e.raw_kind = Kind::Float;
            }
            break;
        }
        case DataType::Bool:
            e.flag = r.boolean(key);
            e.raw_kind = Kind::Bool;
            break;
        case DataType::String:
            e.str = r.text(key);
            if (!sub->failed()) e.raw_kind = Kind::String;
            break;
        default: break;                                         // kept raw only
        }
        if (e.defined) adopt_raw(e);
    }
    ++revision_;
    return {};
}

Status SettingsStore::save(const std::string &path) const {
    DataWriter w;
    w.begin_object();
    for (const Entry &e : entries_) {
        if (!e.defined) {                                       // unknown key: write it back unchanged
            auto one = DataDocument::parse(e.raw_json, path);
            if (!one) continue;
            DataReader r = one->root();
            w.copy(r, e.key);
            continue;
        }
        switch (e.kind) {
        case Kind::Int: w.key(e.key).value(static_cast<std::int64_t>(e.num)); break;
        case Kind::Float: w.key(e.key).value(e.num); break;
        case Kind::Bool: w.key(e.key).value(e.flag); break;
        case Kind::String: w.key(e.key).value(std::string_view(e.str)); break;
        case Kind::Raw: break;
        }
    }
    w.end_object();
    auto text = w.finish(true);
    if (!text) return text.error();
    return write_file_atomic(path, *text);
}

std::uint32_t SettingsStore::revision() const { return revision_; }

} // namespace lam
