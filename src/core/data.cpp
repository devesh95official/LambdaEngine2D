// Strict JSON reading and canonical writing on nlohmann::ordered_json. nlohmann and its
// exceptions never leave this file: every entry point catches and converts to Error.
#include <lam/core/data.h>

#include <charconv>
#include <cmath>
#include <deque>
#include <limits>

#include <nlohmann/json.hpp>

#include <lam/core/text.h>

namespace lam {
namespace {

using Json = nlohmann::ordered_json;

struct Record {
    const Json *node = nullptr;
    std::string path;
    std::vector<std::string> read;
    bool all_read = false;
};

constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;

const std::string &empty_string() {
    static const std::string s;
    return s;
}

DataType type_of(const Json *v) {
    if (!v) return DataType::Missing;
    switch (v->type()) {
    case Json::value_t::null: return DataType::Null;
    case Json::value_t::boolean: return DataType::Bool;
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
    case Json::value_t::number_float: return DataType::Number;
    case Json::value_t::string: return DataType::String;
    case Json::value_t::array: return DataType::Array;
    case Json::value_t::object: return DataType::Object;
    default: return DataType::Missing;
    }
}

bool parse_hex(std::string_view s, std::uint64_t &out) {
    if (s.empty() || s.size() > 16) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | static_cast<std::uint64_t>(d);
    }
    out = v;
    return true;
}

} // namespace

struct DataDocument::Impl {
    Json root;
    std::string file;
    std::string error;
    std::deque<Record> records;

    void fail(const std::string &path, std::string_view message) {
        if (!error.empty()) return;
        error = file + ": " + (path.empty() ? std::string("/") : path) + ": " + std::string(message);
    }
    std::uint32_t add_record(const Json *node, std::string path) {
        records.push_back(Record{node, std::move(path), {}, false});
        return static_cast<std::uint32_t>(records.size() - 1);
    }
};

namespace {

using Impl = DataDocument::Impl;

Impl *impl_of(void *doc) { return static_cast<Impl *>(doc); }

const Json *node_of(const void *node) { return static_cast<const Json *>(node); }

// Scalar decoding shared by objects and arrays. `fail` records the error at the right path.
template <class Fail> double as_number(const Json *v, double min, double max, Fail fail) {
    if (!v->is_number()) {
        fail("expected a number");
        return 0;
    }
    const double d = v->get<double>();
    if (!std::isfinite(d)) {
        fail("number is not finite");
        return 0;
    }
    if (d < min || d > max) {
        fail("number " + std::to_string(d) + " is outside [" + std::to_string(min) + ", " + std::to_string(max) + "]");
        return 0;
    }
    return d;
}

template <class Fail> std::uint64_t as_u64(const Json *v, Fail fail) {
    if (v->is_number_unsigned()) return v->get<std::uint64_t>();
    if (v->is_number_integer() && v->get<std::int64_t>() >= 0) return static_cast<std::uint64_t>(v->get<std::int64_t>());
    fail("expected a non-negative integer");
    return 0;
}

template <class Fail> bool as_int64(const Json *v, std::int64_t &out, Fail fail) {
    // is_number_integer() is also true for unsigned values, so test unsigned first.
    if (v->is_number_unsigned()) {
        const auto u = v->get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            fail("integer too large");
            return false;
        }
        out = static_cast<std::int64_t>(u);
        return true;
    }
    if (v->is_number_integer()) {
        out = v->get<std::int64_t>();
        return true;
    }
    if (v->is_number_float()) {
        const double d = v->get<double>();
        if (!std::isfinite(d) || d != std::floor(d)) {
            fail("expected a whole number");
            return false;
        }
        if (d < -9.2233720368547758e18 || d >= 9.2233720368547758e18) {
            fail("integer out of range");
            return false;
        }
        out = static_cast<std::int64_t>(d);
        return true;
    }
    fail("expected an integer");
    return false;
}

template <class Fail> std::int64_t as_int_range(const Json *v, std::int64_t min, std::int64_t max, Fail fail) {
    std::int64_t i = 0;
    if (!as_int64(v, i, fail)) return 0;
    if (i < min || i > max) {
        fail("integer " + std::to_string(i) + " is outside [" + std::to_string(min) + ", " + std::to_string(max) + "]");
        return 0;
    }
    return i;
}

template <class Fail> std::string as_string(const Json *v, bool allow_empty, Fail fail) {
    if (!v->is_string()) {
        fail("expected a string");
        return {};
    }
    const auto &s = v->get_ref<const std::string &>();
    if (!allow_empty && s.empty()) {
        fail("string must not be empty");
        return {};
    }
    if (!utf8_valid(s)) {
        fail("string is not valid UTF-8");
        return {};
    }
    return s;
}

template <class Fail> StrId as_id(const Json *v, Fail fail) {
    const std::string s = as_string(v, false, fail);
    if (s.empty()) return {};
    if (s.size() == 17 && s[0] == '#') {
        std::uint64_t value = 0;
        if (parse_hex(std::string_view(s).substr(1), value)) return StrId{value};
    }
    return intern(s);
}

template <class Fail> Vec2 as_vec2(const Json *v, Fail fail) {
    if (!v->is_array() || v->size() != 2) {
        fail("expected [x, y]");
        return {};
    }
    const float lo = -std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::max();
    return {static_cast<float>(as_number(&(*v)[0], lo, hi, fail)),
            static_cast<float>(as_number(&(*v)[1], lo, hi, fail))};
}

template <class Fail> IVec2 as_ivec2(const Json *v, Fail fail) {
    if (!v->is_array() || v->size() != 2) {
        fail("expected [x, y]");
        return {};
    }
    constexpr std::int64_t lo = std::numeric_limits<int>::min(), hi = std::numeric_limits<int>::max();
    return {static_cast<int>(as_int_range(&(*v)[0], lo, hi, fail)),
            static_cast<int>(as_int_range(&(*v)[1], lo, hi, fail))};
}

template <class Fail> Color as_color(const Json *v, Fail fail) {
    if (v->is_string()) {
        const auto &s = v->get_ref<const std::string &>();
        std::uint64_t value = 0;
        if (s.size() == 7 && s[0] == '#' && parse_hex(std::string_view(s).substr(1), value))
            return Color::hex(static_cast<std::uint32_t>((value << 8) | 0xFFu));
        if (s.size() == 9 && s[0] == '#' && parse_hex(std::string_view(s).substr(1), value))
            return Color::hex(static_cast<std::uint32_t>(value));
        fail("expected a colour \"#rrggbb\" or \"#rrggbbaa\"");
        return {};
    }
    if (v->is_array() && (v->size() == 3 || v->size() == 4)) {
        Color c;
        c.r = static_cast<std::uint8_t>(as_int_range(&(*v)[0], 0, 255, fail));
        c.g = static_cast<std::uint8_t>(as_int_range(&(*v)[1], 0, 255, fail));
        c.b = static_cast<std::uint8_t>(as_int_range(&(*v)[2], 0, 255, fail));
        c.a = v->size() == 4 ? static_cast<std::uint8_t>(as_int_range(&(*v)[3], 0, 255, fail)) : 255;
        return c;
    }
    fail("expected a colour \"#rrggbb[aa]\" or [r, g, b(, a)]");
    return {};
}

template <class Fail> Rect as_rect(const Json *v, Fail fail) {
    if (!v->is_array() || v->size() != 4) {
        fail("expected [x, y, w, h]");
        return {};
    }
    const float lo = -std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::max();
    Rect r;
    r.x = static_cast<float>(as_number(&(*v)[0], lo, hi, fail));
    r.y = static_cast<float>(as_number(&(*v)[1], lo, hi, fail));
    r.w = static_cast<float>(as_number(&(*v)[2], lo, hi, fail));
    r.h = static_cast<float>(as_number(&(*v)[3], lo, hi, fail));
    return r;
}

template <class Fail> IRect as_irect(const Json *v, Fail fail) {
    if (!v->is_array() || v->size() != 4) {
        fail("expected [x, y, w, h]");
        return {};
    }
    constexpr std::int64_t lo = std::numeric_limits<int>::min(), hi = std::numeric_limits<int>::max();
    IRect r;
    r.x = static_cast<int>(as_int_range(&(*v)[0], lo, hi, fail));
    r.y = static_cast<int>(as_int_range(&(*v)[1], lo, hi, fail));
    r.w = static_cast<int>(as_int_range(&(*v)[2], lo, hi, fail));
    r.h = static_cast<int>(as_int_range(&(*v)[3], lo, hi, fail));
    return r;
}

float neutral(float min, float max) { return 0.0f < min ? min : (0.0f > max ? max : 0.0f); }

} // namespace

// ---------------------------------------------------------------------------------------------
// DataDocument

DataDocument::DataDocument() : impl_(std::make_unique<Impl>()) {}
DataDocument::~DataDocument() = default;
DataDocument::DataDocument(DataDocument &&) noexcept = default;
DataDocument &DataDocument::operator=(DataDocument &&) noexcept = default;

namespace {
// Deepest array/object nesting in JSON text (brackets inside strings ignored); stops counting once
// past `limit`. Malformed text is left to the parser.
int nesting_depth(std::string_view text, int limit) {
    int depth = 0, deepest = 0;
    bool in_string = false, escaped = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '[' || c == '{') {
            if (++depth > deepest) deepest = depth;
            if (deepest > limit) return deepest;
        } else if (c == ']' || c == '}') {
            --depth;
        }
    }
    return deepest;
}
} // namespace

Result<DataDocument> DataDocument::parse(std::string_view text, std::string_view file_name) {
    // nlohmann's dump() and copies recurse once per level: bound the nesting before parsing
    // (a 100k-deep save summary crashed SaveSlots::summaries).
    if (nesting_depth(text, kMaxDataDepth) > kMaxDataDepth)
        return make_error(ErrorCode::Parse, std::string(file_name) + ": JSON nested deeper than " +
                                                std::to_string(kMaxDataDepth) + " levels");
    DataDocument doc;
    doc.impl_->file = std::string(file_name);
    try {
        doc.impl_->root = Json::parse(text.begin(), text.end());
    } catch (const std::exception &e) {
        return make_error(ErrorCode::Parse, std::string(file_name) + ": " + e.what());
    }
    return doc;
}

DataReader DataDocument::root() {
    DataReader r;
    r.doc_ = impl_.get();
    if (!impl_->root.is_object()) {
        impl_->fail("", "the document root must be an object");
        r.slot_ = kNoSlot;
        return r;
    }
    r.node_ = &impl_->root;
    r.slot_ = impl_->add_record(&impl_->root, "");
    return r;
}

Status DataDocument::finish() {
    if (impl_->error.empty()) {
        for (const Record &rec : impl_->records) {
            if (rec.all_read || !rec.node) continue;
            for (const auto &item : rec.node->items()) {
                bool read = false;
                for (const std::string &k : rec.read)
                    if (k == item.key()) {
                        read = true;
                        break;
                    }
                if (!read) {
                    impl_->fail(rec.path, "unknown key \"" + item.key() + "\"");
                    break;
                }
            }
            if (!impl_->error.empty()) break;
        }
    }
    if (!impl_->error.empty()) return make_error(ErrorCode::Parse, impl_->error);
    return {};
}

bool DataDocument::failed() const { return !impl_->error.empty(); }

// ---------------------------------------------------------------------------------------------
// DataReader

namespace {

struct ReaderAccess {
    Impl *doc;
    const Json *node;
    std::uint32_t slot;

    const Json *find(std::string_view key) const {
        if (!node) return nullptr;
        const auto it = node->find(key);
        return it == node->end() ? nullptr : &*it;
    }
    void mark(std::string_view key) const {
        if (!doc || slot == kNoSlot) return;
        Record &rec = doc->records[slot];
        for (const std::string &k : rec.read)
            if (k == key) return;
        rec.read.emplace_back(key);
    }
    const std::string &path() const {
        if (!doc || slot == kNoSlot) return empty_string();
        return doc->records[slot].path;
    }
    void fail_at(std::string_view key, std::string_view message) const {
        if (!doc) return;
        doc->fail(path() + "/" + std::string(key), message);
    }
    // Looks up a required key: marks it read; records "missing" when absent.
    const Json *require(std::string_view key) const {
        mark(key);
        if (!doc || !doc->error.empty()) return nullptr;
        const Json *v = find(key);
        if (!v) fail_at(key, "missing required key");
        return v;
    }
    // Looks up an optional key: marks it read; null when absent or after an earlier error.
    const Json *optional(std::string_view key) const {
        mark(key);
        if (!doc || !doc->error.empty()) return nullptr;
        return find(key);
    }
};

ReaderAccess access(const DataReader &r, void *doc, const void *node, std::uint32_t slot) {
    (void)r;
    return {impl_of(doc), node_of(node), slot};
}

} // namespace

#define LAM_READER ReaderAccess a = access(*this, doc_, node_, slot_)
#define LAM_FAILER(key) [&](std::string_view m) { a.fail_at(key, m); }

bool DataReader::has(std::string_view key) const {
    LAM_READER;
    return a.find(key) != nullptr;
}

DataType DataReader::type(std::string_view key) const {
    LAM_READER;
    return type_of(a.find(key));
}

float DataReader::number(std::string_view key, float min, float max) {
    LAM_READER;
    const Json *v = a.require(key);
    if (!v) return neutral(min, max);
    return static_cast<float>(as_number(v, min, max, LAM_FAILER(key)));
}

float DataReader::number_or(std::string_view key, float fallback, float min, float max) {
    LAM_READER;
    const Json *v = a.optional(key);
    if (!v) return fallback;
    return static_cast<float>(as_number(v, min, max, LAM_FAILER(key)));
}

int DataReader::integer(std::string_view key, int min, int max) {
    LAM_READER;
    const Json *v = a.require(key);
    if (!v) return static_cast<int>(neutral(static_cast<float>(min), static_cast<float>(max)));
    return static_cast<int>(as_int_range(v, min, max, LAM_FAILER(key)));
}

int DataReader::integer_or(std::string_view key, int fallback, int min, int max) {
    LAM_READER;
    const Json *v = a.optional(key);
    if (!v) return fallback;
    return static_cast<int>(as_int_range(v, min, max, LAM_FAILER(key)));
}

std::int64_t DataReader::integer64(std::string_view key, std::int64_t min, std::int64_t max) {
    LAM_READER;
    const Json *v = a.require(key);
    if (!v) return 0;
    return as_int_range(v, min, max, LAM_FAILER(key));
}

std::uint64_t DataReader::u64(std::string_view key) {
    LAM_READER;
    const Json *v = a.require(key);
    return v ? as_u64(v, LAM_FAILER(key)) : 0;
}

double DataReader::number64(std::string_view key, double min, double max) {
    LAM_READER;
    const Json *v = a.require(key);
    if (!v) return 0.0 < min ? min : (0.0 > max ? max : 0.0);
    return as_number(v, min, max, LAM_FAILER(key));
}

bool DataReader::boolean(std::string_view key) {
    LAM_READER;
    const Json *v = a.require(key);
    if (!v) return false;
    if (!v->is_boolean()) {
        a.fail_at(key, "expected true or false");
        return false;
    }
    return v->get<bool>();
}

bool DataReader::boolean_or(std::string_view key, bool fallback) {
    LAM_READER;
    const Json *v = a.optional(key);
    if (!v) return fallback;
    if (!v->is_boolean()) {
        a.fail_at(key, "expected true or false");
        return fallback;
    }
    return v->get<bool>();
}

std::string DataReader::string(std::string_view key) {
    LAM_READER;
    const Json *v = a.require(key);
    return v ? as_string(v, false, LAM_FAILER(key)) : std::string{};
}

std::string DataReader::text(std::string_view key) {
    LAM_READER;
    const Json *v = a.require(key);
    return v ? as_string(v, true, LAM_FAILER(key)) : std::string{};
}

std::string DataReader::text_or(std::string_view key, std::string_view fallback) {
    LAM_READER;
    const Json *v = a.optional(key);
    return v ? as_string(v, true, LAM_FAILER(key)) : std::string(fallback);
}

StrId DataReader::id(std::string_view key) {
    LAM_READER;
    const Json *v = a.require(key);
    return v ? as_id(v, LAM_FAILER(key)) : StrId{};
}

Vec2 DataReader::vec2(std::string_view key) {
    LAM_READER;
    const Json *v = a.require(key);
    return v ? as_vec2(v, LAM_FAILER(key)) : Vec2{};
}

IVec2 DataReader::ivec2(std::string_view key) {
    LAM_READER;
    const Json *v = a.require(key);
    return v ? as_ivec2(v, LAM_FAILER(key)) : IVec2{};
}

Color DataReader::color(std::string_view key) {
    LAM_READER;
    const Json *v = a.require(key);
    return v ? as_color(v, LAM_FAILER(key)) : Color{};
}

Rect DataReader::rect(std::string_view key) {
    LAM_READER;
    const Json *v = a.require(key);
    return v ? as_rect(v, LAM_FAILER(key)) : Rect{};
}

IRect DataReader::irect(std::string_view key) {
    LAM_READER;
    const Json *v = a.require(key);
    return v ? as_irect(v, LAM_FAILER(key)) : IRect{};
}

DataReader DataReader::object(std::string_view key) {
    LAM_READER;
    DataReader r;
    r.doc_ = doc_;
    r.slot_ = kNoSlot;
    const Json *v = a.require(key);
    if (!v) return r;
    if (!v->is_object()) {
        a.fail_at(key, "expected an object");
        return r;
    }
    r.node_ = v;
    r.slot_ = a.doc->add_record(v, a.path() + "/" + std::string(key));
    return r;
}

DataArray DataReader::array(std::string_view key) {
    LAM_READER;
    DataArray arr;
    arr.doc_ = doc_;
    arr.path_ = a.path() + "/" + std::string(key);
    const Json *v = a.require(key);
    if (!v) return arr;
    if (!v->is_array()) {
        a.fail_at(key, "expected an array");
        return arr;
    }
    arr.node_ = v;
    return arr;
}

std::vector<std::string_view> DataReader::keys() const {
    std::vector<std::string_view> out;
    const Json *n = node_of(node_);
    if (!n) return out;
    for (const auto &item : n->items()) out.emplace_back(item.key());
    return out;
}

void DataReader::ignore(std::string_view key) {
    LAM_READER;
    a.mark(key);
}

void DataReader::ignore_unknown() {
    if (!doc_ || slot_ == kNoSlot) return;
    impl_of(doc_)->records[slot_].all_read = true;
}

const std::string &DataReader::path() const {
    LAM_READER;
    return a.path();
}

void DataReader::fail(std::string_view message) {
    if (!doc_) return;
    impl_of(doc_)->fail(path(), message);
}

bool DataReader::failed() const { return doc_ && !impl_of(doc_)->error.empty(); }

#undef LAM_READER
#undef LAM_FAILER

// ---------------------------------------------------------------------------------------------
// DataArray

namespace {
struct ArrayAccess {
    Impl *doc;
    const Json *node;
    const std::string &path;
    void fail_at(std::size_t i, std::string_view m) const {
        if (doc) doc->fail(path + "/" + std::to_string(i), m);
    }
    const Json *at(std::size_t i) const {
        if (!doc || !doc->error.empty() || !node) return nullptr;
        if (i >= node->size()) {
            fail_at(i, "index out of range");
            return nullptr;
        }
        return &(*node)[i];
    }
};
} // namespace

#define LAM_ARRAY ArrayAccess a{impl_of(doc_), node_of(node_), path_}
#define LAM_AFAIL [&](std::string_view m) { a.fail_at(i, m); }

std::size_t DataArray::size() const {
    const Json *n = node_of(node_);
    return n ? n->size() : 0;
}

DataType DataArray::type(std::size_t i) const {
    const Json *n = node_of(node_);
    if (!n || i >= n->size()) return DataType::Missing;
    return type_of(&(*n)[i]);
}

DataReader DataArray::object(std::size_t i) {
    LAM_ARRAY;
    DataReader r;
    r.doc_ = doc_;
    r.slot_ = kNoSlot;
    const Json *v = a.at(i);
    if (!v) return r;
    if (!v->is_object()) {
        a.fail_at(i, "expected an object");
        return r;
    }
    r.node_ = v;
    r.slot_ = a.doc->add_record(v, path_ + "/" + std::to_string(i));
    return r;
}

DataArray DataArray::array(std::size_t i) {
    LAM_ARRAY;
    DataArray arr;
    arr.doc_ = doc_;
    arr.path_ = path_ + "/" + std::to_string(i);
    const Json *v = a.at(i);
    if (!v) return arr;
    if (!v->is_array()) {
        a.fail_at(i, "expected an array");
        return arr;
    }
    arr.node_ = v;
    return arr;
}

float DataArray::number(std::size_t i, float min, float max) {
    LAM_ARRAY;
    const Json *v = a.at(i);
    return v ? static_cast<float>(as_number(v, min, max, LAM_AFAIL)) : neutral(min, max);
}

int DataArray::integer(std::size_t i, int min, int max) {
    LAM_ARRAY;
    const Json *v = a.at(i);
    return v ? static_cast<int>(as_int_range(v, min, max, LAM_AFAIL)) : 0;
}

std::int64_t DataArray::integer64(std::size_t i, std::int64_t min, std::int64_t max) {
    LAM_ARRAY;
    const Json *v = a.at(i);
    return v ? as_int_range(v, min, max, LAM_AFAIL) : 0;
}

std::string DataArray::string(std::size_t i) {
    LAM_ARRAY;
    const Json *v = a.at(i);
    return v ? as_string(v, false, LAM_AFAIL) : std::string{};
}

std::string DataArray::text(std::size_t i) {
    LAM_ARRAY;
    const Json *v = a.at(i);
    return v ? as_string(v, true, LAM_AFAIL) : std::string{};
}

double DataArray::number64(std::size_t i, double min, double max) {
    LAM_ARRAY;
    const Json *v = a.at(i);
    if (!v) return 0.0 < min ? min : (0.0 > max ? max : 0.0);
    return as_number(v, min, max, LAM_AFAIL);
}

std::uint64_t DataArray::u64(std::size_t i) {
    LAM_ARRAY;
    const Json *v = a.at(i);
    return v ? as_u64(v, LAM_AFAIL) : 0;
}

StrId DataArray::id(std::size_t i) {
    LAM_ARRAY;
    const Json *v = a.at(i);
    return v ? as_id(v, LAM_AFAIL) : StrId{};
}

bool DataArray::boolean(std::size_t i) {
    LAM_ARRAY;
    const Json *v = a.at(i);
    if (!v) return false;
    if (!v->is_boolean()) {
        a.fail_at(i, "expected true or false");
        return false;
    }
    return v->get<bool>();
}

Vec2 DataArray::vec2(std::size_t i) {
    LAM_ARRAY;
    const Json *v = a.at(i);
    return v ? as_vec2(v, LAM_AFAIL) : Vec2{};
}

const std::string &DataArray::path() const { return path_; }

void DataArray::fail(std::string_view message) {
    if (doc_) impl_of(doc_)->fail(path_, message);
}

#undef LAM_ARRAY
#undef LAM_AFAIL

// ---------------------------------------------------------------------------------------------
// DataWriter

namespace {
constexpr std::uint8_t kObject = 0, kArray = 1, kHasItems = 0x80;
}

DataWriter::DataWriter() { stack_.reserve(16); }
DataWriter::~DataWriter() = default;
DataWriter::DataWriter(DataWriter &&) noexcept = default;
DataWriter &DataWriter::operator=(DataWriter &&) noexcept = default;

void DataWriter::fail(std::string_view message) {
    if (error_.empty()) error_ = std::string(message);
}

void DataWriter::before_value() {
    if (!error_.empty()) return;
    if (stack_.empty()) {
        if (!out_.empty()) fail("more than one top-level value");
        return;
    }
    std::uint8_t &top = stack_.back();
    if ((top & 0x7F) == kObject) {
        if (!expect_value_) fail("a value in an object needs a key first");
        expect_value_ = false;
        return;
    }
    if (top & kHasItems) out_ += ',';
    top |= kHasItems;
}

void DataWriter::put_string(std::string_view s) {
    if (!utf8_valid(s)) {
        fail("string is not valid UTF-8");
        return;
    }
    out_ += '"';
    for (char c : s) {
        switch (c) {
        case '"': out_ += "\\\""; break;
        case '\\': out_ += "\\\\"; break;
        case '\n': out_ += "\\n"; break;
        case '\r': out_ += "\\r"; break;
        case '\t': out_ += "\\t"; break;
        case '\b': out_ += "\\b"; break;
        case '\f': out_ += "\\f"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out_ += buf;
            } else {
                out_ += c;
            }
        }
    }
    out_ += '"';
}

DataWriter &DataWriter::begin_object() {
    before_value();
    out_ += '{';
    stack_.push_back(kObject);
    return *this;
}

DataWriter &DataWriter::end_object() {
    if (stack_.empty() || (stack_.back() & 0x7F) != kObject || expect_value_) {
        fail("end_object() does not match an open object");
        return *this;
    }
    stack_.pop_back();
    out_ += '}';
    return *this;
}

DataWriter &DataWriter::begin_array() {
    before_value();
    out_ += '[';
    stack_.push_back(kArray);
    return *this;
}

DataWriter &DataWriter::end_array() {
    if (stack_.empty() || (stack_.back() & 0x7F) != kArray) {
        fail("end_array() does not match an open array");
        return *this;
    }
    stack_.pop_back();
    out_ += ']';
    return *this;
}

DataWriter &DataWriter::key(std::string_view k) {
    if (stack_.empty() || (stack_.back() & 0x7F) != kObject || expect_value_) {
        fail("key() outside an object or after another key");
        return *this;
    }
    if (stack_.back() & kHasItems) out_ += ',';
    stack_.back() |= kHasItems;
    put_string(k);
    out_ += ':';
    expect_value_ = true;
    return *this;
}

DataWriter &DataWriter::value(bool v) {
    before_value();
    out_ += v ? "true" : "false";
    return *this;
}

DataWriter &DataWriter::write_i64(std::int64_t v) {
    before_value();
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    out_.append(buf, r.ptr);
    return *this;
}

DataWriter &DataWriter::write_u64(std::uint64_t v) {
    before_value();
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    out_.append(buf, r.ptr);
    return *this;
}

DataWriter &DataWriter::value(double v) {
    before_value();
    if (!std::isfinite(v)) {
        fail("NaN or infinity cannot be written");
        out_ += '0';
        return *this;
    }
    // to_chars gives "-0", which JSON readers take as the integer 0: write a float so the sign
    // survives a save and load (a loaded run continues bit-identically).
    if (v == 0.0 && std::signbit(v)) {
        out_ += "-0.0";
        return *this;
    }
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    out_.append(buf, r.ptr);
    return *this;
}

DataWriter &DataWriter::value(const char *v) { return value(std::string_view(v ? v : "")); }

DataWriter &DataWriter::value(std::string_view v) {
    before_value();
    put_string(v);
    return *this;
}

DataWriter &DataWriter::value(StrId v) {
    const std::string_view name = id_name(v);
    if (!name.empty()) return value(name);
    return value(std::string_view(id_display(v)));
}

DataWriter &DataWriter::value(Vec2 v) { return begin_array().value(v.x).value(v.y).end_array(); }

DataWriter &DataWriter::value(IVec2 v) { return begin_array().value(v.x).value(v.y).end_array(); }

DataWriter &DataWriter::value(Rect v) { return begin_array().value(v.x).value(v.y).value(v.w).value(v.h).end_array(); }

DataWriter &DataWriter::value(IRect v) { return begin_array().value(v.x).value(v.y).value(v.w).value(v.h).end_array(); }

DataWriter &DataWriter::value(Color v) {
    char buf[16];
    if (v.a == 255) std::snprintf(buf, sizeof buf, "#%02x%02x%02x", v.r, v.g, v.b);
    else std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", v.r, v.g, v.b, v.a);
    return value(std::string_view(buf));
}

DataWriter &DataWriter::null() {
    before_value();
    out_ += "null";
    return *this;
}

DataWriter &DataWriter::raw(std::string_view json) {
    before_value();
    out_ += json;
    return *this;
}

DataWriter &DataWriter::copy(DataReader &in, std::string_view k) {
    const Json *node = node_of(in.node_);
    in.ignore(k);
    if (!node) return *this;
    const auto it = node->find(k);
    if (it == node->end()) return *this;
    try {
        key(k);
        raw(it->dump());
    } catch (const std::exception &e) {
        fail(e.what());
    }
    return *this;
}

DataWriter &DataWriter::copy_rest(DataReader &in) {
    const Json *node = node_of(in.node_);
    if (!node || !in.doc_ || in.slot_ == kNoSlot) return *this;
    const Record &rec = impl_of(in.doc_)->records[in.slot_];
    std::vector<std::string> unread;
    for (const auto &item : node->items()) {
        bool read = rec.all_read;
        for (const std::string &r : rec.read)
            if (r == item.key()) read = true;
        if (!read) unread.push_back(item.key());
    }
    for (const std::string &k : unread) copy(in, k);
    return *this;
}

Result<std::string> DataWriter::finish(bool pretty) {
    if (!error_.empty()) return make_error(ErrorCode::Invalid, "DataWriter: " + error_);
    if (!stack_.empty() || expect_value_) return make_error(ErrorCode::Invalid, "DataWriter: unbalanced document");
    if (out_.empty()) return make_error(ErrorCode::Invalid, "DataWriter: empty document");
    if (!pretty) return out_;
    try {
        return Json::parse(out_).dump(2);
    } catch (const std::exception &e) {
        return make_error(ErrorCode::Invalid, std::string("DataWriter: ") + e.what());
    }
}

} // namespace lam
