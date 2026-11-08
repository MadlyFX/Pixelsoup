#pragma once

// One HardwareMappingPanel::draw as an object. draw() builds a Frame from its
// arguments and runs the panel's phases in order; state one phase hands to
// another lives here, everything else stays local to its phase. Internal to
// the editor library.

#include "hardware_map_support.hpp"

#include <optional>
#include <vector>

namespace mediaserver::editor {

struct HardwareMappingPanel::Frame final {
  HardwareMappingPanel &panel;
  gui::UiContext &ctx;
  const gui::Rect &bounds;
  const topology::HardwareMap &map;
  const topology::HardwareRuntimeSnapshot &runtime;
  const sync::SyncPolicy &sync_policy;
  scene::Canvas scene_canvas;
  HardwareMappingFrame result{};

  // Set by navigate(): the canvas between the palette and the details rail,
  // the strip of machines found on the network along its bottom, the part
  // left for the map, and the map's projection after this frame's pan/zoom.
  gui::Rect canvas{};
  gui::Rect strip{};
  gui::Rect view{};
  float wheel{};
  IsometricProjection projection{};

  // Set by draw_map_floor(): what each cable into a monitored processor carries.
  std::vector<topology::HardwareSignalCheck> signal_checks;

  // Set by draw_palette().
  mapping::hardware::PaletteLayout layout{};

  // Set by move_devices(): the device under the pointer, whose ports and
  // name tag the stage labels.
  doc::NodeId hovered_device{};

  // Whether a palette row fits above the palette's bottom edge.
  [[nodiscard]] bool fits(const gui::Rect &row) const noexcept {
    return row.bottom() <= layout.column.bottom();
  }

  // `original` where it is drawn this frame: at the cell a device drag would
  // drop it, including on the frame its move is released.
  [[nodiscard]] topology::HardwareDevice
  displayed(const topology::HardwareDevice &original) const;
  [[nodiscard]] mapping::hardware::HardwareDevicePresentation
  presentation_for(const topology::HardwareDevice &device) const;
  [[nodiscard]] const topology::HardwareSignalCheck *
  check_for(doc::NodeId link) const noexcept;
  [[nodiscard]] std::optional<mapping::hardware::CableRoute>
  route_for(const topology::HardwareLink &link) const;
  [[nodiscard]] gui::UiColor
  cable_color(const topology::HardwareLink &link) const;

  // The input port nearest the pointer, within reach of it.
  [[nodiscard]] std::optional<topology::HardwareEndpoint>
  input_at_pointer() const;
  // What dropping the cable in hand on `endpoint` would do.
  [[nodiscard]] mapping::hardware::PortTargetState
  target_state(topology::HardwareEndpoint endpoint) const;
  // Ends the cable in hand on `endpoint`: a link request when it may end
  // there, otherwise the reason it may not.
  void attempt_link(topology::HardwareEndpoint endpoint);

  // Points the details rail at `device`, committing the text field the
  // operator leaves.
  void sync_details(const topology::HardwareDevice *device);
  void select_device(const topology::HardwareDevice &device);
};

// The details rail's rows for the selected device: 23px rows under the
// heading, a label column, and number fields that author a property.
struct HardwareMappingPanel::DetailsRows final {
  Frame &f;
  // The rail the rows run down.
  gui::Rect details;
  gui::FormLayout form;

  // A labelled row: draws `label` and returns the field beside it.
  [[nodiscard]] gui::Rect labelled_bounds(std::string_view label);
  // A line of text across the column, `height` tall and `gap` above
  // whatever follows.
  [[nodiscard]] gui::Rect line(float height, float gap) {
    return form.next(height, gap);
  }
  // A labelled number field that writes `property` when edited, clamped to
  // [minimum, maximum]; `salt` tells the device's fields apart.
  void emit_number(std::string_view label, std::string_view property,
                   core::u64 salt, core::u32 value, core::u32 minimum,
                   core::u32 maximum);
  void emit_signed_number(std::string_view label, std::string_view property,
                          core::u64 salt, core::i32 value);
};

} // namespace mediaserver::editor
