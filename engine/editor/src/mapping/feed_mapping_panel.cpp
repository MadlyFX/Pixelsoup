#include "feed_mapping_frame.hpp"

#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/gui/shapes.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mediaserver::editor {

using namespace mapping;
using namespace mapping::feed;

gui::Rect fit_feed_canvas(const gui::Rect &area,
                          scene::Canvas canvas) noexcept {
  if (area.width <= 0.0F || area.height <= 0.0F || canvas.width == 0U ||
      canvas.height == 0U) {
    return gui::Rect{area.x + area.width * 0.5F, area.y + area.height * 0.5F,
                     0.0F, 0.0F};
  }
  const float scale_factor =
      std::min(area.width / static_cast<float>(canvas.width),
               area.height / static_cast<float>(canvas.height));
  const float width = static_cast<float>(canvas.width) * scale_factor;
  const float height = static_cast<float>(canvas.height) * scale_factor;
  return gui::Rect{area.x + (area.width - width) * 0.5F,
                   area.y + (area.height - height) * 0.5F, width, height};
}

gui::Rect feed_mapping_canvas(const gui::Rect &bounds,
                              scene::Canvas canvas) noexcept {
  const float list_width = feed_list_width(bounds);
  const float details_width = feed_details_width(bounds);
  const gui::Rect area{
      bounds.x + list_width + 22.0F, bounds.y + title_height + 30.0F,
      std::max(bounds.width - list_width - details_width - 44.0F, 0.0F),
      std::max(bounds.height - title_height - 132.0F, 0.0F)};
  return fit_feed_canvas(area, canvas);
}

gui::Rect feed_map_screen_rect(const gui::Rect &fitted_canvas,
                               scene::Canvas canvas,
                               scene::CanvasRect source) noexcept {
  if (canvas.width == 0U || canvas.height == 0U ||
      fitted_canvas.width <= 0.0F || fitted_canvas.height <= 0.0F) {
    return gui::Rect{fitted_canvas.x, fitted_canvas.y, 0.0F, 0.0F};
  }
  const float scale_x = fitted_canvas.width / static_cast<float>(canvas.width);
  const float scale_y =
      fitted_canvas.height / static_cast<float>(canvas.height);
  return gui::Rect{fitted_canvas.x + static_cast<float>(source.x) * scale_x,
                   fitted_canvas.y + static_cast<float>(source.y) * scale_y,
                   static_cast<float>(source.width) * scale_x,
                   static_cast<float>(source.height) * scale_y};
}

std::string feed_output_identity(core::u32 node_index) {
  std::array<char, 64U> text{};
  const int length = std::snprintf(
      text.data(), text.size(), "FEED %02u  FEED INDEX %u",
      node_index + 1U, node_index);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

std::string feed_runtime_binding(core::u32 node_index) {
  std::array<char, 96U> text{};
  const int length = std::snprintf(
      text.data(), text.size(),
      "ASSIGN IN HARDWARE MAP  |  FEED INDEX %u", node_index);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

void FeedMappingPanel::Frame::sync_surface_details() {
  FeedMappingPanel &p = panel;
    const output::ProjectionSurface *const surface =
        surface_or_null(feed_map, p.selected_surface_node_);
    const doc::NodeId next = surface != nullptr ? surface->node : doc::NodeId{};
    if (next == p.surface_details_node_) {
      if (surface != nullptr && surface->name != p.surface_name_ &&
          !p.surface_name_dirty_ &&
          ctx.focus() != gui::ui_id(p.id_, "feed.surface.name", next.value())) {
        p.surface_name_ = surface->name;
      }
      const auto camera_field = gui::ui_id(p.id_, "feed.surface.camera-inverse", next.value());
      if (surface != nullptr && surface->color != p.surface_color_observed_
          && !p.surface_numbers_.live() && ctx.focus() != camera_field) {
        // Follow undo/redo without reverting a local gesture while its command
        // is still travelling to the replicated document.
        p.surface_color_ = surface->color;
        p.surface_color_observed_ = surface->color;
      }
      // Native mesh browsing is resolved synchronously by EditorShell but the
      // resulting document reaches this value panel on a later result. Reconcile
      // that one externally-filled value without re-basing ordinary live
      // numeric scrubs on the lagging document.
      if (p.mesh_browse_pending_ && surface != nullptr &&
          surface->shape == output::ProjectionSurfaceShape::ImportedMesh &&
          !surface->mesh_source.empty() &&
          surface->mesh_source != p.surface_mesh_source_) {
        p.surface_name_ = surface->name;
        p.surface_geometry_ = surface->geometry;
        p.surface_shape_ = surface->shape;
        p.surface_horizontal_arc_turns_ = surface->horizontal_arc_turns;
        p.surface_mesh_columns_ = surface->mesh_columns;
        p.surface_mesh_source_ = surface->mesh_source;
        p.surface_mesh_scale_ = surface->mesh_scale;
        p.surface_details_show_frame_ = show_frame;
        p.surface_name_dirty_ = false;
        p.mesh_browse_pending_ = false;
      }
      if (surface != nullptr && show_frame != p.surface_details_show_frame_ &&
          !p.surface_numbers_.live()) {
        // The feed model supplies evaluated centre/yaw at the playhead. Follow
        // timeline motion while idle, but never re-base a live scrub on a
        // document value that may still be a few replicated frames behind.
        p.surface_geometry_.center = surface->geometry.center;
        p.surface_geometry_.right = surface->geometry.right;
        p.surface_geometry_.up = surface->geometry.up;
        p.surface_details_show_frame_ = show_frame;
      }
      return;
    }
    if (p.surface_details_node_ != doc::NodeId{} && p.surface_name_dirty_ &&
        !(p.surface_shape_ == output::ProjectionSurfaceShape::ImportedMesh &&
          p.surface_mesh_source_.empty())) {
      result.surface_edit = FeedSurfaceEditRequest{
          p.surface_details_node_, p.surface_name_,
          p.surface_geometry_,     0U,
          p.surface_shape_,        p.surface_horizontal_arc_turns_,
          p.surface_mesh_columns_, p.surface_mesh_source_,
          p.surface_mesh_scale_,   FeedSurfaceTransform::None};
    }
    p.surface_numbers_.cancel(ctx);
    ctx.clear_focus();
    p.surface_details_node_ = next;
    p.surface_details_show_frame_ = show_frame;
    p.surface_name_ = surface != nullptr ? surface->name : std::string{};
    p.surface_geometry_ =
        surface != nullptr ? surface->geometry : output::PhysicalSurface{};
    p.surface_shape_ = surface != nullptr
                         ? surface->shape
                         : output::ProjectionSurfaceShape::Planar;
    p.surface_horizontal_arc_turns_ =
        surface != nullptr ? surface->horizontal_arc_turns : 0.25F;
    p.surface_mesh_columns_ = surface != nullptr ? surface->mesh_columns : 32U;
    p.surface_mesh_source_ =
        surface != nullptr ? surface->mesh_source : std::string{};
    p.surface_mesh_scale_ = surface != nullptr ? surface->mesh_scale : 1.0F;
    p.surface_color_ = surface != nullptr ? surface->color : scene::WallColorSettings{};
    p.surface_color_observed_ = p.surface_color_;
    p.mesh_browse_pending_ = false;
    p.surface_name_dirty_ = false;
}

bool FeedMappingPanel::Frame::add_button(gui::UiId id, const gui::Rect &button,
                                         std::string_view label, bool enabled) {
  const gui::Interaction interaction =
      enabled ? ctx.grab(id, button) : gui::Interaction{};
  ctx.panel(button, enabled ? (interaction.hovered ? ctx.theme().widget_hover
                                                   : ctx.theme().widget)
                            : ctx.theme().track);
  ctx.outline(button, enabled && interaction.hovered ? ctx.theme().accent
                                                     : ctx.theme().divider);
  ctx.label(button, label,
            enabled ? ctx.theme().text : ctx.theme().text_muted);
  return interaction.clicked;
}

FeedMappingFrame FeedMappingPanel::draw(
    gui::UiContext &ctx, const gui::Rect &bounds, scene::Canvas canvas,
    const output::FeedMap &feed_map, core::u64 show_frame,
    const topology::HardwareMap *hardware,
    const topology::HardwareRuntimeSnapshot *runtime) {
  Frame f{*this,      ctx,      bounds,  canvas,
          feed_map,   show_frame, hardware, runtime};
  if (!begin_frame(f)) {
    return f.result;
  }
  draw_viewport_list(f);
  draw_surface_list(f);
  draw_stage_setup(f);
  draw_work_area(f);
  draw_rectangle_controls(f);
  draw_details(f);
  resolve_surface_drop(f);

  FeedMappingFrame &frame = f.result;
  frame.dragging = drag_kind_ != DragKind::None || f.surface_drag_active ||
                   (row_drag_node_ != doc::NodeId{} && row_drag_moved_);
  frame.editing = frame.editing || rename_.active();

  draw_build_notes(f);
  ctx.pop_clip();
  return frame;
}

bool FeedMappingPanel::begin_frame(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const gui::Rect &bounds = f.bounds;
  const output::FeedMap &feed_map = f.feed_map;
  const topology::HardwareMap *const hardware = f.hardware;
  const topology::HardwareRuntimeSnapshot *const runtime = f.runtime;
  FeedMappingFrame &frame = f.result;
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    rectangle_numbers_.cancel(ctx);
    return false;
  }
  if (ctx.pointer_pressed()) {
    focused_ = bounds.contains(ctx.pointer_x(), ctx.pointer_y());
  }
  if (selected_map_node_.has_value() &&
      feed_map.map_node(*selected_map_node_) == nullptr) {
    selected_map_node_.reset();
    drag_kind_ = DragKind::None;
    rectangle_numbers_.cancel(ctx);
  }
  if (selected_surface_node_.has_value() &&
      feed_map.surface_for(*selected_surface_node_) == nullptr) {
    selected_surface_node_.reset();
    surface_details_node_ = {};
    surface_details_show_frame_ = 0U;
    surface_name_.clear();
    surface_name_dirty_ = false;
    mesh_browse_pending_ = false;
    surface_numbers_.cancel(ctx);
  }
  if (!selected_surface_node_.has_value() && !feed_map.surfaces.empty()) {
    const output::OutputMap *const selected_map =
        selected_map_node_.has_value()
            ? feed_map.map_node(*selected_map_node_)
            : nullptr;
    const output::ProjectionSurface *const assigned =
        selected_map != nullptr && selected_map->projection ==
                                       output::FeedProjection::PlanarSurface
            ? feed_map.surface_for(selected_map->surface)
            : nullptr;
    selected_surface_node_ =
        assigned != nullptr ? assigned->node : feed_map.surfaces.front().node;
  }

  f.sync_surface_details();

  ctx.push_clip(bounds);
  ctx.panel(bounds);
  const bool valid_selection =
      selected_map_node_.has_value() &&
      feed_map.map_node(*selected_map_node_) != nullptr;
  title(ctx, bounds, "FEED MAPPING", {});
  {
    // Build from hardware sits at the right of the title, the status left
    // of it.
    gui::Rect header = bounds;
    header = gui::cut_top(header, title_height);
    const bool cabled =
        hardware != nullptr &&
        std::any_of(hardware->links.begin(), hardware->links.end(),
                    [&](const topology::HardwareLink &link) {
                      const topology::HardwareDevice *const from =
                          hardware->device(link.from.device);
                      return from != nullptr &&
                             from->kind ==
                                 topology::HardwareDeviceKind::RenderServer &&
                             link.from.port >=
                                 topology::first_render_server_video_port;
                    });
    const float build_width = std::min(176.0F, header.width * 0.42F);
    const gui::Rect build{header.right() - build_width - 5.0F, header.y + 3.0F,
                          build_width, title_height - 6.0F};
    if (!valid_selection) {
      const std::string_view status = feed_map.maps.empty()
                                          ? std::string_view{"NOT CONFIGURED"}
                                          : std::string_view{"CONFIGURED"};
      const float status_width = ctx.font_width(status);
      ctx.label(gui::Rect{build.x - status_width - 10.0F, header.y,
                          status_width, header.height},
                status, ctx.theme().text_muted);
    }
    if (pill_button(
            ctx, gui::ui_id(id_, "feed.build"), build, gui::Icon::Map,
            "Build from hardware",
            cabled ? "Lay out the canvas and the viewports of every cabled "
                     "window from Hardware Map and what its processors "
                     "report. Replaces those windows' viewports; one undo "
                     "puts them back."
                   : "Cable render outputs to a processor or a wall in "
                     "Hardware Map first",
            ButtonTone::Quiet, cabled)) {
      const topology::HardwareRuntimeSnapshot none{};
      output::FeedLayoutPlan plan = output::plan_feed_layout(
          *hardware, runtime != nullptr ? *runtime : none);
      build_notes_.clear();
      if (plan.empty()) {
        build_notes_.push_back("Nothing to build: no cabled output has a "
                               "window to lay out");
      } else {
        std::array<char, 112U> summary{};
        std::snprintf(summary.data(), summary.size(),
                      "Built a %ux%u canvas with %zu %s in %zu %s",
                      plan.canvas.width, plan.canvas.height,
                      plan.viewports.size(),
                      plan.viewports.size() == 1U ? "viewport" : "viewports",
                      plan.windows.size(),
                      plan.windows.size() == 1U ? "window" : "windows");
        build_notes_.emplace_back(summary.data());
      }
      build_notes_.insert(build_notes_.end(), plan.notes.begin(),
                          plan.notes.end());
      build_notes_until_ = ctx.time_seconds() + 12.0;
      if (!plan.empty()) {
        frame.layout = FeedLayoutRequest{std::move(plan), 0U};
      }
    }
  }
  if (focused_ && valid_selection && !ctx.typing() &&
      !surface_numbers_.live() && !rectangle_numbers_.live() &&
      ctx.key_pressed(gui::Key::Delete)) {
    frame.remove = FeedMapDeleteRequest{*selected_map_node_};
    frame.consume_delete = true;
  }
  return true;
}


bool detail::wait_for_feed_mapping_projection_cache_for_test(
    FeedMappingPanel &panel) noexcept {
  return render::detail::wait_for_output_projection_cache_for_test(
      panel.preview_projection_cache_);
}

} // namespace mediaserver::editor
