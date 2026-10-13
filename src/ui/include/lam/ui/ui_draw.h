#pragma once
// UI drawing helpers that push quads into a RenderQueue at screen layers. None of them allocate.
#include <cstdint>
#include <lam/math/color.h>
#include <lam/math/rect.h>
#include <lam/render/font.h>
#include <lam/render/sprite.h>
#include <lam/render/texture.h>
#include <lam/ui/message_log.h>
#include <lam/ui/toasts.h>
#include <lam/ui/ui.h>
namespace lam {
class RenderQueue;
class DialogueRunner;
// Draws every shown node of a laid-out tree (UiTree::layout first). `font` null: no text.
void draw_ui_tree(RenderQueue &q, const UiTree &tree, const UiTheme &theme, const Font *font,
                  std::uint8_t layer = kLayerUi);
// Nine quads: corners keep `border` texels unscaled, edges and centre stretch over `dest`.
void draw_nine_slice(RenderQueue &q, const TextureRegion &region, float border, Rect dest, Color tint,
                     std::uint8_t layer);
// Toasts stacked upward from the bottom centre of `screen`, newest lowest; "text x3" for merges.
void draw_toast_list(RenderQueue &q, const ToastQueue &toasts, const UiTheme &theme, const Font *font,
                     Rect screen, std::uint8_t layer = kLayerOverlay);
// The newest lines of `log` that fit in `area`, oldest at the top. size 0 = the font's base size.
void draw_message_log(RenderQueue &q, const MessageLog &log, const Font &font, Rect area, float size = 0,
                      std::uint8_t layer = kLayerUi);
// A dialogue box: panel, speaker, typed text and, once typing is done, the choices. `layout` is
// the caller's wrap cache, one per box.
void draw_dialogue_box(RenderQueue &q, const DialogueRunner &runner, TextLayout &layout, const UiTheme &theme,
                       const Font &font, Rect area, int selected = 0, std::uint8_t layer = kLayerUi);
}
