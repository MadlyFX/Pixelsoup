#include <mediaserver/editor/masks_panel.hpp>

#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/rect_cut.hpp>

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
// Space around and between the strip controls, and a row's text inset.
constexpr float gap = 3.0F;
constexpr float text_inset = 7.0F;
constexpr float maximum_list_height = 130.0F;
// Matches the Timeline's track headers: the first click selects a row, a
// second one inside this window opens its name.
constexpr double rename_click_seconds = 0.35;

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
  gui::Rect area = bounds;
  gui::Rect title = gui::cut_top(area, title_height);
  ctx.panel(title, ctx.theme().background);
  // A target with nothing flat to cut says so where the space would be named,
  // and the tooltip says what can be masked instead.
  const bool no_surface = target.target && !target.flat_surface;
  const std::string_view title_text =
      no_surface ? "NOTHING FLAT TO MASK"
      : target.coordinate_space == scene::MaskCoordinateSpace::AuthoredCanvas
          ? "CANVAS SPACE"
          : "SOURCE SPACE";
  ctx.label(gui::inset(title, text_inset, 0.0F), title_text,
            no_surface ? ctx.theme().warning : ctx.theme().text);
  if (no_surface) {
    // A group's masks still reach the renderer, which refuses the frame;
    // any other target's are dropped and the layer renders uncut.
    ctx.tooltip(title,
                target.coordinate_space ==
                        scene::MaskCoordinateSpace::AuthoredCanvas
                    ? "Masks cut a flat layer: a clip, quad or plane. A mask "
                      "here stops the render until it is turned off or "
                      "deleted."
                    : "Masks cut a flat layer: a clip, quad or plane. Masks "
                      "already here are not drawn; turn them off or delete "
                      "them.");
  }
  ctx.panel(gui::cut_bottom(title, 1.0F), ctx.theme().divider);

  const gui::Rect tools = gui::cut_top(area, tools_height);
  ctx.panel(tools, ctx.theme().background);
  const gui::Rect tool_strip = gui::inset(tools, gap);
  const bool can_create = authoring_enabled && target.scene && target.target &&
                          target.flat_surface;
  const auto tool_bounds = [&](std::size_t index) noexcept {
    return gui::column(tool_strip, index, 3U, gap);
  };
  const auto create = [&](MaskCreateTool tool) {
    const gui::KeyMods modifiers = current_modifiers(ctx);
    frame.intent = MaskCreateIntent{
        target.scene, target.target, target.coordinate_space, tool,
        MaskCreateModifiers{
            gui::has_mod(modifiers, gui::KeyMods::Shift),
            gui::has_mod(modifiers, gui::KeyMods::Alt)}};
  };
  if (action_button(ctx, gui::ui_id(id_, "mask.create.rectangle"), tool_bounds(0U),
                    "RECTANGLE", can_create)) {
    create(MaskCreateTool::Rectangle);
  }
  if (action_button(ctx, gui::ui_id(id_, "mask.create.ellipse"), tool_bounds(1U),
                    "ELLIPSE", can_create)) {
    create(MaskCreateTool::Ellipse);
  }
  if (action_button(ctx, gui::ui_id(id_, "mask.create.bezier"), tool_bounds(2U),
                    "PEN", can_create)) {
    create(MaskCreateTool::Bezier);
  }

  // Rows carry the stack, so the detail block below holds only what a mask is
  // and nothing that says what it is not: with nothing selected it reserves no
  // height at all and the list takes the panel.
  const float detail_reserve = selected != nullptr
      ? (selected->kind == scene::MaskKind::Bezier ? 170.0F : 81.0F)
      : 0.0F;
  const float list_height = std::clamp(
      area.height - detail_reserve, std::min(row_height * 2.0F, area.height),
      std::min(maximum_list_height, area.height));
  const gui::Rect list = gui::cut_top(area, list_height);
  const bool pointer_in_list =
      list.contains(ctx.pointer_x(), ctx.pointer_y());
  const float scroll = scroll_.begin(ctx, list, row_height * 2.0F);

  // A mask dragged up or down the list reorders the stack. Each frame steps one
  // place toward the row under the pointer rather than jumping to an absolute
  // index, so the model keeps normalising order the one way it already does and
  // the whole drag stays one gesture.
  std::size_t hovered_index = rows.size();
  if (pointer_in_list && !rows.empty()) {
    const float local = ctx.pointer_y() - list.y + scroll;
    if (local >= 0.0F) {
      const auto candidate = static_cast<std::size_t>(local / row_height);
      if (candidate < rows.size()) {
        hovered_index = candidate;
      }
    }
  }
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const scene::MaskInstance &mask = *rows[index];
    const gui::Rect row = gui::list_row(list, index, row_height, scroll);
    const bool is_selected = mask.source == frame.selection.mask;
    ctx.panel(row, is_selected
                       ? ctx.theme().selection
                       : (index % 2U == 0U ? ctx.theme().row
                                          : ctx.theme().row_alt));
    if (mask.source == drag_node_) {
      gui::Rect marker = row;
      ctx.panel(gui::cut_left(marker, 2.0F), ctx.theme().accent);
    }
    // The on/off toggle at the left, DELETE at the right, the mask's name,
    // kind, combine and any fault between them.
    gui::Rect controls = gui::inset(row, gap);
    constexpr float toggle_width = 29.0F;
    const gui::Rect enabled_bounds = gui::cut_left(controls, toggle_width);
    if (action_button(ctx, gui::ui_id(id_, "mask.enabled", mask.source.value()),
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
    // Destruction belongs to the mask row it affects. Keeping DELETE here
    // also means the operator never has to select one row and then verify a
    // distant footer before acting.
    constexpr float delete_width = 52.0F;
    const gui::Rect delete_bounds = gui::cut_right(controls, delete_width);
    if (action_button(ctx, gui::ui_id(id_, "mask.delete", mask.source.value()),
                      delete_bounds, "DELETE", authoring_enabled)) {
      frame.intent = MaskDeleteIntent{
          target.scene, target.target, mask.source};
    }
    const gui::Rect body = gui::inset(
        row, gui::Padding{gap * 2.0F + toggle_width, 0.0F,
                          gap + delete_width + 2.0F, 0.0F});
    gui::Rect columns = body;
    const gui::Rect name_column = gui::cut_left(columns, body.width * 0.42F);
    const gui::Rect name_bounds =
        gui::inset(name_column, gui::Padding{5.0F, 2.0F, 3.0F, 2.0F});
    const bool renaming = rename_node_ == mask.source && authoring_enabled;
    // The name field owns its part of the row while it is open, so a row being
    // renamed is grabbed by the strip right of it - the same bargain the
    // Timeline strikes with a track header.
    const gui::Rect grab_bounds = renaming ? columns : body;
    const gui::Interaction interaction =
        pointer_in_list
            ? ctx.grab(gui::ui_id(id_, "mask.row", mask.source.value()), grab_bounds)
            : gui::Interaction{};
    if (interaction.hovered && !is_selected) {
      ctx.panel(grab_bounds, ctx.theme().widget_hover);
    }
    if (renaming) {
      const gui::UiId name_id = gui::ui_id(id_, "mask.name", mask.source.value());
      if (ctx.text_field(name_id, name_bounds, rename_draft_) &&
          valid_name(rename_draft_)) {
        frame.intent = MaskRenameIntent{
            target.scene, target.target, mask.source, rename_draft_};
      }
    } else {
      ctx.label(name_bounds, mask.name,
                is_selected ? ctx.theme().text : ctx.theme().text_muted);
    }
    ctx.label(gui::cut_left(columns, 56.0F), kind_label(mask.kind),
              ctx.theme().text_muted);
    ctx.label(gui::cut_left(columns, 38.0F), combine_label(mask.combine),
              ctx.theme().text_muted);
    if (const scene::MaskDiagnostic *diagnostic =
            first_diagnostic(mask, diagnostics)) {
      ctx.label(gui::inset(columns, gui::Padding{0.0F, 0.0F, 5.0F, 0.0F}),
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
        ctx.focus_text_field(gui::ui_id(id_, "mask.name", mask.source.value()),
                             rename_draft_);
        click_node_ = {};
        click_time_ = 0.0;
      } else {
        click_node_ = mask.source;
        click_time_ = now;
      }
    }
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
  scroll_.end(ctx, row_height * static_cast<float>(rows.size()));

  selected = find_mask(rows, frame.selection.mask);
  const gui::Rect details = area;
  ctx.panel(details, ctx.theme().background);
  gui::Rect rule = details;
  ctx.panel(gui::cut_top(rule, 1.0F), ctx.theme().divider);
  if (selected == nullptr) {
    ctx.pop_clip();
    return frame;
  }

  // Rows of controls 3px apart and 3px in from the sides, starting 4px under
  // the rule.
  gui::Metrics m = ctx.theme().metrics;
  m.control = control_height;
  m.row_gap = gap;
  m.inset = gap;
  m.gap = 4.0F;
  gui::FormLayout form{details, m};
  constexpr std::array combines{
      scene::MaskCombine::Add, scene::MaskCombine::Subtract,
      scene::MaskCombine::Intersect, scene::MaskCombine::Difference};
  constexpr float invert_width = 62.0F;
  // The row's own toggle is the mask's on/off state, so this block carries the
  // controls a row has no room for and never a second copy of one it has.
  gui::Rect combine_strip = form.control();
  const gui::Rect invert_button = gui::cut_left(combine_strip, invert_width);
  static_cast<void>(gui::cut_left(combine_strip, gap));
  if (action_button(ctx, gui::ui_id(id_, "mask.invert"), invert_button, "INVERT",
                    authoring_enabled, selected->invert)) {
    frame.intent = MaskInvertIntent{target.scene, target.target,
                                    selected->source, !selected->invert};
  }
  for (std::size_t index = 0U; index < combines.size(); ++index) {
    if (action_button(ctx, gui::ui_id(id_, "mask.combine", index),
                      gui::column(combine_strip, index, combines.size(), gap),
                      combine_label(combines[index]), authoring_enabled,
                      selected->combine == combines[index])) {
      frame.intent = MaskCombineIntent{
          target.scene, target.target, selected->source, combines[index]};
    }
  }

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
  constexpr std::array scalar_properties{
      MaskScalarProperty::Opacity, MaskScalarProperty::FeatherCanvasPx,
      MaskScalarProperty::ExpansionCanvasPx};
  const std::array scalar_values{
      selected->opacity, selected->feather_canvas_px,
      selected->expansion_canvas_px};
  constexpr std::array<std::string_view, 3U> scalar_labels{
      "OPACITY", "FEATHER", "EXPAND"};
  constexpr std::array<double, 3U> scalar_speeds{0.005, 0.25, 0.25};
  const gui::Rect scalar_strip = form.control();
  for (std::size_t index = 0U; index < scalar_properties.size(); ++index) {
    const gui::Rect field =
        gui::column(scalar_strip, index, scalar_properties.size(), gap);
    double value{};
    NumberFieldEdit edit{};
    if (draw_number(gui::ui_id(id_, "mask.scalar", index), field,
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

  if (selected->kind != scene::MaskKind::Bezier) {
    constexpr std::array components{
        MaskBoundsComponent::Left, MaskBoundsComponent::Top,
        MaskBoundsComponent::Right, MaskBoundsComponent::Bottom};
    constexpr std::array<std::string_view, 4U> labels{"L", "T", "R", "B"};
    const std::array values{selected->bounds.left, selected->bounds.top,
                            selected->bounds.right,
                            selected->bounds.bottom};
    const gui::Rect bounds_strip = form.control();
    for (std::size_t index = 0U; index < components.size(); ++index) {
      const gui::Rect field =
          gui::column(bounds_strip, index, components.size(), gap);
      double value{};
      NumberFieldEdit edit{};
      if (draw_number(gui::ui_id(id_, "mask.bounds", index), field, labels[index],
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
    ctx.label(form.control(),
              std::string_view{points_text.data(),
                               static_cast<std::size_t>(
                                   std::max(points_length, 0))},
              has_selected_point ? ctx.theme().text : ctx.theme().text_muted);

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
      // Two to a row: the point, then its in and out handles.
      gui::Rect pair_strip{};
      for (std::size_t index = 0U; index < components.size(); ++index) {
        if (index % 2U == 0U) {
          pair_strip = form.control();
        }
        const gui::Rect field = gui::column(pair_strip, index % 2U, 2U, gap);
        double value{};
        NumberFieldEdit edit{};
        if (draw_number(gui::ui_id(id_, "mask.point-value", index), field,
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
    }

    const gui::Rect point_actions = form.control();
    if (action_button(ctx, gui::ui_id(id_, "mask.point.insert"),
                      gui::column(point_actions, 0U, 2U, gap),
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
    if (action_button(ctx, gui::ui_id(id_, "mask.point.delete"),
                      gui::column(point_actions, 1U, 2U, gap),
                      "DELETE POINT",
                      authoring_enabled && frame.selection.point.has_value() &&
                          selected->points.size() > 3U)) {
      frame.intent = MaskPointDeleteIntent{
          target.scene, target.target, selected->source,
          *frame.selection.point};
    }
  }

  ctx.pop_clip();
  return frame;
}

} // namespace mediaserver::editor
