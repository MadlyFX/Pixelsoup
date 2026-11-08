#pragma once

// Drawing helpers the Hardware and Feed mapping panels share. Internal to
// the editor library.

#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <algorithm>
#include <string_view>

namespace mediaserver::editor::mapping {

inline constexpr float title_height = 26.0F;

[[nodiscard]] inline gui::UiColor alpha(gui::UiColor color, float opacity) noexcept {
  color.a = opacity;
  return color;
}

[[nodiscard]] inline gui::UiColor scale(gui::UiColor color, float amount) noexcept {
  color.r = std::clamp(color.r * amount, 0.0F, 1.0F);
  color.g = std::clamp(color.g * amount, 0.0F, 1.0F);
  color.b = std::clamp(color.b * amount, 0.0F, 1.0F);
  return color;
}

inline void draw_remove(gui::UiContext &ctx, float x, float y,
                        gui::UiColor color) {
  ctx.draw_list().add_line(x - 3.0F, y - 3.0F, x + 3.0F, y + 3.0F, 1.0F,
                           color);
  ctx.draw_list().add_line(x + 3.0F, y - 3.0F, x - 3.0F, y + 3.0F, 1.0F,
                           color);
}

inline void title(gui::UiContext &ctx, const gui::Rect &bounds, std::string_view name,
           std::string_view trailing) {
  gui::Rect header = bounds;
  header = gui::cut_top(header, title_height);
  ctx.panel(header, ctx.theme().background);
  gui::Rect rule = header;
  ctx.panel(gui::cut_bottom(rule, 1.0F), ctx.theme().divider);
  gui::Rect text = gui::inset(header, 9.0F, 0.0F);
  ctx.label(text, name, ctx.theme().text);
  if (!trailing.empty()) {
    ctx.label(gui::cut_right(text, ctx.font_width(trailing)), trailing,
              ctx.theme().text_muted);
  }
}

} // namespace mediaserver::editor::mapping
