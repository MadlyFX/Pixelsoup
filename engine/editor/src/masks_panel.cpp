#include <mediaserver/editor/masks_panel.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace mediaserver::editor {
namespace {

constexpr float title_height = 25.0F;
constexpr float tools_height = 28.0F;
constexpr float row_height = 26.0F;
constexpr float control_height = 24.0F;
constexpr float gap = 3.0F;
constexpr float maximum_list_height = 130.0F;
// Matches the Timeline's track headers: the first click selects a row, a
// second one inside this window opens its name.
constexpr double rename_click_seconds = 0.35;

[[nodiscard]] gui::UiId mix(gui::UiId base, std::string_view name,
                            core::u64 index = 0U) noexcept {
  core::u64 hash = base ^ 0x9e3779b97f4a7c15ULL;
  for (const char character : name) {
    hash ^= static_cast<core::u8>(character);
    hash *= 1099511628211ULL;
  }
  hash ^= index + 0x9e3779b9ULL;
  hash *= 1099511628211ULL;
  return hash;
}

void border(gui::UiContext &ctx, const gui::Rect &bounds,
            gui::UiColor color) {
  auto &draw = ctx.draw_list();
  draw.add_rect(bounds.x, bounds.y, bounds.width, 1.0F, color);
  draw.add_rect(bounds.x, bounds.y + bounds.height - 1.0F, bounds.width, 1.0F,
                color);
  draw.add_rect(bounds.x, bounds.y, 1.0F, bounds.height, color);
  draw.add_rect(bounds.x + bounds.width - 1.0F, bounds.y, 1.0F,
                bounds.height, color);
}

[[nodiscard]] bool action_button(gui::UiContext &ctx, gui::UiId id,
                                 const gui::Rect &bounds,
                                 std::string_view label, bool enabled,
                                 bool selected = false) {
  const gui::Interaction interaction =
      enabled ? ctx.grab(id, bounds) : gui::Interaction{};
  const gui::UiColor fill = !enabled
      ? ctx.theme().track
      : (interaction.held
             ? ctx.theme().widget_active
             : (selected ? ctx.theme().selection
                         : (interaction.hovered ? ctx.theme().widget_hover
                                                : ctx.theme().widget)));
  ctx.panel(bounds, fill);
  border(ctx, bounds, enabled ? ctx.theme().header : ctx.theme().divider);
  ctx.label(bounds, label, enabled ? ctx.theme().text
                                  : ctx.theme().text_muted);
  return enabled && interaction.clicked;
}

[[nodiscard]] std::string_view kind_label(scene::MaskKind kind) noexcept {
  switch (kind) {
  case scene::MaskKind::Rectangle:
    return "RECT";
  case scene::MaskKind::Ellipse:
    return "ELLIPSE";
  case scene::MaskKind::Bezier:
    return "BEZIER";
  }
  return "MASK";
}

[[nodiscard]] std::string_view combine_label(
    scene::MaskCombine combine) noexcept {
  switch (combine) {
  case scene::MaskCombine::Add:
    return "ADD";
  case scene::MaskCombine::Subtract:
    return "SUB";
  case scene::MaskCombine::Intersect:
    return "INT";
  case scene::MaskCombine::Difference:
    return "DIFF";
  }
  return "?";
}

[[nodiscard]] const scene::MaskInstance *find_mask(
    std::span<const scene::MaskInstance *const> masks,
    doc::NodeId id) noexcept {
  const auto found = std::find_if(
      masks.begin(), masks.end(),
      [id](const scene::MaskInstance *mask) noexcept {
        return mask->source == id;
      });
  return found == masks.end() ? nullptr : *found;
}

[[nodiscard]] const scene::MaskPoint *find_point(
    const scene::MaskInstance &mask, doc::NodeId id) noexcept {
  const auto found = std::find_if(
      mask.points.begin(), mask.points.end(),
      [id](const scene::MaskPoint &point) noexcept {
        return point.source == id;
      });
  return found == mask.points.end() ? nullptr : &*found;
}

[[nodiscard]] const scene::MaskDiagnostic *first_diagnostic(
    const scene::MaskInstance &mask,
    std::span<const scene::MaskDiagnostic> diagnostics) noexcept {
  for (const scene::MaskDiagnostic &diagnostic : diagnostics) {
    if (diagnostic.subject == mask.source) {
      return &diagnostic;
    }
    if (std::any_of(
            mask.points.begin(), mask.points.end(),
            [&diagnostic](const scene::MaskPoint &point) noexcept {
              return point.source == diagnostic.subject;
            })) {
      return &diagnostic;
    }
  }
  return nullptr;
}

[[nodiscard]] bool valid_name(std::string_view name) noexcept {
  return !name.empty() && name.size() <= scene::max_mask_name_bytes;
}

[[nodiscard]] gui::KeyMods current_modifiers(
    const gui::UiContext &ctx) noexcept {
  gui::KeyMods modifiers = gui::KeyMods::None;
  if (ctx.shift_held()) {
    modifiers = modifiers | gui::KeyMods::Shift;
  }
  if (ctx.ctrl_held()) {
    modifiers = modifiers | gui::KeyMods::Ctrl;
  }
  if (ctx.alt_held()) {
    modifiers = modifiers | gui::KeyMods::Alt;
  }
  return modifiers;
}

[[nodiscard]] scene::MaskPointGeometry insertion_geometry(
    const scene::MaskInstance &mask,
    std::optional<doc::NodeId> selected) noexcept {
  if (mask.points.empty()) {
    return scene::MaskPointGeometry{0.5F, 0.5F, 0.5F, 0.5F, 0.5F, 0.5F};
  }
  std::size_t left = mask.points.size() - 1U;
  if (selected.has_value()) {
    const auto found = std::find_if(
        mask.points.begin(), mask.points.end(),
        [selected](const scene::MaskPoint &point) noexcept {
          return point.source == *selected;
        });
    if (found != mask.points.end()) {
      left = static_cast<std::size_t>(found - mask.points.begin());
    }
  }
  const std::size_t right = (left + 1U) % mask.points.size();
  const float x = (mask.points[left].x + mask.points[right].x) * 0.5F;
  const float y = (mask.points[left].y + mask.points[right].y) * 0.5F;
  return scene::MaskPointGeometry{x, y, x, y, x, y};
}

} // namespace

std::string_view mask_diagnostic_message(
    scene::MaskDiagnosticKind kind) noexcept {
  switch (kind) {
  case scene::MaskDiagnosticKind::MissingSceneOwner:
    return "MISSING SCENE OWNER";
  case scene::MaskDiagnosticKind::InvalidSceneOwner:
    return "INVALID SCENE OWNER";
  case scene::MaskDiagnosticKind::MissingTarget:
    return "TARGET MISSING";
  case scene::MaskDiagnosticKind::MissingTargetSceneOwner:
    return "TARGET OWNER MISSING";
  case scene::MaskDiagnosticKind::CrossSceneTarget:
    return "TARGET IN ANOTHER SCENE";
  case scene::MaskDiagnosticKind::InvalidTarget:
    return "TARGET CANNOT BE MASKED";
  case scene::MaskDiagnosticKind::MissingMask:
    return "PARENT MASK MISSING";
  case scene::MaskDiagnosticKind::CrossSceneMask:
    return "POINT IN ANOTHER SCENE";
  case scene::MaskDiagnosticKind::MalformedPath:
    return "MALFORMED PATH";
  case scene::MaskDiagnosticKind::DuplicatePointOrder:
    return "DUPLICATE POINT ORDER";
  case scene::MaskDiagnosticKind::InvalidFirstCombine:
    return "FIRST ACTIVE MASK MUST ADD";
  case scene::MaskDiagnosticKind::MaskLimitExceeded:
    return "MASK LIMIT EXCEEDED";
  case scene::MaskDiagnosticKind::PointLimitExceeded:
    return "POINT LIMIT EXCEEDED";
  }
  return "MASK ERROR";
}

MasksPanelFrame MasksPanel::draw(
    gui::UiContext &ctx, const gui::Rect &bounds,
    const MasksPanelTarget &target,
    std::span<const scene::MaskInstance> masks,
    std::span<const scene::MaskDiagnostic> diagnostics,
    const MasksPanelSelection &selection, bool authoring_enabled) {
  MasksPanelFrame frame{};
  frame.selection = selection;
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return frame;
  }
  if (!authoring_enabled && number_field_.live()) {
    number_field_.cancel(ctx);
  }

  std::vector<const scene::MaskInstance *> rows;
  rows.reserve(masks.size());
  for (const scene::MaskInstance &mask : masks) {
    if (mask.scene == target.scene && mask.target == target.target) {
      rows.push_back(&mask);
    }
  }
  std::sort(rows.begin(), rows.end(),
            [](const scene::MaskInstance *left,
               const scene::MaskInstance *right) noexcept {
              return std::tie(left->order, left->source) <
                     std::tie(right->order, right->source);
            });

  if (frame.selection.scene != target.scene ||
      frame.selection.target != target.target) {
    frame.selection = MasksPanelSelection{target.scene, target.target, {}, {}};
    frame.selection_changed = true;
    number_field_.cancel(ctx);
    rename_node_ = {};
    click_node_ = {};
    drag_node_ = {};
  }
  if (rename_node_ != doc::NodeId{} &&
      (!authoring_enabled || find_mask(rows, rename_node_) == nullptr)) {
    rename_node_ = {};
  }
  if (!ctx.pointer_down()) {
    drag_node_ = {};
  }
  if (drag_node_ != doc::NodeId{} &&
      find_mask(rows, drag_node_) == nullptr) {
    drag_node_ = {};
  }
  const scene::MaskInstance *selected =
      find_mask(rows, frame.selection.mask);
  if (frame.selection.mask && selected == nullptr) {
    frame.selection.mask = {};
    frame.selection.point.reset();
    frame.selection_changed = true;
    number_field_.cancel(ctx);
  }
  if (selected != nullptr && frame.selection.point.has_value() &&
      find_point(*selected, *frame.selection.point) == nullptr) {
    frame.selection.point.reset();
    frame.selection_changed = true;
    number_field_.cancel(ctx);
  }

  ctx.push_clip(bounds);
  ctx.panel(bounds);
  const gui::Rect title{bounds.x, bounds.y, bounds.width,
                        std::min(bounds.height, title_height)};
  ctx.panel(title, ctx.theme().background);
  ctx.panel(gui::Rect{title.x, title.y + title.height - 1.0F,
                      title.width, 1.0F},
            ctx.theme().divider);
  const std::string title_text = std::string{"MASKS  /  "} +
      (target.coordinate_space == scene::MaskCoordinateSpace::AuthoredCanvas
           ? "CANVAS SPACE"
           : "SOURCE SPACE");
  ctx.label(gui::Rect{title.x + 7.0F, title.y,
                      std::max(title.width - 14.0F, 0.0F), title.height},
            title_text, ctx.theme().text);

  const gui::Rect tools{
      bounds.x, bounds.y + title_height, bounds.width,
      std::min(tools_height,
               std::max(bounds.height - title_height, 0.0F))};
  ctx.panel(tools, ctx.theme().background);
  const float tool_width = std::max((tools.width - gap * 4.0F) / 3.0F, 0.0F);
  const bool can_create = authoring_enabled && target.scene && target.target;
  const auto tool_bounds = [&](std::size_t index) noexcept {
    return gui::Rect{tools.x + gap +
                         static_cast<float>(index) * (tool_width + gap),
                     tools.y + 3.0F, tool_width,
                     std::max(tools.height - 6.0F, 0.0F)};
  };
  const auto create = [&](MaskCreateTool tool) {
    const gui::KeyMods modifiers = current_modifiers(ctx);
    frame.intent = MaskCreateIntent{
        target.scene, target.target, target.coordinate_space, tool,
        MaskCreateModifiers{
            gui::has_mod(modifiers, gui::KeyMods::Shift),
            gui::has_mod(modifiers, gui::KeyMods::Alt)}};
  };
  if (action_button(ctx, mix(id_, "mask.create.rectangle"), tool_bounds(0U),
                    "RECTANGLE", can_create)) {
    create(MaskCreateTool::Rectangle);
  }
  if (action_button(ctx, mix(id_, "mask.create.ellipse"), tool_bounds(1U),
                    "ELLIPSE", can_create)) {
    create(MaskCreateTool::Ellipse);
  }
  if (action_button(ctx, mix(id_, "mask.create.bezier"), tool_bounds(2U),
                    "PEN", can_create)) {
    create(MaskCreateTool::Bezier);
  }

  // Rows carry the stack, so the detail block below holds only what a mask is
  // and nothing that says what it is not: with nothing selected it reserves no
  // height at all and the list takes the panel.
  const float detail_reserve = selected != nullptr
      ? (selected->kind == scene::MaskKind::Bezier ? 197.0F : 108.0F)
      : 0.0F;
  const float below_tools = std::max(
      bounds.height - title_height - tools_height, 0.0F);
  const float list_height = std::clamp(
      below_tools - detail_reserve, std::min(row_height * 2.0F, below_tools),
      std::min(maximum_list_height, below_tools));
  const gui::Rect list{bounds.x, tools.y + tools.height,
                       bounds.width, list_height};
  const float content_height = row_height * static_cast<float>(rows.size());
  const float maximum_scroll = std::max(content_height - list.height, 0.0F);
  const bool pointer_in_list =
      list.contains(ctx.pointer_x(), ctx.pointer_y());
  if (pointer_in_list) {
    mask_scroll_ -= ctx.wheel() * row_height * 2.0F;
  }
  mask_scroll_ = std::clamp(mask_scroll_, 0.0F, maximum_scroll);

  ctx.push_clip(list);
  float row_y = list.y - mask_scroll_;
  // A mask dragged up or down the list reorders the stack. Each frame steps one
  // place toward the row under the pointer rather than jumping to an absolute
  // index, so the model keeps normalising order the one way it already does and
  // the whole drag stays one gesture.
  std::size_t hovered_index = rows.size();
  if (pointer_in_list && !rows.empty()) {
    const float local = ctx.pointer_y() - list.y + mask_scroll_;
    if (local >= 0.0F) {
      const auto candidate = static_cast<std::size_t>(local / row_height);
      if (candidate < rows.size()) {
        hovered_index = candidate;
      }
    }
  }
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const scene::MaskInstance &mask = *rows[index];
    const gui::Rect row{list.x, row_y, list.width, row_height};
    const bool is_selected = mask.source == frame.selection.mask;
    ctx.panel(row, is_selected
                       ? ctx.theme().selection
                       : (index % 2U == 0U ? ctx.theme().row
                                          : ctx.theme().row_alt));
    if (mask.source == drag_node_) {
      ctx.panel(gui::Rect{row.x, row.y, 2.0F, row.height}, ctx.theme().accent);
    }
    const gui::Rect enabled_bounds{row.x + 3.0F, row.y + 3.0F, 29.0F,
                                   row.height - 6.0F};
    if (action_button(ctx, mix(id_, "mask.enabled", mask.source.value()),
                      enabled_bounds, mask.enabled ? "ON" : "OFF",
                      authoring_enabled, mask.enabled)) {
      if (!is_selected) {
        frame.selection = MasksPanelSelection{
            target.scene, target.target, mask.source, {}};
        frame.selection_changed = true;
        number_field_.cancel(ctx);
        rename_node_ = {};
      }
      frame.intent = MaskEnabledIntent{
          target.scene, target.target, mask.source, !mask.enabled};
    }
    const gui::Rect body{row.x + 35.0F, row.y,
                         std::max(row.width - 35.0F, 0.0F), row.height};
    const float name_width = std::max(body.width * 0.42F, 0.0F);
    const gui::Rect name_bounds{body.x + 5.0F, body.y + 2.0F,
                                std::max(name_width - 8.0F, 0.0F),
                                std::max(body.height - 4.0F, 0.0F)};
    const bool renaming = rename_node_ == mask.source && authoring_enabled;
    // The name field owns its part of the row while it is open, so a row being
    // renamed is grabbed by the strip right of it - the same bargain the
    // Timeline strikes with a track header.
    const gui::Rect grab_bounds =
        renaming ? gui::Rect{body.x + name_width, body.y,
                             std::max(body.width - name_width, 0.0F),
                             body.height}
                 : body;
    const gui::Interaction interaction =
        pointer_in_list
            ? ctx.grab(mix(id_, "mask.row", mask.source.value()), grab_bounds)
            : gui::Interaction{};
    if (interaction.hovered && !is_selected) {
      ctx.panel(grab_bounds, ctx.theme().widget_hover);
    }
    if (renaming) {
      const gui::UiId name_id = mix(id_, "mask.name", mask.source.value());
      if (ctx.text_field(name_id, name_bounds, rename_draft_) &&
          valid_name(rename_draft_)) {
        frame.intent = MaskRenameIntent{
            target.scene, target.target, mask.source, rename_draft_};
      }
    } else {
      ctx.label(name_bounds, mask.name,
                is_selected ? ctx.theme().text : ctx.theme().text_muted);
    }
    ctx.label(gui::Rect{body.x + name_width, body.y, 56.0F, body.height},
              kind_label(mask.kind), ctx.theme().text_muted);
    ctx.label(gui::Rect{body.x + name_width + 56.0F, body.y, 38.0F,
                        body.height},
              combine_label(mask.combine), ctx.theme().text_muted);
    if (const scene::MaskDiagnostic *diagnostic =
            first_diagnostic(mask, diagnostics)) {
      ctx.label(gui::Rect{body.x + name_width + 94.0F, body.y,
                          std::max(body.width - name_width - 99.0F, 0.0F),
                          body.height},
                mask_diagnostic_message(diagnostic->kind),
                ctx.theme().warning);
    }
    if (interaction.pressed && authoring_enabled) {
      drag_node_ = mask.source;
    }
    if (interaction.clicked) {
      const double now = ctx.time_seconds();
      if (!is_selected) {
        frame.selection = MasksPanelSelection{
            target.scene, target.target, mask.source, {}};
        frame.selection_changed = true;
        number_field_.cancel(ctx);
        rename_node_ = {};
        click_node_ = mask.source;
        click_time_ = now;
      } else if (authoring_enabled && click_node_ == mask.source &&
                 now >= click_time_ &&
                 now - click_time_ <= rename_click_seconds) {
        // Second click on the row that is already selected opens its name,
        // exactly as a second click opens a track header's.
        rename_node_ = mask.source;
        rename_draft_ = mask.name;
        // The field is not drawn until the next frame, so the click that
        // opened it cannot also focus it. Say so rather than making the
        // operator click a third time.
        ctx.focus_text_field(mix(id_, "mask.name", mask.source.value()),
                             rename_draft_);
        click_node_ = {};
        click_time_ = 0.0;
      } else {
        click_node_ = mask.source;
        click_time_ = now;
      }
    }
    row_y += row_height;
  }
  if (drag_node_ != doc::NodeId{} && hovered_index < rows.size() &&
      rows[hovered_index]->source != drag_node_) {
    const auto dragged = std::find_if(
        rows.begin(), rows.end(),
        [this](const scene::MaskInstance *candidate) noexcept {
          return candidate->source == drag_node_;
        });
    if (dragged != rows.end()) {
      const auto from = static_cast<std::size_t>(dragged - rows.begin());
      frame.intent = MaskReorderIntent{
          target.scene, target.target, drag_node_,
          hovered_index < from ? from - 1U : from + 1U};
    }
  }
  ctx.pop_clip();

  selected = find_mask(rows, frame.selection.mask);
  const gui::Rect details{bounds.x, list.y + list.height, bounds.width,
                          std::max(bounds.y + bounds.height - list.y -
                                       list.height,
                                   0.0F)};
  ctx.panel(details, ctx.theme().background);
  ctx.panel(gui::Rect{details.x, details.y, details.width, 1.0F},
            ctx.theme().divider);
  if (selected == nullptr) {
    ctx.pop_clip();
    return frame;
  }

  float y = details.y + 4.0F;
  constexpr std::array combines{
      scene::MaskCombine::Add, scene::MaskCombine::Subtract,
      scene::MaskCombine::Intersect, scene::MaskCombine::Difference};
  const float toggle_width = 62.0F;
  // The row's own toggle is the mask's on/off state, so this block carries the
  // controls a row has no room for and never a second copy of one it has.
  const gui::Rect invert_button{details.x + gap, y, toggle_width,
                                control_height};
  if (action_button(ctx, mix(id_, "mask.invert"), invert_button, "INVERT",
                    authoring_enabled, selected->invert)) {
    frame.intent = MaskInvertIntent{target.scene, target.target,
                                    selected->source, !selected->invert};
  }
  const float combine_x = invert_button.x + invert_button.width + gap;
  const float combine_width = std::max(
      (details.x + details.width - combine_x - gap * 4.0F) / 4.0F, 0.0F);
  for (std::size_t index = 0U; index < combines.size(); ++index) {
    const gui::Rect button{
        combine_x + static_cast<float>(index) * (combine_width + gap), y,
        combine_width, control_height};
    if (action_button(ctx, mix(id_, "mask.combine", index), button,
                      combine_label(combines[index]), authoring_enabled,
                      selected->combine == combines[index])) {
      frame.intent = MaskCombineIntent{
          target.scene, target.target, selected->source, combines[index]};
    }
  }
  y += control_height + gap;

  const auto draw_number = [&](gui::UiId field_id, const gui::Rect &field_bounds,
                               std::string_view label, double value,
                               double speed, int decimals, double &out,
                               NumberFieldEdit &edit) {
    if (!authoring_enabled) {
      ctx.push_pointer_block();
    }
    const bool changed = number_field_.draw(
        ctx, field_id, field_bounds, label, value, speed, out, decimals, &edit);
    if (!authoring_enabled) {
      ctx.pop_pointer_block();
    }
    return authoring_enabled && changed && std::isfinite(out);
  };
  const float scalar_width =
      std::max((details.width - gap * 4.0F) / 3.0F, 0.0F);
  constexpr std::array scalar_properties{
      MaskScalarProperty::Opacity, MaskScalarProperty::FeatherCanvasPx,
      MaskScalarProperty::ExpansionCanvasPx};
  const std::array scalar_values{
      selected->opacity, selected->feather_canvas_px,
      selected->expansion_canvas_px};
  constexpr std::array<std::string_view, 3U> scalar_labels{
      "OPACITY", "FEATHER", "EXPAND"};
  constexpr std::array<double, 3U> scalar_speeds{0.005, 0.25, 0.25};
  for (std::size_t index = 0U; index < scalar_properties.size(); ++index) {
    const gui::Rect field{
        details.x + gap + static_cast<float>(index) * (scalar_width + gap), y,
        scalar_width, control_height};
    double value{};
    NumberFieldEdit edit{};
    if (draw_number(mix(id_, "mask.scalar", index), field,
                    scalar_labels[index], scalar_values[index],
                    scalar_speeds[index], index == 0U ? 3 : 1, value, edit)) {
      float canonical = static_cast<float>(value);
      if (scalar_properties[index] == MaskScalarProperty::Opacity) {
        canonical = std::clamp(canonical, 0.0F, 1.0F);
      } else if (scalar_properties[index] ==
                 MaskScalarProperty::FeatherCanvasPx) {
        canonical = std::clamp(canonical, 0.0F,
                               scene::max_mask_feather_canvas_px);
      } else {
        canonical = std::clamp(canonical,
                               -scene::max_mask_expansion_canvas_px,
                               scene::max_mask_expansion_canvas_px);
      }
      edit.value = static_cast<double>(canonical);
      frame.intent = MaskScalarEditIntent{
          target.scene, target.target, selected->source,
          scalar_properties[index], canonical, edit};
    }
  }
  y += control_height + gap;

  if (selected->kind != scene::MaskKind::Bezier) {
    constexpr std::array components{
        MaskBoundsComponent::Left, MaskBoundsComponent::Top,
        MaskBoundsComponent::Right, MaskBoundsComponent::Bottom};
    constexpr std::array<std::string_view, 4U> labels{"L", "T", "R", "B"};
    const std::array values{selected->bounds.left, selected->bounds.top,
                            selected->bounds.right,
                            selected->bounds.bottom};
    const float width = std::max((details.width - gap * 5.0F) / 4.0F, 0.0F);
    for (std::size_t index = 0U; index < components.size(); ++index) {
      const gui::Rect field{
          details.x + gap + static_cast<float>(index) * (width + gap), y,
          width, control_height};
      double value{};
      NumberFieldEdit edit{};
      if (draw_number(mix(id_, "mask.bounds", index), field, labels[index],
                      values[index], 0.005, 3, value, edit)) {
        scene::MaskBounds changed = selected->bounds;
        const float canonical =
            std::clamp(static_cast<float>(value), 0.0F, 1.0F);
        switch (components[index]) {
        case MaskBoundsComponent::Left:
          changed.left = canonical;
          break;
        case MaskBoundsComponent::Top:
          changed.top = canonical;
          break;
        case MaskBoundsComponent::Right:
          changed.right = canonical;
          break;
        case MaskBoundsComponent::Bottom:
          changed.bottom = canonical;
          break;
        }
        edit.value = static_cast<double>(canonical);
        frame.intent = MaskBoundsEditIntent{
            target.scene, target.target, selected->source, changed,
            components[index], edit};
      }
    }
    y += control_height + gap;
  } else {
    // A point is picked by clicking it on the Stage, which is where it is.
    // This row states which one that left selected and how many the path has;
    // it is a readout, not a strip of numbered buttons standing in for the
    // gesture that already exists.
    std::size_t selected_index{};
    bool has_selected_point{};
    if (frame.selection.point.has_value()) {
      const auto found = std::find_if(
          selected->points.begin(), selected->points.end(),
          [&frame](const scene::MaskPoint &point) noexcept {
            return point.source == *frame.selection.point;
          });
      if (found != selected->points.end()) {
        selected_index =
            static_cast<std::size_t>(found - selected->points.begin());
        has_selected_point = true;
      }
    }
    std::array<char, 48U> points_text{};
    const int points_length =
        has_selected_point
            ? std::snprintf(points_text.data(), points_text.size(),
                            "POINT %zu OF %zu", selected_index + 1U,
                            selected->points.size())
            : std::snprintf(points_text.data(), points_text.size(),
                            "%zu POINTS", selected->points.size());
    ctx.label(gui::Rect{details.x + gap, y,
                        std::max(details.width - gap * 2.0F, 0.0F),
                        control_height},
              std::string_view{points_text.data(),
                               static_cast<std::size_t>(
                                   std::max(points_length, 0))},
              has_selected_point ? ctx.theme().text : ctx.theme().text_muted);
    y += control_height + gap;

    const scene::MaskPoint *point = frame.selection.point.has_value()
        ? find_point(*selected, *frame.selection.point)
        : nullptr;
    if (point != nullptr) {
      constexpr std::array components{
          MaskPointComponent::X, MaskPointComponent::Y,
          MaskPointComponent::InX, MaskPointComponent::InY,
          MaskPointComponent::OutX, MaskPointComponent::OutY};
      constexpr std::array<std::string_view, 6U> labels{
          "X", "Y", "IN X", "IN Y", "OUT X", "OUT Y"};
      const std::array values{point->x, point->y, point->in_x, point->in_y,
                              point->out_x, point->out_y};
      const float width = std::max((details.width - gap * 3.0F) / 2.0F, 0.0F);
      for (std::size_t index = 0U; index < components.size(); ++index) {
        const std::size_t column = index % 2U;
        const std::size_t row_index = index / 2U;
        const gui::Rect field{
            details.x + gap + static_cast<float>(column) * (width + gap),
            y + static_cast<float>(row_index) * (control_height + gap), width,
            control_height};
        double value{};
        NumberFieldEdit edit{};
        if (draw_number(mix(id_, "mask.point-value", index), field,
                        labels[index], values[index], 0.005, 3, value, edit)) {
          const float canonical =
              std::clamp(static_cast<float>(value), 0.0F, 1.0F);
          scene::MaskPointPatch patch{};
          switch (components[index]) {
          case MaskPointComponent::X:
            patch.x = canonical;
            break;
          case MaskPointComponent::Y:
            patch.y = canonical;
            break;
          case MaskPointComponent::InX:
            patch.in_x = canonical;
            break;
          case MaskPointComponent::InY:
            patch.in_y = canonical;
            break;
          case MaskPointComponent::OutX:
            patch.out_x = canonical;
            break;
          case MaskPointComponent::OutY:
            patch.out_y = canonical;
            break;
          }
          edit.value = static_cast<double>(canonical);
          frame.intent = MaskPointEditIntent{
              target.scene, target.target, selected->source, point->source,
              std::move(patch), components[index], edit};
        }
      }
      y += 3.0F * (control_height + gap);
    }

    const float half = std::max((details.width - gap * 3.0F) * 0.5F, 0.0F);
    const gui::Rect insert_button{details.x + gap, y, half, control_height};
    const gui::Rect delete_point_button{insert_button.x + half + gap, y, half,
                                        control_height};
    if (action_button(ctx, mix(id_, "mask.point.insert"), insert_button,
                      "INSERT POINT", authoring_enabled &&
                          selected->points.size() <
                              scene::max_mask_points_per_mask)) {
      std::size_t insertion_index = selected->points.size();
      if (has_selected_point) {
        insertion_index = selected_index + 1U;
      }
      frame.intent = MaskPointInsertIntent{
          target.scene, target.target, selected->source, insertion_index,
          insertion_geometry(*selected, frame.selection.point)};
    }
    if (action_button(ctx, mix(id_, "mask.point.delete"), delete_point_button,
                      "DELETE POINT",
                      authoring_enabled && frame.selection.point.has_value() &&
                          selected->points.size() > 3U)) {
      frame.intent = MaskPointDeleteIntent{
          target.scene, target.target, selected->source,
          *frame.selection.point};
    }
    y += control_height + gap;
  }

  // A mask is reordered by dragging its row, so the only thing left down here
  // is the one action with nothing to drag.
  const float action_width =
      std::max((details.width - gap * 4.0F) / 3.0F, 0.0F);
  const gui::Rect delete_bounds{
      details.x + std::max(details.width - action_width - gap, 0.0F), y,
      action_width, control_height};
  if (action_button(ctx, mix(id_, "mask.delete"), delete_bounds, "DELETE",
                    authoring_enabled)) {
    frame.intent = MaskDeleteIntent{
        target.scene, target.target, selected->source};
  }

  ctx.pop_clip();
  return frame;
}

} // namespace mediaserver::editor
