// The Feed Mapping list column: window viewports, projection surfaces and
// the stage they are calibrated in.

#include "feed_mapping_frame.hpp"

#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/gui/shapes.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mediaserver::editor {

using namespace mapping;
using namespace mapping::feed;

void FeedMappingPanel::draw_viewport_list(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const gui::Rect &bounds = f.bounds;
  const scene::Canvas canvas = f.canvas;
  const output::FeedMap &feed_map = f.feed_map;
  const topology::HardwareMap *const hardware = f.hardware;
  FeedMappingFrame &frame = f.result;
  gui::Rect content = bounds;
  static_cast<void>(gui::cut_top(content, title_height));
  const gui::Rect list = gui::cut_left(content, feed_list_width(bounds));
  const gui::Rect work = content;
  ctx.panel(list, scale(ctx.theme().panel, 0.94F));
  {
    gui::Rect edge = list;
    ctx.panel(gui::cut_right(edge, 1.0F), ctx.theme().divider);
  }
  // Down the list column: buttons and rows 6px in from its sides, headings
  // 3px further in.
  gui::Metrics column_metrics = ctx.theme().metrics;
  column_metrics.inset = 6.0F;
  column_metrics.gap = 0.0F;
  f.list_column.emplace(list, column_metrics);
  gui::FormLayout &list_column = *f.list_column;
  ctx.label(gui::inset(list_column.next(feed_list_header_height, 0.0F), 3.0F, 0.0F),
            "WINDOW VIEWPORTS", ctx.theme().text_muted);

  const output::OutputMap *const selected_viewport =
      selected_map_node_.has_value()
          ? feed_map.map_node(*selected_map_node_)
          : nullptr;
  std::optional<core::u32> add_index;
  if (selected_viewport != nullptr) {
    add_index = selected_viewport->node_index;
  } else if (hardware != nullptr && !hardware->output_bindings.empty()) {
    add_index = hardware->output_bindings.front().feed_index;
  } else if (!feed_map.maps.empty()) {
    add_index = feed_map.maps.front().node_index;
  } else {
    add_index = 0U;
  }
  const bool below_limit = feed_map.maps.size() < output::max_output_maps;
  const bool can_add = canvas.width > 0U && canvas.height > 0U && below_limit;
  const gui::Rect add_bounds = list_column.next(
      feed_add_button_height, feed_add_button_step - feed_add_button_height);
  if (f.add_button(gui::ui_id(id_, "feed.add"), add_bounds,
                 !below_limit
                     ? std::string_view{"VIEWPORT LIMIT"}
                     : (can_add ? std::string_view{"+ ADD VIEWPORT"}
                                : std::string_view{"CANVAS REQUIRED"}),
                 can_add)) {
    const scene::Canvas window =
        feed_window_raster(*add_index, canvas, hardware);
    const scene::CanvasRect source =
        selected_viewport != nullptr
            ? selected_viewport->source
            : scene::CanvasRect{0U, 0U, canvas.width, canvas.height};
    scene::CanvasRect viewport =
        selected_viewport != nullptr ? selected_viewport->viewport
                                     : scene::whole(window);
    core::u32 rightmost = 0U;
    for (const output::OutputMap &map : feed_map.maps_for(*add_index)) {
      rightmost = std::max(rightmost, map.viewport.x + map.viewport.width);
    }
    if (rightmost <= window.width && viewport.width <= window.width - rightmost) {
      viewport.x = rightmost;
    }
    frame.add = FeedMapAddRequest{
        *add_index, source, viewport};
  }

  // A surface is assigned by dragging it out of the list onto the output it
  // drives. Both views of an output take the drop - its row here and its
  // rectangle in the work area - so the gesture ends wherever the operator is
  // already looking. Targets are collected as they are drawn and resolved once,
  // after the whole panel has laid out.
  if (surface_drag_node_ != doc::NodeId{} &&
      feed_map.surface_for(surface_drag_node_) == nullptr) {
    surface_drag_node_ = {};
    surface_drag_moved_ = false;
  }
  // The threshold is measured here rather than in the row loop below, so the
  // targets light up on the frame the drag starts moving and not the one after.
  if (surface_drag_node_ != doc::NodeId{} && ctx.pointer_down()) {
    const float dx = ctx.pointer_x() - surface_drag_start_x_;
    const float dy = ctx.pointer_y() - surface_drag_start_y_;
    surface_drag_moved_ = surface_drag_moved_ || dx * dx + dy * dy >= 16.0F;
  }
  const bool surface_drag_active =
      surface_drag_node_ != doc::NodeId{} && surface_drag_moved_;
  f.surface_drag_active = surface_drag_active;
  std::vector<std::pair<gui::Rect, doc::NodeId>> &surface_drop_targets =
      f.surface_drop_targets;
  surface_drop_targets.reserve(feed_map.maps.size() * 2U);

  // A viewport row dragged up or down its window's rows reorders it there,
  // which is also its compositing order. Only its own window's rows are
  // targets: moving between windows is a placement change, not a reorder.
  struct ViewportRow final {
    gui::Rect bounds;
    doc::NodeId node;
    core::u32 window{};
  };
  std::vector<ViewportRow> viewport_rows;
  viewport_rows.reserve(feed_map.maps.size());
  if (row_drag_node_ != doc::NodeId{} &&
      feed_map.map_node(row_drag_node_) == nullptr) {
    row_drag_node_ = {};
    row_drag_moved_ = false;
  }
  if (row_drag_node_ != doc::NodeId{} && ctx.pointer_down()) {
    row_drag_moved_ = row_drag_moved_ ||
                      std::abs(ctx.pointer_y() - row_drag_start_y_) >= 4.0F;
  }
  // F2 names whichever row was clicked last, as in every layer list.
  if (focused_ && !ctx.typing() && !rename_.active() &&
      ctx.key_pressed(gui::Key::Rename)) {
    if (const output::OutputMap *const map = feed_map.map_node(rename_row_)) {
      rename_.begin(ctx, map->node.value(),
                    feed_viewport_title(*map,
                                        feed_viewport_ordinal(feed_map, *map)));
    } else if (const output::ProjectionSurface *const surface =
                   feed_map.surface_for(rename_row_);
               surface != nullptr && !surface->locked) {
      rename_.begin(ctx, surface->node.value(), surface->name);
    }
  }

  for (std::size_t index = 0U; index < feed_map.maps.size(); ++index) {
    const output::OutputMap &map = feed_map.maps[index];
    const core::u32 viewport_ordinal =
        feed_viewport_ordinal(feed_map, map);
    const std::string title = feed_viewport_title(map, viewport_ordinal);
    const gui::Rect row =
        list_column.next(feed_row_height, feed_row_step - feed_row_height);
    // The row's name and details, then DUPLICATE and REMOVE at its end.
    const float action_width = std::min(26.0F, row.width * 0.25F);
    gui::Rect row_content = row;
    const gui::Rect remove = gui::cut_right(row_content, action_width);
    const gui::Rect duplicate = gui::cut_right(row_content, action_width);
    const bool renaming = rename_.editing(map.node.value());
    const gui::Interaction interaction =
        renaming ? gui::Interaction{}
                 : ctx.grab(gui::ui_id(id_, "feed.row", map.node.value()),
                            row_content);
    const gui::Interaction duplicate_interaction = ctx.grab(
        gui::ui_id(id_, "feed.row.duplicate", map.node.value()), duplicate);
    const gui::Interaction remove_interaction =
        ctx.grab(gui::ui_id(id_, "feed.row.remove", map.node.value()), remove);
    const bool selected = selected_map_node_ == map.node;
    ctx.panel(row,
              selected ? ctx.theme().selection
                       : (interaction.hovered || remove_interaction.hovered ||
                                  duplicate_interaction.hovered
                              ? ctx.theme().widget_hover
                              : ctx.theme().row));
    ctx.panel(row_stripe(row), feed_color(ctx.theme(), map.node_index));
    const gui::Rect title_bounds = gui::inset(
        row_content, gui::Padding{10.0F, 1.0F, 4.0F, row.height - 18.0F});
    if (renaming) {
      if (auto name = rename_.draw(ctx, title_bounds)) {
        frame.organize = FeedOrganizeRequest{FeedOrganizeAction::Rename,
                                             map.node, std::move(*name)};
      }
    } else {
      ctx.label(title_bounds, title, ctx.theme().text);
    }
    ctx.label(gui::inset(row_content,
                         gui::Padding{10.0F, 16.0F, 4.0F, row.height - 31.0F}),
              feed_row_details(map), ctx.theme().text_muted);
    ctx.tooltip(row_content, feed_row_tooltip(map, title, feed_map));
    ctx.panel(action_rule(duplicate, 6.0F), ctx.theme().divider);
    gui::draw_icon(ctx.draw_list(), gui::Icon::Duplicate,
                   duplicate.center_x(), duplicate.center_y(),
                   duplicate_interaction.hovered ? ctx.theme().text
                                                 : ctx.theme().text_muted);
    ctx.tooltip(duplicate, "Duplicate viewport");
    draw_remove(ctx, remove.center_x(), remove.center_y(),
                remove_interaction.hovered ? ctx.theme().warning
                                           : ctx.theme().text_muted);
    if (interaction.pressed) {
      row_drag_node_ = map.node;
      row_drag_start_y_ = ctx.pointer_y();
      row_drag_moved_ = false;
    }
    if (interaction.clicked) {
      selected_map_node_ = map.node;
      frame.selection = map.node;
      rename_row_ = map.node;
      if (map.projection == output::FeedProjection::PlanarSurface &&
          feed_map.surface_for(map.surface) != nullptr) {
        selected_surface_node_ = map.surface;
        frame.surface_selection = map.surface;
      }
      if (clicks_.double_click(map.node.value(), ctx.time_seconds())) {
        rename_.begin(ctx, map.node.value(), title);
      }
    }
    if (duplicate_interaction.clicked) {
      frame.organize =
          FeedOrganizeRequest{FeedOrganizeAction::Duplicate, map.node};
    }
    if (remove_interaction.clicked) {
      frame.remove = FeedMapDeleteRequest{map.node};
    }
    viewport_rows.push_back(ViewportRow{row, map.node, map.node_index});
    if (surface_drag_active) {
      surface_drop_targets.emplace_back(row, map.node);
      ctx.outline(row,
                  row.contains(ctx.pointer_x(), ctx.pointer_y())
                 ? ctx.theme().accent
                 : alpha(ctx.theme().accent, 0.35F),
                  row.contains(ctx.pointer_x(), ctx.pointer_y()) ? 2.0F : 1.0F);
    }
  }

  if (row_drag_node_ != doc::NodeId{}) {
    const output::OutputMap *const dragged = feed_map.map_node(row_drag_node_);
    if (row_drag_moved_ && dragged != nullptr) {
      for (const ViewportRow &target : viewport_rows) {
        if (target.window != dragged->node_index ||
            !target.bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
          continue;
        }
        const bool after =
            ctx.pointer_y() >= target.bounds.y + target.bounds.height * 0.5F;
        // Two thick, a pixel clear of the edge it lands on.
        ctx.panel(gui::rule_y(target.bounds,
                              after ? target.bounds.bottom() + 2.0F
                                    : target.bounds.y - 1.0F,
                              2.0F),
                  ctx.theme().accent);
        if (!ctx.pointer_down() && target.node != dragged->node) {
          std::vector<doc::NodeId> current;
          for (const output::OutputMap &map :
               feed_map.maps_for(dragged->node_index)) {
            current.push_back(map.node);
          }
          frame.organize = FeedOrganizeRequest{
              FeedOrganizeAction::Reorder, dragged->node, {},
              reordered(current, dragged->node, target.node, after)};
          if (frame.organize->order == current) {
            frame.organize.reset();
          }
        }
        break;
      }
    }
    if (!ctx.pointer_down()) {
      row_drag_node_ = {};
      row_drag_moved_ = false;
    }
  }
  f.content = content;
  f.work = work;
}

void FeedMappingPanel::draw_surface_list(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const scene::Canvas canvas = f.canvas;
  const output::FeedMap &feed_map = f.feed_map;
  FeedMappingFrame &frame = f.result;
  gui::FormLayout &list_column = *f.list_column;
  std::vector<std::pair<gui::Rect, doc::NodeId>> &surface_rows = f.surface_rows;
  list_column.space(7.0F);
  ctx.label(gui::inset(list_column.next(feed_details_label_height, 3.0F), 3.0F,
                       0.0F),
            "PROJECTION SURFACES", ctx.theme().text_muted);
  const bool can_add_surface =
      scene::valid(canvas) &&
      feed_map.surfaces.size() < output::max_projection_surfaces;
  if (f.add_button(gui::ui_id(id_, "feed.surface.add"),
                 list_column.next(feed_add_button_height,
                             feed_add_button_step - feed_add_button_height),
                 can_add_surface ? std::string_view{"+ ADD SURFACE"}
                                 : std::string_view{"SURFACE LIMIT"},
                 can_add_surface)) {
    frame.surface_add =
        FeedSurfaceAddRequest{default_surface_name(feed_map.surfaces.size()),
                              default_surface(canvas)};
  }
  if (f.add_button(gui::ui_id(id_, "feed.surface.import-mesh"),
                 list_column.next(feed_add_button_height,
                             feed_surface_row_step - feed_add_button_height),
                 can_add_surface ? std::string_view{"+ IMPORT UV MESH"}
                                 : std::string_view{"SURFACE LIMIT"},
                 can_add_surface)) {
    FeedSurfaceAddRequest request{
        default_mesh_surface_name(feed_map.surfaces.size()),
        default_mesh_surface(canvas),
        0U,
        output::ProjectionSurfaceShape::ImportedMesh,
        0.25F,
        32U,
        {},
        1.0F};
    request.assign_map_node = selected_map_node_;
    frame.surface_add = std::move(request);
  }
  // Collected for the drag resolution below: a surface dropped on another
  // surface row reorders the list instead of assigning it to an output.
  surface_rows.reserve(feed_map.surfaces.size());
  for (const output::ProjectionSurface &surface : feed_map.surfaces) {
    const gui::Rect row = list_column.next(
        feed_surface_row_height,
        feed_surface_row_step - feed_surface_row_height);
    // The name, then DUPLICATE, LOCK and REMOVE at the row's end.
    gui::Rect row_content = row;
    const gui::Rect remove = gui::cut_right(row_content, 26.0F);
    const gui::Rect lock = gui::cut_right(row_content, 26.0F);
    const gui::Rect duplicate = gui::cut_right(row_content, 26.0F);
    const bool renaming = rename_.editing(surface.node.value());
    const gui::Interaction interaction =
        surface.locked || renaming
            ? gui::Interaction{}
            : ctx.grab(gui::ui_id(id_, "feed.surface.row", surface.node.value()),
                       row_content);
    const gui::Interaction duplicate_interaction = ctx.grab(
        gui::ui_id(id_, "feed.surface.row.duplicate", surface.node.value()),
        duplicate);
    const gui::Interaction lock_interaction = ctx.grab(
        gui::ui_id(id_, "feed.surface.row.lock", surface.node.value()), lock);
    const gui::Interaction remove_interaction =
        surface.locked
            ? gui::Interaction{}
            : ctx.grab(gui::ui_id(id_, "feed.surface.row.remove", surface.node.value()),
                       remove);
    const bool selected = selected_surface_node_ == surface.node;
    ctx.panel(row,
              selected ? ctx.theme().selection
                       : (interaction.hovered || lock_interaction.hovered ||
                                  remove_interaction.hovered ||
                                  duplicate_interaction.hovered
                              ? ctx.theme().widget_hover
                              : ctx.theme().row));
    ctx.panel(row_stripe(row),
              selected ? ctx.theme().accent : ctx.theme().keyframe);
    const gui::Rect name_bounds =
        gui::inset(row_content, gui::Padding{10.0F, 0.0F, 4.0F, 0.0F});
    if (renaming) {
      if (auto name = rename_.draw(ctx, name_bounds)) {
        frame.organize = FeedOrganizeRequest{FeedOrganizeAction::Rename,
                                             surface.node, std::move(*name)};
      }
    } else {
      ctx.label(name_bounds,
                surface.name.empty() ? std::string_view{"UNTITLED SURFACE"}
                                     : std::string_view{surface.name},
                surface.locked
                    ? ctx.theme().text_muted
                    : (selected ? ctx.theme().text : ctx.theme().text_muted));
    }
    ctx.tooltip(row_content, surface.locked
                                 ? std::string_view{"Unlock to rename or move"}
                                 : std::string_view{surface.name});
    ctx.panel(action_rule(duplicate, 5.0F), ctx.theme().divider);
    gui::draw_icon(ctx.draw_list(), gui::Icon::Duplicate,
                   duplicate.center_x(), duplicate.center_y(),
                   duplicate_interaction.hovered ? ctx.theme().text
                                                 : ctx.theme().text_muted);
    ctx.tooltip(duplicate, "Duplicate surface");
    ctx.panel(action_rule(lock, 5.0F), ctx.theme().divider);
    gui::draw_padlock_glyph(ctx.draw_list(), lock.center_x(),
                            lock.center_y(), surface.locked,
                            surface.locked || lock_interaction.hovered
                                ? ctx.theme().warning
                                : ctx.theme().text_muted);
    ctx.tooltip(lock, surface.locked ? std::string_view{"Unlock surface"}
                                     : std::string_view{"Lock surface"});
    ctx.panel(action_rule(remove, 5.0F), ctx.theme().divider);
    draw_remove(ctx, remove.center_x(), remove.center_y(),
                remove_interaction.hovered ? ctx.theme().warning
                                           : ctx.theme().text_muted);
    if (interaction.pressed) {
      surface_drag_node_ = surface.node;
      surface_drag_start_x_ = ctx.pointer_x();
      surface_drag_start_y_ = ctx.pointer_y();
      surface_drag_moved_ = false;
    }
    if (interaction.clicked) {
      selected_surface_node_ = surface.node;
      frame.surface_selection = surface.node;
      rename_row_ = surface.node;
      if (clicks_.double_click(surface.node.value(), ctx.time_seconds())) {
        rename_.begin(ctx, surface.node.value(), surface.name);
      }
    }
    if (duplicate_interaction.clicked) {
      frame.organize =
          FeedOrganizeRequest{FeedOrganizeAction::Duplicate, surface.node};
    }
    if (lock_interaction.clicked) {
      frame.surface_lock =
          FeedSurfaceLockRequest{surface.node, !surface.locked};
      if (!surface.locked) {
        selected_surface_node_ = {};
      }
    }
    if (remove_interaction.clicked) {
      frame.surface_remove = FeedSurfaceDeleteRequest{surface.node};
    }
    surface_rows.emplace_back(row, surface.node);
  }
}

void FeedMappingPanel::draw_stage_setup(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const output::FeedMap &feed_map = f.feed_map;
  FeedMappingFrame &frame = f.result;
  gui::FormLayout &list_column = *f.list_column;
  // The stage the surfaces above are calibrated in. Project setup, like the
  // surfaces themselves, so it sits with them rather than with any one output.
  if (!stage_numbers_.live()) {
    stage_ = feed_map.stage;
  }
  const auto emit_stage = [&] {
    frame.stage_edit = FeedStageEditRequest{stage_, 0U};
  };
  list_column.space(7.0F);
  {
    gui::Rect heading = gui::inset(
        list_column.next(feed_details_label_height, 3.0F), 3.0F, 0.0F);
    const gui::Rect reset = gui::cut_right(heading, reset_button_width);
    ctx.label(heading, "Stage", ctx.theme().text_muted);
    if (stage_.anchor != output::StageAnchor::World &&
        reset_button(ctx, gui::ui_id(id_, "feed.stage.anchor.reset"), reset,
                     "Reset to fixed in the world")) {
      stage_.anchor = output::StageAnchor::World;
      emit_stage();
    }
    const gui::Rect choice = list_column.next(feed_add_button_height, 4.0F);
    if (pill_button(ctx, gui::ui_id(id_, "feed.stage.world"),
                    gui::column(choice, 0U, 2U, 3.0F), std::nullopt, "Fixed",
                    "Surfaces stand still in the world and the camera moves "
                    "among them, as in a virtual-production volume",
                    ButtonTone::Quiet, true,
                    stage_.anchor == output::StageAnchor::World) &&
        stage_.anchor != output::StageAnchor::World) {
      stage_.anchor = output::StageAnchor::World;
      emit_stage();
    }
    if (pill_button(ctx, gui::ui_id(id_, "feed.stage.camera"),
                    gui::column(choice, 1U, 2U, 3.0F), std::nullopt,
                    "Follows camera",
                    "The whole stage travels and turns with the scene camera: "
                    "an immersive room or CAVE whose camera motion is the "
                    "show. Surfaces are measured in the room, and tracked "
                    "camera plates are not drawn",
                    ButtonTone::Quiet, true,
                    stage_.anchor == output::StageAnchor::Camera) &&
        stage_.anchor != output::StageAnchor::Camera) {
      stage_.anchor = output::StageAnchor::Camera;
      emit_stage();
    }
    if (stage_.anchor == output::StageAnchor::Camera) {
      gui::Rect eye_heading = gui::inset(
          list_column.next(feed_details_label_height, 2.0F), 3.0F, 0.0F);
      const gui::Rect eye_reset = gui::cut_right(eye_heading, reset_button_width);
      ctx.label(eye_heading, "Viewer eye", ctx.theme().text_muted);
      ctx.tooltip(eye_heading,
                  "Where the viewer stands in the room, in stage metres. This "
                  "point rides on the scene camera's eye");
      if (stage_.eye != scene::Vec3{} &&
          reset_button(ctx, gui::ui_id(id_, "feed.stage.eye.reset"), eye_reset,
                       "Reset to the stage origin (0, 0, 0)")) {
        stage_.eye = scene::Vec3{};
        emit_stage();
      }
      const gui::Rect strip = list_column.next(22.0F, 4.0F);
      const std::array<std::pair<std::string_view, float *>, 3U> axes{{
          {"X", &stage_.eye.x},
          {"Y", &stage_.eye.y},
          {"Z", &stage_.eye.z},
      }};
      for (std::size_t axis = 0U; axis < axes.size(); ++axis) {
        double changed{};
        if (stage_numbers_.draw(ctx, gui::ui_id(id_, "feed.stage.eye", axis),
                                gui::column(strip, axis, axes.size(), 3.0F),
                                axes[axis].first,
                                static_cast<double>(*axes[axis].second), 0.01,
                                changed, 3)) {
          *axes[axis].second =
              static_cast<float>(std::clamp(changed, -1'000'000.0, 1'000'000.0));
          emit_stage();
        }
      }
    }
  }
  frame.editing = frame.editing || stage_numbers_.live();

  rename_.end_frame(ctx);
  f.sync_surface_details();
}

} // namespace mediaserver::editor
