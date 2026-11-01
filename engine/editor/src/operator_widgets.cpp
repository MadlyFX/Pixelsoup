#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/gui/shapes.hpp>

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
  return gui::blend(a, b, t);
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
    ctx.outline(body, color);
    ctx.outline(cabin, color);
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
    ctx.outline(frame, color);
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
    ctx.outline(body, color);
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
      ctx.outline(panel, color);
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
    ctx.outline(body, color);
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
  case ElementKind::ParticleEffect: {
    // A burst: sparks thrown out from a point, larger as they travel.
    draw_filled_circle(ctx, cx, cy, 1.6F, color);
    for (std::size_t spark = 0U; spark < 7U; ++spark) {
      const float angle = tau * static_cast<float>(spark) / 7.0F - tau / 4.0F;
      const float reach = spark % 2U == 0U ? 6.0F : 4.2F;
      draw_filled_circle(ctx, cx + std::cos(angle) * reach,
                         cy + std::sin(angle) * reach,
                         spark % 2U == 0U ? 1.3F : 1.0F,
                         spark % 2U == 0U ? color : with_alpha(color, 0.7F));
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
  // A round target under the pointer, centred on the mark.
  const float target = std::min(bounds.width, bounds.height) - 4.0F;
  const gui::Rect disc{bounds.center_x() - target * 0.5F,
                       bounds.center_y() - target * 0.5F, target, target};
  if (interaction.held && !interaction.released) {
    ctx.rounded(disc, theme.widget_active, target * 0.5F);
  } else if (interaction.hovered) {
    ctx.rounded(disc, theme.widget_hover, target * 0.5F);
  }
  // Hidden and locked are the states worth seeing from across a stage, so
  // they are the ones drawn bright, in the amber that asks for a look. The
  // resting states stay in the column, faint, so the toggle is always where
  // the eye expects it.
  const bool noteworthy = flag == RowFlag::Visible ? !state : state;
  gui::UiColor color{};
  if (noteworthy) {
    color = theme.caution;
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
  return ctx.font_width(text) + 16.0F;
}

gui::Rect draw_pill(gui::UiContext &ctx, float x, float centre_y,
                    std::string_view text, gui::UiColor color, bool filled) {
  const float height = std::min(ctx.line_height() + 4.0F, 20.0F);
  const gui::Rect bounds{x, centre_y - height * 0.5F, pill_width(ctx, text),
                         height};
  if (filled) {
    // A lit lozenge with a soft halo and dark ink, so it reads like an
    // indicator lamp.
    ctx.glow(bounds, color, height * 0.5F, 8.0F);
    ctx.rounded(bounds, color, height * 0.5F);
    ctx.label(gui::Rect{bounds.x + 8.0F, bounds.y, bounds.width - 8.0F,
                        bounds.height},
              text, gui::UiColor{0.04F, 0.05F, 0.06F, 1.0F});
  } else {
    // Unlit: a tinted capsule carrying its colour in the text.
    ctx.rounded(bounds, with_alpha(color, 0.16F), height * 0.5F);
    ctx.label(gui::Rect{bounds.x + 8.0F, bounds.y, bounds.width - 8.0F,
                        bounds.height},
              text, color);
  }
  return bounds;
}

void draw_filled_circle(gui::UiContext &ctx, float cx, float cy, float radius,
                        gui::UiColor color) {
  gui::add_disc(ctx.draw_list(), gui::UiPoint{cx, cy}, radius, color, 14U);
}

void draw_status_dot(gui::UiContext &ctx, float cx, float cy,
                     gui::UiColor color, float radius) {
  // A soft halo makes a lit status read as lit, not as a coloured speck.
  draw_filled_circle(ctx, cx, cy, radius + 2.0F, with_alpha(color, 0.22F));
  draw_filled_circle(ctx, cx, cy, radius, color);
}

void draw_proxy_progress(gui::UiContext &ctx, gui::UiId id,
                         const gui::Rect &control, float fraction) {
  const gui::Theme &theme = ctx.theme();
  // Along the control's foot below its text, filling in quickly as
  // segments land.
  const gui::Rect track{control.x + 8.0F, control.bottom() - 2.0F,
                        std::max(control.width - 16.0F, 0.0F), 2.0F};
  ctx.rounded(track, with_alpha(theme.track, 0.9F), track.height * 0.5F);
  const float shown = ctx.ease(id, std::clamp(fraction, 0.0F, 1.0F));
  if (shown > 0.0F) {
    ctx.rounded(gui::Rect{track.x, track.y, track.width * shown,
                          track.height},
                theme.accent, track.height * 0.5F);
  }
}

bool search_box(gui::UiContext &ctx, gui::UiId id, const gui::Rect &field,
                std::string &value, std::string_view placeholder) {
  const bool changed = ctx.text_field(id, field, value);
  if (value.empty() && ctx.focus() != id) {
    ctx.label(gui::inset(field, gui::Padding{19.0F, 0.0F, 4.0F, 0.0F}),
              placeholder, ctx.theme().text_muted);
  }
  // The lens 9 in from the edge and a little above the middle, beside where
  // the text starts.
  gui::draw_icon(ctx.draw_list(), gui::Icon::Search, field.x + 10.5F,
                 field.y + field.height * 0.46F + 1.5F, ctx.theme().text_muted);
  return changed;
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
  // A soft rounded body that eases in under the pointer, sinks a pixel
  // while pressed, and stays tinted with the accent while the tool is on.
  const float half = std::min(bounds.width, bounds.height) * 0.5F;
  const float radius = half > 14.0F ? theme.radius : half;
  const float hover = ctx.ease(
      id ^ gui::ui_id("icon-button.hover"),
      enabled && (interaction.hovered || interaction.held) ? 1.0F : 0.0F);
  if (interaction.held && !interaction.released) {
    ctx.rounded(bounds.inflated(-1.0F, -1.0F), theme.widget_active, radius);
  } else if (engaged) {
    ctx.rounded(bounds,
                mixed(with_alpha(theme.accent, 0.18F),
                      with_alpha(theme.accent, 0.28F), hover),
                radius);
  } else if (hover > 0.0F) {
    ctx.rounded(bounds, with_alpha(theme.widget_hover, hover), radius);
  }
  const gui::UiColor color =
      !enabled ? with_alpha(theme.text_muted, 0.4F)
               : (engaged ? theme.accent
                          : mixed(mixed(theme.text_muted, theme.text, 0.45F),
                                  theme.text, hover));
  gui::draw_icon(ctx.draw_list(), icon, bounds.center_x(), bounds.center_y(),
                 color);
  ctx.tooltip(bounds, tooltip);
  return enabled && interaction.clicked;
}

bool tool_key(gui::UiContext &ctx, gui::UiId id, const gui::Rect &bounds,
              gui::Icon icon, bool engaged, std::string_view tooltip,
              gui::UiColor engaged_color, float icon_scale) {
  const gui::Interaction interaction = ctx.grab(id, bounds);
  ctx.cursor_for(interaction, gui::Cursor::Hand);
  const gui::Theme &theme = ctx.theme();
  const float hover = ctx.ease(id ^ gui::ui_id("timeline.icon.hover"),
                               interaction.hovered ? 1.0F : 0.0F);
  const float radius = std::min(bounds.height * 0.5F, theme.radius);
  const bool pressed = interaction.held && !interaction.released;
  ctx.rounded(pressed ? bounds.inflated(-1.0F, -1.0F) : bounds,
              pressed ? theme.widget_active
                      : (engaged ? with_alpha(engaged_color, 0.22F + 0.08F * hover)
                                 : mixed(theme.widget, theme.widget_hover, hover)),
              radius);
  gui::draw_icon(ctx.draw_list(), icon, bounds.center_x(), bounds.center_y(),
                 engaged ? engaged_color
                         : mixed(theme.text_muted, theme.text, hover),
                 icon_scale);
  ctx.tooltip(bounds, tooltip);
  return interaction.clicked;
}

bool pill_button(gui::UiContext &ctx, gui::UiId id, const gui::Rect &bounds,
                 std::optional<gui::Icon> icon, std::string_view label,
                 std::string_view tooltip, ButtonTone tone, bool enabled,
                 bool engaged) {
  const gui::Theme &theme = ctx.theme();
  const gui::Interaction interaction =
      enabled ? ctx.grab(id, bounds) : gui::Interaction{};
  if (enabled) {
    ctx.cursor_for(interaction, gui::Cursor::Hand);
  }
  const float hover = ctx.ease(
      id ^ gui::ui_id("pill-button.hover"),
      enabled && (interaction.hovered || interaction.held) ? 1.0F : 0.0F);
  const bool pressed = interaction.held && !interaction.released;
  const gui::Rect shape = pressed ? bounds.inflated(-1.0F, -1.0F) : bounds;
  const float radius = shape.height * 0.5F;
  gui::UiColor fill{};
  gui::UiColor ink = theme.text;
  switch (tone) {
  case ButtonTone::Quiet:
    fill = engaged ? with_alpha(theme.accent, 0.2F + 0.08F * hover)
                   : mixed(theme.widget, theme.widget_hover, hover);
    ink = engaged ? theme.accent : theme.text;
    break;
  case ButtonTone::Primary:
    fill = with_alpha(theme.accent, 0.22F + 0.12F * hover);
    break;
  case ButtonTone::Danger:
    fill = with_alpha(theme.warning, 0.18F * hover);
    ink = theme.warning;
    break;
  }
  if (!enabled) {
    fill = with_alpha(theme.widget, 0.5F);
    ink = with_alpha(theme.text_muted, 0.6F);
  } else if (pressed) {
    fill = theme.widget_active;
  }
  ctx.rounded(shape, fill, radius);
  if (tone == ButtonTone::Danger && enabled) {
    ctx.rounded_outline(shape, with_alpha(theme.warning, 0.6F), radius, 1.5F);
  }
  // The icon and the word as one centred group.
  constexpr float icon_width = 16.0F;
  constexpr float gap = 6.0F;
  const float text_width = label.empty() ? 0.0F : ctx.font_width(label);
  const float content = (icon.has_value() ? icon_width : 0.0F) +
                        (icon.has_value() && !label.empty() ? gap : 0.0F) +
                        text_width;
  float x = bounds.center_x() - content * 0.5F;
  if (icon.has_value()) {
    gui::draw_icon(ctx.draw_list(), *icon, x + icon_width * 0.5F,
                   bounds.center_y(), ink);
    x += icon_width + gap;
  }
  if (!label.empty()) {
    ctx.label(gui::Rect{x, bounds.y,
                        std::max(bounds.right() - x - 4.0F, 0.0F),
                        bounds.height},
              label, ink);
  }
  if (!tooltip.empty()) {
    ctx.tooltip(bounds, tooltip);
  }
  return enabled && interaction.clicked;
}

bool reset_button(gui::UiContext &ctx, gui::UiId id, const gui::Rect &bounds,
                  std::string_view tooltip) {
  const gui::Interaction interaction = ctx.grab(id, bounds);
  ctx.cursor_for(interaction, gui::Cursor::Hand);
  const gui::Theme &theme = ctx.theme();
  const float side = std::min({bounds.width, bounds.height, 18.0F});
  const gui::Rect disc{bounds.center_x() - side * 0.5F,
                       bounds.center_y() - side * 0.5F, side, side};
  if (interaction.hovered || interaction.held) {
    ctx.rounded(disc, with_alpha(theme.caution, interaction.held ? 0.32F : 0.18F),
                side * 0.5F);
  }
  gui::draw_icon(ctx.draw_list(), gui::Icon::Reset, bounds.center_x(),
                 bounds.center_y(),
                 interaction.hovered ? theme.caution
                                     : with_alpha(theme.caution, 0.8F),
                 0.85F);
  ctx.tooltip(bounds, tooltip);
  return interaction.clicked;
}

bool action_button(gui::UiContext &ctx, gui::UiId id, const gui::Rect &bounds,
                   std::string_view label, bool enabled, bool selected) {
  const gui::Interaction interaction =
      enabled ? ctx.grab(id, bounds) : gui::Interaction{};
  if (enabled) {
    ctx.cursor_for(interaction, gui::Cursor::Hand);
  }
  const gui::Theme &theme = ctx.theme();
  const float hover = ctx.ease(
      id ^ gui::ui_id("action-button.hover"),
      enabled && (interaction.hovered || interaction.held) ? 1.0F : 0.0F);
  const bool pressed = interaction.held && !interaction.released;
  const gui::UiColor fill =
      !enabled ? theme.track
               : (pressed ? theme.widget_active
                          : (selected ? mixed(theme.selection,
                                              with_alpha(theme.accent, 0.4F),
                                              hover * 0.3F)
                                      : mixed(theme.widget, theme.widget_hover,
                                              hover)));
  // A pill, so it reads as something to press; a selected toggle is ringed
  // in the accent as well as tinted.
  const gui::Rect shape = pressed ? bounds.inflated(-1.0F, -1.0F) : bounds;
  const float radius = std::min(shape.height * 0.5F, theme.radius * 1.5F);
  ctx.rounded(shape, fill, radius);
  if (selected && enabled) {
    ctx.rounded_outline(shape, with_alpha(theme.accent, 0.55F), radius);
  }
  ctx.label_centered(gui::inset(bounds, gui::Padding{6.0F, 0.0F, 6.0F, 0.0F}),
                     label, enabled ? theme.text : theme.text_muted);
  return enabled && interaction.clicked;
}

void draw_drop_target(gui::UiContext &ctx, const gui::Rect &area,
                      std::string_view instruction, bool hovered,
                      float callout_width) {
  if (area.width <= 0.0F || area.height <= 0.0F) {
    return;
  }
  const gui::UiColor color =
      with_alpha(ctx.theme().accent, hovered ? 1.0F : 0.35F);
  const float stroke = hovered ? 2.0F : 1.0F;
  const float radius = ctx.theme().radius_large;
  ctx.rounded(area, with_alpha(ctx.theme().accent, hovered ? 0.12F : 0.025F),
              radius);
  ctx.rounded_outline(area, color, radius, stroke);

  constexpr float margin = 9.0F;
  constexpr float callout_height = 30.0F;
  gui::Rect corner = gui::inset(area, margin);
  gui::Rect strip = gui::cut_top(corner, callout_height);
  const gui::Rect callout = gui::cut_left(strip, callout_width);
  if (callout.width <= 0.0F || callout.height <= 0.0F) {
    return;
  }
  ctx.rounded(callout, hovered ? ctx.theme().widget_active : ctx.theme().header,
              callout.height * 0.5F);
  ctx.rounded_outline(callout, color, callout.height * 0.5F, stroke);
  gui::Rect text = callout;
  ctx.label(gui::inset(text, gui::Padding{12.0F, 0.0F, 8.0F, 0.0F}),
            instruction, hovered ? ctx.theme().text : ctx.theme().text_muted);
}

void draw_diamond(gui::UiContext &ctx, float cx, float cy, float radius,
                  gui::UiColor color) {
  gui::add_diamond(ctx.draw_list(), gui::UiPoint{cx, cy}, radius, color);
}

bool key_button(gui::UiContext &ctx, gui::UiId id, const gui::Rect &bounds,
                bool animated, bool keyed_here, gui::UiColor background) {
  const gui::Interaction interaction = ctx.grab(id, bounds);
  gui::UiColor color =
      keyed_here ? ctx.theme().playhead
                 : (animated ? ctx.theme().keyframe : ctx.theme().text_muted);
  if (interaction.hovered) {
    color = ctx.theme().accent;
  }
  const float cx = bounds.center_x();
  const float cy = bounds.center_y();
  draw_diamond(ctx, cx, cy, interaction.held ? 5.0F : 4.5F, color);
  if (!keyed_here && !interaction.held) {
    draw_diamond(ctx, cx, cy, 2.4F, background);
  }
  return interaction.clicked;
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
