#include <lam/core/text.h>

namespace lam {

Utf8Next utf8_next(std::string_view s, std::size_t at) {
    if (at >= s.size()) return {0, 0, false};
    const auto b0 = static_cast<unsigned char>(s[at]);
    if (b0 < 0x80) return {b0, 1, true};
    int len = 0;
    char32_t cp = 0;
    char32_t min = 0;
    if ((b0 & 0xE0) == 0xC0) {
        len = 2;
        cp = b0 & 0x1F;
        min = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3;
        cp = b0 & 0x0F;
        min = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4;
        cp = b0 & 0x07;
        min = 0x10000;
    } else {
        return {0xFFFD, 1, false};
    }
    if (at + static_cast<std::size_t>(len) > s.size()) return {0xFFFD, 1, false};
    for (int i = 1; i < len; ++i) {
        const auto b = static_cast<unsigned char>(s[at + static_cast<std::size_t>(i)]);
        if ((b & 0xC0) != 0x80) return {0xFFFD, 1, false};
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return {0xFFFD, 1, false};
    return {cp, static_cast<std::uint8_t>(len), true};
}

int utf8_length(std::string_view s) {
    int n = 0;
    for (std::size_t i = 0; i < s.size(); i += utf8_next(s, i).bytes) ++n;
    return n;
}

std::string_view utf8_prefix(std::string_view s, int count) {
    std::size_t i = 0;
    for (int n = 0; n < count && i < s.size(); ++n) i += utf8_next(s, i).bytes;
    return s.substr(0, i);
}

std::size_t utf8_truncate(std::string_view s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return s.size();
    std::size_t i = 0;
    for (;;) {
        const std::size_t step = utf8_next(s, i).bytes;
        if (step == 0 || i + step > max_bytes) return i;
        i += step;
    }
}

bool utf8_valid(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const Utf8Next n = utf8_next(s, i);
        if (!n.valid) return false;
        i += n.bytes;
    }
    return true;
}

std::string group_thousands(std::int64_t v) {
    const bool negative = v < 0;
    // Magnitude in unsigned arithmetic: well defined for INT64_MIN.
    std::uint64_t m = negative ? 0ULL - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
    char buf[32];
    int pos = 31;
    buf[pos] = '\0';
    int digits = 0;
    do {
        if (digits > 0 && digits % 3 == 0) buf[--pos] = ',';
        buf[--pos] = static_cast<char>('0' + m % 10);
        m /= 10;
        ++digits;
    } while (m > 0);
    if (negative) buf[--pos] = '-';
    return std::string(buf + pos);
}

} // namespace lam
