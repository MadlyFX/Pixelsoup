#include <mediaserver/editor/history_panel.hpp>

#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/rect_cut.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace mediaserver::editor {
namespace {

namespace gui = mediaserver::gui;

constexpr float chip_height = 28.0F;
constexpr float chip_gap = 10.0F;
constexpr float chip_max_width = 240.0F;
constexpr float chip_icon = 22.0F;
// How long an on-air jump waits for its second click.
constexpr double arm_seconds = 4.0;

[[nodiscard]] gui::UiColor alpha(gui::UiColor color, float opacity) noexcept {
  color.a = opacity;
  return color;
}

[[nodiscard]] std::size_t applied_count(const HistoryContextState &state) noexcept {
  return static_cast<std::size_t>(std::count_if(
      state.steps.begin(), state.steps.end(),
      [](const HistoryStep &step) { return step.applied; }));
}

[[nodiscard]] std::string plural_steps(core::u64 count) {
  return std::to_string(count) + (count == 1U ? " step" : " steps");
}

} // namespace

bool history_toggle(gui::UiContext &ctx, gui::UiId id, const gui::Rect &bounds,
                    bool shown, std::string_view label) {
  const gui::Theme &theme = ctx.theme();
  const gui::Interaction it = ctx.grab(id, bounds);
  ctx.cursor_for(it, gui::Cursor::Hand);
  if (it.hovered || it.held || !label.empty()) {
    ctx.rounded(bounds, it.hovered || it.held ? theme.widget_hover : theme.panel,
                bounds.height * 0.5F);
  }
  const gui::UiColor ink = it.hovered ? theme.text : theme.text_muted;
  // A chevron: down to put the strip away, up to bring it back.
  const float cx = bounds.x + std::min(bounds.width, bounds.height) * 0.5F +
                   (label.empty() ? 0.0F : 4.0F);
  const float cy = bounds.center_y();
  const float lift = shown ? 2.0F : -2.0F;
  const std::array<gui::UiPoint, 3U> chevron{
      gui::UiPoint{cx - 5.0F, cy - lift}, gui::UiPoint{cx, cy + lift},
      gui::UiPoint{cx + 5.0F, cy - lift}};
  ctx.draw_list().add_polyline(chevron, 1.75F, ink);
  if (!label.empty()) {
    ctx.label(gui::Rect{cx + 10.0F, bounds.y,
                        std::max(bounds.right() - cx - 12.0F, 0.0F),
                        bounds.height},
              label, ink);
  }
  ctx.tooltip(bounds, shown ? "Hide the History strip"
                            : "Show the History strip");
  return it.clicked;
}

HistoryJump history_jump(const HistoryContextState &state,
                         std::optional<std::size_t> index) noexcept {
  const std::size_t applied = applied_count(state);
  if (index.has_value() && *index >= state.steps.size()) {
    return HistoryJump{};
  }
  const std::size_t target = index.has_value() ? *index + 1U : 0U;
  return HistoryJump{
      index.has_value() ? state.steps[*index].step : 0U,
      static_cast<core::u64>(target > applied ? target - applied
                                              : applied - target)};
}

void HistoryPanel::reset() noexcept {
  pinned_.reset();
  scroll_ = 0.0F;
  content_width_ = 0.0F;
  followed_revision_ = 0U;
  armed_.reset();
  armed_until_ = -1.0;
}

HistoryPanelFrame HistoryPanel::draw(gui::UiContext &ctx,
                                     const gui::Rect &bounds,
                                     const HistoryState &state,
                                     HistoryContext active, bool on_air) {
  HistoryPanelFrame frame{};
  const gui::Theme &theme = ctx.theme();
  ctx.rounded(bounds, theme.panel, theme.radius);
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return frame;
  }
  const HistoryContext context = shown(active);
  const HistoryContextState &history = state.context(context);
  const std::size_t applied = applied_count(history);
  const double now = ctx.time_seconds();
  if (armed_.has_value() &&
      (now >= armed_until_ || armed_context_ != context)) {
    armed_.reset();
  }

  gui::Rect area = gui::inset(bounds, 8.0F, 0.0F);
  const float row_y = bounds.y + std::max((bounds.height - chip_height) * 0.5F, 0.0F);
  const auto row_slot = [&](gui::Rect &from, float width) {
    gui::Rect slot = gui::cut_left(from, width);
    return gui::Rect{slot.x, row_y, slot.width, std::min(chip_height, bounds.height)};
  };

  // --- Put the strip away; the status bar offers it back.
  if (history_toggle(ctx, gui::ui_id(id_, "hide"), row_slot(area, chip_height),
                     true)) {
    frame.hide = true;
  }
  static_cast<void>(gui::cut_left(area, 4.0F));

  // --- Which history: Scene or Compositor. Following the last-edited one
  // unless pinned; a click pins, and a click on the pinned one lets go.
  for (const HistoryContext option :
       {HistoryContext::Scene, HistoryContext::Compositor}) {
    const std::string_view name =
        option == HistoryContext::Scene ? "Scene" : "Compositor";
    const gui::Rect button = row_slot(area, ctx.font_width(name) + 24.0F);
    static_cast<void>(gui::cut_left(area, 4.0F));
    const bool pinned_here = pinned_ == option;
    const std::string tip =
        pinned_here ? "Pinned to the " + std::string{name} +
                          " history. Click to follow your last edit again"
        : option == context
            ? "Showing the " + std::string{name} +
                  " history because you last edited there. Click to pin it"
            : "Show the " + std::string{name} + " history";
    if (pill_button(ctx, gui::ui_id(id_, "context", static_cast<core::u64>(option)),
                    button, std::nullopt, name, tip, ButtonTone::Quiet, true,
                    option == context)) {
      pinned_ = pinned_here ? std::nullopt : std::optional{option};
    }
    if (pinned_here) {
      draw_status_dot(ctx, button.right() - 6.0F, button.y + 6.0F,
                      theme.accent, 2.5F);
    }
  }
  static_cast<void>(gui::cut_left(area, 6.0F));

  // --- Step back and forward, the same as Ctrl+Z and Ctrl+Shift+Z.
  const bool can_undo = applied > 0U;
  const bool can_redo = applied < history.steps.size();
  if (icon_button(ctx, gui::ui_id(id_, "undo"), row_slot(area, chip_height),
                  gui::Icon::StepBack, "Undo", false, can_undo) &&
      can_undo) {
    frame.request = HistoryRequest{HistoryAction::Undo, context, 0U, 0U, 1U};
  }
  static_cast<void>(gui::cut_left(area, 2.0F));
  if (icon_button(ctx, gui::ui_id(id_, "redo"), row_slot(area, chip_height),
                  gui::Icon::StepForward, "Redo", false, can_redo) &&
      can_redo) {
    frame.request = HistoryRequest{HistoryAction::Redo, context, 0U, 0U, 1U};
  }
  static_cast<void>(gui::cut_left(area, 10.0F));
  ctx.panel(gui::Rect{area.x - 6.0F, row_y + 4.0F, 1.0F, chip_height - 8.0F},
           theme.divider);

  // --- The steps, scrolled sideways.
  const gui::Rect strip = area;
  if (strip.width <= 0.0F) {
    return frame;
  }
  struct Chip final {
    std::optional<std::size_t> index;
    std::string label;
    float x{};
    float width{};
  };
  std::vector<Chip> chips;
  chips.reserve(history.steps.size() + 1U);
  float cursor = 0.0F;
  const auto add_chip = [&](std::optional<std::size_t> index,
                            std::string label) {
    const float width =
        std::min(ctx.font_width(label) + chip_icon + 18.0F, chip_max_width);
    chips.push_back(Chip{index, std::move(label), cursor, width});
    cursor += width + chip_gap;
  };
  add_chip(std::nullopt, history.retired == 0U
                             ? std::string{"Start"}
                             : std::to_string(history.retired) + " earlier");
  for (std::size_t index = 0U; index < history.steps.size(); ++index) {
    add_chip(index, history.steps[index].label);
  }
  content_width_ = std::max(cursor - chip_gap, 0.0F);
  const float max_scroll = std::max(content_width_ - strip.width, 0.0F);

  // Keep the marker in view when the history moves, but leave an operator's
  // own scrolling alone until it does.
  const Chip &current = chips[applied];
  if (history.revision != followed_revision_ || context != followed_context_) {
    followed_revision_ = history.revision;
    followed_context_ = context;
    // Three quarters across leaves room for what Redo would bring back.
    const float marker_x = current.x + current.width + chip_gap * 0.5F;
    scroll_ = marker_x - strip.width * 0.75F;
  }
  if (strip.contains(ctx.pointer_x(), ctx.pointer_y()) && ctx.wheel() != 0.0F) {
    scroll_ -= ctx.wheel() * 60.0F;
  }
  scroll_ = std::clamp(scroll_, 0.0F, max_scroll);

  // A thin scroll bar along the card's bottom edge, only while the steps
  // overflow. Pressing the track brings the thumb under the pointer; dragging
  // carries it from there.
  if (max_scroll > 0.0F) {
    const gui::Rect hit{strip.x, bounds.bottom() - 5.0F, strip.width, 5.0F};
    const float thumb_width = std::clamp(
        strip.width * strip.width / content_width_, 24.0F, strip.width);
    const float travel = std::max(strip.width - thumb_width, 1.0F);
    const auto thumb_at = [&] {
      return strip.x + scroll_ / max_scroll * travel;
    };
    const gui::Interaction it = ctx.grab(gui::ui_id(id_, "scrollbar"), hit);
    if (it.pressed) {
      const float thumb_x = thumb_at();
      const bool on_thumb = ctx.pointer_x() >= thumb_x &&
                            ctx.pointer_x() < thumb_x + thumb_width;
      thumb_grab_ = on_thumb ? ctx.pointer_x() - thumb_x : thumb_width * 0.5F;
    }
    if (it.held) {
      scroll_ = std::clamp(
          (ctx.pointer_x() - thumb_grab_ - strip.x) / travel * max_scroll,
          0.0F, max_scroll);
    }
    const bool lit = it.hovered || it.held;
    const float thickness = lit ? 4.0F : 2.0F;
    const gui::Rect thumb{thumb_at(), bounds.bottom() - 2.0F - thickness,
                          thumb_width, thickness};
    ctx.rounded(thumb, lit ? theme.text_muted : alpha(theme.text_muted, 0.45F),
                thickness * 0.5F);
    ctx.tooltip(hit, "Drag to scroll through the history");
  }

  ctx.push_clip(strip);
  if (history.steps.empty()) {
    ctx.label(gui::Rect{strip.x + chips.front().width + chip_gap + 4.0F,
                        row_y, 200.0F, chip_height},
              "No edits yet", theme.text_muted);
  }
  for (const Chip &chip : chips) {
    const gui::Rect box{strip.x + chip.x - scroll_, row_y, chip.width,
                        std::min(chip_height, bounds.height)};
    if (box.right() < strip.x || box.x > strip.right()) {
      continue;
    }
    const bool is_step = chip.index.has_value();
    const bool is_applied = !is_step || history.steps[*chip.index].applied;
    const bool is_current = &chip == &current;
    const HistoryJump jump = history_jump(history, chip.index);
    const bool armed_here = armed_.has_value() && *armed_ == jump;

    const gui::Interaction it = ctx.grab(
        gui::ui_id(id_, "step", is_step ? history.steps[*chip.index].step : 0U), box);
    if (jump.steps > 0U) {
      ctx.cursor_for(it, gui::Cursor::Hand);
    }
    const float radius = theme.radius_small;
    if (armed_here) {
      ctx.glow(box, theme.caution, radius, 8.0F);
    }
    const gui::UiColor fill =
        is_current ? theme.selection
        : it.hovered && jump.steps > 0U
            ? theme.widget_hover
            : (is_applied ? theme.widget : alpha(theme.widget, 0.0F));
    ctx.rounded(box, fill, radius);
    if (armed_here) {
      ctx.rounded_outline(box, theme.caution, radius, 1.5F);
    } else if (is_current) {
      ctx.rounded_outline(box, alpha(theme.accent, 0.8F), radius, 1.0F);
    } else if (!is_applied) {
      ctx.rounded_outline(box, theme.border, radius, 1.0F);
    }
    const gui::UiColor ink = is_applied ? theme.text : theme.text_muted;
    const float icon_x = box.x + 6.0F + chip_icon * 0.5F;
    if (!is_step) {
      gui::draw_icon(ctx.draw_list(), gui::Icon::ToStart, icon_x, box.center_y(),
                ink, 0.8F);
    } else {
      switch (history.steps[*chip.index].kind) {
      case HistoryStepKind::Add:
        gui::draw_icon(ctx.draw_list(), gui::Icon::Plus, icon_x, box.center_y(),
                  ink, 0.8F);
        break;
      case HistoryStepKind::Delete:
        gui::draw_icon(ctx.draw_list(), gui::Icon::Trash, icon_x, box.center_y(),
                  ink, 0.8F);
        break;
      case HistoryStepKind::Change:
      case HistoryStepKind::Mixed:
        draw_filled_circle(ctx, icon_x, box.center_y(), 3.0F, ink);
        break;
      }
    }
    ctx.push_clip(gui::inset(box, 2.0F, 0.0F));
    ctx.label(gui::Rect{box.x + chip_icon + 8.0F, box.y,
                        std::max(box.width - chip_icon - 14.0F, 0.0F),
                        box.height},
              armed_here ? "Click again" : std::string_view{chip.label}, ink);
    ctx.pop_clip();

    std::string tip = chip.label;
    if (!is_step && history.retired > 0U) {
      tip += " - older steps are no longer kept; this is the state after them";
    }
    if (armed_here) {
      tip += ". Click again to jump " + plural_steps(jump.steps) +
             ": this scene is on air, so the wall changes at once";
    } else if (jump.steps == 0U) {
      tip += ". You are here";
    } else {
      tip += is_applied ? ". Click to undo back to here ("
                        : ". Click to redo up to here (";
      tip += plural_steps(jump.steps) + ')';
    }
    ctx.tooltip(box, tip);
    if (it.hovered && is_step) {
      frame.hovered_nodes = history.steps[*chip.index].nodes;
    }

    if (it.clicked && jump.steps > 0U && !frame.request.has_value()) {
      if (on_air && jump.steps > 1U && !armed_here) {
        armed_ = jump;
        armed_context_ = context;
        armed_until_ = now + arm_seconds;
      } else {
        frame.request = HistoryRequest{HistoryAction::Jump, context, 0U,
                                       jump.target_step, jump.steps};
        armed_.reset();
      }
    }
  }

  // The marker: after the current step, where the next edit will go.
  const float marker_x =
      strip.x + current.x + current.width + chip_gap * 0.5F - scroll_;
  const float eased = ctx.ease(gui::ui_id(id_, "marker"), marker_x, 1.5F);
  // Kept to the chips' height so it stops short of the scroll bar.
  ctx.panel(gui::Rect{eased - 1.0F, row_y - 1.0F, 2.0F, chip_height + 2.0F},
           theme.accent);
  ctx.pop_clip();
  return frame;
}

} // namespace mediaserver::editor
