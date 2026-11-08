// The Feed Mapping details rail: the selected viewport's projection and
// the selected surface's calibration.

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

void FeedMappingPanel::draw_details(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const gui::Rect &bounds = f.bounds;
  const scene::Canvas canvas = f.canvas;
  const output::FeedMap &feed_map = f.feed_map;
  FeedMappingFrame &frame = f.result;
  const bool surface_drag_active = f.surface_drag_active;
  std::vector<std::pair<gui::Rect, doc::NodeId>> &surface_drop_targets =
      f.surface_drop_targets;
  const output::OutputMap *const selected_map = f.selected_map;
  const bool uv_mesh_domain = f.uv_mesh_domain;
  const float details_width = feed_details_width(bounds);
  if (details_width > 0.0F) {
    gui::Rect details = bounds;
    static_cast<void>(gui::cut_top(details, title_height));
    details = gui::cut_right(details, details_width);
    ctx.panel(details, scale(ctx.theme().panel, 0.96F));
    {
      gui::Rect edge = details;
      ctx.panel(gui::cut_left(edge, 1.0F), ctx.theme().divider);
    }
    gui::Rect title_strip =
        gui::inset(details, gui::Padding{10.0F, 3.0F, 10.0F, 0.0F});
    const gui::Rect projection_title = gui::cut_top(title_strip, 20.0F);
    if (selected_map != nullptr) {
      ctx.label(projection_title,
                feed_output_identity(selected_map->node_index),
                ctx.theme().text);
    } else {
      ctx.label(projection_title, "OUTPUT PROJECTION", ctx.theme().text_muted);
    }

    // Rows down the column 10px in from its sides; each call says how tall
    // the next strip is and how far below it the one after starts.
    gui::Metrics details_metrics = ctx.theme().metrics;
    details_metrics.inset = 10.0F;
    details_metrics.gap = 28.0F;
    gui::FormLayout details_column{details, details_metrics};
    constexpr float field_height = 22.0F;
    constexpr float field_gap = 3.0F;
    if (selected_map != nullptr) {
      ctx.label(details_column.next(feed_details_label_height, 1.0F),
                feed_runtime_binding(selected_map->node_index),
                ctx.theme().text_muted);
      const bool has_surfaces = !feed_map.surfaces.empty();
      const gui::Rect modes = details_column.next(field_height, 4.0F);
      const gui::Rect canvas_mode =
          has_surfaces ? gui::column(modes, 0U, 2U, field_gap) : modes;
      const gui::Rect physical_mode = gui::column(modes, 1U, 2U, field_gap);
      if (ctx.button(gui::ui_id(id_, "feed.mode.canvas", selected_map->node.value()),
                     canvas_mode, "CANVAS") &&
          selected_map->projection != output::FeedProjection::CanvasSlice) {
        const scene::CanvasRect source =
            scene::contains(canvas, selected_map->source)
                ? selected_map->source
                : scene::whole(canvas);
        frame.projection_edit =
            FeedProjectionEdit{selected_map->node,
                               output::FeedProjection::CanvasSlice,
                               {},
                               source};
      }
      if (has_surfaces &&
          ctx.button(gui::ui_id(id_, "feed.mode.physical", selected_map->node.value()),
                     physical_mode, "PHYSICAL") &&
          selected_map->projection != output::FeedProjection::PlanarSurface) {
        const output::ProjectionSurface *target =
            surface_or_null(feed_map, selected_surface_node_);
        if (target == nullptr) {
          target = &feed_map.surfaces.front();
        }
        selected_surface_node_ = target->node;
        frame.surface_selection = target->node;
        frame.projection_edit = FeedProjectionEdit{
            selected_map->node, output::FeedProjection::PlanarSurface,
            target->node, scene::whole(target->geometry.raster)};
      }
      if (selected_map->projection == output::FeedProjection::CanvasSlice ||
          !has_surfaces) {
        ctx.outline(canvas_mode, ctx.theme().accent, 1.5F);
      } else {
        ctx.outline(physical_mode, ctx.theme().accent, 1.5F);
      }

      std::string_view status{"SHARED CANVAS CROP"};
      gui::UiColor status_color = ctx.theme().text_muted;
      if (selected_map->projection == output::FeedProjection::PlanarSurface) {
        if (selected_map->perspective_ready()) {
          status = uv_mesh_domain
                       ? std::string_view{"CALIBRATED UV SURFACE - READY"}
                       : std::string_view{"CALIBRATED WORLD SURFACE - READY"};
          status_color = ctx.theme().good;
        } else if (feed_map.surface_for(selected_map->surface) == nullptr) {
          status = "WORLD SURFACE - MISSING ASSIGNMENT";
          status_color = ctx.theme().warning;
        } else {
          status = "WORLD SURFACE - REGION OUTSIDE RASTER";
          status_color = ctx.theme().warning;
        }
      }
      ctx.label(details_column.next(18.0F, 3.0F), status, status_color);

    // A project with no surfaces authored has no surface row: a heading over an
    // empty well is a container invented to hold an exception.
    if (has_surfaces) {
      ctx.label(details_column.next(feed_details_label_height, 1.0F), "SURFACE",
                ctx.theme().text_muted);
      // A surface reaches an output by being dragged onto it, so this row
      // states the surface that is on this one and takes the drop as well. It
      // is not a stepper: cycling blindly through a wall's surfaces to find
      // the right one is the gesture nobody could decide on.
      const gui::Rect surface_choice = details_column.next(field_height, 7.0F);
      const output::ProjectionSurface *const assigned =
          feed_map.surface_for(selected_map->surface);
      const gui::Interaction assignment_interaction =
          assigned != nullptr
              ? ctx.grab(gui::ui_id(id_, "feed.surface.choice",
                             selected_map->node.value()),
                         surface_choice)
              : gui::Interaction{};
      ctx.panel(surface_choice, assignment_interaction.hovered
                                    ? ctx.theme().widget_hover
                                    : ctx.theme().track);
      const gui::Rect assignment_label = gui::inset(surface_choice, 7.0F, 0.0F);
      if (assigned != nullptr) {
        ctx.label(assignment_label,
                  assigned->name.empty()
                      ? std::string_view{"UNTITLED SURFACE"}
                      : std::string_view{assigned->name},
                  ctx.theme().text);
        if (assignment_interaction.clicked) {
          selected_surface_node_ = assigned->node;
          frame.surface_selection = assigned->node;
        }
      } else if (selected_map->surface != doc::NodeId{}) {
        ctx.label(assignment_label, "MISSING SURFACE", ctx.theme().warning);
      } else {
        ctx.label(assignment_label, "DRAG A SURFACE HERE",
                  ctx.theme().text_muted);
      }
      if (surface_drag_active) {
        surface_drop_targets.emplace_back(surface_choice, selected_map->node);
        ctx.outline(surface_choice,
                    surface_choice.contains(ctx.pointer_x(), ctx.pointer_y())
                   ? ctx.theme().accent
                   : alpha(ctx.theme().accent, 0.45F),
                    1.0F);
      }
    }
    } else {
      ctx.label(details_column.next(40.0F, 33.0F),
                "Select an output region to choose its projection.",
                ctx.theme().text_muted);
    }
    draw_surface_calibration(f, details, details_column);
  }
}

void FeedMappingPanel::draw_surface_calibration(
    Frame &f, const gui::Rect &details, gui::FormLayout &details_column) {
  gui::UiContext &ctx = f.ctx;
  const output::FeedMap &feed_map = f.feed_map;
  FeedMappingFrame &frame = f.result;
  constexpr float field_height = 22.0F;
  constexpr float field_gap = 3.0F;
  f.sync_surface_details();
  // The rule runs a pixel wider than the rows on each side.
  ctx.panel(gui::cell(gui::inset(details, 9.0F, 0.0F), details_column.next(1.0F, 5.0F)),
            ctx.theme().divider);
  const gui::Rect calibration_heading = details_column.next(20.0F, 2.0F);
  ctx.label(calibration_heading, "SURFACE CALIBRATION",
            ctx.theme().text_muted);

  const output::ProjectionSurface *const details_surface =
      feed_map.surface_for(surface_details_node_);
  if (details_surface != nullptr) {
    const bool details_locked = details_surface->locked;
    if (details_locked) {
      gui::Rect locked_strip = calibration_heading;
      ctx.label(gui::cut_right(locked_strip, 68.0F), "LOCKED",
                ctx.theme().warning);
      ctx.push_pointer_block();
    }
    const auto emit_surface_edit = [&](FeedSurfaceTransform transform =
                                           FeedSurfaceTransform::None) {
      if (details_locked) {
        return;
      }
      if (surface_shape_ == output::ProjectionSurfaceShape::ImportedMesh &&
          surface_mesh_source_.empty()) {
        return;
      }
      if (frame.surface_edit.has_value()) {
        transform |= frame.surface_edit->transform_components;
      }
      frame.surface_edit = FeedSurfaceEditRequest{
          surface_details_node_, surface_name_,
          surface_geometry_,     0U,
          surface_shape_,        surface_horizontal_arc_turns_,
          surface_mesh_columns_, surface_mesh_source_,
          surface_mesh_scale_,   transform};
    };
    const auto draw_float = [&](gui::UiId field_id, const gui::Rect &field,
                                std::string_view axis, float &value,
                                double speed, double minimum, double maximum,
                                int decimals,
                                FeedSurfaceTransform transform =
                                    FeedSurfaceTransform::None) {
      double changed{};
      if (!surface_numbers_.draw(ctx, field_id, field, axis,
                                 static_cast<double>(value), speed, changed,
                                 decimals)) {
        return;
      }
      value = static_cast<float>(std::clamp(changed, minimum, maximum));
      emit_surface_edit(transform);
    };
    // `count` fields across the next row, `gap_after` above the row
    // after it - first behind a label, for labelled_columns.
    const auto field_columns = [&](std::size_t count, float gap_after) {
      const gui::Rect strip = details_column.next(field_height, gap_after);
      std::array<gui::Rect, 3U> fields{};
      for (std::size_t index = 0U; index < count; ++index) {
        fields[index] = gui::column(strip, index, count, field_gap);
      }
      return fields;
    };
    const auto labelled_columns = [&](std::string_view label,
                                      std::size_t count,
                                      float gap_after = 5.0F) {
      constexpr float label_width = 50.0F;
      gui::Rect strip = details_column.next(field_height, gap_after);
      ctx.label(gui::cut_left(strip, label_width), label,
                ctx.theme().text_muted);
      static_cast<void>(gui::cut_left(strip, field_gap));
      std::array<gui::Rect, 3U> fields{};
      for (std::size_t index = 0U; index < count; ++index) {
        fields[index] = gui::column(strip, index, count, field_gap);
      }
      return fields;
    };

    const auto page_fields = field_columns(2U, 7.0F);
    if (ctx.button(gui::ui_id(id_, "feed.surface.geometry-page"), page_fields[0], "GEOMETRY")) surface_color_page_ = false;
    if (ctx.button(gui::ui_id(id_, "feed.surface.color-page"), page_fields[1], "COLOUR")) surface_color_page_ = true;
    const gui::UiId name_id = gui::ui_id(id_, "feed.surface.name", surface_details_node_.value());
    const gui::UiId camera_id = gui::ui_id(id_, "feed.surface.camera-inverse", surface_details_node_.value());
    if (!surface_color_page_) {
    auto fields = field_columns(3U, 5.0F);
    const gui::Rect planar_shape = fields[0U];
    const gui::Rect arc_shape = fields[1U];
    const gui::Rect mesh_shape = fields[2U];
    if (ctx.button(gui::ui_id(id_, "feed.surface.shape.planar",
                       surface_details_node_.value()),
                   planar_shape, "FLAT") &&
        surface_shape_ != output::ProjectionSurfaceShape::Planar) {
      surface_shape_ = output::ProjectionSurfaceShape::Planar;
      emit_surface_edit();
    }
    if (ctx.button(
            gui::ui_id(id_, "feed.surface.shape.arc", surface_details_node_.value()),
            arc_shape, "ARC") &&
        surface_shape_ != output::ProjectionSurfaceShape::HorizontalArc) {
      surface_shape_ = output::ProjectionSurfaceShape::HorizontalArc;
      emit_surface_edit();
    }
    if (ctx.button(gui::ui_id(id_, "feed.surface.shape.mesh",
                       surface_details_node_.value()),
                   mesh_shape, "UV MESH") &&
        surface_shape_ != output::ProjectionSurfaceShape::ImportedMesh) {
      surface_shape_ = output::ProjectionSurfaceShape::ImportedMesh;
      // An imported surface is not document-valid until it has a UV mesh.
      // Keep the local choice visible so the explicit chooser appears, but
      // never publish a broken surface while the source is still empty.
      if (!surface_mesh_source_.empty()) {
        emit_surface_edit();
      }
    }
    const gui::Rect selected_shape =
        surface_shape_ == output::ProjectionSurfaceShape::Planar
            ? planar_shape
            : (surface_shape_ == output::ProjectionSurfaceShape::HorizontalArc
                   ? arc_shape
                   : mesh_shape);
    ctx.outline(selected_shape, ctx.theme().accent, 1.5F);

    fields = labelled_columns("NAME", 1U);
    if (ctx.text_field(name_id, fields[0U], surface_name_)) {
      surface_name_dirty_ = true;
    }

    fields = labelled_columns(
        surface_shape_ == output::ProjectionSurfaceShape::ImportedMesh
            ? std::string_view{"ORIGIN"}
            : std::string_view{"CENTER"},
        3U);
    draw_float(
        gui::ui_id(id_, "feed.surface.center.x", surface_details_node_.value()),
        fields[0U], "X", surface_geometry_.center.x, 0.01, -1'000'000.0,
        1'000'000.0, 3, FeedSurfaceTransform::PositionX);
    draw_float(
        gui::ui_id(id_, "feed.surface.center.y", surface_details_node_.value()),
        fields[1U], "Y", surface_geometry_.center.y, 0.01, -1'000'000.0,
        1'000'000.0, 3, FeedSurfaceTransform::PositionY);
    draw_float(
        gui::ui_id(id_, "feed.surface.center.z", surface_details_node_.value()),
        fields[2U], "Z", surface_geometry_.center.z, 0.01, -1'000'000.0,
        1'000'000.0, 3, FeedSurfaceTransform::PositionZ);

    constexpr double pi = 3.14159265358979323846;
    double yaw_degrees =
        std::atan2(-static_cast<double>(surface_geometry_.right.z),
                   static_cast<double>(surface_geometry_.right.x)) *
        180.0 / pi;
    double changed_yaw{};
    fields = labelled_columns("YAW", 1U);
    if (surface_numbers_.draw(
            ctx, gui::ui_id(id_, "feed.surface.yaw", surface_details_node_.value()),
            fields[0U], "YAW", yaw_degrees, 0.25, changed_yaw, 1)) {
      output::set_yaw_keeping_pitch(
          surface_geometry_,
          static_cast<float>(std::clamp(changed_yaw, -3600.0, 3600.0) /
                             360.0));
      emit_surface_edit(FeedSurfaceTransform::Yaw);
    }

    // Tilt about the face's own right edge: a quarter turn faces it down,
    // which is a ceiling over the viewer, and minus a quarter turn is a
    // floor. Orientation, so it travels with yaw as one transform bit.
    const output::SurfacePitchBasis pitch_basis =
        output::surface_pitch_basis(surface_geometry_);
    const double pitch_degrees =
        std::atan2(static_cast<double>(pitch_basis.sine),
                   static_cast<double>(pitch_basis.cosine)) *
        180.0 / pi;
    fields = labelled_columns("PITCH", 1U);
    const auto set_pitch = [&](double degrees) {
      output::orient(surface_geometry_, static_cast<float>(yaw_degrees / 360.0),
                     static_cast<float>(degrees / 360.0));
      emit_surface_edit(FeedSurfaceTransform::Yaw);
    };
    gui::Rect pitch_field = fields[0U];
    if (std::abs(pitch_degrees) > 1.0e-4) {
      const gui::Rect reset = gui::cut_right(pitch_field, reset_button_width);
      static_cast<void>(gui::cut_right(pitch_field, field_gap));
      if (reset_button(ctx,
                       gui::ui_id(id_, "feed.surface.pitch.reset",
                           surface_details_node_.value()),
                       reset, "Reset to upright (0 degrees)")) {
        set_pitch(0.0);
      }
    }
    double changed_pitch{};
    if (surface_numbers_.draw(
            ctx,
            gui::ui_id(id_, "feed.surface.pitch", surface_details_node_.value()),
            pitch_field, "PITCH", pitch_degrees, 0.25, changed_pitch, 1)) {
      set_pitch(std::clamp(changed_pitch, -180.0, 180.0));
    }

    if (surface_shape_ == output::ProjectionSurfaceShape::HorizontalArc) {
      fields = labelled_columns("ARC", 2U);
      double bend_degrees =
          static_cast<double>(surface_horizontal_arc_turns_) * 360.0;
      double changed_bend{};
      if (surface_numbers_.draw(
              ctx,
              gui::ui_id(id_, "feed.surface.arc", surface_details_node_.value()),
              fields[0U], "DEG", bend_degrees, 0.25, changed_bend, 1)) {
        changed_bend = std::clamp(changed_bend, -179.0, 179.0);
        if (std::abs(changed_bend) < 0.1) {
          changed_bend = changed_bend < 0.0 ? -0.1 : 0.1;
        }
        surface_horizontal_arc_turns_ =
            static_cast<float>(changed_bend / 360.0);
        emit_surface_edit();
      }
      double changed_columns{};
      if (surface_numbers_.draw(
              ctx,
              gui::ui_id(id_, "feed.surface.columns", surface_details_node_.value()),
              fields[1U], "COL", static_cast<double>(surface_mesh_columns_),
              1.0, changed_columns, 0)) {
        surface_mesh_columns_ =
            static_cast<core::u32>(std::llround(std::clamp(
                changed_columns,
                static_cast<double>(output::min_horizontal_arc_columns),
                static_cast<double>(output::max_horizontal_arc_columns))));
        emit_surface_edit();
      }
    }

    if (surface_shape_ == output::ProjectionSurfaceShape::ImportedMesh) {
      const std::string mesh_status =
          mesh_source_status(surface_mesh_source_);
      ctx.label(details_column.next(16.0F, 0.0F), mesh_status,
                surface_mesh_source_.empty() ? ctx.theme().warning
                                             : ctx.theme().text_muted);
      if (ctx.button(gui::ui_id(id_, "feed.surface.mesh.browse",
                         surface_details_node_.value()),
                     details_column.next(field_height, 5.0F),
                     surface_mesh_source_.empty() ? "CHOOSE UV MESH"
                                                  : "REPLACE UV MESH")) {
        mesh_browse_pending_ = true;
        // A replacement may deliberately overwrite the same path. Record a
        // new generation without doing any file work on this UI frame; the
        // prior exact-key UV mesh remains visible until publication.
        if (!surface_mesh_source_.empty()) {
          static_cast<void>(
              preview_projection_cache_.refresh_imported_mesh());
        }
        frame.mesh_browse = FeedSurfaceEditRequest{
            surface_details_node_,
            surface_name_,
            surface_geometry_,
            0U,
            output::ProjectionSurfaceShape::ImportedMesh,
            surface_horizontal_arc_turns_,
            surface_mesh_columns_,
            surface_mesh_source_,
            surface_mesh_scale_,
            FeedSurfaceTransform::None};
      }
      fields = labelled_columns("SCALE", 1U);
      double changed_scale{};
      if (surface_numbers_.draw(ctx,
                                gui::ui_id(id_, "feed.surface.mesh.scale",
                                    surface_details_node_.value()),
                                fields[0U], "S",
                                static_cast<double>(surface_mesh_scale_),
                                0.01, changed_scale, 3)) {
        surface_mesh_scale_ = static_cast<float>(
            std::clamp(changed_scale, 0.001,
                       static_cast<double>(output::max_imported_mesh_scale)));
        if (!surface_mesh_source_.empty()) {
          emit_surface_edit();
        }
      }
    }

    if (surface_shape_ != output::ProjectionSurfaceShape::ImportedMesh) {
      fields = labelled_columns("SIZE", 2U);
      draw_float(
          gui::ui_id(id_, "feed.surface.width", surface_details_node_.value()),
          fields[0U], "W", surface_geometry_.width, 0.01, 0.01, 1'000'000.0,
          3);
      draw_float(
          gui::ui_id(id_, "feed.surface.height", surface_details_node_.value()),
          fields[1U], "H", surface_geometry_.height, 0.01, 0.01, 1'000'000.0,
          3);
    } else {
      ctx.label(details_column.next(16.0F, 4.0F),
                "AUTHORED COORDINATES / SHARED ORIGIN",
                ctx.theme().text_muted);
    }

    fields = labelled_columns("RASTER", 2U, 7.0F);
    const auto draw_raster = [&](gui::UiId field_id, const gui::Rect &field,
                                 std::string_view axis, core::u32 &value) {
      double changed{};
      if (!surface_numbers_.draw(ctx, field_id, field, axis,
                                 static_cast<double>(value), 1.0, changed,
                                 0)) {
        return;
      }
      value = static_cast<core::u32>(
          std::llround(std::clamp(changed, 1.0, 131'072.0)));
      emit_surface_edit();
    };
    draw_raster(
        gui::ui_id(id_, "feed.surface.raster.width", surface_details_node_.value()),
        fields[0U], "W", surface_geometry_.raster.width);
    draw_raster(
        gui::ui_id(id_, "feed.surface.raster.height", surface_details_node_.value()),
        fields[1U], "H", surface_geometry_.raster.height);

    } else {
      bool changed = ctx.checkbox(gui::ui_id(id_, "feed.surface.color-enabled", surface_details_node_.value()),
          details_column.next(24.0F, 8.0F), surface_color_.enabled,
          "Enable compensation");
      ctx.label(details_column.next(20.0F, 4.0F), "WALL PRIMARIES (LINEAR)",
                ctx.theme().text_muted);
      constexpr std::array<std::string_view,3> rows{"RED", "GREEN", "BLUE"};
      for (std::size_t row = 0; row < 3U; ++row) {
        const auto cells = labelled_columns(rows[row], 3U);
        for (std::size_t column = 0; column < 3U; ++column) {
          const std::size_t index = row * 3U + column;
          double value{};
          if (surface_numbers_.draw(ctx, gui::ui_id(id_, "feed.surface.primary", surface_details_node_.value() * 9U + index),
              cells[column], {}, surface_color_.primaries[index], 0.001, value, 4)) {
            surface_color_.primaries[index] = static_cast<float>(std::clamp(value, -16.0, 16.0)); changed = true;
          }
        }
      }
      details_column.space(10.0F);
      ctx.label(details_column.next(20.0F, 3.0F), "CAMERA IDT INVERSE (.cube)",
                ctx.theme().text_muted);
      changed |= ctx.text_field(camera_id, details_column.next(24.0F, 8.0F),
                                surface_color_.camera_inverse_lut_source);
      ctx.label(details_column.next(18.0F, 0.0F), "Measured for this wall / camera.",
                ctx.theme().text_muted);
      ctx.label(details_column.next(18.0F, 0.0F), "Applied after the show look.",
                ctx.theme().text_muted);
      if (changed) {
        emit_surface_edit();
        if (frame.surface_edit) frame.surface_edit->color = surface_color_;
      }
    }
    if (surface_name_dirty_ && ctx.focus() != name_id) {
      emit_surface_edit();
      surface_name_dirty_ = false;
    }
    frame.editing = frame.editing || (!details_locked &&
                    (surface_numbers_.live() || ctx.focus() == name_id || ctx.focus() == camera_id));
    if (details_locked) {
      ctx.pop_pointer_block();
    }
  } else {
    ctx.label(details_column.next(40.0F, 0.0F),
              "Add or select a physical surface to calibrate it.",
              ctx.theme().text_muted);
  }
}

} // namespace mediaserver::editor
