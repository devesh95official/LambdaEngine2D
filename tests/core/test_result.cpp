// Result<T>, Status and the LAM_TRY macros.
#include <doctest/doctest.h>

#include <memory>
#include <string>

#include <lam/core/result.h>

using namespace lam;

namespace {

Result<int> parse_digit(char c) {
    if (c < '0' || c > '9') return make_error(ErrorCode::Parse, std::string("not a digit: ") + c);
    return c - '0';
}

Status check_positive(int v) {
    if (v <= 0) return make_error(ErrorCode::OutOfRange, "not positive");
    return {};
}

// Uses both macros: any error returns early with the inner Error unchanged.
Result<int> sum_digits(char a, char b, int *reached) {
    LAM_TRY_ASSIGN(const int x, parse_digit(a));
    LAM_TRY_ASSIGN(int y, parse_digit(b));
    LAM_TRY(check_positive(x + y));
    *reached = 1;
    return x + y;
}

Status validate(char c) {
    LAM_TRY(parse_digit(c)); // a Result works in LAM_TRY too
    LAM_TRY(check_positive(c - '0'));
    return {};
}

} // namespace

TEST_CASE("result: holds a value or an error") {
    Result<int> ok = 5;
    CHECK(ok.ok());
    CHECK(static_cast<bool>(ok));
    CHECK(ok.value() == 5);
    CHECK(*ok == 5);
    CHECK(ok.value_or(9) == 5);

    Result<int> bad = make_error(ErrorCode::NotFound, "nope");
    CHECK_FALSE(bad.ok());
    CHECK_FALSE(static_cast<bool>(bad));
    CHECK(bad.error().code == ErrorCode::NotFound);
    CHECK(bad.error().message == "nope");
    CHECK(bad.value_or(9) == 9);

    Result<std::string> s = std::string("abc");
    CHECK(s->size() == 3);

    Result<std::unique_ptr<int>> moved = std::make_unique<int>(4);
    REQUIRE(moved.ok());
    std::unique_ptr<int> p = std::move(*moved);
    CHECK(*p == 4);
}

TEST_CASE("result: Status defaults to success") {
    Status s;
    CHECK(s.ok());
    Status e = make_error(ErrorCode::Io, "disk");
    CHECK_FALSE(e.ok());
    CHECK(e.error().code == ErrorCode::Io);
    CHECK(e.error().message == "disk");
}

TEST_CASE("result: error_code_name") {
    CHECK(std::string(error_code_name(ErrorCode::NotFound)) == "NotFound");
    CHECK(std::string(error_code_name(ErrorCode::Parse)) == "Parse");
    CHECK(std::string(error_code_name(ErrorCode::NotImplemented)) == "NotImplemented");
    CHECK(std::string(error_code_name(ErrorCode::Device)) == "Device");
    const Error e = make_error(ErrorCode::Busy, "later");
    CHECK(e.code == ErrorCode::Busy);
    CHECK(e.message == "later");
}

TEST_CASE("result: LAM_TRY and LAM_TRY_ASSIGN propagate the first error") {
    int reached = 0;
    const Result<int> good = sum_digits('3', '4', &reached);
    REQUIRE(good.ok());
    CHECK(*good == 7);
    CHECK(reached == 1);

    reached = 0;
    const Result<int> first = sum_digits('x', '4', &reached);
    REQUIRE_FALSE(first.ok());
    CHECK(first.error().code == ErrorCode::Parse);
    CHECK(first.error().message == "not a digit: x");
    CHECK(reached == 0);

    const Result<int> second = sum_digits('1', 'y', &reached);
    REQUIRE_FALSE(second.ok());
    CHECK(second.error().message == "not a digit: y");

    const Result<int> zero = sum_digits('0', '0', &reached);
    REQUIRE_FALSE(zero.ok());
    CHECK(zero.error().code == ErrorCode::OutOfRange);
    CHECK(reached == 0);

    CHECK(validate('5').ok());
    CHECK(validate('z').error().code == ErrorCode::Parse);
    CHECK(validate('0').error().code == ErrorCode::OutOfRange);
}
