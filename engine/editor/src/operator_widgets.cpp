#include <mediaserver/editor/operator_widgets.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace mediaserver::editor {
namespace {

namespace gui = mediaserver::gui;

constexpr double double_click_seconds = 0.4;
constexpr float tau = 6.28318530718F;

[[nodiscard]] gui::UiColor with_alpha(gui::UiColor color, float alpha) noexcept {
  color.a = alpha;
  return color;
}

[[nodiscard]] gui::UiColor mixed(gui::UiColor a, gui::UiColor b,
                                 float t) noexcept {
  return gui::UiColor{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                      a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

void circle_outline(gui::UiContext &ctx, float cx, float cy, float radius,
                    float thickness, gui::UiColor color) {
  constexpr std::size_t steps = 16U;
  for (std::size_t index = 0U; index < steps; ++index) {
    const float a0 =
        tau * static_cast<float>(index) / static_cast<float>(steps);
    const float a1 =
        tau * static_cast<float>(index + 1U) / static_cast<float>(steps);
    ctx.draw_list().add_line(cx + std::cos(a0) * radius,
                             cy + std::sin(a0) * radius,
                             cx + std::cos(a1) * radius,
                             cy + std::sin(a1) * radius, thickness, color);
  }
}

void rect_outline(gui::UiContext &ctx, const gui::Rect &bounds,
                  gui::UiColor color) {
  auto &draw = ctx.draw_list();
  draw.add_rect(bounds.x, bounds.y, bounds.width, 1.0F, color);
  draw.add_rect(bounds.x, bounds.y + bounds.height - 1.0F, bounds.width, 1.0F,
                color);
  draw.add_rect(bounds.x, bounds.y, 1.0F, bounds.height, color);
  draw.add_rect(bounds.x + bounds.width - 1.0F, bounds.y, 1.0F, bounds.height,
                color);
}

[[nodiscard]] std::string title_case(std::string_view text) {
  std::string out{text};
  bool start = true;
  for (char &character : out) {
    if (start && character >= 'a' && character <= 'z') {
      character = static_cast<char>(character - 'a' + 'A');
    }
    start = character == ' ';
  }
  return out;
}

} // namespace

std::string element_display_name(const ElementInfo &element) {
  if (!element.display_name.empty()) {
    return element.display_name;
  }
  std::array<char, 64U> text{};
  const std::string kind = title_case(kind_name(element));
  const int length = std::snprintf(
      text.data(), text.size(), "%s %llu", kind.c_str(),
      static_cast<unsigned long long>(element.node.value()));
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

gui::UiColor element_kind_color(ElementKind kind) noexcept {
  return kind_color(kind);
}

gui::UiColor element_kind_color(const ElementInfo &element) noexcept {
  return element_kind_color(element.kind);
}

void draw_element_icon(gui::UiContext &ctx, const ElementInfo &element,
                       float cx, float cy, gui::UiColor color) {
  if (element.kind == ElementKind::Group &&
      element.group_kind == scene::GroupKind::VehicleProcess) {
    // A vehicle: a body, a cabin and two wheels - the group a vehicle's
    // processing work is filed under.
    auto &draw = ctx.draw_list();
    const gui::Rect body{cx - 7.0F, cy - 2.0F, 14.0F, 5.0F};
    const gui::Rect cabin{cx - 4.0F, cy - 5.5F, 7.0F, 3.5F};
    draw.add_rect(body.x, body.y, body.width, body.height,
                  with_alpha(color, 0.22F));
    rect_outline(ctx, body, color);
    rect_outline(ctx, cabin, color);
    draw_filled_circle(ctx, cx - 4.0F, cy + 4.0F, 2.0F, color);
    draw_filled_circle(ctx, cx + 4.0F, cy + 4.0F, 2.0F, color);
    return;
  }
  draw_kind_icon(ctx, element.kind, element.local_kind, cx, cy, color);
}

void draw_kind_icon(gui::UiContext &ctx, ElementKind kind,
                    scene::LocalLightKind light, float cx, float cy,
                    gui::UiColor color) {
  auto &draw = ctx.draw_list();
  const gui::UiColor fill = with_alpha(color, 0.22F);
  constexpr float stroke = 1.2F;
  switch (kind) {
  case ElementKind::Media: {
    // A frame of film with a play mark: something that runs.
    const gui::Rect frame{cx - 6.5F, cy - 5.0F, 13.0F, 10.0F};
    draw.add_rect(frame.x, frame.y, frame.width, frame.height, fill);
    rect_outline(ctx, frame, color);
    draw.add_triangle(cx - 2.0F, cy - 2.5F, cx + 3.0F, cy, cx - 2.0F,
                      cy + 2.5F, color);
    return;
  }
  case ElementKind::Model: {
    // A faceted solid: a hexagon cut into three faces.
    std::array<gui::UiPoint, 6U> hull{};
    for (std::size_t index = 0U; index < hull.size(); ++index) {
      const float angle =
          tau * (static_cast<float>(index) / 6.0F) - tau / 4.0F;
      hull[index] =
          gui::UiPoint{cx + std::cos(angle) * 6.5F, cy + std::sin(angle) * 6.5F};
    }
    draw.add_convex_poly(hull, fill);
    for (std::size_t index = 0U; index < hull.size(); ++index) {
      const gui::UiPoint &a = hull[index];
      const gui::UiPoint &b = hull[(index + 1U) % hull.size()];
      draw.add_line(a.x, a.y, b.x, b.y, stroke, color);
    }
    draw.add_line(cx, cy, hull[0].x, hull[0].y, stroke, color);
    draw.add_line(cx, cy, hull[2].x, hull[2].y, stroke, color);
    draw.add_line(cx, cy, hull[4].x, hull[4].y, stroke, color);
    return;
  }
  case ElementKind::Group: {
    // A folder: everything filed under it moves with it.
    const gui::Rect body{cx - 7.0F, cy - 3.5F, 14.0F, 9.0F};
    draw.add_rect(cx - 7.0F, cy - 6.0F, 6.0F, 3.0F, color);
    draw.add_rect(body.x, body.y, body.width, body.height, fill);
    rect_outline(ctx, body, color);
    return;
  }
  case ElementKind::DirectionalLight: {
    // The sun: a disc with long rays.
    draw_filled_circle(ctx, cx, cy, 3.0F, color);
    for (std::size_t ray = 0U; ray < 8U; ++ray) {
      const float angle = tau * static_cast<float>(ray) / 8.0F;
      draw.add_line(cx + std::cos(angle) * 4.8F, cy + std::sin(angle) * 4.8F,
                    cx + std::cos(angle) * 7.0F, cy + std::sin(angle) * 7.0F,
                    stroke, color);
    }
    return;
  }
  case ElementKind::PointLight: {
    if (light == scene::LocalLightKind::Spot) {
      // A spot: a lamp head and the cone it throws.
      draw_filled_circle(ctx, cx, cy - 4.5F, 2.2F, color);
      const std::array<gui::UiPoint, 3U> cone{
          {{cx, cy - 4.0F}, {cx + 6.0F, cy + 6.0F}, {cx - 6.0F, cy + 6.0F}}};
      draw.add_convex_poly(cone, fill);
      draw.add_line(cx, cy - 4.0F, cx + 6.0F, cy + 6.0F, stroke, color);
      draw.add_line(cx, cy - 4.0F, cx - 6.0F, cy + 6.0F, stroke, color);
      draw.add_line(cx - 6.0F, cy + 6.0F, cx + 6.0F, cy + 6.0F, stroke, color);
      return;
    }
    if (light == scene::LocalLightKind::Rectangle) {
      // A soft box: a lit panel.
      const gui::Rect panel{cx - 6.0F, cy - 4.0F, 12.0F, 8.0F};
      draw.add_rect(panel.x, panel.y, panel.width, panel.height,
                    with_alpha(color, 0.55F));
      rect_outline(ctx, panel, color);
      return;
    }
    // A point: a bulb with a halo.
    draw_filled_circle(ctx, cx, cy, 3.0F, color);
    circle_outline(ctx, cx, cy, 6.0F, 1.0F, with_alpha(color, 0.7F));
    return;
  }
  case ElementKind::Camera: {
    const gui::Rect body{cx - 7.0F, cy - 4.0F, 9.0F, 8.0F};
    draw.add_rect(body.x, body.y, body.width, body.height, fill);
    rect_outline(ctx, body, color);
    draw.add_triangle(cx + 2.0F, cy, cx + 7.0F, cy - 4.0F, cx + 7.0F,
                      cy + 4.0F, color);
    return;
  }
  case ElementKind::Track: {
    // A stack of bands: the clips filed on this screen.
    draw.add_rect(cx - 7.0F, cy - 5.0F, 14.0F, 2.5F, color);
    draw.add_rect(cx - 7.0F, cy - 1.25F, 10.0F, 2.5F, with_alpha(color, 0.75F));
    draw.add_rect(cx - 7.0F, cy + 2.5F, 12.0F, 2.5F, with_alpha(color, 0.55F));
    return;
  }
  case ElementKind::Environment: {
    // A globe: the world around the stage.
    circle_outline(ctx, cx, cy, 6.5F, stroke, color);
    draw.add_line(cx - 6.5F, cy, cx + 6.5F, cy, 1.0F, color);
    constexpr std::size_t steps = 10U;
    for (std::size_t index = 0U; index < steps; ++index) {
      const float a0 = tau * 0.5F * static_cast<float>(index) /
                           static_cast<float>(steps) - tau / 4.0F;
      const float a1 = tau * 0.5F * static_cast<float>(index + 1U) /
                           static_cast<float>(steps) - tau / 4.0F;
      draw.add_line(cx + std::cos(a0) * 3.0F, cy + std::sin(a0) * 6.5F,
                    cx + std::cos(a1) * 3.0F, cy + std::sin(a1) * 6.5F, 1.0F,
                    color);
      draw.add_line(cx - std::cos(a0) * 3.0F, cy + std::sin(a0) * 6.5F,
                    cx - std::cos(a1) * 3.0F, cy + std::sin(a1) * 6.5F, 1.0F,
                    color);
    }
    return;
  }
  case ElementKind::Object:
    break;
  }
  // A cube, three faces showing: the generated primitives.
  const std::array<gui::UiPoint, 4U> top{
      {{cx, cy - 7.0F}, {cx + 6.0F, cy - 4.0F}, {cx, cy - 1.0F},
       {cx - 6.0F, cy - 4.0F}}};
  draw.add_convex_poly(top, with_alpha(color, 0.35F));
  const std::array<gui::UiPoint, 4U> left{
      {{cx - 6.0F, cy - 4.0F}, {cx, cy - 1.0F}, {cx, cy + 7.0F},
       {cx - 6.0F, cy + 4.0F}}};
  draw.add_convex_poly(left, fill);
  draw.add_line(cx, cy - 7.0F, cx + 6.0F, cy - 4.0F, stroke, color);
  draw.add_line(cx + 6.0F, cy - 4.0F, cx, cy - 1.0F, stroke, color);
  draw.add_line(cx, cy - 1.0F, cx - 6.0F, cy - 4.0F, stroke, color);
  draw.add_line(cx - 6.0F, cy - 4.0F, cx, cy - 7.0F, stroke, color);
  draw.add_line(cx - 6.0F, cy - 4.0F, cx - 6.0F, cy + 4.0F, stroke, color);
  draw.add_line(cx + 6.0F, cy - 4.0F, cx + 6.0F, cy + 4.0F, stroke, color);
  draw.add_line(cx, cy - 1.0F, cx, cy + 7.0F, stroke, color);
  draw.add_line(cx - 6.0F, cy + 4.0F, cx, cy + 7.0F, stroke, color);
  draw.add_line(cx, cy + 7.0F, cx + 6.0F, cy + 4.0F, stroke, color);
}

bool row_flag_toggle(gui::UiContext &ctx, gui::UiId id, const gui::Rect &bounds,
                     RowFlag flag, bool state, bool row_hovered) {
  const gui::Interaction interaction = ctx.grab(id, bounds);
  ctx.cursor_for(interaction, gui::Cursor::Hand);
  const gui::Theme &theme = ctx.theme();
  if (interaction.held && !interaction.released) {
    ctx.panel(bounds.inflated(-2.0F, -2.0F), theme.widget_active);
  } else if (interaction.hovered) {
    ctx.panel(bounds.inflated(-2.0F, -2.0F), theme.widget_hover);
  }
  // Hidden and locked are the states worth seeing from across a stage, so
  // they are the ones drawn bright. The resting states stay in the column,
  // faint, so the toggle is always where the eye expects it.
  const bool noteworthy = flag == RowFlag::Visible ? !state : state;
  constexpr gui::UiColor lock_amber{0.96F, 0.72F, 0.28F, 1.0F};
  gui::UiColor color{};
  if (noteworthy) {
    color = flag == RowFlag::Visible ? theme.warning : lock_amber;
  } else if (interaction.hovered) {
    color = theme.text;
  } else {
    color = with_alpha(theme.text_muted,
                       flag == RowFlag::Visible ? 0.85F
                                                : (row_hovered ? 0.7F : 0.35F));
  }
  const float cx = bounds.x + bounds.width * 0.5F;
  const float cy = bounds.y + bounds.height * 0.5F;
  if (flag == RowFlag::Visible) {
    gui::draw_eye_glyph(ctx.draw_list(), cx, cy, state, color);
    ctx.tooltip(bounds, state ? "Hide" : "Show");
  } else {
    gui::draw_padlock_glyph(ctx.draw_list(), cx, cy + 1.0F, state, color);
    ctx.tooltip(bounds, state ? "Unlock" : "Lock");
  }
  return interaction.clicked;
}

void draw_twirl(gui::UiContext &ctx, float cx, float cy, bool open,
                gui::UiColor color) {
  auto &draw = ctx.draw_list();
  if (open) {
    draw.add_triangle(cx - 4.0F, cy - 2.0F, cx + 4.0F, cy - 2.0F, cx,
                      cy + 3.0F, color);
  } else {
    draw.add_triangle(cx - 2.0F, cy - 4.0F, cx + 3.0F, cy, cx - 2.0F,
                      cy + 4.0F, color);
  }
}

void draw_indent_guides(gui::UiContext &ctx, float x, float y, float height,
                        std::size_t depth, gui::UiColor color) {
  for (std::size_t level = 0U; level < depth; ++level) {
    const float guide =
        x + static_cast<float>(level) * list_metrics::indent +
        list_metrics::twirl_width * 0.5F;
    ctx.draw_list().add_rect(guide, y, 1.0F, height, color);
  }
}

float pill_width(const gui::UiContext &ctx, std::string_view text) {
  return ctx.font_width(text) + 12.0F;
}

gui::Rect draw_pill(gui::UiContext &ctx, float x, float centre_y,
                    std::string_view text, gui::UiColor color, bool filled) {
  const float height = std::min(ctx.line_height() + 4.0F, 18.0F);
  const gui::Rect bounds{x, centre_y - height * 0.5F, pill_width(ctx, text),
                         height};
  if (filled) {
    ctx.panel(bounds, color);
    // Dark ink on a lit pill, so it reads like an indicator lamp.
    ctx.label(gui::Rect{bounds.x + 6.0F, bounds.y, bounds.width - 6.0F,
                        bounds.height},
              text, gui::UiColor{0.04F, 0.05F, 0.06F, 1.0F});
  } else {
    ctx.panel(bounds, with_alpha(color, 0.16F));
    rect_outline(ctx, bounds, with_alpha(color, 0.65F));
    ctx.label(gui::Rect{bounds.x + 6.0F, bounds.y, bounds.width - 6.0F,
                        bounds.height},
              text, color);
  }
  return bounds;
}

void draw_filled_circle(gui::UiContext &ctx, float cx, float cy, float radius,
                        gui::UiColor color) {
  constexpr std::size_t steps = 14U;
  std::array<gui::UiPoint, steps> points{};
  for (std::size_t index = 0U; index < steps; ++index) {
    const float angle =
        tau * static_cast<float>(index) / static_cast<float>(steps);
    points[index] = gui::UiPoint{cx + std::cos(angle) * radius,
                                 cy + std::sin(angle) * radius};
  }
  ctx.draw_list().add_convex_poly(points, color);
}

void draw_status_dot(gui::UiContext &ctx, float cx, float cy,
                     gui::UiColor color, float radius) {
  // A soft halo makes a lit status read as lit, not as a coloured speck.
  draw_filled_circle(ctx, cx, cy, radius + 2.0F, with_alpha(color, 0.22F));
  draw_filled_circle(ctx, cx, cy, radius, color);
}

bool icon_button(gui::UiContext &ctx, gui::UiId id, const gui::Rect &bounds,
                 gui::Icon icon, std::string_view tooltip, bool engaged,
                 bool enabled) {
  const gui::Theme &theme = ctx.theme();
  const gui::Interaction interaction =
      enabled ? ctx.grab(id, bounds) : gui::Interaction{};
  if (enabled) {
    ctx.cursor_for(interaction, gui::Cursor::Hand);
  }
  if (interaction.held && !interaction.released) {
    ctx.panel(bounds, theme.widget_active);
  } else if (engaged) {
    ctx.panel(bounds, with_alpha(theme.accent, 0.20F));
    ctx.panel(gui::Rect{bounds.x, bounds.y + bounds.height - 2.0F,
                        bounds.width, 2.0F},
              theme.accent);
  } else if (interaction.hovered) {
    ctx.panel(bounds, theme.widget_hover);
  }
  const gui::UiColor color =
      !enabled ? with_alpha(theme.text_muted, 0.4F)
               : (engaged ? theme.accent
                          : (interaction.hovered ? theme.text
                                                 : mixed(theme.text_muted,
                                                         theme.text, 0.35F)));
  gui::draw_icon(ctx.draw_list(), icon, bounds.x + bounds.width * 0.5F,
                 bounds.y + bounds.height * 0.5F, color);
  ctx.tooltip(bounds, tooltip);
  return enabled && interaction.clicked;
}

bool ClickTracker::double_click(core::u64 key, double now) noexcept {
  const bool second = key != 0U && key == key_ && time_ >= 0.0 &&
                      now >= time_ && now - time_ <= double_click_seconds;
  if (second) {
    key_ = 0U;
    time_ = -1.0;
    return true;
  }
  key_ = key;
  time_ = now;
  return false;
}

void ClickTracker::reset() noexcept {
  key_ = 0U;
  time_ = -1.0;
}

void InlineRename::begin(gui::UiContext &ctx, core::u64 key,
                         std::string_view current) {
  key_ = key;
  draft_ = std::string{current};
  original_ = draft_;
  drawn_ = true;
  ctx.focus_text_field_selected(id_, draft_);
}

std::optional<std::string> InlineRename::draw(gui::UiContext &ctx,
                                              const gui::Rect &bounds) {
  if (key_ == 0U) {
    return std::nullopt;
  }
  drawn_ = true;
  static_cast<void>(ctx.text_field(id_, bounds, draft_));
  if (ctx.focus() == id_) {
    return std::nullopt;
  }
  // Focus has gone: Enter, Tab, a click elsewhere, or Escape (which put the
  // original back before letting go).
  std::string committed = draft_;
  const auto first = committed.find_first_not_of(' ');
  const auto last = committed.find_last_not_of(' ');
  committed = first == std::string::npos
                  ? std::string{}
                  : committed.substr(first, last - first + 1U);
  const bool changed = !committed.empty() && committed != original_;
  key_ = 0U;
  draft_.clear();
  original_.clear();
  if (!changed) {
    return std::nullopt;
  }
  return committed;
}

void InlineRename::end_frame(gui::UiContext &ctx) noexcept {
  if (key_ != 0U && !drawn_) {
    cancel(ctx);
  }
  drawn_ = false;
}

void InlineRename::reset() noexcept {
  key_ = 0U;
  draft_.clear();
  original_.clear();
  drawn_ = false;
}

void InlineRename::cancel(gui::UiContext &ctx) noexcept {
  if (key_ != 0U && ctx.focus() == id_) {
    ctx.clear_focus();
  }
  key_ = 0U;
  draft_.clear();
  original_.clear();
}

} // namespace mediaserver::editor
