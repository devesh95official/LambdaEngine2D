#pragma once
// Private (lam_ui): keeps a build_menu panel on screen once its sizes are known.
#include <lam/math/rect.h>
namespace lam {
class UiTree;
struct MenuState;
namespace detail {
// Margin (px) kept between a menu panel and the top and bottom of the screen.
inline constexpr float kMenuScreenMargin = 8.0f;
// Shrinks the item list, then the body, when the panel is taller than the screen allows.
// Returns true if the tree needs another layout.
bool fit_menu(UiTree &tree, const MenuState &menu, Rect screen);
} // namespace detail
} // namespace lam
