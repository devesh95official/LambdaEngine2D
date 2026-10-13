// FixedString<N>: truncation on UTF-8 boundaries, format_to and equality.
#include <doctest/doctest.h>

#include <cstring>
#include <string_view>

#include <lam/core/fixed_string.h>
#include <lam/test/support.h>

using namespace lam;
using namespace std::string_view_literals;

// Assignment takes std::string_view; a bare string literal is ambiguous with the converting
// constructors, so these tests use "..."sv.
static_assert(FixedString<8>::capacity() == 7);

TEST_CASE("fixed string: assignment keeps what fits") {
    FixedString<8> s;
    CHECK(s.empty());
    CHECK(s.size() == 0);
    s = "hello"sv;
    CHECK(s.view() == "hello");
    CHECK(s.size() == 5);
    CHECK(std::strlen(s.c_str()) == 5);
    s = "hello world"sv;
    CHECK(s.view() == "hello w");
    CHECK(s.size() == 7);
    CHECK(std::strlen(s.c_str()) == 7);
    const FixedString<8> from_view(std::string_view("abc"));
    CHECK(from_view.view() == "abc");
    s.clear();
    CHECK(s.empty());
    CHECK(s.view().empty());
}

TEST_CASE("fixed string: truncation never splits a UTF-8 sequence") {
    FixedString<6> s;     // 5 bytes
    s = "abcd\xC3\xA9"sv; // "abcdé": 6 bytes, the 2-byte é does not fit
    CHECK(s.view() == "abcd");
    s = "ab\xE2\x82\xAC"sv; // "ab€": exactly 5 bytes
    CHECK(s.view() == "ab\xE2\x82\xAC");
    s = "abc\xE2\x82\xAC"sv; // "abc€": 6 bytes, the 3-byte € does not fit
    CHECK(s.view() == "abc");
    s = "a\xF0\x9F\x98\x80"
        "b"sv; // "a😀b"
    CHECK(s.view() == "a\xF0\x9F\x98\x80");
    FixedString<4> t;
    t = "\xF0\x9F\x98\x80"sv; // a 4-byte code point into 3 bytes: nothing fits
    CHECK(t.empty());
}

TEST_CASE("fixed string: format_to formats, truncates and does not allocate") {
    FixedString<32> s;
    format_to(s, "HP {}/{}", 7, 10);
    CHECK(s.view() == "HP 7/10");
    FixedString<8> small;
    format_to(small, "{}", "0123456789");
    CHECK(small.view() == "0123456");
    format_to(small, "{}", 3);
    CHECK(small.view() == "3");
    CHECK(std::strlen(small.c_str()) == 1);
    FixedString<32> hud;
    CHECK_NO_ALLOC(format_to(hud, "x={} y={} hp={}", 12, -4, 99));
    CHECK(hud.view() == "x=12 y=-4 hp=99");
    CHECK_NO_ALLOC(hud = std::string_view("assigned"));
}

TEST_CASE("fixed string: equality ignores what was there before") {
    FixedString<16> a, b;
    a = "a much longer one"sv;
    a = "short"sv;
    b = "short"sv;
    CHECK(a == b);
    b = "shore"sv;
    CHECK_FALSE(a == b);
    FixedString<16> c;
    format_to(c, "{}", "short");
    CHECK(a == c);
}

// Regression: format_to_n can stop inside a multi-byte sequence, and the partial tail used to be
// kept ("abcd\xC3").
TEST_CASE("fixed string: format_to truncation never splits a UTF-8 sequence") {
    FixedString<6> utf;
    format_to(utf, "{}\xC3\xA9", "abcd");
    CHECK(utf.view() == "abcd");
    FixedString<4> emoji;
    format_to(emoji, "a{}", "\xF0\x9F\x98\x80");
    CHECK(emoji.view() == "a");
}

TEST_CASE("fixed string: a literal assigns directly") {
    FixedString<8> s;
    s = "literal";
    CHECK(s.view() == "literal");
    s = "much too long";
    CHECK(s.view() == "much to");
}
