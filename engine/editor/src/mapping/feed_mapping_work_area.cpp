// The Feed Mapping work area: output windows and their viewports drawn
// over the canvas, the selected viewport's rectangle fields, a dragged
// surface's drop, and the notes the last Build from hardware left.

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

void FeedMappingPanel::draw_work_area(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const gui::Rect &bounds = f.bounds;
  const scene::Canvas canvas = f.canvas;
  const output::FeedMap &feed_map = f.feed_map;
  const topology::HardwareMap *const hardware = f.hardware;
  FeedMappingFrame &frame = f.result;
  const gui::Rect &work = f.work;
  const bool surface_drag_active = f.surface_drag_active;
  std::vector<std::pair<gui::Rect, doc::NodeId>> &surface_drop_targets =
      f.surface_drop_targets;
  ctx.panel(work, scale(ctx.theme().background, 0.76F));
  const output::OutputMap *const selected_map =
      selected_map_node_.has_value() ? feed_map.map_node(*selected_map_node_)
                                     : nullptr;
  const output::ProjectionSurface *const mapping_surface =
      selected_map != nullptr &&
              selected_map->projection == output::FeedProjection::PlanarSurface
          ? feed_map.surface_for(selected_map->surface)
          : nullptr;
  const bool uv_mesh_domain =
      mapping_surface != nullptr &&
      mapping_surface->shape == output::ProjectionSurfaceShape::ImportedMesh;
  // This is a destination-UV preview: stage origin, orientation, and uniform
  // scale cannot change it. Observation/admission is nonblocking; an exact-key
  // stale mesh remains visible while an explicit replacement loads.
  preview_mesh_ = uv_mesh_domain && selected_map != nullptr
                      ? preview_projection_cache_.resolve_mesh(
                            uv_preview_map(*selected_map, *mapping_surface))
                      : std::shared_ptr<const output::ProjectionMesh>{};
  const std::vector<FeedWindowLayout> windows =
      feed_window_layouts(feed_map, canvas, hardware);
  scene::Canvas mapping_canvas{};
  for (const FeedWindowLayout &window : windows) {
    const core::u64 right = static_cast<core::u64>(window.offset_x) +
                            window.raster.width;
    mapping_canvas.width = static_cast<core::u32>(
        std::min<core::u64>(std::max<core::u64>(mapping_canvas.width, right),
                            UINT32_MAX));
    mapping_canvas.height = std::max(mapping_canvas.height,
                                     window.raster.height);
  }
  const gui::Rect fitted = feed_mapping_canvas(bounds, mapping_canvas);
  if (mapping_canvas.width > 0U && mapping_canvas.height > 0U &&
      fitted.width > 0.0F && fitted.height > 0.0F) {
    const float scale_factor =
        fitted.width / static_cast<float>(mapping_canvas.width);
    ctx.panel(fitted, gui::UiColor{0.012F, 0.016F, 0.025F, 1.0F});
    for (const FeedWindowLayout &window : windows) {
      const scene::CanvasRect window_rect{window.offset_x, 0U,
                                          window.raster.width,
                                          window.raster.height};
      const gui::Rect screen =
          feed_map_screen_rect(fitted, mapping_canvas, window_rect);
      ctx.panel(screen, gui::UiColor{0.018F, 0.023F, 0.034F, 1.0F});
      for (core::u32 division = 1U; division < 4U; ++division) {
        const float fraction = static_cast<float>(division) / 4.0F;
        ctx.draw_list().add_line(
            screen.x + screen.width * fraction, screen.y,
            screen.x + screen.width * fraction, screen.bottom(),
            division == 2U ? 1.0F : 0.6F,
            alpha(ctx.theme().widget, division == 2U ? 0.64F : 0.34F));
        ctx.draw_list().add_line(
            screen.x, screen.y + screen.height * fraction,
            screen.right(), screen.y + screen.height * fraction,
            division == 2U ? 1.0F : 0.6F,
            alpha(ctx.theme().widget, division == 2U ? 0.64F : 0.34F));
      }
      ctx.outline(screen, ctx.theme().widget_hover);
    }

    for (const output::OutputMap &map : feed_map.maps) {
      const core::u32 viewport_ordinal =
          feed_viewport_ordinal(feed_map, map);
      const FeedWindowLayout *const window =
          feed_window_layout_for(windows, map.node_index);
      if (window == nullptr) {
        continue;
      }
      scene::CanvasRect viewport = map.viewport;
      if (drag_kind_ != DragKind::None && drag_node_ == map.node) {
        viewport = preview_;
      }
      const scene::CanvasRect placed{
          window->offset_x + viewport.x, viewport.y, viewport.width,
          viewport.height};
      gui::Rect rectangle =
          feed_map_screen_rect(fitted, mapping_canvas, placed);
      const bool selected = selected_map_node_ == map.node;

      // The resize handle gets first refusal over the rectangle body.
      const gui::Rect handle{rectangle.right() - 6.0F,
                             rectangle.bottom() - 6.0F, 12.0F,
                             12.0F};
      gui::Interaction handle_interaction{};
      if (selected) {
        handle_interaction =
            ctx.grab(gui::ui_id(id_, "feed.resize", map.node.value()), handle);
        if (handle_interaction.pressed) {
          drag_kind_ = DragKind::Resize;
          drag_node_ = map.node;
          drag_original_ = map.viewport;
          preview_ = drag_original_;
          drag_start_x_ = ctx.pointer_x();
          drag_start_y_ = ctx.pointer_y();
        }
        if (handle_interaction.held && drag_kind_ == DragKind::Resize &&
            drag_node_ == map.node) {
          const core::i64 dx = static_cast<core::i64>(
              std::llround((ctx.pointer_x() - drag_start_x_) / scale_factor));
          const core::i64 dy = static_cast<core::i64>(
              std::llround((ctx.pointer_y() - drag_start_y_) / scale_factor));
          preview_ = resize_feed_rect(drag_original_, window->raster, dx, dy);
          viewport = preview_;
          rectangle = feed_map_screen_rect(
              fitted, mapping_canvas,
              scene::CanvasRect{window->offset_x + viewport.x, viewport.y,
                                viewport.width, viewport.height});
        }
        if (handle_interaction.released && drag_kind_ == DragKind::Resize &&
            drag_node_ == map.node) {
          if (preview_ != drag_original_) {
            frame.edit = FeedMapRectEdit{map.node, preview_};
          }
          drag_kind_ = DragKind::None;
        }
      }

      const gui::Interaction body_interaction =
          ctx.grab(gui::ui_id(id_, "feed.rectangle", map.node.value()), rectangle);
      if (body_interaction.pressed) {
        selected_map_node_ = map.node;
        frame.selection = map.node;
        drag_kind_ = DragKind::Move;
        drag_node_ = map.node;
        drag_original_ = map.viewport;
        preview_ = drag_original_;
        drag_start_x_ = ctx.pointer_x();
        drag_start_y_ = ctx.pointer_y();
      }
      if (body_interaction.held && drag_kind_ == DragKind::Move &&
          drag_node_ == map.node) {
        const core::i64 dx = static_cast<core::i64>(
            std::llround((ctx.pointer_x() - drag_start_x_) / scale_factor));
        const core::i64 dy = static_cast<core::i64>(
            std::llround((ctx.pointer_y() - drag_start_y_) / scale_factor));
        preview_ = move_feed_rect(drag_original_, window->raster, dx, dy);
        viewport = preview_;
        rectangle = feed_map_screen_rect(
            fitted, mapping_canvas,
            scene::CanvasRect{window->offset_x + viewport.x, viewport.y,
                              viewport.width, viewport.height});
      }
      if (body_interaction.released && drag_kind_ == DragKind::Move &&
          drag_node_ == map.node) {
        if (preview_ != drag_original_) {
          frame.edit = FeedMapRectEdit{map.node, preview_};
        }
        drag_kind_ = DragKind::None;
      }

      const gui::UiColor color = feed_color(ctx.theme(), map.node_index);
      ctx.panel(rectangle, alpha(color, selected ? 0.30F : 0.19F));
      ctx.outline(rectangle, selected ? ctx.theme().accent : color,
                  selected ? 2.0F : 1.0F);
      if (surface_drag_active) {
        const bool under_pointer =
            rectangle.contains(ctx.pointer_x(), ctx.pointer_y());
        surface_drop_targets.emplace_back(rectangle, map.node);
        ctx.panel(rectangle,
                  alpha(ctx.theme().accent, under_pointer ? 0.26F : 0.10F));
        ctx.outline(rectangle,
                    under_pointer ? ctx.theme().accent
                             : alpha(ctx.theme().accent, 0.45F),
                    under_pointer ? 2.0F : 1.0F);
      }
      // A title band across its top, inside the outline.
      gui::Rect title_band = gui::inset(rectangle, 1.0F);
      title_band.height = 18.0F;
      ctx.panel(title_band, alpha(color, selected ? 0.78F : 0.56F));
      ctx.label(gui::inset(title_band, gui::Padding{6.0F, 0.0F, 6.0F, 1.0F}),
                map.name.empty() ? feed_viewport_short_name(viewport_ordinal)
                                 : map.name,
                ctx.theme().text);
      ctx.tooltip(rectangle,
                  feed_row_tooltip(map,
                                   feed_viewport_title(map, viewport_ordinal),
                                   feed_map));
      if (selected) {
        const gui::Rect current_handle =
            gui::around(rectangle.right(), rectangle.bottom(), 10.0F, 10.0F);
        ctx.panel(current_handle, ctx.theme().accent);
        ctx.outline(current_handle, ctx.theme().background);
      }
    }

    if (uv_mesh_domain && preview_mesh_ != nullptr && selected_map != nullptr) {
      const output::ProjectionMesh &mesh = *preview_mesh_;
      const std::size_t triangle_count = mesh.indices.size() / 3U;
      constexpr std::size_t max_preview_triangles = 1'024U;
      const std::size_t drawn = std::min(triangle_count, max_preview_triangles);
      gui::Rect viewport_screen{};
      for (const FeedWindowLayout &window : windows) {
        if (window.node_index != selected_map->node_index) {
          continue;
        }
        const scene::CanvasRect viewport = selected_map->viewport;
        viewport_screen = feed_map_screen_rect(
            fitted, mapping_canvas,
            scene::CanvasRect{window.offset_x + viewport.x, viewport.y,
                              viewport.width, viewport.height});
        break;
      }
      const auto screen = [&viewport_screen](
                              const output::ProjectionMeshVertex &vertex) {
        return IsoPoint{
            viewport_screen.x + vertex.destination_u * viewport_screen.width,
            viewport_screen.y + vertex.destination_v * viewport_screen.height};
      };
      for (std::size_t preview_index = 0U; preview_index < drawn;
           ++preview_index) {
        const std::size_t triangle = preview_index * triangle_count / drawn;
        const std::size_t first = triangle * 3U;
        const IsoPoint a = screen(mesh.vertices[mesh.indices[first]]);
        const IsoPoint b = screen(mesh.vertices[mesh.indices[first + 1U]]);
        const IsoPoint c = screen(mesh.vertices[mesh.indices[first + 2U]]);
        const gui::UiColor wire = alpha(ctx.theme().accent, 0.44F);
        ctx.draw_list().add_line(a.x, a.y, b.x, b.y, 0.75F, wire);
        ctx.draw_list().add_line(b.x, b.y, c.x, c.y, 0.75F, wire);
        ctx.draw_list().add_line(c.x, c.y, a.x, a.y, 0.75F, wire);
      }
    } else if (uv_mesh_domain) {
      gui::Rect foot = gui::inset(fitted, gui::Padding{8.0F, 0.0F, 8.0F, 6.0F});
      ctx.label(gui::cut_bottom(foot, 18.0F), "UV MESH PREVIEW UNAVAILABLE",
                ctx.theme().warning);
    }
  }

  if (feed_map.maps.empty()) {
    gui::Rect message =
        gui::inset(work, gui::Padding{18.0F, 36.0F, 18.0F, 0.0F});
    const gui::Rect empty = gui::cut_top(message, 22.0F);
    ctx.label(empty, "NO OUTPUT VIEWPORTS CONFIGURED", ctx.theme().text_muted);
    ctx.tooltip(empty, "Add a viewport inside an application window to begin");
  } else {
    std::array<char, 72U> canvas_text{};
    const int length = std::snprintf(
        canvas_text.data(), canvas_text.size(),
        "OUTPUT WINDOWS  %u x %u",
        mapping_canvas.width, mapping_canvas.height);
    gui::Rect heading = gui::inset(work, gui::Padding{12.0F, 5.0F, 12.0F, 0.0F});
    const gui::Rect heading_line = gui::cut_top(heading, 18.0F);
    ctx.label(
        heading_line,
        std::string_view{canvas_text.data(),
                         static_cast<std::size_t>(std::max(length, 0))},
        ctx.theme().text_muted);
    ctx.tooltip(heading_line, "Viewports sit inside fixed output rasters");
  }
  f.selected_map = selected_map;
  f.mapping_surface = mapping_surface;
  f.uv_mesh_domain = uv_mesh_domain;
}

void FeedMappingPanel::draw_rectangle_controls(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const gui::Rect &bounds = f.bounds;
  const scene::Canvas canvas = f.canvas;
  const topology::HardwareMap *const hardware = f.hardware;
  FeedMappingFrame &frame = f.result;
  const gui::Rect &content = f.content;
  const gui::Rect &work = f.work;
  const output::OutputMap *const selected_map = f.selected_map;
  const output::ProjectionSurface *const mapping_surface = f.mapping_surface;
  const bool uv_mesh_domain = f.uv_mesh_domain;
  const float details_width = feed_details_width(bounds);
  const gui::Rect rectangle_controls{
      work.x + 12.0F, bounds.bottom() - 78.0F,
      std::max(work.width - details_width - 24.0F, 0.0F), 72.0F};
  if (selected_map != nullptr && rectangle_controls.width >= 240.0F &&
      content.height >= 180.0F) {
    if (rectangle_mesh_domain_ != uv_mesh_domain) {
      rectangle_numbers_.cancel(ctx);
      rectangle_mesh_domain_ = uv_mesh_domain;
    }
    const auto draw_rectangle = [&](std::string_view label,
                                     std::string_view domain,
                                     scene::CanvasRect original,
                                     scene::Canvas extent, float y) {
      scene::CanvasRect edited = original;
      // The extent bounds every field below, so it is stated beside them: a
      // crop that stops short is otherwise a silent clamp against a raster
      // authored somewhere else in the panel.
      std::array<char, 40U> bound{};
      const int bound_length =
          std::snprintf(bound.data(), bound.size(), "  |  WITHIN %u x %u",
                        extent.width, extent.height);
      ctx.label(gui::Rect{rectangle_controls.x, y,
                          rectangle_controls.width, 17.0F},
                std::string{label} +
                    std::string{bound.data(), static_cast<std::size_t>(
                                                  std::max(bound_length, 0))},
                ctx.theme().text_muted);
      const float field_width = (rectangle_controls.width - 9.0F) / 4.0F;
      const std::array<std::string_view, 4U> names{"X", "Y", "W", "H"};
      const std::array<core::u32, 4U> values{
          original.x, original.y, original.width, original.height};
      bool changed = false;
      for (std::size_t index = 0U; index < values.size(); ++index) {
        double value{};
        const std::string key = std::string{domain} + "." +
                                std::string{names[index]};
        if (!rectangle_numbers_.draw(
                ctx, gui::ui_id(id_, key, selected_map->node.value()),
                gui::Rect{rectangle_controls.x +
                              static_cast<float>(index) * (field_width + 3.0F),
                          y + 17.0F, field_width, 20.0F},
                names[index], static_cast<double>(values[index]), 1.0,
                value, 0)) continue;
        // Preserve the other rectangle components when moving. A source
        // crop and a destination viewport are independent authored values.
        const auto bounded = [&](core::u32 minimum, core::u32 maximum) {
          return static_cast<core::u32>(std::clamp(
              std::round(value), static_cast<double>(minimum),
              static_cast<double>(std::max(minimum, maximum))));
        };
        switch (index) {
        case 0U: edited.x = bounded(0U, extent.width -
                     std::min(original.width, extent.width)); break;
        case 1U: edited.y = bounded(0U, extent.height -
                     std::min(original.height, extent.height)); break;
        case 2U: edited.width = bounded(1U, extent.width -
                     std::min(original.x, extent.width)); break;
        case 3U: edited.height = bounded(1U, extent.height -
                     std::min(original.y, extent.height)); break;
        default: break;
        }
        changed = true;
      }
      return changed ? std::optional{edited} : std::nullopt;
    };
    const auto viewport = draw_rectangle(
        "OUTPUT WINDOW  /  DESTINATION PIXELS", "feed.viewport",
        selected_map->viewport,
        feed_window_raster(selected_map->node_index, canvas, hardware),
        rectangle_controls.y);
    if (viewport) frame.edit = FeedMapRectEdit{selected_map->node, *viewport};
    if (uv_mesh_domain) {
      ctx.label(gui::Rect{rectangle_controls.x, rectangle_controls.y + 39.0F,
                         rectangle_controls.width, 20.0F},
                "UV MESH USES THE FULL SURFACE RASTER", ctx.theme().text_muted);
    } else {
      const auto source = draw_rectangle(
          selected_map->projection == output::FeedProjection::PlanarSurface
              ? "LED SURFACE  /  SOURCE PIXELS" : "CANVAS  /  SOURCE PIXELS",
          "feed.source", selected_map->source,
          mapping_surface != nullptr ? mapping_surface->geometry.raster : canvas,
          rectangle_controls.y + 39.0F);
      if (source) frame.projection_edit = FeedProjectionEdit{
          selected_map->node, selected_map->projection, selected_map->surface,
          *source};
    }
    frame.editing = frame.editing || rectangle_numbers_.live();
  } else {
    // A resize or replicated deletion can remove the active field without
    // another draw call to release its keyboard focus.
    rectangle_numbers_.cancel(ctx);
  }
}

void FeedMappingPanel::resolve_surface_drop(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const output::FeedMap &feed_map = f.feed_map;
  FeedMappingFrame &frame = f.result;
  const bool surface_drag_active = f.surface_drag_active;
  std::vector<std::pair<gui::Rect, doc::NodeId>> &surface_drop_targets =
      f.surface_drop_targets;
  std::vector<std::pair<gui::Rect, doc::NodeId>> &surface_rows = f.surface_rows;
  // One resolution point for the whole panel: every target has been laid out by
  // now, so a release names exactly one output and the assignment is a single
  // document edit.
  if (surface_drag_node_ != doc::NodeId{}) {
    const output::ProjectionSurface *const dragged =
        feed_map.surface_for(surface_drag_node_);
    bool over_surface_list = false;
    if (surface_drag_active && dragged != nullptr) {
      for (const auto &[row_bounds, node] : surface_rows) {
        if (!row_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
          continue;
        }
        over_surface_list = true;
        const bool after =
            ctx.pointer_y() >= row_bounds.y + row_bounds.height * 0.5F;
        ctx.panel(gui::rule_y(row_bounds,
                              after ? row_bounds.bottom() + 2.0F
                                    : row_bounds.y - 1.0F,
                              2.0F),
                  ctx.theme().accent);
        if (!ctx.pointer_down() && node != dragged->node) {
          std::vector<doc::NodeId> current;
          current.reserve(feed_map.surfaces.size());
          for (const output::ProjectionSurface &surface : feed_map.surfaces) {
            current.push_back(surface.node);
          }
          frame.organize = FeedOrganizeRequest{
              FeedOrganizeAction::Reorder, dragged->node, {},
              reordered(current, dragged->node, node, after)};
          if (frame.organize->order == current) {
            frame.organize.reset();
          }
        }
        break;
      }
    }
    if (surface_drag_active && dragged != nullptr) {
      for (const auto &[target_bounds, map_node] : surface_drop_targets) {
        if (over_surface_list ||
            !target_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
          continue;
        }
        if (!ctx.pointer_down()) {
          selected_map_node_ = map_node;
          selected_surface_node_ = dragged->node;
          frame.selection = map_node;
          frame.surface_selection = dragged->node;
          frame.projection_edit = FeedProjectionEdit{
              map_node, output::FeedProjection::PlanarSurface, dragged->node,
              scene::whole(dragged->geometry.raster)};
        }
        break;
      }
      const gui::Rect ghost{ctx.pointer_x() + 12.0F, ctx.pointer_y() + 8.0F,
                            152.0F, 22.0F};
      ctx.panel(ghost, ctx.theme().widget_active);
      gui::Rect edge = ghost;
      ctx.panel(gui::cut_left(edge, 3.0F), ctx.theme().accent);
      ctx.label(gui::inset(ghost, gui::Padding{9.0F, 0.0F, 3.0F, 0.0F}),
                dragged->name.empty() ? std::string_view{"UNTITLED SURFACE"}
                                      : std::string_view{dragged->name},
                ctx.theme().text);
    }
    if (!ctx.pointer_down()) {
      surface_drag_node_ = {};
      surface_drag_moved_ = false;
    }
  }
}

void FeedMappingPanel::draw_build_notes(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const gui::Rect &bounds = f.bounds;
  if (!build_notes_.empty() && ctx.time_seconds() < build_notes_until_) {
    constexpr float line = 18.0F;
    const std::size_t lines = std::min<std::size_t>(build_notes_.size(), 6U);
    const float width = std::min(bounds.width - 24.0F, 560.0F);
    const gui::Rect card{bounds.x + 12.0F,
                         bounds.bottom() - 16.0F -
                             static_cast<float>(lines) * line - 14.0F,
                         width, static_cast<float>(lines) * line + 14.0F};
    ctx.shadow(card, ctx.theme().radius);
    ctx.rounded(card, ctx.theme().widget, ctx.theme().radius);
    gui::Rect text = gui::inset(card, 12.0F, 7.0F);
    for (std::size_t index = 0U; index < lines; ++index) {
      ctx.label(gui::cut_top(text, line), build_notes_[index],
                index == 0U ? ctx.theme().text : ctx.theme().text_muted);
    }
    if (card.contains(ctx.pointer_x(), ctx.pointer_y()) &&
        ctx.pointer_pressed()) {
      build_notes_until_ = 0.0;
    }
  }
}

} // namespace mediaserver::editor
