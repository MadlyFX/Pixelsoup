// The Hardware Mapping details rail for the selected device or cable.

#include "hardware_mapping_frame.hpp"

#include <mediaserver/core/output_limits.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/topology/hardware_map_schema.hpp>

#include <algorithm>
#include <array>
#include <limits>
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

gui::Rect HardwareMappingPanel::DetailsRows::labelled_bounds(
    std::string_view label) {
  const gui::FormRow row = form.row();
  f.ctx.label(row.label, label, f.ctx.theme().text_muted);
  return row.field;
}

void HardwareMappingPanel::DetailsRows::emit_number(std::string_view label,
                                                    std::string_view property,
                                                    core::u64 salt,
                                                    core::u32 value,
                                                    core::u32 minimum,
                                                    core::u32 maximum) {
  const gui::Rect field = labelled_bounds(label);
  HardwareMappingPanel &panel = f.panel;
  double changed{};
  if (!panel.details_numbers_.draw(
          f.ctx,
          gui::ui_id(panel.id_, "hardware.details.number",
                     panel.details_node_.value() * 8U + salt),
          field, {}, static_cast<double>(value), 1.0, changed, 0)) {
    return;
  }
  const auto rounded = static_cast<core::i64>(
      std::llround(std::clamp(changed, static_cast<double>(minimum),
                              static_cast<double>(maximum))));
  f.result.property_edit = HardwarePropertyEdit{
      panel.details_node_, std::string{property}, doc::PropertyValue{rounded}};
}

void HardwareMappingPanel::DetailsRows::emit_signed_number(
    std::string_view label, std::string_view property, core::u64 salt,
    core::i32 value) {
  const gui::Rect field = labelled_bounds(label);
  HardwareMappingPanel &panel = f.panel;
  double changed{};
  if (!panel.details_numbers_.draw(
          f.ctx,
          gui::ui_id(panel.id_, "hardware.details.number",
                     panel.details_node_.value() * 8U + salt),
          field, {}, static_cast<double>(value), 1.0, changed, 0)) {
    return;
  }
  const auto rounded = static_cast<core::i64>(std::llround(std::clamp(
      changed, static_cast<double>(std::numeric_limits<core::i32>::min()),
      static_cast<double>(std::numeric_limits<core::i32>::max()))));
  f.result.property_edit = HardwarePropertyEdit{
      panel.details_node_, std::string{property}, doc::PropertyValue{rounded}};
}

void HardwareMappingPanel::draw_details(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const gui::Rect &bounds = f.bounds;
  const topology::HardwareMap &map = f.map;
  // Selection may have changed in the card/port pass above. The details rail
  // follows immediately and occupies space excluded from the grid, so its
  // controls never fight a device beneath them for pointer capture.
  const topology::HardwareDevice *const selected_device =
      selection_.kind == HardwareSelectionKind::Device
          ? map.device(selection_.node)
          : nullptr;
  f.sync_details(selected_device);
  gui::Rect details = bounds;
  static_cast<void>(gui::cut_top(details, title_height));
  details = gui::cut_right(details, hardware_details_width(bounds));
  if (details.width > 0.0F) {
    ctx.panel(details, scale(ctx.theme().panel, 0.96F));
    {
      gui::Rect edge = details;
      ctx.panel(gui::cut_left(edge, 1.0F), ctx.theme().divider);
      gui::Rect heading = gui::inset(details, gui::Padding{10.0F, 3.0F, 10.0F, 0.0F});
      ctx.label(gui::cut_top(heading, 22.0F), "DEVICE DETAILS",
                ctx.theme().text_muted);
    }

    if (selected_device != nullptr && details_node_ == selected_device->node) {
      draw_device_details(f, details, selected_device);
    } else if (selection_.kind == HardwareSelectionKind::Link) {
      draw_cable_details(f, details);
    } else {
      gui::Rect message =
          gui::inset(details, gui::Padding{10.0F, 35.0F, 10.0F, 0.0F});
      ctx.label(gui::cut_top(message, 44.0F), "Select a device or cable",
                ctx.theme().text_muted);
    }
  }
}

void HardwareMappingPanel::draw_device_details(
    Frame &f, const gui::Rect &details,
    const topology::HardwareDevice *selected_device) {
  gui::UiContext &ctx = f.ctx;
  const topology::HardwareMap &map = f.map;
  HardwareMappingFrame &frame = f.result;
  // Labelled rows 23px tall and 4px apart under the heading, a 72px
  // label column, 9px in from the column's sides.
  gui::Metrics m = ctx.theme().metrics;
  m.inset = 9.0F;
  m.gap = 31.0F;
  m.control = details_row_height;
  m.row_gap = 4.0F;
  m.label_column = details_label_width;
  DetailsRows rows{f, details, gui::FormLayout{details, m}};
  const gui::UiId name_id =
      gui::ui_id(id_, "hardware.details.name", details_node_.value());
  const gui::Rect name_bounds = rows.labelled_bounds("Name");
  if (ctx.text_field(name_id, name_bounds, details_name_)) {
    details_name_dirty_ = true;
  }
  const gui::UiId address_id =
      gui::ui_id(id_, "hardware.details.address", details_node_.value());
  const bool render_server =
      selected_device->kind == topology::HardwareDeviceKind::RenderServer;
  const bool processor =
      selected_device->kind == topology::HardwareDeviceKind::LedProcessor;
  if (processor) {
    // How the Director reaches it. NovaStar COEX reads the processor's
    // inputs, EDIDs and screens; Generic leaves it a box with cables.
    const gui::Rect protocol_bounds = rows.labelled_bounds("Protocol");
    if (ctx.button(gui::ui_id(id_, "hardware.details.protocol",
                       details_node_.value()),
                   protocol_bounds,
                   topology::processor_protocol_name(
                       selected_device->protocol))) {
      // Generic, then NovaStar COEX, then Brompton Tessera.
      std::string_view next = topology::schema::novastar_coex_protocol;
      if (selected_device->protocol ==
          topology::HardwareProcessorProtocol::NovastarCoex) {
        next = topology::schema::brompton_tessera_protocol;
      } else if (selected_device->protocol ==
                 topology::HardwareProcessorProtocol::BromptonTessera) {
        next = topology::schema::generic_protocol;
      }
      frame.property_edit = HardwarePropertyEdit{
          details_node_, std::string{topology::schema::protocol},
          doc::PropertyValue{std::string{next}}};
    }
    ctx.tooltip(protocol_bounds,
                "How the Director reaches this processor. NovaStar COEX "
                "reads its inputs, EDIDs and screens (HTTP port 8001); "
                "Brompton Tessera its input, panels and overrides (port "
                "80, IP control on in Live Control); Generic is not "
                "monitored.");
  }
  const gui::Rect address_bounds =
      rows.labelled_bounds(render_server ? "Host IP" : "Address");
  if (ctx.text_field(address_id, address_bounds, details_address_)) {
    details_address_dirty_ = true;
  }
  if (render_server && details_address_.empty() &&
      ctx.focus() != address_id) {
    ctx.label(gui::inset(address_bounds, 6.0F, 0.0F),
              "LOCAL (127.0.0.1)", ctx.theme().text_muted);
  }
  if (processor && details_address_.empty() &&
      ctx.focus() != address_id) {
    ctx.label(gui::inset(address_bounds, 6.0F, 0.0F),
              selected_device->protocol ==
                      topology::HardwareProcessorProtocol::Generic
                  ? "OPTIONAL"
                  : "E.G. 192.168.0.40",
              ctx.theme().text_muted);
  }
  if (render_server) {
    const gui::Rect target_bounds = rows.labelled_bounds("Control");
    ctx.panel(target_bounds, ctx.theme().track);
    ctx.outline(target_bounds, ctx.theme().divider);
    const std::optional<std::string> target =
        topology::lifecycle_endpoint_for(map, *selected_device);
    ctx.push_clip(target_bounds);
    ctx.label(gui::inset(target_bounds, 6.0F, 0.0F),
              target.has_value() ? std::string_view{*target}
                                 : std::string_view{"UNAVAILABLE"},
              target.has_value() ? ctx.theme().text_muted
                                 : ctx.theme().warning);
    ctx.pop_clip();
  }

  const bool explicit_output_bindings =
      render_server && draw_server_outputs(f, rows, selected_device);
  // A raster and rate mean something on a Render Server (what it sends)
  // and an LED Wall (what it lights). A processor's input modes come from
  // the outputs cabled to it and from its own report, so neither it nor an
  // Operator Machine shows them.
  const bool has_raster =
      render_server ||
      selected_device->kind == topology::HardwareDeviceKind::LedWall;
  if (!explicit_output_bindings && has_raster) {
    const core::u32 maximum_width =
        render_server ? topology::max_hardware_output_width : 32'768U;
    const core::u32 maximum_height =
        render_server ? topology::max_hardware_output_height : 32'768U;
    rows.emit_number("Width", topology::schema::width, 0U,
                selected_device->width, 1U, maximum_width);
    rows.emit_number("Height", topology::schema::height, 1U,
                selected_device->height, 1U, maximum_height);
    {
      // Hertz as typed; the Director keeps the exact fraction.
      const core::u32 millihertz =
          topology::refresh_millihertz(selected_device->refresh_numerator,
                                       selected_device->refresh_denominator);
      double changed{};
      if (details_numbers_.draw(
              ctx,
              gui::ui_id(id_, "hardware.details.number",
                  details_node_.value() * 8U + 2U),
              rows.labelled_bounds("Refresh"), {},
              static_cast<double>(millihertz) / 1000.0, 0.01, changed,
              topology::refresh_decimals(millihertz))) {
        frame.property_edit = HardwarePropertyEdit{
            details_node_, std::string{hardware_refresh_edit},
            doc::PropertyValue{std::clamp(
                changed, 1.0, topology::max_refresh_hertz)}};
      }
    }
  }

  if (render_server) {
    draw_server_status(f, rows, selected_device);
  }
  if (processor &&
      selected_device->protocol !=
          topology::HardwareProcessorProtocol::Generic) {
    draw_processor_monitor(f, rows, selected_device);
  }

  if (details_name_dirty_ && ctx.focus() != name_id) {
    frame.property_edit = HardwarePropertyEdit{
        details_node_, std::string{topology::schema::name},
        doc::PropertyValue{details_name_}};
    details_name_dirty_ = false;
  } else if (details_address_dirty_ && ctx.focus() != address_id) {
    frame.property_edit = HardwarePropertyEdit{
        details_node_, std::string{topology::schema::address},
        doc::PropertyValue{details_address_}};
    details_address_dirty_ = false;
  }
  frame.editing = details_numbers_.live() || ctx.focus() == name_id ||
                  ctx.focus() == address_id;
}

bool HardwareMappingPanel::draw_server_outputs(
    Frame &f, DetailsRows &rows,
    const topology::HardwareDevice *selected_device) {
  gui::UiContext &ctx = f.ctx;
  const topology::HardwareMap &map = f.map;
  const topology::HardwareRuntimeSnapshot &runtime = f.runtime;
  HardwareMappingFrame &frame = f.result;
  bool explicit_output_bindings = false;
  rows.emit_number("Node index", topology::schema::node_index, 4U,
              selected_device->node_index, 0U,
              topology::max_hardware_node_index);
  {
    // Whose playhead the Timeline draws beside the editor's.
    const gui::Rect main_bounds = rows.labelled_bounds("Timeline");
    bool main = selected_device->main_node;
    if (ctx.checkbox(gui::ui_id(id_, "hardware.details.main-node",
                         details_node_.value()),
                     main_bounds, main, "Main node")) {
      frame.property_edit = HardwarePropertyEdit{
          details_node_, std::string{topology::schema::main_node},
          doc::PropertyValue{main}};
    }
    ctx.tooltip(main_bounds,
                "Show this node's playhead on the Timeline. One node "
                "per show; choosing it clears the others.");
  }
  std::vector<const topology::HardwareOutputBinding *> bindings;
  for (const topology::HardwareOutputBinding &binding :
       map.output_bindings) {
    if (binding.render_server == selected_device->node) {
      bindings.push_back(&binding);
    }
  }
  explicit_output_bindings = !bindings.empty();
  if (details_output_binding_ != doc::NodeId{} &&
      map.output_binding(details_output_binding_) == nullptr) {
    details_output_binding_ = {};
  }

  const topology::HardwareRuntimeNode *const deployment_runtime =
      runtime.node(selected_device->node);
  const auto target_for = [&](const topology::HardwareDisplayTargetKey &key)
      -> const topology::HardwareVideoOutput * {
    if (deployment_runtime == nullptr ||
        !runtime_inventory_is_usable(deployment_runtime)) {
      return nullptr;
    }
    for (const topology::HardwareVideoOutput &candidate :
         deployment_runtime->video_outputs) {
      if (candidate.target == key) {
        return &candidate;
      }
    }
    return nullptr;
  };
  const auto next_unbound_target = [&]()
      -> const topology::HardwareVideoOutput * {
    if (deployment_runtime == nullptr ||
        !runtime_inventory_is_usable(deployment_runtime)) {
      return nullptr;
    }
    for (const topology::HardwareVideoOutput &candidate :
         deployment_runtime->video_outputs) {
      const bool bound = std::any_of(
          bindings.begin(), bindings.end(),
          [&](const topology::HardwareOutputBinding *binding) {
            return binding->target == candidate.target;
          });
      if (candidate.active && candidate.available &&
          candidate.target.valid() && !bound) {
        return &candidate;
      }
    }
    return nullptr;
  };
  ctx.label(rows.line(19.0F, 1.0F), "OUTPUT DEPLOYMENT",
            ctx.theme().text_muted);
  for (const topology::HardwareOutputBinding *binding : bindings) {
    const topology::HardwareVideoOutput *const observed =
        target_for(binding->target);
    const bool current = observed != nullptr && observed->active &&
        observed->available;
    const bool mode_matches = !current ||
        binding->window_mode !=
            topology::HardwareWindowMode::Fullscreen ||
        (observed->width == binding->width &&
         observed->height == binding->height &&
         observed->refresh_numerator == binding->refresh_numerator &&
         std::max(observed->refresh_denominator, core::u32{1U}) ==
             binding->refresh_denominator);
    const std::string label =
        "PORT " + std::to_string(binding->logical_port) +
        "  FEED " + std::to_string(binding->feed_index) +
        (!current ? "  MISSING"
                  : (mode_matches ? "  READY" : "  MODE MISMATCH"));
    const gui::Rect row = rows.line(22.0F, 2.0F);
    if (ctx.button(gui::ui_id(id_, "hardware.binding.select",
                       binding->node.value()),
                   row, label)) {
      details_output_binding_ = binding->node;
    }
  }
  ctx.label(rows.line(19.0F, 3.0F),
            deployment_runtime == nullptr ||
                    !runtime_inventory_is_usable(deployment_runtime)
                ? "START LISTENER, THEN WIRE VIDEO OUT"
                : "WIRE VIDEO OUT TO CREATE OUTPUT",
            deployment_runtime == nullptr ||
                    !runtime_inventory_is_usable(deployment_runtime)
                ? ctx.theme().warning
                : ctx.theme().text_muted);

  const topology::HardwareOutputBinding *const selected_binding =
      map.output_binding(details_output_binding_);
  if (selected_binding != nullptr &&
      selected_binding->render_server == selected_device->node) {
    topology::HardwareOutputBinding changed_binding = *selected_binding;
    const auto emit_binding_number =
        [&](std::string_view label, core::u64 salt, core::u32 value,
            core::u32 minimum, core::u32 maximum,
            core::u32 &destination) {
      const gui::Rect field = rows.labelled_bounds(label);
      double changed{};
      if (details_numbers_.draw(
              ctx,
              gui::ui_id(id_, "hardware.binding.number",
                  selected_binding->node.value() * 8U + salt),
              field, {}, static_cast<double>(value), 1.0, changed, 0)) {
        destination = static_cast<core::u32>(std::llround(std::clamp(
            changed, static_cast<double>(minimum),
            static_cast<double>(maximum))));
        frame.output_binding_edit = HardwareOutputBindingEdit{
            changed_binding};
      }
    };
    emit_binding_number("Feed", 0U, selected_binding->feed_index, 0U,
                        topology::max_hardware_node_index,
                        changed_binding.feed_index);
    const gui::Rect binding_mode = rows.labelled_bounds("Mode");
    if (ctx.button(gui::ui_id(id_, "hardware.binding.mode",
                       selected_binding->node.value()),
                   binding_mode,
                   window_mode_label(selected_binding->window_mode))) {
      switch (selected_binding->window_mode) {
      case topology::HardwareWindowMode::Windowed:
        changed_binding.window_mode =
            topology::HardwareWindowMode::Borderless;
        break;
      case topology::HardwareWindowMode::Borderless:
        changed_binding.window_mode =
            topology::HardwareWindowMode::Fullscreen;
        break;
      case topology::HardwareWindowMode::Fullscreen:
      case topology::HardwareWindowMode::Inherit:
        changed_binding.window_mode =
            topology::HardwareWindowMode::Windowed;
        break;
      }
      frame.output_binding_edit = HardwareOutputBindingEdit{
          changed_binding};
    }
    const auto precision_field = rows.labelled_bounds("Precision");
    if (ctx.button(gui::ui_id(id_, "hardware.binding.precision", selected_binding->node.value()),
        precision_field, selected_binding->output_bit_depth == 10U ? "10-BIT RGB"
          : selected_binding->output_bit_depth == 16U ? "16-FLOAT LINEAR" : "8-BIT RGB")) {
      changed_binding.output_bit_depth = selected_binding->output_bit_depth == 8U ? 10U
          : selected_binding->output_bit_depth == 10U ? 16U : 8U;
      frame.output_binding_edit = HardwareOutputBindingEdit{changed_binding};
    }
    emit_binding_number("Width", 1U, selected_binding->width, 1U,
                        topology::max_hardware_output_width,
                        changed_binding.width);
    emit_binding_number("Height", 2U, selected_binding->height, 1U,
                        topology::max_hardware_output_height,
                        changed_binding.height);
    {
      const core::u32 millihertz = topology::refresh_millihertz(
          selected_binding->refresh_numerator,
          selected_binding->refresh_denominator);
      double changed{};
      if (details_numbers_.draw(
              ctx,
              gui::ui_id(id_, "hardware.binding.number",
                  selected_binding->node.value() * 8U + 3U),
              rows.labelled_bounds("Refresh"), {},
              static_cast<double>(millihertz) / 1000.0, 0.01, changed,
              topology::refresh_decimals(millihertz))) {
        const topology::HardwareRefresh rate =
            topology::refresh_from_hertz(
                std::clamp(changed, 1.0, topology::max_refresh_hertz));
        changed_binding.refresh_numerator = rate.numerator;
        changed_binding.refresh_denominator = rate.denominator;
        frame.output_binding_edit =
            HardwareOutputBindingEdit{changed_binding};
      }
    }

    const topology::HardwareVideoOutput *const replacement =
        next_unbound_target();
    if (target_for(selected_binding->target) == nullptr &&
        replacement != nullptr) {
      const gui::Rect repair = rows.line(22.0F, 3.0F);
      if (ctx.button(gui::ui_id(id_, "hardware.binding.repair",
                         selected_binding->node.value()),
                     repair, "REPAIR WITH NEXT ACTIVE OUTPUT")) {
        changed_binding.target = replacement->target;
        changed_binding.width = replacement->width;
        changed_binding.height = replacement->height;
        changed_binding.refresh_numerator =
            replacement->refresh_numerator;
        changed_binding.refresh_denominator =
            std::max(replacement->refresh_denominator, 1U);
        frame.output_binding_edit = HardwareOutputBindingEdit{
            changed_binding};
      }
    }
    ctx.label(rows.line(19.0F, 3.0F), "DISCONNECT CABLE TO REMOVE OUTPUT",
              ctx.theme().text_muted);
  }

  if (!explicit_output_bindings) {
    const gui::Rect mode_bounds = rows.labelled_bounds("Window");
    if (ctx.button(
            gui::ui_id(id_, "hardware.details.window-mode",
                details_node_.value()),
            mode_bounds,
            window_mode_label(selected_device->window_mode))) {
      const topology::HardwareWindowMode next =
          next_window_mode(selected_device->window_mode);
      frame.property_edit = HardwarePropertyEdit{
          details_node_, std::string{topology::schema::window_mode},
          doc::PropertyValue{std::string{window_mode_property(next)}}};
    }
    const std::string_view settings_source =
        hardware_window_settings_source_text(
            selected_device->window_mode);
    ctx.label(rows.form.note(), settings_source,
              selected_device->window_mode ==
                      topology::HardwareWindowMode::Inherit
                  ? ctx.theme().warning
                  : ctx.theme().text_muted);
    rows.emit_signed_number("Desktop X", topology::schema::desktop_x, 5U,
                       selected_device->desktop_x);
    rows.emit_signed_number("Desktop Y", topology::schema::desktop_y, 6U,
                       selected_device->desktop_y);
  }
  return explicit_output_bindings;
}

void HardwareMappingPanel::draw_server_status(
    Frame &f, DetailsRows &rows,
    const topology::HardwareDevice *selected_device) {
  const gui::Rect &details = rows.details;
  gui::UiContext &ctx = f.ctx;
  const topology::HardwareMap &map = f.map;
  const topology::HardwareRuntimeSnapshot &runtime = f.runtime;
  rows.form.space(3.0F);
  ctx.panel(rows.line(1.0F, 4.0F), ctx.theme().divider);
  ctx.label(rows.line(19.0F, 1.0F), "LIVE SYSTEM", ctx.theme().text_muted);

  const auto live_row = [&](std::string_view label,
                            std::string_view value, gui::UiColor color) {
    if (rows.form.cursor() + 19.0F > details.bottom() - 5.0F) {
      return false;
    }
    gui::Rect value_bounds = rows.line(19.0F, 1.0F);
    ctx.label(gui::cut_left(value_bounds, details_label_width), label,
              ctx.theme().text_muted);
    ctx.label(value_bounds, value, color);
    return true;
  };

  const HardwareDevicePresentation live_presentation =
      hardware_presentation(map, *selected_device, runtime);
  const topology::HardwareRuntimeNode *const live =
      live_presentation.runtime;
  if (live == nullptr) {
    static_cast<void>(
        live_row("Status", "UNMONITORED", ctx.theme().text_muted));
    static_cast<void>(
        live_row("Outputs", "OUTPUTS UNKNOWN", ctx.theme().text_muted));
    static_cast<void>(live_row("Info", "No authenticated telemetry",
                               ctx.theme().text_muted));
  } else {
    const std::string status = runtime_card_status(live_presentation);
    static_cast<void>(
        live_row("Status", status,
                 runtime_status_color(ctx.theme(), live_presentation)));
    if (!live->hostname.empty()) {
      static_cast<void>(
          live_row("Host", live->hostname, ctx.theme().text));
    }
    if (!live->operating_system.empty()) {
      static_cast<void>(
          live_row("OS", live->operating_system, ctx.theme().text_muted));
    }
    if (live->logical_processor_count != 0U) {
      const std::string cpu =
          std::to_string(live->logical_processor_count) + " logical";
      static_cast<void>(live_row("CPU", cpu, ctx.theme().text_muted));
    }
    if (live->total_memory_bytes != 0U) {
      const std::string memory = capacity_text(live->total_memory_bytes);
      static_cast<void>(live_row("RAM", memory, ctx.theme().text_muted));
    }

    constexpr std::size_t maximum_gpu_rows = 2U;
    const std::size_t gpu_rows =
        std::min(live->gpu_adapters.size(), maximum_gpu_rows);
    for (std::size_t index = 0U; index < gpu_rows; ++index) {
      const topology::HardwareGpuAdapter &adapter =
          live->gpu_adapters[index];
      std::string gpu =
          adapter.name.empty() ? "Unknown GPU" : adapter.name;
      if (adapter.dedicated_video_memory_bytes != 0U) {
        gpu +=
            " | " + capacity_text(adapter.dedicated_video_memory_bytes);
      }
      if (adapter.software) {
        gpu += " | SOFTWARE";
      } else if (adapter.remote) {
        gpu += " | REMOTE";
      }
      if (!live_row(index == 0U ? "GPU" : "", gpu,
                    adapter.software ? ctx.theme().warning
                                     : ctx.theme().text_muted)) {
        break;
      }
    }
    if (live->gpu_adapters.size() > gpu_rows) {
      const std::string more =
          "+" + std::to_string(live->gpu_adapters.size() - gpu_rows) +
          " more adapters";
      static_cast<void>(live_row("", more, ctx.theme().text_muted));
    }

    const std::string outputs = runtime_output_status(live_presentation);
    const bool output_warning =
        live->inventory ==
            topology::HardwareRuntimeInventoryState::Failed ||
        live->inventory ==
            topology::HardwareRuntimeInventoryState::Unsupported;
    static_cast<void>(live_row("Outputs", outputs,
                               output_warning ? ctx.theme().warning
                                              : ctx.theme().text_muted));
    constexpr std::size_t maximum_output_rows = 2U;
    std::size_t shown_outputs{};
    if (runtime_inventory_is_usable(live)) {
      for (const topology::HardwareVideoOutput &output :
           live->video_outputs) {
        if (!output.active || shown_outputs >= maximum_output_rows) {
          continue;
        }
        const std::string mode = output_mode_text(output);
        if (!live_row("", mode, ctx.theme().text_muted)) {
          break;
        }
        ++shown_outputs;
      }
    }
    if (live_presentation.active_video_output_count > shown_outputs) {
      const std::string more =
          "+" +
          std::to_string(live_presentation.active_video_output_count -
                         shown_outputs) +
          " more outputs";
      static_cast<void>(live_row("", more, ctx.theme().text_muted));
    }
  }
}

void HardwareMappingPanel::draw_processor_monitor(
    Frame &f, DetailsRows &rows,
    const topology::HardwareDevice *selected_device) {
  const gui::Rect &details = rows.details;
  gui::UiContext &ctx = f.ctx;
  const topology::HardwareRuntimeSnapshot &runtime = f.runtime;
  HardwareMappingFrame &frame = f.result;
  rows.form.space(3.0F);
  ctx.panel(rows.line(1.0F, 4.0F), ctx.theme().divider);
  ctx.label(rows.line(19.0F, 1.0F), "PROCESSOR", ctx.theme().text_muted);
  const auto room = [&](float height) {
    return rows.form.cursor() + height <= details.bottom() - 5.0F;
  };
  const auto row = [&](std::string_view label, std::string_view value,
                       gui::UiColor color) {
    if (!room(19.0F)) {
      return false;
    }
    gui::Rect value_bounds = rows.line(19.0F, 1.0F);
    ctx.label(gui::cut_left(value_bounds, details_label_width), label,
              ctx.theme().text_muted);
    ctx.push_clip(value_bounds);
    ctx.label(value_bounds, value, color);
    ctx.pop_clip();
    return true;
  };
  const topology::HardwareProcessorNode *const observed =
      runtime.processor(selected_device->node);
  static_cast<void>(
      row("Status", processor_status_text(*selected_device, observed),
          processor_status_color(ctx.theme(), *selected_device,
                                 observed)));
  if (observed != nullptr) {
    if (!observed->name.empty()) {
      static_cast<void>(row("Name", observed->name, ctx.theme().text));
    }
    if (!observed->software.empty()) {
      static_cast<void>(
          row("Software", observed->software, ctx.theme().text_muted));
    }
    for (const topology::HardwareProcessorInput &input :
         observed->inputs) {
      std::string value = input.name;
      value += input.signal
                   ? "  " + topology::format_video_mode(
                                input.width, input.height,
                                input.refresh_millihertz)
                   : std::string{"  no signal"};
      if (!row("In " + std::to_string(input.number), value,
               input.signal ? ctx.theme().text : ctx.theme().text_muted)) {
        break;
      }
      if (input.edid_width != 0U &&
          !row("", "EDID " + topology::format_video_mode(
                                 input.edid_width, input.edid_height,
                                 input.edid_refresh_millihertz),
               ctx.theme().text_muted)) {
        break;
      }
    }
    for (const topology::HardwareProcessorScreen &screen :
         observed->screens) {
      const std::string value =
          (screen.name.empty() ? std::string{"Screen"} : screen.name) +
          "  " + std::to_string(screen.width) + "x" +
          std::to_string(screen.height) + ", " +
          std::to_string(screen.cabinets) + " cabinets";
      if (!row("Screen", value, ctx.theme().text_muted)) {
        break;
      }
    }
    // Online without a screen: what it answered instead, so a
    // processor that will not describe its wall says why.
    if (observed->screens.empty() && !observed->detail.empty() &&
        observed->reachability ==
            topology::HardwareRuntimeReachability::Online) {
      static_cast<void>(
          row("Screen", observed->detail, ctx.theme().caution));
    }
    if (observed->panels != 0U) {
      std::string panels = std::to_string(observed->panels_online) +
                           " of " + std::to_string(observed->panels) +
                           " online";
      if (observed->panels_faulted != 0U) {
        panels += ", " + std::to_string(observed->panels_faulted) +
                  " faulted";
      }
      static_cast<void>(row(
          "Panels", panels,
          observed->panels_online < observed->panels ||
                  observed->panels_faulted != 0U
              ? ctx.theme().caution
              : ctx.theme().text_muted));
    }
    if (!observed->preset.empty()) {
      static_cast<void>(
          row("Preset", observed->preset, ctx.theme().text_muted));
    }
    // An override puts something other than the input on the wall.
    std::string overrides;
    for (const auto &[on, word] :
         {std::pair{observed->blackout, "Blackout"},
          std::pair{observed->frozen, "Freeze"},
          std::pair{observed->test_pattern, "Test pattern"}}) {
      if (on) {
        overrides += overrides.empty() ? word : std::string{", "} + word;
      }
    }
    if (!overrides.empty()) {
      static_cast<void>(row("Override", overrides + " on",
                            ctx.theme().caution));
    }
    if (!observed->notice.empty()) {
      static_cast<void>(row("", observed->notice,
                            observed->notice_failed ? ctx.theme().warning
                                                    : ctx.theme().good));
    }
  }
  // An input whose EDID offers another mode than the output cabled to
  // it sends can be told to offer the output's.
  for (const topology::HardwareSignalCheck &check : f.signal_checks) {
    if (check.processor != selected_device->node ||
        check.issue != topology::HardwareSignalIssue::EdidMismatch ||
        !check.edid_writable || !room(24.0F)) {
      continue;
    }
    const std::string label =
        "SET IN " + std::to_string(check.input) + " EDID TO " +
        topology::format_video_mode(check.width, check.height,
                                    check.refresh_millihertz);
    const gui::Rect button = rows.line(22.0F, 3.0F);
    if (ctx.button(gui::ui_id(id_, "hardware.details.edid",
                       details_node_.value() * 32U + check.input),
                   button, label)) {
      frame.processor_edid = HardwareProcessorEdidRequest{
          check.processor, check.input, check.width, check.height,
          check.refresh_millihertz};
    }
    ctx.tooltip(button, topology::signal_check_text(check));
  }
}

void HardwareMappingPanel::draw_cable_details(Frame &f,
                                              const gui::Rect &details) {
  gui::UiContext &ctx = f.ctx;
  const topology::HardwareMap &map = f.map;
  const topology::HardwareRuntimeSnapshot &runtime = f.runtime;
  HardwareMappingFrame &frame = f.result;
  const topology::HardwareLink *const link = map.link(selection_.node);
  gui::Rect lines =
      gui::inset(details, gui::Padding{10.0F, 34.0F, 10.0F, 0.0F});
  ctx.label(gui::cut_top(lines, 22.0F), "CABLE", ctx.theme().text);
  static_cast<void>(gui::cut_top(lines, 3.0F));
  if (link != nullptr) {
    const auto end_text = [&](const topology::HardwareEndpoint &end) {
      const topology::HardwareDevice *const device = map.device(end.device);
      if (device == nullptr) {
        return std::string{"Missing device"};
      }
      const HardwareDevicePresentation presentation =
          hardware_presentation(map, *device, runtime);
      const PresentedHardwarePort *const shown = presentation.find(end.port);
      return device->name + "  " +
             (shown != nullptr ? std::string{shown->port.name}
                               : port_label(device->kind, end.port));
    };
    ctx.push_clip(lines);
    ctx.label(gui::cut_top(lines, 19.0F), end_text(link->from),
              ctx.theme().keyframe);
    ctx.label(gui::cut_top(lines, 19.0F), "to", ctx.theme().text_muted);
    ctx.label(gui::cut_top(lines, 19.0F), end_text(link->to),
              ctx.theme().keyframe);
    ctx.pop_clip();
    static_cast<void>(gui::cut_top(lines, 6.0F));

    if (const topology::HardwareSignalCheck *const check =
            f.check_for(link->node)) {
      const std::string sent =
          "Sends " + topology::format_video_mode(check->width, check->height,
                                                 check->refresh_millihertz);
      ctx.label(gui::cut_top(lines, 19.0F), sent, ctx.theme().text_muted);
      if (check->issue == topology::HardwareSignalIssue::None) {
        ctx.label(gui::cut_top(lines, 19.0F), "Processor receives it",
                  ctx.theme().good);
      } else {
        // What is wrong, then what the processor reports, one short line
        // each so a narrow rail never cuts the numbers off.
        const std::string input = "Input " + std::to_string(check->input);
        const std::string seen = topology::format_video_mode(
            check->seen_width, check->seen_height,
            check->seen_refresh_millihertz);
        std::string_view headline;
        std::string detail;
        switch (check->issue) {
        case topology::HardwareSignalIssue::EdidMismatch:
          headline = "EDID mismatch";
          detail = input + " EDID offers " + seen;
          break;
        case topology::HardwareSignalIssue::ModeMismatch:
          headline = "Mode mismatch";
          detail = input + " receives " + seen;
          break;
        case topology::HardwareSignalIssue::NoSignal:
          headline = "No signal";
          detail = input + " sees nothing";
          break;
        case topology::HardwareSignalIssue::None:
          break;
        }
        ctx.push_clip(lines);
        ctx.label(gui::cut_top(lines, 19.0F), headline, ctx.theme().caution);
        ctx.label(gui::cut_top(lines, 19.0F), detail, ctx.theme().caution);
        ctx.pop_clip();
        if (check->issue == topology::HardwareSignalIssue::EdidMismatch &&
            check->edid_writable) {
          static_cast<void>(gui::cut_top(lines, 4.0F));
          const gui::Rect button = gui::cut_top(lines, 22.0F);
          if (ctx.button(gui::ui_id(id_, "hardware.link.edid", link->node.value()),
                         button, "MATCH PROCESSOR EDID")) {
            frame.processor_edid = HardwareProcessorEdidRequest{
                check->processor, check->input, check->width,
                check->height, check->refresh_millihertz};
          }
          ctx.tooltip(button,
                      "Have the processor offer this output's mode on "
                      "its input, so the render machine's GPU is offered "
                      "it too");
        }
      }
    }
    // The part of the render output this cable carries: all of it, or a
    // tile when one output feeds several processors (NVIDIA Mosaic).
    const topology::HardwareDevice *const source =
        map.device(link->from.device);
    if (source != nullptr &&
        source->kind == topology::HardwareDeviceKind::RenderServer &&
        link->from.port >= topology::first_render_server_video_port &&
        link->from.port <= topology::last_render_server_video_port) {
      const topology::HardwareLinkRegion carried =
          topology::link_output_region(map, *link, runtime);
      static_cast<void>(gui::cut_top(lines, 8.0F));
      gui::Rect heading = gui::cut_top(lines, 19.0F);
      if (carried.source == topology::HardwareRegionSource::Authored) {
        const gui::Rect reset =
            gui::cut_right(heading, reset_button_width);
        if (reset_button(ctx,
                         gui::ui_id(id_, "hardware.link.region.reset",
                             link->node.value()),
                         reset, "Reset to its share of the output")) {
          frame.link_region_edit =
              HardwareLinkRegionEdit{link->node, {}};
        }
      }
      ctx.label(heading, "Carries", ctx.theme().text_muted);
      std::string_view origin;
      switch (carried.source) {
      case topology::HardwareRegionSource::Authored:
        origin = "Set by hand";
        break;
      case topology::HardwareRegionSource::WholeOutput:
        origin = "The whole output";
        break;
      case topology::HardwareRegionSource::MosaicTile:
        origin = "Its Mosaic display";
        break;
      case topology::HardwareRegionSource::EvenShare:
        origin = "An even share of the output";
        break;
      }
      const gui::Rect origin_line = gui::cut_top(lines, 19.0F);
      ctx.label(origin_line, origin, ctx.theme().text_muted);
      if (carried.source == topology::HardwareRegionSource::MosaicTile) {
        ctx.tooltip(origin_line,
                    "The render machine reports this output as an NVIDIA "
                    "Mosaic surface; this cable takes the display in the "
                    "same place in its grid");
      }
      const std::array<std::pair<std::string_view, core::u32>, 4U> fields{
          {{"X", carried.region.x},
           {"Y", carried.region.y},
           {"Width", carried.region.width},
           {"Height", carried.region.height}}};
      for (std::size_t index = 0U; index < fields.size(); ++index) {
        static_cast<void>(gui::cut_top(lines, 4.0F));
        gui::Rect row = gui::cut_top(lines, details_row_height);
        ctx.label(gui::cut_left(row, details_label_width),
                  fields[index].first, ctx.theme().text_muted);
        double changed{};
        if (!link_numbers_.draw(
                ctx,
                gui::ui_id(id_, "hardware.link.region",
                    link->node.value() * 4U + index),
                row, {}, static_cast<double>(fields[index].second), 1.0,
                changed, 0)) {
          continue;
        }
        const bool size = index >= 2U;
        const auto value = static_cast<core::u32>(std::llround(std::clamp(
            changed, size ? 1.0 : 0.0,
            static_cast<double>(topology::max_hardware_output_width))));
        topology::HardwareOutputRegion edited = carried.region;
        (index == 0U   ? edited.x
         : index == 1U ? edited.y
         : index == 2U ? edited.width
                       : edited.height) = value;
        if (!edited.empty()) {
          frame.link_region_edit =
              HardwareLinkRegionEdit{link->node, edited};
        }
      }
    }
  }
}

} // namespace mediaserver::editor
