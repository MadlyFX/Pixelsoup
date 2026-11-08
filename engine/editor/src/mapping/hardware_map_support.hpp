#pragma once

// Layout, presentation and drawing helpers behind the Hardware Mapping
// panel: the palette, device sprites and ports, cables, runtime status and
// the machines found on the network. Internal to the editor library.

#include <mediaserver/editor/hardware_mapping_panel.hpp>
#include <mediaserver/editor/hardware_sprites.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/topology/hardware_map_schema.hpp>
#include <mediaserver/topology/hardware_signal.hpp>

#include "mapping_shared.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mediaserver::editor::mapping::hardware {

inline constexpr float palette_header_height = 28.0F;
inline constexpr float palette_row_height = 42.0F;
inline constexpr float palette_row_step = 46.0F;
inline constexpr float device_width = 160.0F;
inline constexpr float device_height = 56.0F;
inline constexpr float details_label_width = 72.0F;
inline constexpr float details_row_height = 23.0F;
inline constexpr float port_radius = 5.0F;
inline constexpr float port_hit_radius = 14.0F;
inline constexpr float hardware_port_row_step = 15.0F;
inline constexpr float link_drag_threshold = 4.0F;
inline constexpr float minimum_view_zoom = 0.45F;
inline constexpr float maximum_view_zoom = 2.50F;

// What the palette offers to drag onto the map. A NovaStar processor is an
// LED Processor that arrives already speaking its protocol.
struct PaletteEntry final {
  topology::HardwareDeviceKind kind{};
  topology::HardwareProcessorProtocol protocol{};
  std::string_view name;
  // The brand under the name; none for a kind without one.
  std::string_view blurb;
};

inline constexpr std::array<PaletteEntry, 6U> palette_entries{{
    {topology::HardwareDeviceKind::RenderServer,
     topology::HardwareProcessorProtocol::Generic, "Render server", {}},
    {topology::HardwareDeviceKind::OperatorMachine,
     topology::HardwareProcessorProtocol::Generic, "Operator machine", {}},
    {topology::HardwareDeviceKind::LedProcessor,
     topology::HardwareProcessorProtocol::Generic, "LED processor",
     "Any brand"},
    {topology::HardwareDeviceKind::LedProcessor,
     topology::HardwareProcessorProtocol::NovastarCoex, "NovaStar processor",
     "COEX"},
    {topology::HardwareDeviceKind::LedProcessor,
     topology::HardwareProcessorProtocol::BromptonTessera, "Brompton processor",
     "Tessera"},
    {topology::HardwareDeviceKind::LedWall,
     topology::HardwareProcessorProtocol::Generic, "LED wall", {}},
}};

[[nodiscard]] std::size_t
palette_index(topology::HardwareDeviceKind kind,
              topology::HardwareProcessorProtocol protocol) noexcept;

enum class PortTargetState : core::u8 {
  None,
  Compatible,
  // Holds a cable from the dragged output's own device, which the drop moves.
  Replaces,
  Incompatible,
  Occupied,
};

// The feedback a cable drop on a port in `state` gives.
[[nodiscard]] HardwareLinkFeedback
link_feedback_for(PortTargetState state) noexcept;

enum class RuntimePortState : core::u8 {
  Unknown,
  Active,
  Stale,
  Missing,
};

struct PresentedHardwarePort final {
  topology::HardwarePort port{};
  const topology::HardwareVideoOutput *output{};
  RuntimePortState runtime_state{RuntimePortState::Unknown};
  // A processor input as the processor reported it.
  const topology::HardwareProcessorInput *input{};
};

struct HardwareDevicePresentation final {
  std::array<PresentedHardwarePort,
             static_cast<std::size_t>(topology::max_hardware_video_outputs) +
                 1U>
      ports{};
  std::size_t port_count{};
  core::u8 video_port_count{};
  core::u8 active_video_output_count{};
  const topology::HardwareRuntimeNode *runtime{};
  // What a monitored LED processor last reported.
  const topology::HardwareProcessorNode *processor{};
  // A Render Server no cable touches: kept on the map, not in the show.
  bool parked{};

  [[nodiscard]] const PresentedHardwarePort *
  find(core::u8 index) const noexcept {
    for (std::size_t position = 0U; position < port_count; ++position) {
      if (ports[position].port.index == index) {
        return &ports[position];
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::size_t video_ordinal(core::u8 index) const noexcept {
    std::size_t ordinal{};
    for (std::size_t position = 0U; position < port_count; ++position) {
      const topology::HardwarePort &port = ports[position].port;
      if (port.signal != topology::HardwareSignal::Video ||
          port.direction != topology::HardwarePortDirection::Output) {
        continue;
      }
      if (port.index == index) {
        return ordinal;
      }
      ++ordinal;
    }
    return 0U;
  }
};

[[nodiscard]] bool
runtime_is_current(const topology::HardwareRuntimeNode *runtime) noexcept;

[[nodiscard]] bool runtime_inventory_is_reported(
    const topology::HardwareRuntimeNode *runtime) noexcept;

[[nodiscard]] bool runtime_inventory_is_usable(
    const topology::HardwareRuntimeNode *runtime) noexcept;

[[nodiscard]] HardwareDevicePresentation hardware_presentation(
    const topology::HardwareMap &map, const topology::HardwareDevice &device,
    const topology::HardwareRuntimeSnapshot &runtime_snapshot) noexcept;

void circle(gui::UiContext &ctx, float x, float y, float radius,
            gui::UiColor color);

[[nodiscard]] float hardware_palette_width(const gui::Rect &bounds) noexcept;

[[nodiscard]] float hardware_details_width(const gui::Rect &bounds) noexcept;

// The palette's settings rows: 23px rows that touch, a 77px label column,
// 9px in from the palette's sides.
[[nodiscard]] gui::Metrics palette_metrics() noexcept;

// The palette column under the title, top to bottom: the DRAG TO PLACE
// header, a card per device kind, then PROJECT SETUP - the control port and
// the scene canvas - and SYNC POLICY. Drawing and the bounds that tests and
// hit-tests ask for both read the rows from here.
struct PaletteLayout final {
  gui::Rect column;
  gui::Rect header;
  // Card `i` is list_row(cards, i, palette_row_height, 0, card gap).
  gui::Rect cards;
  gui::Rect setup_rule;
  gui::Rect setup_heading;
  gui::FormRow control_port;
  gui::Rect setup_status;
  gui::FormRow canvas_width;
  gui::FormRow canvas_height;
  gui::Rect sync_rule;
  gui::Rect sync_heading;
  gui::Rect sync_mode;
  // Where the sync policy's own rows begin.
  float sync_rows{};
};

[[nodiscard]] PaletteLayout palette_layout(const gui::Rect &bounds) noexcept;

[[nodiscard]] gui::UiColor
device_color(const gui::Theme &theme,
             topology::HardwareDeviceKind kind) noexcept;

[[nodiscard]] gui::UiColor
signal_color(const gui::Theme &theme,
             topology::HardwareSignal signal) noexcept;

[[nodiscard]] std::string port_label(topology::HardwareDeviceKind kind,
                                     core::u8 index);

void draw_device_icon(gui::UiContext &ctx, topology::HardwareDeviceKind kind,
                      float x, float y, gui::UiColor color);

// The sprite a device is drawn with.
[[nodiscard]] HardwareSprite
sprite_for(const topology::HardwareDevice &device) noexcept;

// Screen units per sprite pixel. A map cell is 32 sprite pixels across, so
// the pixel art keeps its proportions to the floor at any zoom.
[[nodiscard]] float art_pixel(const IsometricProjection &projection) noexcept;

[[nodiscard]] IsoPoint project_point(const IsometricProjection &projection,
                                     float x, float y) noexcept;

// Where a device's sprite is drawn: its ground anchor on the device's cell.
[[nodiscard]] gui::Rect
sprite_bounds(const IsometricProjection &projection,
              const topology::HardwareDevice &device) noexcept;

// Ports run down a column on the face they plug into, this many sprite
// pixels apart, the first this far below the column's top.
inline constexpr float port_step_pixels = 7.5F;
inline constexpr float port_inset_pixels = 2.0F;

// How many ports of `direction` come before port `index` in the card.
[[nodiscard]] std::size_t
port_ordinal(const HardwareDevicePresentation &presentation, core::u8 index,
             topology::HardwarePortDirection direction) noexcept;

[[nodiscard]] IsoPoint
presented_port_position(const IsometricProjection &projection,
                        const topology::HardwareDevice &device,
                        const HardwareDevicePresentation &presentation,
                        core::u8 port_index) noexcept;

// The sprite, and below it any ports a busy machine runs on past its face.
[[nodiscard]] gui::Rect presented_device_bounds(
    const IsometricProjection &projection,
    const topology::HardwareDevice &device,
    const HardwareDevicePresentation &presentation) noexcept;

// The ground just in front of a device, where its cables reach the floor.
[[nodiscard]] float cable_ground(const IsometricProjection &projection,
                                 const topology::HardwareDevice &device) noexcept;

// A cable as it lies on the stage: out of its socket, down to the floor,
// along the two grid axes, and up into the input it plugs into.
struct CableRoute final {
  std::array<IsoPoint, 7U> points{};
};

// Each port's cable keeps a lane of its own - out a little further and down
// a little nearer for each port above it - so cables leaving one machine
// together never lie on top of one another.
[[nodiscard]] CableRoute
cable_route(const IsometricProjection &projection, IsoPoint from,
            float from_ground, std::size_t from_lane,
            const gui::Rect &from_device, IsoPoint to, float to_ground,
            std::size_t to_lane, const gui::Rect &to_device) noexcept;

[[nodiscard]] std::string
runtime_output_status(const HardwareDevicePresentation &presentation);

[[nodiscard]] std::string
runtime_card_status(const HardwareDevicePresentation &presentation);

[[nodiscard]] gui::UiColor
runtime_status_color(const gui::Theme &theme,
                     const HardwareDevicePresentation &presentation) noexcept;

// How bright sprites are drawn: as painted by day, dimmed with the night
// theme's text so the map never outshines the wall.
[[nodiscard]] gui::UiColor sprite_tint(const gui::Theme &theme,
                                       float opacity = 1.0F) noexcept;

// What a processor card says about itself, and its light.
[[nodiscard]] std::string
processor_status_text(const topology::HardwareDevice &device,
                      const topology::HardwareProcessorNode *observed);

[[nodiscard]] gui::UiColor
processor_status_color(const gui::Theme &theme,
                       const topology::HardwareDevice &device,
                       const topology::HardwareProcessorNode *observed) noexcept;

// A sprite fitted into `area`, or the kind's line icon without a sheet.
void draw_sprite_thumbnail(gui::UiContext &ctx, const gui::Rect &area,
                           const topology::HardwareDevice &device,
                           const rhi::Texture *sheet, float opacity = 1.0F);

[[nodiscard]] std::string capacity_text(core::u64 bytes);

[[nodiscard]] std::string
output_mode_text(const topology::HardwareVideoOutput &output);

[[nodiscard]] constexpr std::string_view
window_mode_label(topology::HardwareWindowMode mode) noexcept {
  switch (mode) {
  case topology::HardwareWindowMode::Inherit:
    return "INHERIT";
  case topology::HardwareWindowMode::Windowed:
    return "WINDOWED";
  case topology::HardwareWindowMode::Borderless:
    return "BORDERLESS";
  case topology::HardwareWindowMode::Fullscreen:
    return "FULLSCREEN";
  }
  return "INHERIT";
}

[[nodiscard]] constexpr std::string_view
window_mode_property(topology::HardwareWindowMode mode) noexcept {
  switch (mode) {
  case topology::HardwareWindowMode::Inherit:
    return topology::schema::inherit_window_mode;
  case topology::HardwareWindowMode::Windowed:
    return topology::schema::windowed_window_mode;
  case topology::HardwareWindowMode::Borderless:
    return topology::schema::borderless_window_mode;
  case topology::HardwareWindowMode::Fullscreen:
    return topology::schema::fullscreen_window_mode;
  }
  return topology::schema::inherit_window_mode;
}

[[nodiscard]] constexpr topology::HardwareWindowMode
next_window_mode(topology::HardwareWindowMode mode) noexcept {
  switch (mode) {
  case topology::HardwareWindowMode::Inherit:
    return topology::HardwareWindowMode::Windowed;
  case topology::HardwareWindowMode::Windowed:
    return topology::HardwareWindowMode::Borderless;
  case topology::HardwareWindowMode::Borderless:
    return topology::HardwareWindowMode::Fullscreen;
  case topology::HardwareWindowMode::Fullscreen:
    return topology::HardwareWindowMode::Inherit;
  }
  return topology::HardwareWindowMode::Inherit;
}

// A device as pixel art: its sprite from the sheet or, with no sheet (a
// headless test), a plain shaded box standing on its cell.
void draw_device_sprite(gui::UiContext &ctx,
                        const IsometricProjection &projection,
                        const topology::HardwareDevice &device,
                        const rhi::Texture *sheet, float opacity);

// The stage the hardware stands on: a raised platform of floor tiles, each
// cell centred on a grid point, reaching three cells past every device.
// `highlight` is a cell to light (where a drag would put something);
// `selected` a cell to ring (the selected device's).
void draw_floor(gui::UiContext &ctx, const IsometricProjection &projection,
                const topology::HardwareMap &map,
                std::optional<std::pair<core::i32, core::i32>> highlight,
                std::optional<std::pair<core::i32, core::i32>> selected);

// A device's name tag above its sprite: a status light and its name, and
// on hover or selection a second line saying what state it is in.
void draw_nameplate(gui::UiContext &ctx, const gui::Rect &sprite,
                    std::string_view name, gui::UiColor light,
                    std::string_view status, bool selected, bool hovered);

// A port as a jack on the device's face: a dark socket in a rim of its
// signal's colour. A cable drag lights the jacks it may end on.
void draw_socket(gui::UiContext &ctx, const IsoPoint &point,
                 const topology::HardwarePort &port, float px, bool pending,
                 PortTargetState target_state,
                 RuntimePortState runtime_state = RuntimePortState::Unknown);

// A cable's run along the floor, or the short risers at either end that
// climb the device faces to their sockets.
enum class CablePart : core::u8 {
  Floor,
  Risers,
};

void draw_cable(gui::UiContext &ctx, const CableRoute &route, float px,
                gui::UiColor color, bool selected, CablePart part);

[[nodiscard]] float point_segment_distance(float px, float py, float x0,
                                           float y0, float x1,
                                           float y1) noexcept;

[[nodiscard]] bool cable_hit(const CableRoute &route, float x,
                             float y) noexcept;

// A small amber triangle with a bang, where a cable's signal is wrong.
void draw_signal_badge(gui::UiContext &ctx, const IsoPoint &at,
                       gui::UiColor color);

void draw_wire(gui::UiContext &ctx, const IsoPoint &from, const IsoPoint &to,
               gui::UiColor color, bool selected);

[[nodiscard]] const topology::HardwareLink *
link_into(const topology::HardwareMap &map,
          topology::HardwareEndpoint endpoint) noexcept;

[[nodiscard]] std::string hardware_count_text(std::size_t count);

inline constexpr float discovery_header_height = 24.0F;
inline constexpr float discovery_card_width = 214.0F;
inline constexpr float discovery_card_height = 52.0F;
inline constexpr float discovery_card_gap = 6.0F;
// The strip gives way rather than leave the map less room than this.
inline constexpr float discovery_minimum_view_height = 140.0F;

enum class DiscoveredTone : core::u8 {
  Add,
  OnMap,
  Warning,
};

struct DiscoveredStatus final {
  std::string text;
  DiscoveredTone tone{DiscoveredTone::Add};
  // What a click selects instead of adding another card: the machine's own
  // card, or the one already using its node index.
  const topology::HardwareDevice *existing{};
};

[[nodiscard]] bool local_address(std::string_view address) noexcept;

[[nodiscard]] bool has_render_server(const topology::HardwareMap &map) noexcept;

// With no Render Server on the map yet nothing depends on the project's
// control range, so the first machine placed may move it to where that
// machine actually listens.
[[nodiscard]] std::optional<core::u32>
adopted_control_base(const topology::HardwareMap &map,
                     const topology::HardwareDiscoveredMachine &machine) noexcept;

[[nodiscard]] DiscoveredStatus
discovered_status(const topology::HardwareMap &map,
                  const topology::HardwareDiscovery &discovery,
                  const topology::HardwareDiscoveredMachine &machine);

// The processor card already at the address a search found it on.
[[nodiscard]] const topology::HardwareDevice *
discovered_processor_device(
    const topology::HardwareMap &map,
    const topology::HardwareDiscoveredProcessor &found) noexcept;

// A found processor's identity in the strip: its address and protocol, so a
// card keeps hold of the pointer while the list around it changes.
[[nodiscard]] gui::UiId
discovered_processor_id(gui::UiId panel,
                        const topology::HardwareDiscoveredProcessor &found) noexcept;

[[nodiscard]] core::u64
discovered_key(const topology::HardwareDiscoveredMachine &machine) noexcept;

// The first cell clear of every card, in rows below the origin: where a click
// puts a detected machine. Cards are compared in screen-aligned grid units -
// across (x - y) and down (x + y) - each half a tile.
[[nodiscard]] std::pair<core::i32, core::i32>
free_grid_cell(const topology::HardwareMap &map) noexcept;


} // namespace mediaserver::editor::mapping::hardware
