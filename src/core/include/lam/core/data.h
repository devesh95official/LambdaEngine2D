#pragma once
// Strict JSON reading and canonical writing. nlohmann is private to data.cpp.
// Unread keys are errors unless ignored; every object handed out is checked by finish();
// errors are sticky and name the file and JSON path; no exception leaves.
//   Result<Enemy> r = parse_data<Enemy>(text, "enemy.json", [](DataReader &in) {
//       return Enemy{.hp = in.integer("hp", 1, 999), .speed = in.number_or("speed", 1, 0, 10)};
//   });
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <lam/core/id.h>
#include <lam/core/result.h>
#include <lam/math/color.h>
#include <lam/math/rect.h>
namespace lam {
class DataReader;
class DataArray;
class DataWriter;
enum class DataType : std::uint8_t { Missing, Null, Bool, Number, String, Array, Object };
// Owns parsed JSON. Move-only. Readers point into it and must not outlive it.
class DataDocument {
public:
    // Fails on malformed JSON and on nesting deeper than kMaxDataDepth, which would blow the stack.
    static Result<DataDocument> parse(std::string_view text, std::string_view file_name);
    static constexpr int kMaxDataDepth = 256;
    // The root object (a non-object root makes every read fail).
    DataReader root();
    // The first error, or any unread key in any object handed out.
    Status finish();
    // The first error so far, if any (does not check unread keys).
    bool failed() const;
    ~DataDocument();
    DataDocument(DataDocument &&) noexcept;
    DataDocument &operator=(DataDocument &&) noexcept;
    struct Impl;                                                // opaque (data.cpp)
private:
    DataDocument();
    std::unique_ptr<Impl> impl_;
};
// One JSON object. Every typed read marks its key as read; a failed read records a sticky error
// (with file and path) and returns a neutral value (0, "", {}), so build code needs no checks.
class DataReader {
public:
    DataReader() = default;
    bool has(std::string_view key) const;
    DataType type(std::string_view key) const;
    float number(std::string_view key, float min, float max);   // finite, in range
    float number_or(std::string_view key, float fallback, float min, float max);
    int integer(std::string_view key, int min, int max);        // 3.0 ok, 3.5 error
    int integer_or(std::string_view key, int fallback, int min, int max);
    std::int64_t integer64(std::string_view key, std::int64_t min, std::int64_t max);
    std::uint64_t u64(std::string_view key);
    double number64(std::string_view key, double min, double max);   // finite, in range, full precision
    bool boolean(std::string_view key);
    bool boolean_or(std::string_view key, bool fallback);
    std::string string(std::string_view key);                   // non-empty, valid UTF-8
    std::string text(std::string_view key);                     // may be empty
    std::string text_or(std::string_view key, std::string_view fallback);
    StrId id(std::string_view key);                             // name (interned) or "#<16 hex>"
    Vec2 vec2(std::string_view key);                            // [x, y]
    IVec2 ivec2(std::string_view key);
    Color color(std::string_view key);                          // "#rrggbb[aa]" or [r, g, b(, a)]
    Rect rect(std::string_view key);                            // [x, y, w, h]
    IRect irect(std::string_view key);
    DataReader object(std::string_view key);
    DataArray array(std::string_view key);
    // For dictionary-shaped objects. Views into the document; allocates.
    std::vector<std::string_view> keys() const;                 // document order
    void ignore(std::string_view key);                          // mark read (foreign formats: Tiled)
    void ignore_unknown();                                      // mark every key of this object read
    const std::string &path() const;                            // "/terrains/grass"
    void fail(std::string_view message);                        // custom error at this path
    bool failed() const;
    bool valid() const { return node_ != nullptr; }             // false after a failed object()/array()
private:
    friend class DataDocument;
    friend class DataArray;
    friend class DataWriter;
    void *doc_ = nullptr;
    const void *node_ = nullptr;
    std::uint32_t slot_ = 0;
};
class DataArray {
public:
    DataArray() = default;
    std::size_t size() const;
    DataType type(std::size_t i) const;
    DataReader object(std::size_t i);
    DataArray array(std::size_t i);
    float number(std::size_t i, float min, float max);
    int integer(std::size_t i, int min, int max);
    std::int64_t integer64(std::size_t i, std::int64_t min, std::int64_t max);   // Tiled GIDs
    double number64(std::size_t i, double min, double max);
    std::uint64_t u64(std::size_t i);
    std::string string(std::size_t i);
    std::string text(std::size_t i);                            // may be empty
    StrId id(std::size_t i);
    bool boolean(std::size_t i);
    Vec2 vec2(std::size_t i);
    const std::string &path() const;
    void fail(std::string_view message);
private:
    friend class DataReader;
    void *doc_ = nullptr;
    const void *node_ = nullptr;
    std::string path_;
};
// parse + build + finish in one call; no exception leaves it. build: T(DataReader &root).
template <class T, class Build>
Result<T> parse_data(std::string_view text, std::string_view file_name, Build build) {
    auto doc = DataDocument::parse(text, file_name);
    if (!doc) return doc.error();
    try {
        DataReader root = doc->root();
        T value = build(root);
        Status s = doc->finish();
        if (!s) return s.error();
        return value;
    } catch (...) {
        return make_error(ErrorCode::Parse, std::string(file_name) + ": exception while building data");
    }
}
// Canonical JSON writer: insertion order, no NaN/inf, invalid UTF-8 -> Error.
//   DataWriter w; w.begin_object().key("hp").value(3).end_object(); Result<std::string> s = w.finish();
class DataWriter {
public:
    DataWriter();
    ~DataWriter();
    DataWriter(DataWriter &&) noexcept;
    DataWriter &operator=(DataWriter &&) noexcept;
    DataWriter &begin_object();
    DataWriter &end_object();
    DataWriter &begin_array();
    DataWriter &end_array();
    DataWriter &key(std::string_view k);
    DataWriter &value(bool v);
    template <class I>
        requires(std::is_integral_v<I> && !std::is_same_v<I, bool>)
    DataWriter &value(I v) {
        if constexpr (std::is_signed_v<I>) return write_i64(static_cast<std::int64_t>(v));
        else return write_u64(static_cast<std::uint64_t>(v));
    }
    DataWriter &value(double v);
    DataWriter &value(float v) { return value(static_cast<double>(v)); }
    DataWriter &value(const char *v);                           // string literals are strings, not bools
    DataWriter &value(std::string_view v);
    DataWriter &value(const std::string &v) { return value(std::string_view(v)); }
    DataWriter &value(StrId v);                                 // name, else "#<16 hex>"
    DataWriter &value(Vec2 v);
    DataWriter &value(IVec2 v);
    DataWriter &value(Rect v);
    DataWriter &value(IRect v);
    DataWriter &value(Color v);
    DataWriter &null();
    DataWriter &copy(DataReader &in, std::string_view key);     // copies key: value; marks it read
    DataWriter &copy_rest(DataReader &in);                      // copies every unread key (migrations)
    DataWriter &raw(std::string_view json);                     // a pre-validated JSON value (engine use)
    Result<std::string> finish(bool pretty = false);            // Error on unbalanced/invalid input
private:
    DataWriter &write_i64(std::int64_t v);
    DataWriter &write_u64(std::uint64_t v);
    void before_value();
    void fail(std::string_view message);
    void put_string(std::string_view s);
    std::string out_;
    std::vector<std::uint8_t> stack_;                           // 0 object, 1 array; high bit: has items
    bool expect_value_ = false;                                 // a key was just written
    std::string error_;
};
}
