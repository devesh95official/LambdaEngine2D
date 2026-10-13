#pragma once
// UTF-8 decoding and small text helpers. An invalid byte always counts as one U+FFFD.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
namespace lam {
struct Utf8Next { char32_t cp; std::uint8_t bytes; bool valid; };
// Decodes the code point at byte `at`. Invalid byte -> U+FFFD, 1 byte. at >= size -> {0, 0, false}.
Utf8Next utf8_next(std::string_view s, std::size_t at);
// Code points (invalid bytes count one each).
int utf8_length(std::string_view s);
// The first `count` code points.
std::string_view utf8_prefix(std::string_view s, int count);
// The longest byte length <= max_bytes that ends on a code point boundary.
std::size_t utf8_truncate(std::string_view s, std::size_t max_bytes);
bool utf8_valid(std::string_view s);
// 1234567 -> "1,234,567"; handles INT64_MIN.
std::string group_thousands(std::int64_t v);
}
