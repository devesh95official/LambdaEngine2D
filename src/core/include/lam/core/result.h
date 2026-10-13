#pragma once
// Data and user errors return Result; programmer errors are LAM_ASSERT. No exception leaves
// a public function.
//   Result<Level> r = load_level(path);
//   if (!r) { LAM_LOG_ERROR("{}", r.error().message); return; }
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <lam/core/assert.h>
namespace lam {
enum class ErrorCode : std::uint16_t { Unknown, NotFound, Io, Parse, Invalid, OutOfRange,
    Unsupported, VersionTooNew, MigrationFailed, Busy, NotImplemented, Device };
struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string message;                                        // names file and JSON path when relevant
};
// A value or an Error. Converts from either, so `return value;` and `return make_error(...)` work.
template <class T> class [[nodiscard]] Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Error error) : error_(std::move(error)) {}
    bool ok() const { return value_.has_value(); }
    explicit operator bool() const { return ok(); }
    // LAM_CHECK(ok()).
    T &value() {
        LAM_CHECK(ok(), "Result::value() on an error");
        return *value_;
    }
    const T &value() const {
        LAM_CHECK(ok(), "Result::value() on an error");
        return *value_;
    }
    T value_or(T fallback) const { return ok() ? *value_ : std::move(fallback); }
    // LAM_CHECK(!ok()).
    const Error &error() const {
        LAM_CHECK(!ok(), "Result::error() on a value");
        return error_;
    }
    T *operator->() { return &value(); }
    const T *operator->() const { return &value(); }
    T &operator*() { return value(); }
    const T &operator*() const { return value(); }

private:
    std::optional<T> value_;
    Error error_;
};
template <> class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), ok_(false) {}
    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }
    const Error &error() const {
        LAM_CHECK(!ok_, "Status::error() on success");
        return error_;
    }

private:
    Error error_;
    bool ok_ = true;
};
using Status = Result<void>;
Error make_error(ErrorCode code, std::string message);
// Name of an error code ("NotFound").
const char *error_code_name(ErrorCode code);
}
#define LAM_DETAIL_CAT2(a, b) a##b
#define LAM_DETAIL_CAT(a, b) LAM_DETAIL_CAT2(a, b)
// Evaluates a Status/Result; on error returns its Error from the enclosing function.
#define LAM_TRY(expr)                                                                              \
    do {                                                                                           \
        auto &&lam_try_r_ = (expr);                                                                \
        if (!lam_try_r_.ok()) return lam_try_r_.error();                                           \
    } while (false)
// auto r = expr; on error returns it; else `decl = std::move(*r)`.
#define LAM_TRY_ASSIGN(decl, expr)                                                                 \
    auto LAM_DETAIL_CAT(lam_try_a_, __LINE__) = (expr);                                            \
    if (!LAM_DETAIL_CAT(lam_try_a_, __LINE__).ok()) return LAM_DETAIL_CAT(lam_try_a_, __LINE__).error(); \
    decl = std::move(*LAM_DETAIL_CAT(lam_try_a_, __LINE__))
