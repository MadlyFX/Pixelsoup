// The Hardware Mapping palette column: device kinds to drag onto the map,
// project setup (control port, scene canvas) and the sync policy.

#include "hardware_mapping_frame.hpp"

#include <mediaserver/core/output_limits.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/topology/hardware_map_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mediaserver::editor {

using namespace mapping;
using namespace mapping::hardware;

void HardwareMappingPanel::draw_palette(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const gui::Rect &bounds = f.bounds;
  HardwareMappingFrame &frame = f.result;
  const gui::Rect &view = f.view;
  const IsometricProjection &projection = f.projection;
  // Palette: these are authored device kinds, not pretend discovered hosts.
  f.layout = palette_layout(bounds);
  const PaletteLayout &layout = f.layout;
  const gui::Rect palette = layout.column;
  ctx.panel(palette, scale(ctx.theme().panel, 0.94F));
  {
    gui::Rect edge = palette;
    ctx.panel(gui::cut_right(edge, 1.0F), ctx.theme().divider);
  }
  ctx.label(layout.header, "DRAG TO PLACE", ctx.theme().text_muted);

  // Every palette picture is drawn after the loop, in one sprite run: one
  // render pass for all of them rather than one per row.
  std::array<std::pair<gui::Rect, topology::HardwareDevice>,
             palette_entries.size()>
      thumbnails{};
  std::size_t thumbnail_count{};
  for (std::size_t index = 0U; index < palette_entries.size(); ++index) {
    const PaletteEntry &entry = palette_entries[index];
    const gui::Rect item =
        hardware_palette_item_bounds(bounds, entry.kind, entry.protocol);
    if (!f.fits(item)) {
      continue;
    }
    const gui::Interaction interaction =
        ctx.grab(gui::ui_id(id_, "hardware.palette", index), item);
    ctx.rounded(item,
                interaction.held
                    ? ctx.theme().selection
                    : (interaction.hovered ? ctx.theme().widget_hover
                                           : ctx.theme().row),
                ctx.theme().radius);
    ctx.rounded_outline(item,
                        interaction.held ? ctx.theme().accent
                                         : ctx.theme().divider,
                        ctx.theme().radius);
    topology::HardwareDevice sample{};
    sample.kind = entry.kind;
    sample.protocol = entry.protocol;
    thumbnails[thumbnail_count++] = {
        gui::Rect{item.x + 4.0F, item.y + 4.0F, 40.0F, item.height - 8.0F},
        sample};
    // The kind's name, over its brand where it has one, right of the picture.
    const gui::Rect text =
        gui::inset(item, gui::Padding{50.0F, 0.0F, 5.0F, 0.0F});
    ctx.push_clip(text);
    if (entry.blurb.empty()) {
      ctx.label(text, entry.name, ctx.theme().text);
    } else {
      ctx.label(gui::inset(text, gui::Padding{0.0F, 3.0F, 0.0F, 21.0F}),
                entry.name, ctx.theme().text);
      ctx.label(gui::inset(text, gui::Padding{0.0F, 21.0F, 0.0F, 2.0F}),
                entry.blurb, ctx.theme().text_muted);
    }
    ctx.pop_clip();

    if (interaction.pressed) {
      drag_kind_ = DragKind::Palette;
      palette_kind_ = entry.kind;
      palette_protocol_ = entry.protocol;
    }
    if (interaction.released && drag_kind_ == DragKind::Palette &&
        palette_kind_ == entry.kind && palette_protocol_ == entry.protocol) {
      if (view.contains(ctx.pointer_x(), ctx.pointer_y())) {
        const auto [grid_x, grid_y] =
            unproject_isometric(projection, ctx.pointer_x(), ctx.pointer_y());
        HardwarePlaceRequest request{entry.kind, grid_x, grid_y};
        request.protocol = entry.protocol;
        frame.place = std::move(request);
      }
      drag_kind_ = DragKind::None;
    }
  }
  for (std::size_t index = 0U; index < thumbnail_count; ++index) {
    draw_sprite_thumbnail(ctx, thumbnails[index].first,
                          thumbnails[index].second, sprites_);
  }
}

void HardwareMappingPanel::draw_project_setup(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const topology::HardwareMap &map = f.map;
  const scene::Canvas scene_canvas = f.scene_canvas;
  HardwareMappingFrame &frame = f.result;
  const PaletteLayout &layout = f.layout;
  // One lifecycle port range governs the project. Render Servers contribute
  // their host and node index; asking operators to retype host:port on every
  // card made a wall configuration both repetitive and easy to diverge.
  if (f.fits(layout.control_port.field)) {
    ctx.panel(layout.setup_rule, ctx.theme().divider);
    ctx.label(layout.setup_heading, "PROJECT SETUP", ctx.theme().text_muted);
    ctx.label(layout.control_port.label, "Control port",
              ctx.theme().text_muted);
    double changed_port{};
    if (settings_numbers_.draw(
            ctx, gui::ui_id(id_, "hardware.settings.control-port"),
            layout.control_port.field, {},
            static_cast<double>(map.control.base_port), 1.0, changed_port, 0)) {
      const auto rounded = static_cast<core::i64>(std::llround(std::clamp(
          changed_port, 1.0,
          static_cast<double>(topology::max_hardware_control_base_port))));
      frame.control_port_edit = HardwareControlPortEdit{
          map.control.node, static_cast<core::u32>(rounded)};
    }
    const std::string_view setup_status =
        map.control.authored ? std::string_view{"SAVED PROJECT RANGE"}
                             : std::string_view{"DEFAULT - EDIT TO SAVE"};
    ctx.label(layout.setup_status, setup_status, ctx.theme().text_muted);
  }

  // The canvas every scene renders, which canvas feed mappings crop. Its
  // rows are reserved even without a scene so the sync controls below never
  // move under the pointer when one appears.
  if (scene::valid(scene_canvas)) {
    const auto canvas_row = [&](bool height) {
      const gui::FormRow row =
          height ? layout.canvas_height : layout.canvas_width;
      if (!f.fits(row.field)) {
        return;
      }
      ctx.label(row.label, height ? "Canvas H" : "Canvas W",
                ctx.theme().text_muted);
      const core::u32 current =
          height ? scene_canvas.height : scene_canvas.width;
      double edited{};
      if (!canvas_numbers_.draw(
              ctx,
              gui::ui_id(id_, height ? "hardware.settings.canvas-height"
                              : "hardware.settings.canvas-width"),
              row.field, "px", static_cast<double>(current), 1.0, edited, 0)) {
        return;
      }
      const auto rounded = static_cast<core::u32>(std::llround(std::clamp(
          edited, 1.0, static_cast<double>(core::max_output_dimension))));
      scene::Canvas next = frame.canvas_edit.has_value()
                               ? frame.canvas_edit->canvas
                               : scene_canvas;
      (height ? next.height : next.width) = rounded;
      if (next != scene_canvas) {
        frame.canvas_edit = HardwareCanvasEdit{next};
      }
    };
    canvas_row(false);
    canvas_row(true);
  }
}

void HardwareMappingPanel::draw_sync_policy(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const sync::SyncPolicy &sync_policy = f.sync_policy;
  HardwareMappingFrame &frame = f.result;
  const PaletteLayout &layout = f.layout;
  const gui::Rect &palette = f.layout.column;
  // The project synchronization policy sits directly under the control port
  // because both are project settings, and because the sync groups this
  // policy governs are derived from the very cables drawn in this panel.
  if (f.fits(layout.sync_mode)) {
    ctx.panel(layout.sync_rule, ctx.theme().divider);
    ctx.label(layout.sync_heading, "SYNC POLICY", ctx.theme().text_muted);

    sync::SyncPolicy edited = sync_policy;
    bool changed = false;
    // The policy's rows continue down the palette from the MODE button.
    gui::Metrics rows_metrics = palette_metrics();
    gui::FormLayout rows{
        gui::Rect{palette.x, layout.sync_rows, palette.width,
                  std::max(palette.bottom() - layout.sync_rows, 0.0F)},
        rows_metrics};
    const auto cycle_button = [&](std::string_view suffix,
                                  const gui::Rect &button,
                                  std::string_view label,
                                  bool emphasised) -> bool {
      if (!f.fits(button)) {
        return false;
      }
      const gui::Interaction interaction = ctx.grab(gui::ui_id(id_, suffix), button);
      const gui::UiColor base = emphasised
                                    ? scale(ctx.theme().selection, 0.9F)
                                    : ctx.theme().widget;
      ctx.panel(button, interaction.hovered ? scale(base, 1.2F) : base);
      ctx.outline(button, ctx.theme().divider);
      ctx.label(gui::inset(button, 6.0F, 0.0F), label,
                emphasised ? ctx.theme().text : ctx.theme().text_muted);
      return interaction.clicked;
    };

    std::string mode_label{"MODE: "};
    mode_label += sync::sync_mode_name(sync_policy.mode);
    if (cycle_button("hardware.sync.mode", layout.sync_mode, mode_label,
                     sync_policy.requires_physical_sync())) {
      switch (sync_policy.mode) {
      case sync::SyncMode::Unsynced:
        edited.mode = sync::SyncMode::Framelock;
        break;
      case sync::SyncMode::Framelock:
        edited.mode = sync::SyncMode::ExternalGenlock;
        break;
      case sync::SyncMode::ExternalGenlock:
        edited.mode = sync::SyncMode::Unsynced;
        break;
      }
      changed = true;
    }

    const auto number_row = [&](std::string_view suffix,
                                std::string_view label, double value,
                                double minimum, double maximum,
                                double &result) -> bool {
      const gui::FormRow row = rows.row();
      if (!f.fits(row.field)) {
        return false;
      }
      ctx.label(row.label, label, ctx.theme().text_muted);
      double edited_value{};
      if (!sync_numbers_.draw(ctx, gui::ui_id(id_, suffix), row.field, {}, value, 1.0,
                              edited_value, 0)) {
        return false;
      }
      result = std::clamp(edited_value, minimum, maximum);
      return true;
    };

    if (sync_policy.requires_physical_sync()) {
      double next{};
      if (number_row("hardware.sync.width", "Width",
                     static_cast<double>(sync_policy.required_timing.width),
                     1.0, static_cast<double>(sync::max_sync_timing_width),
                     next)) {
        edited.required_timing.width =
            static_cast<core::u32>(std::llround(next));
        changed = true;
      }
      if (number_row("hardware.sync.height", "Height",
                     static_cast<double>(sync_policy.required_timing.height),
                     1.0, static_cast<double>(sync::max_sync_timing_height),
                     next)) {
        edited.required_timing.height =
            static_cast<core::u32>(std::llround(next));
        changed = true;
      }
      {
        // Hertz as typed, kept as the exact fraction (23.976 is 24000/1001).
        const core::u32 millihertz = topology::refresh_millihertz(
            sync_policy.required_timing.refresh_numerator,
            sync_policy.required_timing.refresh_denominator);
        const gui::FormRow row = rows.row();
        if (f.fits(row.field)) {
          ctx.label(row.label, "Refresh", ctx.theme().text_muted);
          double typed{};
          if (sync_numbers_.draw(ctx, gui::ui_id(id_, "hardware.sync.refresh"),
                                 row.field, {},
                                 static_cast<double>(millihertz) / 1000.0,
                                 0.01, typed,
                                 topology::refresh_decimals(millihertz))) {
            const topology::HardwareRefresh rate =
                topology::refresh_from_hertz(
                    std::clamp(typed, 1.0, topology::max_refresh_hertz));
            edited.required_timing.refresh_numerator = rate.numerator;
            edited.required_timing.refresh_denominator = rate.denominator;
            changed = true;
          }
        }
      }
      if (number_row("hardware.sync.group", "Swap group",
                     static_cast<double>(sync_policy.group), 1.0,
                     static_cast<double>(sync::max_sync_group_id), next)) {
        edited.group = static_cast<core::u32>(std::llround(next));
        changed = true;
      }
      if (number_row("hardware.sync.barrier", "Barrier",
                     static_cast<double>(sync_policy.barrier), 1.0,
                     static_cast<double>(sync::max_sync_barrier_id), next)) {
        edited.barrier = static_cast<core::u32>(std::llround(next));
        changed = true;
      }
      rows.space(2.0F);

      std::string failure_label{"ON FAILURE: "};
      failure_label += sync::sync_failure_policy_name(sync_policy.failure);
      if (cycle_button("hardware.sync.failure",
                       rows.next(details_row_height - 2.0F, 2.0F),
                       failure_label,
                       sync_policy.failure ==
                           sync::SyncFailurePolicy::Strict)) {
        edited.failure = sync_policy.failure == sync::SyncFailurePolicy::Strict
            ? sync::SyncFailurePolicy::Degraded
            : sync::SyncFailurePolicy::Strict;
        changed = true;
      }

      std::string late_label{"LATE FRAME: "};
      late_label += sync::late_frame_policy_name(sync_policy.late_frame);
      if (cycle_button("hardware.sync.late-frame",
                       rows.next(details_row_height - 2.0F, 4.0F), late_label,
                       sync_policy.late_frame ==
                           sync::LateFramePolicy::HoldGroup)) {
        edited.late_frame =
            sync_policy.late_frame == sync::LateFramePolicy::HoldGroup
            ? sync::LateFramePolicy::AdvanceIndependently
            : sync::LateFramePolicy::HoldGroup;
        changed = true;
      }
    }

    const gui::Rect status = rows.next(17.0F, 0.0F);
    if (f.fits(status)) {
      const std::string_view sync_status =
          !sync_policy.authored
              ? std::string_view{"DEFAULT - EDIT TO SAVE"}
              : (sync_policy.requires_physical_sync()
                     ? std::string_view{"SAVED - VERIFIED IN OUTPUT MONITOR"}
                     : std::string_view{"SAVED - NO PHYSICAL TIMING CLAIM"});
      ctx.label(status, sync_status, ctx.theme().text_muted);
    }

    if (changed) {
      // Authoring always writes a complete, valid policy value, so a
      // half-edited raster can never reach the document and widen what the
      // project claims about physical timing.
      edited.authored = true;
      if (sync::valid_policy(edited)) {
        frame.sync_policy_edit =
            HardwareSyncPolicyEdit{sync_policy.node, edited};
      }
    }
  }
}

} // namespace mediaserver::editor
