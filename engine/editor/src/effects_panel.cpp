#include <mediaserver/editor/effects_panel.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace mediaserver::editor {
namespace {

[[nodiscard]] gui::UiId mix(gui::UiId base, core::u64 value) noexcept {
  base ^= value + 0x9E3779B97F4A7C15ULL + (base << 6U) + (base >> 2U);
  return base;
}

[[nodiscard]] gui::UiColor alpha(gui::UiColor color, float opacity) noexcept {
  color.a = opacity;
  return color;
}

[[nodiscard]] bool matches(std::string_view value,
                           std::string_view query) noexcept {
  if (query.empty()) {
    return true;
  }
  return std::search(
             value.begin(), value.end(), query.begin(), query.end(),
             [](char left, char right) {
               return std::tolower(static_cast<unsigned char>(left)) ==
                      std::tolower(static_cast<unsigned char>(right));
             }) != value.end();
}

void draw_border(gui::UiContext &ctx, const gui::Rect &bounds,
                 gui::UiColor color, float thickness) {
  if (bounds.width <= 0.0F || bounds.height <= 0.0F || thickness <= 0.0F) {
    return;
  }
  ctx.panel(gui::Rect{bounds.x, bounds.y, bounds.width, thickness}, color);
  ctx.panel(gui::Rect{bounds.x,
                      bounds.y + std::max(bounds.height - thickness, 0.0F),
                      bounds.width, thickness},
            color);
  ctx.panel(gui::Rect{bounds.x, bounds.y, thickness, bounds.height}, color);
  ctx.panel(gui::Rect{bounds.x + std::max(bounds.width - thickness, 0.0F),
                      bounds.y, thickness, bounds.height},
            color);
}

struct EffectDescription final {
  scene::EffectKind kind;
  std::string_view group;
  std::string_view name;
  std::string_view description;
  gui::UiColor color;
};

constexpr std::array<EffectDescription, 14U> effects{{
    {scene::EffectKind::Brightness, "COLOR", "Brightness", "Linear exposure multiplier",
     gui::UiColor{0.980F, 0.760F, 0.260F, 1.0F}},
    {scene::EffectKind::Saturation, "COLOR", "Saturation", "Colour intensity amount",
     gui::UiColor{0.965F, 0.380F, 0.600F, 1.0F}},
    {scene::EffectKind::Levels, "COLOR", "Levels", "Master and per-channel tonal range",
     gui::UiColor{0.330F, 0.850F, 0.680F, 1.0F}},
    {scene::EffectKind::LiftGammaGain, "COLOR", "Lift / Gamma / Gain",
     "RGB shadows, midtones, and highlights",
     gui::UiColor{0.940F, 0.620F, 0.240F, 1.0F}},
    {scene::EffectKind::ContrastPivot, "COLOR", "Contrast / Pivot",
     "Contrast around an explicit tonal pivot",
     gui::UiColor{0.720F, 0.480F, 0.980F, 1.0F}},
    {scene::EffectKind::ColorMatrix, "COLOR", "Channel Mixer",
     "3x3 RGB colour matrix and offset",
     gui::UiColor{0.260F, 0.820F, 0.880F, 1.0F}},
    {scene::EffectKind::WhiteBalance, "COLOR", "White Balance",
     "D65 temperature and tint adaptation",
     gui::UiColor{0.980F, 0.700F, 0.420F, 1.0F}},
    {scene::EffectKind::Curves, "COLOR", "Curves",
     "Deterministic master and RGB tonal curves",
     gui::UiColor{0.520F, 0.880F, 0.980F, 1.0F}},
    {scene::EffectKind::ChromaKey, "KEYING", "Chroma Key",
     "Colour-distance key with spill suppression",
     gui::UiColor{0.300F, 0.900F, 0.430F, 1.0F}},
    {scene::EffectKind::LumaKey, "KEYING", "Luma Key",
     "Threshold key from source luminance",
     gui::UiColor{0.780F, 0.820F, 0.880F, 1.0F}},
    {scene::EffectKind::Blur, "BLUR", "Gaussian Blur",
     "Smooth separable Gaussian kernel",
     gui::UiColor{0.420F, 0.710F, 1.000F, 1.0F}},
    {scene::EffectKind::BoxBlur, "BLUR", "Box Blur", "Exact integer-radius box passes",
     gui::UiColor{0.260F, 0.820F, 0.880F, 1.0F}},
    {scene::EffectKind::DirectionalBlur, "BLUR", "Directional Blur",
     "Centred line blur by length and angle",
     gui::UiColor{0.620F, 0.500F, 0.980F, 1.0F}},
    {scene::EffectKind::FilmGrain, "TEXTURE", "Film Grain",
     "Frame-stable canvas-space texture",
     gui::UiColor{0.760F, 0.720F, 0.580F, 1.0F}},
}};

[[nodiscard]] constexpr const EffectDescription *
description_for(scene::EffectKind kind) noexcept {
  switch (kind) {
  case scene::EffectKind::Brightness:
    return &effects[0U];
  case scene::EffectKind::Saturation:
    return &effects[1U];
  case scene::EffectKind::Blur:
    return &effects[10U];
  case scene::EffectKind::BoxBlur:
    return &effects[11U];
  case scene::EffectKind::DirectionalBlur:
    return &effects[12U];
  case scene::EffectKind::ChromaKey:
    return &effects[8U];
  case scene::EffectKind::LumaKey:
    return &effects[9U];
  case scene::EffectKind::Levels:
    return &effects[2U];
  case scene::EffectKind::LiftGammaGain:
    return &effects[3U];
  case scene::EffectKind::ContrastPivot:
    return &effects[4U];
  case scene::EffectKind::ColorMatrix:
    return &effects[5U];
  case scene::EffectKind::WhiteBalance:
    return &effects[6U];
  case scene::EffectKind::Curves:
    return &effects[7U];
  case scene::EffectKind::FilmGrain:
    return &effects[13U];
  case scene::EffectKind::Count:
    break;
  }
  return nullptr;
}

void draw_effect_icon(gui::UiContext &ctx, const gui::Rect &bounds,
                      const EffectDescription &effect) {
  ctx.panel(bounds, ctx.theme().track);
  ctx.panel(gui::Rect{bounds.x, bounds.y, 2.0F, bounds.height}, effect.color);
  switch (effect.kind) {
  case scene::EffectKind::Brightness: {
    const float cx = bounds.x + bounds.width * 0.5F;
    const float cy = bounds.y + bounds.height * 0.5F;
    ctx.panel(gui::Rect{cx - 2.0F, cy - 2.0F, 4.0F, 4.0F}, effect.color);
    for (std::size_t index = 0U; index < 4U; ++index) {
      const float dx = index < 2U ? (index == 0U ? -7.0F : 7.0F) : 0.0F;
      const float dy = index >= 2U ? (index == 2U ? -7.0F : 7.0F) : 0.0F;
      ctx.draw_list().add_line(cx + dx * 0.55F, cy + dy * 0.55F, cx + dx,
                               cy + dy, 1.0F, effect.color);
    }
    return;
  }
  case scene::EffectKind::Saturation: {
    constexpr std::array<float, 3U> heights{{8.0F, 16.0F, 11.0F}};
    for (std::size_t index = 0U; index < heights.size(); ++index) {
      const float height = heights[index];
      ctx.draw_list().add_rect(bounds.x + 6.0F +
                                   static_cast<float>(index) * 6.0F,
                               bounds.y + (bounds.height - height) * 0.5F, 3.0F,
                               height, effect.color);
    }
    return;
  }
  case scene::EffectKind::Blur:
    break;
  case scene::EffectKind::BoxBlur: {
    for (std::size_t index = 0U; index < 3U; ++index) {
      const float inset = 5.0F + static_cast<float>(index) * 4.0F;
      const float width = std::max(bounds.width - inset * 2.0F, 1.0F);
      const float height = std::max(bounds.height - inset * 2.0F, 1.0F);
      ctx.draw_list().add_rect(bounds.x + inset, bounds.y + inset, width, 1.0F,
                               effect.color);
      ctx.draw_list().add_rect(bounds.x + inset,
                               bounds.y + inset + height - 1.0F, width, 1.0F,
                               effect.color);
      ctx.draw_list().add_rect(bounds.x + inset, bounds.y + inset, 1.0F, height,
                               effect.color);
      ctx.draw_list().add_rect(bounds.x + inset + width - 1.0F,
                               bounds.y + inset, 1.0F, height, effect.color);
    }
    return;
  }
  case scene::EffectKind::DirectionalBlur: {
    const float left = bounds.x + 5.0F;
    const float right = bounds.x + bounds.width - 5.0F;
    const float bottom = bounds.y + bounds.height - 7.0F;
    const float top = bounds.y + 7.0F;
    for (std::size_t index = 0U; index < 3U; ++index) {
      const float offset = static_cast<float>(index) * 4.0F;
      ctx.draw_list().add_line(left, bottom - offset, right - offset, top, 1.5F,
                               effect.color);
    }
    return;
  }
  case scene::EffectKind::ChromaKey: {
    const float cx = bounds.x + bounds.width * 0.5F;
    const float cy = bounds.y + bounds.height * 0.5F;
    ctx.draw_list().add_rect(cx - 7.0F, cy - 7.0F, 14.0F, 14.0F, effect.color);
    ctx.draw_list().add_rect(cx - 5.0F, cy - 5.0F, 10.0F, 10.0F,
                             ctx.theme().track);
    ctx.draw_list().add_rect(cx - 2.0F, cy - 2.0F, 4.0F, 4.0F,
                             ctx.theme().text);
    return;
  }
  case scene::EffectKind::LumaKey: {
    constexpr std::array<float, 4U> shades{{0.25F, 0.45F, 0.70F, 0.95F}};
    for (std::size_t index = 0U; index < shades.size(); ++index) {
      ctx.draw_list().add_rect(
          bounds.x + 5.0F + static_cast<float>(index) * 5.5F, bounds.y + 7.0F,
          4.0F, std::max(bounds.height - 14.0F, 1.0F),
          gui::UiColor{shades[index], shades[index], shades[index], 1.0F});
    }
    return;
  }
  case scene::EffectKind::Levels: {
    constexpr std::array<float, 5U> heights{{4.0F, 10.0F, 18.0F, 12.0F, 6.0F}};
    for (std::size_t index = 0U; index < heights.size(); ++index) {
      const float height = heights[index];
      ctx.draw_list().add_rect(
          bounds.x + 4.0F + static_cast<float>(index) * 4.5F,
          bounds.y + bounds.height - 5.0F - height, 3.0F, height, effect.color);
    }
    return;
  }
  case scene::EffectKind::LiftGammaGain: {
    for (std::size_t index = 0U; index < 3U; ++index) {
      const float x = bounds.x + 7.0F + static_cast<float>(index) * 7.0F;
      ctx.draw_list().add_rect(x, bounds.y + 8.0F, 4.0F, 14.0F, effect.color);
      ctx.draw_list().add_rect(
          x - 2.0F, bounds.y + 10.0F + static_cast<float>(index) * 4.0F, 8.0F,
          2.0F, ctx.theme().text);
    }
    return;
  }
  case scene::EffectKind::ContrastPivot: {
    const float middle = bounds.x + bounds.width * 0.5F;
    const float top = bounds.y + 6.0F;
    const float bottom = bounds.y + bounds.height - 6.0F;
    ctx.draw_list().add_line(bounds.x + 5.0F, bottom, middle, top, 2.0F,
                             effect.color);
    ctx.draw_list().add_line(middle, top, bounds.x + bounds.width - 5.0F,
                             bottom, 2.0F, effect.color);
    ctx.draw_list().add_rect(middle - 2.0F,
                             bounds.y + bounds.height * 0.5F - 2.0F, 4.0F, 4.0F,
                             ctx.theme().text);
    return;
  }
  case scene::EffectKind::ColorMatrix: {
    for (std::size_t row = 0U; row < 3U; ++row) {
      for (std::size_t column = 0U; column < 3U; ++column) {
        const float size = row == column ? 4.0F : 2.0F;
        ctx.draw_list().add_rect(
            bounds.x + 6.0F + static_cast<float>(column) * 7.0F,
            bounds.y + 6.0F + static_cast<float>(row) * 7.0F, size, size,
            effect.color);
      }
    }
    return;
  }
  case scene::EffectKind::WhiteBalance: {
    const float middle = bounds.x + bounds.width * 0.5F;
    ctx.draw_list().add_line(middle, bounds.y + 5.0F, middle,
                             bounds.y + bounds.height - 5.0F, 2.0F,
                             effect.color);
    ctx.draw_list().add_rect(bounds.x + 5.0F, bounds.y + 8.0F,
                             middle - bounds.x - 7.0F, 12.0F,
                             gui::UiColor{0.35F, 0.62F, 1.0F, 1.0F});
    ctx.draw_list().add_rect(middle + 2.0F, bounds.y + 8.0F,
                             bounds.x + bounds.width - middle - 7.0F, 12.0F,
                             gui::UiColor{1.0F, 0.58F, 0.25F, 1.0F});
    return;
  }
  case scene::EffectKind::Curves: {
    constexpr std::array<float, 5U> values{{0.78F, 0.45F, 0.56F, 0.82F, 0.94F}};
    float previous_x = bounds.x + 4.0F;
    float previous_y = bounds.y + bounds.height - 5.0F;
    for (std::size_t index = 0U; index < values.size(); ++index) {
      const float x = bounds.x + 4.0F +
                      static_cast<float>(index + 1U) * (bounds.width - 8.0F) /
                          static_cast<float>(values.size());
      const float y = bounds.y + bounds.height - 5.0F -
                      values[index] * (bounds.height - 10.0F);
      ctx.draw_list().add_line(previous_x, previous_y, x, y, 1.5F,
                               effect.color);
      ctx.draw_list().add_rect(x - 1.5F, y - 1.5F, 3.0F, 3.0F,
                               ctx.theme().text);
      previous_x = x;
      previous_y = y;
    }
    return;
  }
  case scene::EffectKind::FilmGrain: {
    constexpr std::array<std::array<float, 2U>, 9U> points{{
        {{0.18F, 0.24F}},
        {{0.52F, 0.17F}},
        {{0.79F, 0.29F}},
        {{0.31F, 0.48F}},
        {{0.67F, 0.53F}},
        {{0.14F, 0.72F}},
        {{0.46F, 0.81F}},
        {{0.73F, 0.76F}},
        {{0.88F, 0.61F}},
    }};
    for (const auto &point : points) {
      ctx.draw_list().add_rect(bounds.x + point[0U] * bounds.width - 1.0F,
                               bounds.y + point[1U] * bounds.height - 1.0F,
                               2.0F, 2.0F, effect.color);
    }
    return;
  }
  case scene::EffectKind::Count:
    return;
  }
  for (std::size_t index = 0U; index < 3U; ++index) {
    const float inset = static_cast<float>(index) * 3.0F;
    ctx.draw_list().add_rect(
        bounds.x + 5.0F + inset, bounds.y + 5.0F + inset,
        std::max(bounds.width - 10.0F - inset * 2.0F, 1.0F), 1.0F,
        effect.color);
  }
}

} // namespace

bool effect_drop_targets_selected_layer(
    const EffectApplyRequest &request, const gui::Rect *stage_bounds,
    const gui::Rect *inspector_bounds) noexcept {
  if (request.target == doc::NodeId{}) {
    return false;
  }
  return (stage_bounds != nullptr &&
          stage_bounds->contains(request.screen_x, request.screen_y)) ||
         (inspector_bounds != nullptr &&
          inspector_bounds->contains(request.screen_x, request.screen_y));
}

void draw_effect_drop_target(gui::UiContext &ctx, const gui::Rect &bounds,
                             std::string_view target_name) {
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return;
  }
  const bool hovered =
      bounds.contains(ctx.pointer_x(), ctx.pointer_y());
  const gui::UiColor target_color =
      alpha(ctx.theme().accent, hovered ? 1.0F : 0.35F);

  // Match the Feed Mapping and timeline language: every valid target is
  // visible, and the one under the pointer becomes materially stronger.
  ctx.panel(bounds, alpha(ctx.theme().accent, hovered ? 0.12F : 0.025F));
  draw_border(ctx, bounds, target_color, hovered ? 2.0F : 1.0F);

  constexpr float inset = 9.0F;
  constexpr float callout_height = 30.0F;
  const gui::Rect callout{
      bounds.x + inset, bounds.y + inset,
      std::min(std::max(bounds.width - inset * 2.0F, 0.0F), 320.0F),
      std::min(callout_height,
               std::max(bounds.height - inset * 2.0F, 0.0F))};
  if (callout.width <= 0.0F || callout.height <= 0.0F) {
    return;
  }
  ctx.panel(callout,
            hovered ? ctx.theme().widget_active : ctx.theme().header);
  draw_border(ctx, callout, target_color, hovered ? 2.0F : 1.0F);
  ctx.panel(gui::Rect{callout.x, callout.y, 3.0F, callout.height},
            target_color);

  std::string instruction{
      hovered ? "RELEASE TO APPLY  ·  " : "DROP EFFECT HERE  ·  "};
  instruction += target_name.empty() ? "SELECTED LAYER" : target_name;
  ctx.push_clip(callout);
  ctx.label(gui::Rect{callout.x + 10.0F, callout.y,
                      std::max(callout.width - 14.0F, 0.0F), callout.height},
            instruction, hovered ? ctx.theme().text : ctx.theme().text_muted);
  ctx.pop_clip();
}

std::optional<EffectApplyRequest>
EffectsPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                   doc::NodeId selected_target,
                   std::string_view target_name) {
  std::optional<EffectApplyRequest> request;
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return request;
  }

  ctx.push_clip(bounds);
  ctx.panel(bounds, ctx.theme().panel);
  const gui::Rect heading{bounds.x, bounds.y, bounds.width, 24.0F};
  ctx.panel(heading, ctx.theme().header);
  // The heading states the layer an effect would land on. With nothing
  // selected it says nothing: an empty right-hand side is already the fact.
  if (selected_target.value() != 0U && !target_name.empty()) {
    ctx.label(gui::Rect{heading.x + 8.0F, heading.y,
                        std::max(heading.width - 16.0F, 0.0F), heading.height},
              target_name, ctx.theme().accent);
  }
  ctx.panel(gui::Rect{heading.x, heading.y + heading.height - 1.0F,
                      heading.width, 1.0F},
            ctx.theme().divider);

  constexpr float search_height = 27.0F;
  const gui::Rect search_row{bounds.x, heading.y + heading.height,
                             bounds.width, search_height};
  const gui::Rect search_bounds{search_row.x + 7.0F, search_row.y + 3.0F,
                                std::max(search_row.width - 14.0F, 0.0F),
                                search_row.height - 6.0F};
  const gui::UiId search_id = mix(id_, 0xEFFE'C75EU);
  static_cast<void>(ctx.text_field(search_id, search_bounds, search_));
  if (search_.empty() && ctx.focus() != search_id) {
    ctx.label(gui::Rect{search_bounds.x + 8.0F, search_bounds.y,
                        std::max(search_bounds.width - 12.0F, 0.0F),
                        search_bounds.height},
              "SEARCH EFFECTS", ctx.theme().text_muted);
  }
  ctx.panel(gui::Rect{search_row.x, search_row.y + search_row.height - 1.0F,
                      search_row.width, 1.0F},
            ctx.theme().divider);

  const gui::Rect list_bounds{
      bounds.x, search_row.y + search_row.height, bounds.width,
      std::max(bounds.height - heading.height - search_height, 0.0F)};
  constexpr float row_pitch = 47.0F;
  constexpr float group_height = 22.0F;
  constexpr float top_inset = 7.0F;
  std::array<bool, effects.size()> visible{};
  std::size_t visible_count{};
  std::size_t visible_groups{};
  std::string_view counted_group;
  for (std::size_t index = 0U; index < effects.size(); ++index) {
    const EffectDescription &effect = effects[index];
    visible[index] = matches(effect.name, search_) ||
                     matches(effect.description, search_) ||
                     matches(effect.group, search_);
    if (!visible[index]) {
      continue;
    }
    ++visible_count;
    if (effect.group != counted_group) {
      counted_group = effect.group;
      ++visible_groups;
    }
  }
  const float content_height = top_inset +
      static_cast<float>(visible_count) * row_pitch +
      static_cast<float>(visible_groups) * group_height + top_inset;
  const float maximum_scroll =
      std::max(content_height - list_bounds.height, 0.0F);
  if (list_bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      ctx.wheel() != 0.0F) {
    scroll_ -= ctx.wheel() * row_pitch * 2.0F;
  }
  scroll_ = std::clamp(scroll_, 0.0F, maximum_scroll);
  ctx.push_clip(list_bounds);
  float y = list_bounds.y + top_inset - scroll_;
  std::string_view active_group;
  for (std::size_t index = 0U; index < effects.size(); ++index) {
    const EffectDescription &effect = effects[index];
    if (!visible[index]) {
      continue;
    }
    if (effect.group != active_group) {
      active_group = effect.group;
      ctx.label(gui::Rect{bounds.x + 10.0F, y,
                          std::max(bounds.width - 20.0F, 0.0F),
                          group_height},
                active_group, effect.color);
      ctx.panel(gui::Rect{bounds.x + 68.0F,
                          y + group_height * 0.5F,
                          std::max(bounds.width - 78.0F, 0.0F), 1.0F},
                ctx.theme().divider);
      y += group_height;
    }
    const gui::Rect row{bounds.x + 7.0F, y,
                        std::max(bounds.width - 14.0F, 0.0F), 42.0F};
    const gui::Interaction interaction =
        ctx.grab(mix(id_, static_cast<core::u64>(index + 1U)), row);
    ctx.panel(row, interaction.held
                       ? ctx.theme().widget_active
                       : (interaction.hovered ? ctx.theme().widget_hover
                                              : ctx.theme().row));
    draw_effect_icon(ctx, gui::Rect{row.x + 5.0F, row.y + 7.0F, 28.0F, 28.0F},
                     effect);
    ctx.label(gui::Rect{row.x + 40.0F, row.y + 2.0F,
                        std::max(row.width - 46.0F, 0.0F), 20.0F},
              effect.name, ctx.theme().text);
    ctx.label(gui::Rect{row.x + 40.0F, row.y + 20.0F,
                        std::max(row.width - 46.0F, 0.0F), 18.0F},
              effect.description, ctx.theme().text_muted);
    if (interaction.pressed) {
      dragging_ = effect.kind;
      drag_start_x_ = ctx.pointer_x();
      drag_start_y_ = ctx.pointer_y();
      drag_moved_ = false;
    }
    if (interaction.held && dragging_.has_value()) {
      const float dx = ctx.pointer_x() - drag_start_x_;
      const float dy = ctx.pointer_y() - drag_start_y_;
      drag_moved_ = drag_moved_ || dx * dx + dy * dy >= 16.0F;
    }
    if (interaction.released) {
      if (dragging_.has_value() && drag_moved_) {
        request =
            EffectApplyRequest{selected_target, *dragging_, ctx.pointer_x(),
                               ctx.pointer_y()};
      }
      dragging_.reset();
      drag_moved_ = false;
    }
    y += row_pitch;
  }

  if (visible_count == 0U) {
    ctx.label(gui::Rect{list_bounds.x + 10.0F, list_bounds.y + 8.0F,
                        std::max(list_bounds.width - 20.0F, 0.0F), 22.0F},
              "NO EFFECTS FOUND", ctx.theme().text_muted);
  }

  ctx.pop_clip();
  ctx.pop_clip();

  if (dragging_.has_value()) {
    const EffectDescription *const effect = description_for(*dragging_);
    if (effect == nullptr) {
      dragging_.reset();
      return request;
    }
    const bool has_target =
        selected_target.value() != 0U && !target_name.empty();
    const gui::Rect ghost{ctx.pointer_x() + 12.0F, ctx.pointer_y() + 8.0F,
                          164.0F, has_target ? 38.0F : 24.0F};
    ctx.panel(ghost, ctx.theme().widget_active);
    ctx.panel(gui::Rect{ghost.x, ghost.y, 3.0F, ghost.height}, effect->color);
    ctx.label(gui::Rect{ghost.x + 9.0F, ghost.y,
                        std::max(ghost.width - 12.0F, 0.0F), 24.0F},
              effect->name, ctx.theme().text);
    if (has_target) {
      ctx.label(gui::Rect{ghost.x + 9.0F, ghost.y + 19.0F,
                          std::max(ghost.width - 12.0F, 0.0F), 17.0F},
                target_name, ctx.theme().accent);
    }
  }
  return request;
}

std::string_view EffectsPanel::dragging_name() const noexcept {
  if (!dragging_.has_value()) {
    return {};
  }
  const EffectDescription *const effect = description_for(*dragging_);
  return effect != nullptr ? effect->name : std::string_view{};
}

} // namespace mediaserver::editor
