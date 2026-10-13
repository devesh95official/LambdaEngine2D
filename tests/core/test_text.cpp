// UTF-8 helpers and group_thousands. An invalid byte always decodes as one U+FFFD.
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <lam/core/text.h>

using namespace lam;

TEST_CASE("utf8 helpers count characters, not bytes") {
    const std::string s = "na\xC3\xAFve"; // naïve
    CHECK(utf8_length(s) == 5);
    CHECK(utf8_prefix(s, 3) == "na\xC3\xAF");
    CHECK(utf8_prefix(s, 0).empty());
    CHECK(utf8_prefix(s, 99) == s);
    CHECK(group_thousands(1234567) == "1,234,567");
    CHECK(group_thousands(-950) == "-950");
    CHECK(group_thousands(0) == "0");
}

TEST_CASE("utf8_next decodes 1 to 4 byte sequences") {
    const std::string s = "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"; // A é € 😀
    Utf8Next n = utf8_next(s, 0);
    CHECK(n.cp == U'A');
    CHECK(n.bytes == 1);
    CHECK(n.valid);
    n = utf8_next(s, 1);
    CHECK(n.cp == 0xE9);
    CHECK(n.bytes == 2);
    n = utf8_next(s, 3);
    CHECK(n.cp == 0x20AC);
    CHECK(n.bytes == 3);
    n = utf8_next(s, 6);
    CHECK(n.cp == 0x1F600);
    CHECK(n.bytes == 4);
    CHECK(n.valid);
    n = utf8_next(s, s.size());
    CHECK(n.cp == 0);
    CHECK(n.bytes == 0);
    CHECK_FALSE(n.valid);
    CHECK(utf8_length(s) == 4);
    CHECK(utf8_valid(s));
}

TEST_CASE("utf8_next: every invalid byte is one U+FFFD of one byte") {
    const char *bad[] = {
        "\x80",             // continuation without a lead
        "\xFF",             // never valid
        "\xC3",             // truncated at the end
        "\xE2\x82",         // truncated 3-byte sequence
        "\xC0\xAF",         // overlong '/'
        "\xED\xA0\x80",     // UTF-16 surrogate
        "\xF4\x90\x80\x80", // above U+10FFFF
        "\xC3(",            // lead followed by a non-continuation byte
    };
    for (const char *b : bad) {
        const std::string s = b;
        CAPTURE(s.size());
        const Utf8Next n = utf8_next(s, 0);
        CHECK(n.cp == 0xFFFD);
        CHECK(n.bytes == 1);
        CHECK_FALSE(n.valid);
        CHECK_FALSE(utf8_valid(s));
    }
}

TEST_CASE("utf8: length, prefix, truncate and valid agree on invalid input") {
    const std::string s = "a\xFF"
                          "b\xE2\x82"
                          "A\xC3"; // a, FF, b, E2, 82, A, C3
    CHECK(utf8_length(s) == 7);
    CHECK(utf8_prefix(s, 2) == "a\xFF");
    CHECK(utf8_prefix(s, 4) == "a\xFF"
                               "b\xE2");
    CHECK(utf8_truncate(s, 3) == 3);
    CHECK_FALSE(utf8_valid(s));

    const std::vector<std::string> samples = {"",
                                              "plain",
                                              "na\xC3\xAFve",
                                              s,
                                              "\xF0\x9F\x98\x80\xF0\x9F\x98",
                                              "\xE2\x82\xAC\x80\x80",
                                              "x\xC0\xAFy",
                                              "\xED\xA0\x80z"};
    for (const std::string &str : samples) {
        CAPTURE(str.size());
        // Walking with utf8_next visits exactly utf8_length code points and ends on size().
        int count = 0;
        std::size_t at = 0;
        std::vector<std::size_t> boundaries{0};
        while (at < str.size()) {
            const Utf8Next n = utf8_next(str, at);
            REQUIRE(n.bytes >= 1);
            at += n.bytes;
            boundaries.push_back(at);
            ++count;
        }
        CHECK(at == str.size());
        CHECK(utf8_length(str) == count);
        CHECK(utf8_prefix(str, count) == str);
        for (int k = 0; k <= count; ++k)
            CHECK(utf8_prefix(str, k).size() == boundaries[static_cast<std::size_t>(k)]);
        // utf8_truncate returns the largest boundary <= max_bytes.
        for (std::size_t max = 0; max <= str.size() + 1; ++max) {
            std::size_t want = 0;
            for (std::size_t b : boundaries)
                if (b <= max) want = b;
            REQUIRE(utf8_truncate(str, max) == want);
        }
    }
    CHECK(utf8_truncate("abc", 10) == 3);
    CHECK(utf8_truncate("ab\xE2\x82\xAC", 4) == 2);
    CHECK(utf8_prefix("abc", -1).empty());
}

TEST_CASE("group_thousands covers the whole int64 range") {
    CHECK(group_thousands(999) == "999");
    CHECK(group_thousands(1000) == "1,000");
    CHECK(group_thousands(-1000) == "-1,000");
    CHECK(group_thousands(100000) == "100,000");
    CHECK(group_thousands(-1) == "-1");
    CHECK(group_thousands(std::numeric_limits<std::int64_t>::max()) == "9,223,372,036,854,775,807");
    CHECK(group_thousands(std::numeric_limits<std::int64_t>::min()) ==
          "-9,223,372,036,854,775,808");
}
