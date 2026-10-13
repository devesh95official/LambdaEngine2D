#pragma once
// Roguelike message log: a ring of fixed-size lines, no allocation after construction.
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>
#include <lam/core/fixed_string.h>
#include <lam/math/color.h>
namespace lam {
class MessageLog {                                              // World or App resource (game's choice)
public:
    explicit MessageLog(std::size_t lines = 64);                // lines >= 1
    void add(std::string_view text, Color c = kWhite);          // truncates to 96 bytes
    std::size_t size() const;                                   // lines kept (<= capacity)
    std::string_view line(std::size_t i) const;                 // 0 = oldest kept; valid until add/clear
    Color color(std::size_t i) const;                           // colour of line(i)
    void clear();                                               // drops every line; keeps the storage
    std::size_t capacity() const;                               // lines kept before the oldest drops
    // Lines added since construction (clear() does not reset it): views redraw when it changes.
    std::uint64_t added() const;
private:
    struct Line { FixedString<97> text; Color color; };
    std::vector<Line> ring_;
    std::size_t head_ = 0;                                      // index of the oldest line
    std::size_t size_ = 0;
    std::uint64_t added_ = 0;
};
}
