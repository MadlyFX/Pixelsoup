#pragma once

// Drawing helpers the workspace panels (Scene, Compositor, Output Monitor,
// Preferences and Project Settings) share. Internal to the editor library.

#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <string_view>

namespace mediaserver::editor::panel_helpers {

namespace gui = mediaserver::gui;

inline constexpr float inset = 6.0F;

[[nodiscard]] inline gui::UiColor with_alpha(gui::UiColor color,
                                             float opacity) noexcept {
  color.a = opacity;
  return color;
}

[[nodiscard]] inline gui::UiColor scale(gui::UiColor color,
                                        float amount) noexcept {
  color.r *= amount;
  color.g *= amount;
  color.b *= amount;
  return color;
}

[[nodiscard]] inline std::string element_name(const ElementInfo &element) {
  return element_display_name(element);
}

[[nodiscard]] inline std::string lowercase(std::string_view input) {
  std::string result;
  result.reserve(input.size());
  for (const char character : input) {
    if (character >= 'A' && character <= 'Z') {
      result.push_back(static_cast<char>(character - 'A' + 'a'));
    } else {
      result.push_back(character);
    }
  }
  return result;
}

[[nodiscard]] inline bool matches(std::string_view name,
                                  std::string_view query) {
  return query.empty() ||
         lowercase(name).find(lowercase(query)) != std::string::npos;
}

[[nodiscard]] inline bool
selected(doc::NodeId node, std::span<const doc::NodeId> selection) noexcept {
  return std::find(selection.begin(), selection.end(), node) != selection.end();
}

// A search box inset in `bounds` by the panel's own margins.
inline void search_field(gui::UiContext &ctx, gui::UiId id,
                         const gui::Rect &bounds, std::string &value,
                         std::string_view placeholder) {
  static_cast<void>(
      search_box(ctx, id, gui::inset(bounds, inset, 3.0F), value, placeholder));
}

[[nodiscard]] inline gui::Interaction
clipped_grab(gui::UiContext &ctx, gui::UiId id, const gui::Rect &bounds,
             const gui::Rect &clip) noexcept {
  const gui::Rect hit = gui::intersection(bounds, clip);
  return hit.width > 0.0F && hit.height > 0.0F ? ctx.grab(id, hit)
                                               : gui::Interaction{};
}

// The width of both settings dialogs' card.
inline constexpr float modal_card_width = 620.0F;

// The chrome both settings dialogs share: the workspace dimmed behind a card,
// a titled header, and a footer strip the buttons are cut from. begin_modal()
// pushes the card's clip and end_modal() pops it.
struct ModalCard final {
  gui::Rect card;
  // Between the header and the footer.
  gui::Rect body;
  gui::Rect footer;
};

[[nodiscard]] inline ModalCard begin_modal(gui::UiContext &ctx,
                                           const gui::Rect &bounds,
                                           float height,
                                           std::string_view title) {
  const gui::Metrics &m = ctx.theme().metrics;
  height = std::min(height, bounds.height);
  const gui::Rect centre = gui::centered(bounds, modal_card_width, height);
  ModalCard modal{};
  modal.card = gui::Rect{std::floor(std::max(centre.x, bounds.x)),
                         std::floor(std::max(centre.y, bounds.y)),
                         modal_card_width, height};

  // Dim the workspace. This is the drawing half only; the scrim's hit test is
  // registered by end_modal(), because a full-screen grab placed here would
  // claim the pointer before any control in the card could.
  ctx.panel(bounds, gui::UiColor{0.0F, 0.0F, 0.0F, 0.55F});
  ctx.panel(modal.card, ctx.theme().background);
  ctx.outline(modal.card, ctx.theme().divider);
  ctx.push_clip(modal.card);

  modal.body = modal.card;
  gui::Rect header = gui::cut_top(modal.body, m.header);
  ctx.panel(header, ctx.theme().header);
  ctx.label(gui::inset(header, m.inset, 0.0F), title, ctx.theme().text);
  ctx.panel(gui::cut_bottom(header, m.divider), ctx.theme().divider);
  modal.footer =
      gui::inset(gui::cut_bottom(modal.body, m.control + m.row_gap * 2.0F),
                 m.inset, m.row_gap);
  return modal;
}

// Pops the card's clip, then registers the scrim - last, so it only catches
// presses no control in the card wanted. A click on the card's own background
// lands here too and deliberately does nothing; only a click outside
// dismisses.
[[nodiscard]] inline bool end_modal(gui::UiContext &ctx, gui::UiId scrim_id,
                                    const gui::Rect &bounds,
                                    const ModalCard &modal) {
  ctx.pop_clip();
  const gui::Interaction scrim = ctx.grab(scrim_id, bounds);
  return scrim.clicked &&
         !modal.card.contains(ctx.pointer_x(), ctx.pointer_y());
}

[[nodiscard]] inline bool preferences_button(gui::UiContext &ctx, gui::UiId id,
                                             const gui::Rect &bounds,
                                             std::string_view label,
                                             bool enabled) {
  const gui::Interaction it =
      enabled ? ctx.grab(id, bounds) : gui::Interaction{};
  gui::UiColor body = !enabled ? ctx.theme().track
                               : (it.held && !it.released
                                      ? ctx.theme().widget_active
                                      : (it.hovered ? ctx.theme().widget_hover
                                                    : ctx.theme().widget));
  ctx.panel(bounds, body);
  ctx.outline(bounds, enabled ? ctx.theme().header : ctx.theme().divider);
  ctx.label_centered(bounds, label,
                     enabled ? ctx.theme().text : ctx.theme().text_muted);
  return enabled && it.clicked;
}

} // namespace mediaserver::editor::panel_helpers
