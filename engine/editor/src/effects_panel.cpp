#include <mediaserver/editor/effects_panel.hpp>

#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/rect_cut.hpp>

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

struct EffectDescription final {
  scene::EffectKind kind;
  std::string_view group;
  std::string_view name;
  std::string_view description;
  gui::UiColor color{0.67F, 0.73F, 0.80F, 1.0F};
};

constexpr std::array<EffectDescription, 14U> effects{{
    {scene::EffectKind::Brightness, "COLOR", "Brightness", "Linear exposure multiplier"},
    {scene::EffectKind::Saturation, "COLOR", "Saturation", "Colour intensity amount"},
    {scene::EffectKind::Levels, "COLOR", "Levels", "Master and per-channel tonal range"},
    {scene::EffectKind::LiftGammaGain, "COLOR", "Lift / Gamma / Gain",
     "RGB shadows, midtones, and highlights"},
    {scene::EffectKind::ContrastPivot, "COLOR", "Contrast / Pivot",
     "Contrast around an explicit tonal pivot"},
    {scene::EffectKind::ColorMatrix, "COLOR", "Channel Mixer",
     "3x3 RGB colour matrix and offset"},
    {scene::EffectKind::WhiteBalance, "COLOR", "White Balance",
     "D65 temperature and tint adaptation"},
    {scene::EffectKind::Curves, "COLOR", "Curves",
     "Deterministic master and RGB tonal curves"},
    {scene::EffectKind::ChromaKey, "KEYING", "Chroma Key",
     "Colour-distance key with spill suppression"},
    {scene::EffectKind::LumaKey, "KEYING", "Luma Key",
     "Threshold key from source luminance"},
    {scene::EffectKind::Blur, "BLUR", "Gaussian Blur",
     "Smooth separable Gaussian kernel"},
    {scene::EffectKind::BoxBlur, "BLUR", "Box Blur", "Exact integer-radius box passes"},
    {scene::EffectKind::DirectionalBlur, "BLUR", "Directional Blur",
     "Centred line blur by length and angle"},
    {scene::EffectKind::FilmGrain, "TEXTURE", "Film Grain",
     "Frame-stable canvas-space texture"},
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
  const bool hovered =
      bounds.contains(ctx.pointer_x(), ctx.pointer_y());
  std::string instruction{
      hovered ? "RELEASE TO APPLY  ·  " : "DROP EFFECT HERE  ·  "};
  instruction += target_name.empty() ? "SELECTED LAYER" : target_name;
  draw_drop_target(ctx, bounds, instruction, hovered, 320.0F);
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
  gui::Rect area = bounds;
  gui::Rect heading = gui::cut_top(area, 24.0F);
  ctx.panel(heading, ctx.theme().header);
  // The heading states the layer an effect would land on. With nothing
  // selected it says nothing: an empty right-hand side is already the fact.
  if (selected_target.value() != 0U && !target_name.empty()) {
    ctx.label(gui::inset(heading, 8.0F, 0.0F), target_name,
              ctx.theme().accent);
  }
  ctx.panel(gui::cut_bottom(heading, 1.0F), ctx.theme().divider);

  constexpr float search_height = 27.0F;
  gui::Rect search_row = gui::cut_top(area, search_height);
  const gui::Rect search_bounds = gui::inset(search_row, 7.0F, 3.0F);
  const gui::UiId search_id = mix(id_, 0xEFFE'C75EU);
  static_cast<void>(ctx.text_field(search_id, search_bounds, search_));
  if (search_.empty() && ctx.focus() != search_id) {
    ctx.label(gui::inset(search_bounds, gui::Padding{8.0F, 0.0F, 4.0F, 0.0F}),
              "SEARCH EFFECTS", ctx.theme().text_muted);
  }
  ctx.panel(gui::cut_bottom(search_row, 1.0F), ctx.theme().divider);

  const gui::Rect list_bounds = area;
  // Cards 42px tall and 5px apart under 22px group headings, 7px in from the
  // panel's sides and ends.
  constexpr float card_height = 42.0F;
  constexpr float card_gap = 5.0F;
  constexpr float group_height = 22.0F;
  gui::Metrics m = ctx.theme().metrics;
  m.inset = 7.0F;
  m.gap = 7.0F;
  std::array<bool, effects.size()> visible{};
  std::size_t visible_count{};
  for (std::size_t index = 0U; index < effects.size(); ++index) {
    const EffectDescription &effect = effects[index];
    visible[index] = matches(effect.name, search_) ||
                     matches(effect.description, search_) ||
                     matches(effect.group, search_);
    if (visible[index]) {
      ++visible_count;
    }
  }
  gui::FormLayout list{
      list_bounds, m,
      scroll_.begin(ctx, list_bounds, (card_height + card_gap) * 2.0F)};
  std::string_view active_group;
  for (std::size_t index = 0U; index < effects.size(); ++index) {
    const EffectDescription &effect = effects[index];
    if (!visible[index]) {
      continue;
    }
    if (effect.group != active_group) {
      active_group = effect.group;
      // The group's name, then a rule running out to the right.
      const gui::Rect group = list.next(group_height, 0.0F);
      ctx.label(gui::inset(group, 3.0F, 0.0F), active_group, effect.color);
      const gui::Rect rule =
          gui::inset(group, gui::Padding{61.0F, 0.0F, 3.0F, 0.0F});
      ctx.panel(gui::Rect{rule.x, rule.center_y(), rule.width, 1.0F},
                ctx.theme().divider);
    }
    const gui::Rect row = list.next(card_height, card_gap);
    const gui::Interaction interaction =
        ctx.grab(mix(id_, static_cast<core::u64>(index + 1U)), row);
    ctx.panel(row, interaction.held
                       ? ctx.theme().widget_active
                       : (interaction.hovered ? ctx.theme().widget_hover
                                              : ctx.theme().row));
    // The icon, then the name over its description.
    constexpr float icon_size = 28.0F;
    gui::Rect text = gui::inset(row, gui::Padding{5.0F, 0.0F, 6.0F, 0.0F});
    draw_effect_icon(
        ctx, gui::centered(gui::cut_left(text, icon_size), icon_size, icon_size),
        effect);
    static_cast<void>(gui::cut_left(text, 7.0F));
    ctx.label(gui::inset(text, gui::Padding{0.0F, 2.0F, 0.0F, 20.0F}),
              effect.name, ctx.theme().text);
    ctx.label(gui::inset(text, gui::Padding{0.0F, 20.0F, 0.0F, 4.0F}),
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
  }

  if (visible_count == 0U) {
    gui::Rect empty = gui::inset(list_bounds, gui::Padding{10.0F, 8.0F, 10.0F, 0.0F});
    ctx.label(gui::cut_top(empty, 22.0F), "NO EFFECTS FOUND",
              ctx.theme().text_muted);
  }

  scroll_.end(ctx, list.extent());
  ctx.pop_clip();

  if (dragging_.has_value()) {
    const EffectDescription *const effect = description_for(*dragging_);
    if (effect == nullptr) {
      dragging_.reset();
      return request;
    }
    const bool has_target =
        selected_target.value() != 0U && !target_name.empty();
    // Behind the pointer rather than under it, so the drop target the
    // pointer is over stays readable.
    const gui::Rect ghost{ctx.pointer_x() + 12.0F, ctx.pointer_y() + 8.0F,
                          164.0F, has_target ? 38.0F : 24.0F};
    ctx.panel(ghost, ctx.theme().widget_active);
    gui::Rect text = ghost;
    ctx.panel(gui::cut_left(text, 3.0F), effect->color);
    text = gui::inset(text, gui::Padding{6.0F, 0.0F, 3.0F, 0.0F});
    ctx.label(gui::inset(text, gui::Padding{0.0F, 0.0F, 0.0F, ghost.height - 24.0F}),
              effect->name, ctx.theme().text);
    if (has_target) {
      ctx.label(gui::inset(text, gui::Padding{0.0F, 19.0F, 0.0F, 2.0F}),
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

std::optional<scene::EffectKind> draw_effect_picker(
    gui::UiContext& ctx, gui::UiId id, const gui::Rect& bounds) {
  if (ctx.button(id, bounds, "Add effect..."))
    ctx.open_context_menu(id, bounds.x, bounds.y + bounds.height);
  static constexpr auto names = [] {
    std::array<std::string_view, effects.size()> result{};
    for (std::size_t index = 0; index < effects.size(); ++index)
      result[index] = effects[index].name;
    return result;
  }();
  if (const auto chosen = ctx.context_menu(id, names))
    return effects[*chosen].kind;
  return std::nullopt;
}

} // namespace mediaserver::editor
