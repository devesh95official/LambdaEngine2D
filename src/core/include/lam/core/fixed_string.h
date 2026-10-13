#pragma once
// Fixed-capacity UTF-8 string for HUD, UI and debug text without per-frame allocation.
// Assignments truncate at a UTF-8 boundary.
//   FixedString<32> s;  format_to(s, "HP {}", hp);
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string_view>
#include <utility>
#include <lam/core/text.h>
namespace lam {
template <std::size_t N> class FixedString {
public:
    static_assert(N >= 2 && N <= 65535);
    constexpr FixedString() = default;
    FixedString(std::string_view s) { *this = s; }
    FixedString(const char *s) { *this = std::string_view(s); }
    // Exact match for literals: without it `s = "x";` is ambiguous with the converting constructors.
    FixedString &operator=(const char *s) { return *this = std::string_view(s); }
    // Truncates at a UTF-8 boundary.
    FixedString &operator=(std::string_view s) {
        const std::size_t n = utf8_truncate(s, capacity());
        std::memmove(data_.data(), s.data(), n);                // `s` may view this string's own buffer
        std::memset(data_.data() + n, 0, N - n);
        size_ = static_cast<std::uint16_t>(n);
        return *this;
    }
    std::string_view view() const { return {data_.data(), size_}; }
    const char *c_str() const { return data_.data(); }
    std::size_t size() const { return size_; }
    static constexpr std::size_t capacity() { return N - 1; }
    bool empty() const { return size_ == 0; }
    void clear() {
        std::memset(data_.data(), 0, N);
        size_ = 0;
    }
    bool operator==(const FixedString &) const = default;       // tail bytes are kept zeroed
    // Engine use (format_to): raw buffer write followed by set_size.
    char *buffer() { return data_.data(); }
    void set_size(std::size_t n) {
        n = utf8_truncate(std::string_view(data_.data(), n < capacity() ? n : capacity()), capacity());
        std::memset(data_.data() + n, 0, N - n);
        size_ = static_cast<std::uint16_t>(n);
    }
private:
    std::array<char, N> data_{};
    std::uint16_t size_ = 0;
};
namespace detail {
// Length of s without a trailing UTF-8 sequence that was cut short (a lead byte followed by fewer
// continuation bytes than it announces).
inline std::size_t utf8_drop_cut_tail(const char *s, std::size_t n) {
    std::size_t cont = 0;
    while (cont < 3 && cont < n && (static_cast<unsigned char>(s[n - 1 - cont]) & 0xC0u) == 0x80u) ++cont;
    if (cont == n) return n;
    const auto lead = static_cast<unsigned char>(s[n - 1 - cont]);
    const std::size_t want = lead >= 0xF0u ? 4 : lead >= 0xE0u ? 3 : lead >= 0xC0u ? 2 : 1;
    return cont + 1 < want ? n - 1 - cont : n;
}
} // namespace detail
// format_to_n into the buffer; truncates at a UTF-8 boundary; no allocation.
template <std::size_t N, class... A>
void format_to(FixedString<N> &out, std::format_string<A...> fmt, A &&...args) {
    const auto r = std::format_to_n(out.buffer(), static_cast<std::ptrdiff_t>(FixedString<N>::capacity()), fmt,
                                    std::forward<A>(args)...);
    std::size_t n = static_cast<std::size_t>(r.out - out.buffer());
    if (static_cast<std::size_t>(r.size) > n) n = detail::utf8_drop_cut_tail(out.buffer(), n);   // output was cut
    out.set_size(n);
}
}
