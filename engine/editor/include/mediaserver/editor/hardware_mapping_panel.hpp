#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/canvas_view.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/output/feed_layout.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/render/mapped_scene.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/sync/sync_policy.hpp>
#include <mediaserver/topology/hardware_map.hpp>
#include <mediaserver/topology/hardware_runtime.hpp>
#include <mediaserver/topology/hardware_walls.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Hardware Mapping panel: the isometric map of render machines, LED
// processors and the cables between them.
//
// These panels return small edit intents instead of mutating a document. That
// keeps them useful in a headless test and leaves command/undo ownership with
// EditorShell when it integrates them. Runtime hardware observations are a
// separate borrowed view: drawing them cannot mutate the authored topology.
namespace mediaserver::editor {

struct IsoPoint final {
  float x{};
  float y{};

  bool operator==(const IsoPoint &) const = default;
};

struct IsometricProjection final {
  float origin_x{};
  float origin_y{};
  float tile_width{64.0F};
  float tile_height{32.0F};

  bool operator==(const IsometricProjection &) const = default;
};

[[nodiscard]] IsoPoint project_isometric(const IsometricProjection &projection,
                                         core::i32 grid_x,
                                         core::i32 grid_y) noexcept;

[[nodiscard]] std::pair<core::i32, core::i32>
unproject_isometric(const IsometricProjection &projection, float screen_x,
                    float screen_y) noexcept;

[[nodiscard]] gui::Rect
hardware_mapping_canvas(const gui::Rect &bounds) noexcept;

[[nodiscard]] IsometricProjection
hardware_mapping_projection(const gui::Rect &bounds) noexcept;

[[nodiscard]] gui::Rect
hardware_palette_item_bounds(const gui::Rect &bounds,
                             topology::HardwareDeviceKind kind) noexcept;
// The palette entry for a kind that arrives speaking a protocol: the
// NovaStar processor is an LED Processor with NovaStar COEX already set.
[[nodiscard]] gui::Rect hardware_palette_item_bounds(
    const gui::Rect &bounds, topology::HardwareDeviceKind kind,
    topology::HardwareProcessorProtocol protocol) noexcept;

// The MODE cycle button of the SYNC POLICY section in the palette rail.
// Exposed as a pure presentation query, like the palette item bounds above,
// so a headless test can drive the control without duplicating the panel
// layout constants.
[[nodiscard]] gui::Rect
hardware_sync_mode_button_bounds(const gui::Rect &bounds) noexcept;

// The Control port field of PROJECT SETUP.
[[nodiscard]] gui::Rect
hardware_control_port_field_bounds(const gui::Rect &bounds) noexcept;

// The Canvas W (or, with `height`, Canvas H) field of PROJECT SETUP.
[[nodiscard]] gui::Rect
hardware_canvas_field_bounds(const gui::Rect &bounds, bool height) noexcept;

// The DETECTED ON NETWORK strip along the bottom of the canvas, and one of
// its cards (unscrolled): render machines first, then LED processors a
// search found. Empty while neither discovery nor processor search is
// available, or when the canvas is too short to give it room. The map is
// navigated and edited only above it.
[[nodiscard]] gui::Rect
hardware_discovery_strip_bounds(const gui::Rect &bounds,
                                const topology::HardwareDiscovery &discovery) noexcept;

[[nodiscard]] gui::Rect
hardware_discovered_card_bounds(const gui::Rect &bounds,
                                const topology::HardwareDiscovery &discovery,
                                std::size_t index) noexcept;

// The Render Server card already describing a discovered machine: the same
// node index at one of the addresses it answered from (a blank or loopback
// address for this machine). Null when it is not on the map.
[[nodiscard]] const topology::HardwareDevice *
hardware_discovered_device(const topology::HardwareMap &map,
                           const topology::HardwareDiscoveredMachine &machine) noexcept;

// The warning a detected machine's card shows beneath its address: why it
// would not work as placed, or nothing when it is on the map or would.
[[nodiscard]] std::string
hardware_discovered_status_text(const topology::HardwareMap &map,
                                const topology::HardwareDiscovery &discovery,
                                const topology::HardwareDiscoveredMachine &machine);

[[nodiscard]] gui::Rect
hardware_device_bounds(const IsometricProjection &projection,
                       const topology::HardwareDevice &device) noexcept;

[[nodiscard]] IsoPoint
hardware_port_position(const IsometricProjection &projection,
                       const topology::HardwareDevice &device,
                       core::u8 port) noexcept;

// Runtime-aware presentation geometry. These helpers use the same union as the
// panel: observed active Render Server outputs, output slots referenced by
// authored cables, and legacy slot one. The authored device itself is never
// modified.
[[nodiscard]] gui::Rect hardware_presented_device_bounds(
    const IsometricProjection &projection, const topology::HardwareMap &map,
    const topology::HardwareDevice &device,
    const topology::HardwareRuntimeSnapshot &runtime) noexcept;

[[nodiscard]] IsoPoint hardware_presented_port_position(
    const IsometricProjection &projection, const topology::HardwareMap &map,
    const topology::HardwareDevice &device,
    const topology::HardwareRuntimeSnapshot &runtime, core::u8 port) noexcept;

// The corner where a cable turns on the stage floor: a point on the cable
// away from either device, where its signal warning is drawn. Nothing for a
// cable the map does not have.
[[nodiscard]] std::optional<IsoPoint> hardware_cable_point(
    const IsometricProjection &projection, const topology::HardwareMap &map,
    const topology::HardwareRuntimeSnapshot &runtime, doc::NodeId link) noexcept;

// Exact card status used by the runtime-aware panel. Exposed as a pure
// presentation query so tests and accessibility surfaces can preserve the
// distinction between a detected zero-output machine and unavailable
// inventory.
[[nodiscard]] std::string
hardware_runtime_status_text(const topology::HardwareMap &map,
                             const topology::HardwareDevice &device,
                             const topology::HardwareRuntimeSnapshot &runtime);

// Explains which settings own the Render Server presenter. The details rail
// reserves this row in every mode so cycling modes does not move the placement
// controls out from under the pointer.
[[nodiscard]] std::string_view hardware_window_settings_source_text(
    topology::HardwareWindowMode mode) noexcept;

enum class HardwareSelectionKind : core::u8 {
  None,
  Device,
  Link,
};

struct HardwareSelection final {
  HardwareSelectionKind kind{HardwareSelectionKind::None};
  doc::NodeId node{};

  bool operator==(const HardwareSelection &) const = default;
};

// A render machine found on the network. The card placed for it starts out
// describing that machine instead of a blank Render Server.
struct HardwarePlaceMachine final {
  std::string name;
  std::string address;
  core::u32 node_index{};

  bool operator==(const HardwarePlaceMachine &) const = default;
};

struct HardwarePlaceRequest final {
  topology::HardwareDeviceKind kind{topology::HardwareDeviceKind::RenderServer};
  core::i32 grid_x{};
  core::i32 grid_y{};
  core::u64 gesture{};
  // A render machine, or a processor found on the network: the card starts
  // with its name and address (and, for a machine, its node index).
  std::optional<HardwarePlaceMachine> machine;
  // LED Processors only: the protocol the card is reached by.
  topology::HardwareProcessorProtocol protocol{
      topology::HardwareProcessorProtocol::Generic};

  bool operator==(const HardwarePlaceRequest &) const = default;
};

struct HardwareMoveRequest final {
  doc::NodeId device{};
  core::i32 grid_x{};
  core::i32 grid_y{};
  core::u64 gesture{};

  bool operator==(const HardwareMoveRequest &) const = default;
};

struct HardwareLinkRequest final {
  topology::HardwareEndpoint from;
  topology::HardwareEndpoint to;
  core::u64 gesture{};
  // A Render Server video pin carries its persistent display identity with
  // the cable gesture. A zero binding node creates the binding; a non-zero
  // node reuses it and can recreate a missing Feed Mapping region. The shell
  // assigns a free feed index for new bindings and fills output_region from
  // the current project canvas before this reaches the authoring boundary.
  std::optional<topology::HardwareOutputBinding> output_binding;
  scene::CanvasRect output_region{};
  // The cable this one replaces: the same device's cable already in `to`,
  // removed in the same gesture so the device never goes unwired (a parked
  // Render Server stops being probed, and its discovered outputs vanish).
  doc::NodeId replaces{};

  bool operator==(const HardwareLinkRequest &) const = default;
};

// Immediate authoring feedback for the cable gesture. Invalid drop states are
// kept distinct so an occupied input is not misreported as a signal mismatch.
enum class HardwareLinkFeedback : core::u8 {
  None,
  AwaitingTarget,
  Dragging,
  Compatible,
  // The input holds a cable from the same device; dropping moves it here.
  Replaces,
  Incompatible,
  Occupied,
  Missed,
};

// One committed edit from the selected device's details rail. The property is
// drawn from topology::schema and the value keeps the document's native scalar
// type, so names/addresses stay strings while raster/timing fields stay exact
// integers.
// The property of a HardwarePropertyEdit that sets a card's refresh rate,
// its value hertz as a double (23.976). The authoring boundary writes it as
// refresh_numerator and refresh_denominator together: the exact fraction
// topology::refresh_from_hertz gives, so 23.976 is 24000/1001.
inline constexpr std::string_view hardware_refresh_edit = "refresh";

struct HardwarePropertyEdit final {
  doc::NodeId device{};
  std::string property;
  doc::PropertyValue value;
  core::u64 gesture{};

  bool operator==(const HardwarePropertyEdit &) const = default;
};

// The part of its Render Server output a selected cable carries, set from the
// cable's details. An empty region puts the cable back on its share.
struct HardwareLinkRegionEdit final {
  doc::NodeId link{};
  topology::HardwareOutputRegion region{};
  core::u64 gesture{};

  bool operator==(const HardwareLinkRegionEdit &) const = default;
};

// Wall cards for the screens a processor reported, each cabled from its LED
// OUT, placed as one undoable step. The authoring boundary places nothing
// when that LED OUT has been cabled since.
struct HardwareWallsRequest final {
  doc::NodeId processor{};
  std::vector<topology::HardwareWallProposal> walls;
  core::u64 gesture{};

  bool operator==(const HardwareWallsRequest &) const = default;
};

// The lifecycle listener range is one project setting, not text repeated on
// every Render Server. A zero settings node asks the authoring boundary to
// create hardware.settings; otherwise the existing node is edited. Keeping
// this distinct from HardwarePropertyEdit prevents a settings node from being
// mistaken for a device by selection, deletion, or playback integration.
struct HardwareControlPortEdit final {
  doc::NodeId settings{};
  core::u32 base_port{topology::default_hardware_control_base_port};
  core::u64 gesture{};

  bool operator==(const HardwareControlPortEdit &) const = default;
};

// The pixel size of the picture every scene renders. It is authored with the
// hardware because canvas feed mappings crop it: a wall wider than one
// connector needs a canvas as large as the wall's whole raster. The authoring
// boundary writes it to every scene, since a Program transition is only
// defined between scenes that share one canvas.
struct HardwareCanvasEdit final {
  scene::Canvas canvas{};
  core::u64 gesture{};

  bool operator==(const HardwareCanvasEdit &) const = default;
};

// One explicit process/feed/display deployment binding. A zero binding node
// creates a new persistent hardware.output_binding; a non-zero node replaces
// that binding atomically. The observed target identity is copied only after
// an operator presses Bind/Repair -- discovery never rewrites project state.
struct HardwareOutputBindingEdit final {
  topology::HardwareOutputBinding binding;
  core::u64 gesture{};

  bool operator==(const HardwareOutputBindingEdit &) const = default;
};

struct HardwareOutputBindingDelete final {
  doc::NodeId binding{};
  core::u64 gesture{};

  bool operator==(const HardwareOutputBindingDelete &) const = default;
};

// The project-wide output synchronization policy, edited as one value.
//
// It lives beside the Control port because both are project settings rather
// than per-device state, and because the sync groups this policy governs are
// derived from the cabling drawn in this very panel. A zero policy node asks
// the authoring boundary to create sync.policy; otherwise that node is
// replaced atomically.
struct HardwareSyncPolicyEdit final {
  doc::NodeId policy{};
  sync::SyncPolicy value{};
  core::u64 gesture{};

  bool operator==(const HardwareSyncPolicyEdit &) const = default;
};

// Asks an LED processor to offer one mode on one input, so the render
// machine driving it is offered the mode its output is set to. A request to
// the device, not a document edit: nothing undoes it, and the outcome comes
// back as the processor's notice.
struct HardwareProcessorEdidRequest final {
  doc::NodeId processor{};
  // The VIDEO IN number on the processor's card.
  core::u8 input{};
  core::u32 width{};
  core::u32 height{};
  core::u32 refresh_millihertz{};

  bool operator==(const HardwareProcessorEdidRequest &) const = default;
};

// Removing a device also requires the authoring boundary to erase its incident
// cable nodes. The tagged selection preserves that distinction without making
// the panel depend on document command types.
struct HardwareDeleteRequest final {
  HardwareSelection target;
  core::u64 gesture{};

  bool operator==(const HardwareDeleteRequest &) const = default;
};

struct HardwareMappingFrame final {
  // Set on every explicit device/cable selection click, including a re-click,
  // so the shell can make Hardware the active Delete-key domain.
  std::optional<HardwareSelection> selection;
  std::optional<HardwarePlaceRequest> place;
  std::optional<HardwareMoveRequest> move;
  std::optional<HardwareLinkRequest> link;
  std::optional<HardwarePropertyEdit> property_edit;
  std::optional<HardwareLinkRegionEdit> link_region_edit;
  std::optional<HardwareControlPortEdit> control_port_edit;
  std::optional<HardwareCanvasEdit> canvas_edit;
  std::optional<HardwareSyncPolicyEdit> sync_policy_edit;
  std::optional<HardwareOutputBindingEdit> output_binding_edit;
  std::optional<HardwareOutputBindingDelete> output_binding_delete;
  // The operator pressed Search for LED processors.
  bool search_processors{};
  std::optional<HardwareProcessorEdidRequest> processor_edid;
  // Emitted by the visible REMOVE DEVICE / DISCONNECT CABLE action. For a
  // device, the authoring layer should erase incident link nodes in the same
  // transaction before erasing the device node.
  std::optional<HardwareDeleteRequest> remove;
  // Delete was handled by this focused panel, so the shell must not also
  // delete the current scene selection.
  bool consume_delete{};
  HardwareLinkFeedback link_feedback{HardwareLinkFeedback::None};
  bool incompatible_link_attempted{};
  bool dragging{};
  bool editing{};
  bool navigating{};
};

class HardwareMappingPanel final {
public:
  explicit HardwareMappingPanel(gui::UiId id) noexcept : id_(id) {}

  [[nodiscard]] HardwareMappingFrame draw(gui::UiContext &ctx,
                                          const gui::Rect &bounds,
                                          const topology::HardwareMap &map);

  [[nodiscard]] HardwareMappingFrame
  draw(gui::UiContext &ctx, const gui::Rect &bounds,
       const topology::HardwareMap &map,
       const topology::HardwareRuntimeSnapshot &runtime);

  // The full surface: authored topology, transient runtime observations, and
  // the project synchronization policy those cables are validated against.
  [[nodiscard]] HardwareMappingFrame
  draw(gui::UiContext &ctx, const gui::Rect &bounds,
       const topology::HardwareMap &map,
       const topology::HardwareRuntimeSnapshot &runtime,
       const sync::SyncPolicy &sync_policy);

  // As above, plus the canvas the project's scenes render. A zero canvas
  // means there is no scene to write it to, and its fields are not offered.
  [[nodiscard]] HardwareMappingFrame
  draw(gui::UiContext &ctx, const gui::Rect &bounds,
       const topology::HardwareMap &map,
       const topology::HardwareRuntimeSnapshot &runtime,
       const sync::SyncPolicy &sync_policy, scene::Canvas canvas);

  [[nodiscard]] HardwareSelection selection() const noexcept {
    return selection_;
  }
  // Selects a device or cable as a click on it would.
  void select(HardwareSelection selection) noexcept { selection_ = selection; }
  [[nodiscard]] std::optional<topology::HardwareEndpoint>
  pending_link() const noexcept {
    return pending_link_;
  }
  // The panel-local view transform. The standalone projection helper remains
  // the reset/default view for existing callers and authored tests.
  [[nodiscard]] IsometricProjection
  view_projection(const gui::Rect &bounds) const noexcept;
  [[nodiscard]] float view_zoom() const noexcept { return view_.zoom; }

  // The pixel-art sheet devices are drawn from (see hardware_sprites.hpp),
  // owned by whoever owns the device. Without one - a headless test - each
  // device is a plain shaded box.
  void set_sprites(const rhi::Texture *sheet) noexcept { sprites_ = sheet; }

  // Centres the map on its devices, zoomed out until they all fit, on the
  // next draw. The shell asks when the panel opens; the map's Fit button
  // asks too.
  void frame_devices() noexcept { frame_pending_ = true; }

  // Drops document-owned interaction state after replacing the project.
  // View pan/zoom is editor-local and intentionally survives.
  void reset_project_state() noexcept {
    selection_ = {};
    pending_link_.reset();
    drag_kind_ = DragKind::None;
    drag_device_ = {};
    link_dragging_ = false;
    link_drag_moved_ = false;
    link_press_cancels_ = false;
    link_notice_ = HardwareLinkFeedback::None;
    panning_ = false;
    details_node_ = {};
    details_name_.clear();
    details_address_.clear();
    details_name_dirty_ = false;
    details_address_dirty_ = false;
    details_output_binding_ = {};
    focused_ = false;
    details_numbers_ = NumberField{};
    settings_numbers_ = NumberField{};
    canvas_numbers_ = NumberField{};
    sync_numbers_ = NumberField{};
    link_numbers_ = NumberField{};
  }

private:
  enum class DragKind : core::u8 {
    None,
    Palette,
    Device,
    // A card from the DETECTED ON NETWORK strip.
    Discovered,
    // A processor card from the same strip.
    DiscoveredProcessor,
  };

  // One draw as an object, and its phases in the order draw() runs them.
  // See src/mapping/hardware_mapping_frame.hpp.
  struct Frame;
  [[nodiscard]] bool begin_frame(Frame &f);
  void navigate(Frame &f);
  void draw_map_floor(Frame &f);
  void draw_palette(Frame &f);
  void draw_project_setup(Frame &f);
  void draw_sync_policy(Frame &f);
  void interact_ports(Frame &f);
  void move_devices(Frame &f);
  void pick_cables(Frame &f);
  void draw_stage(Frame &f);
  void draw_cable_prompt(Frame &f);
  void draw_navigation_hint(Frame &f);
  [[nodiscard]] std::optional<gui::Interaction> draw_discovery_strip(Frame &f);
  void drop_discovered(Frame &f, std::optional<gui::Interaction> dragged);
  void draw_details(Frame &f);
  struct DetailsRows;
  void draw_device_details(Frame &f, const gui::Rect &details,
                           const topology::HardwareDevice *selected_device);
  // Returns whether the server's outputs are bound explicitly, in which
  // case they, not the device, carry the raster.
  [[nodiscard]] bool
  draw_server_outputs(Frame &f, DetailsRows &rows,
                      const topology::HardwareDevice *selected_device);
  void draw_server_status(Frame &f, DetailsRows &rows,
                          const topology::HardwareDevice *selected_device);
  void draw_processor_monitor(Frame &f, DetailsRows &rows,
                              const topology::HardwareDevice *selected_device);
  void draw_cable_details(Frame &f, const gui::Rect &details);

  gui::UiId id_{};
  HardwareSelection selection_{};
  std::optional<topology::HardwareEndpoint> pending_link_;
  DragKind drag_kind_{DragKind::None};
  topology::HardwareDeviceKind palette_kind_{
      topology::HardwareDeviceKind::RenderServer};
  topology::HardwareProcessorProtocol palette_protocol_{
      topology::HardwareProcessorProtocol::Generic};
  const rhi::Texture *sprites_{};
  bool frame_pending_{};
  doc::NodeId drag_device_{};
  core::i32 drag_original_x_{};
  core::i32 drag_original_y_{};
  core::i32 preview_x_{};
  core::i32 preview_y_{};
  float drag_offset_x_{};
  float drag_offset_y_{};

  bool link_dragging_{};
  bool link_drag_moved_{};
  bool link_press_cancels_{};
  float link_drag_start_x_{};
  float link_drag_start_y_{};
  HardwareLinkFeedback link_notice_{HardwareLinkFeedback::None};

  // The map's pan and zoom, about the default projection's origin.
  gui::CanvasView view_{};
  bool panning_{};

  // A copy, so a machine that stops answering mid-drag can still be dropped.
  topology::HardwareDiscoveredMachine discovered_drag_;
  topology::HardwareDiscoveredProcessor discovered_processor_drag_;
  float discovered_press_x_{};
  float discovered_press_y_{};
  float discovered_scroll_{};

  doc::NodeId details_node_{};
  std::string details_name_;
  std::string details_address_;
  bool details_name_dirty_{};
  bool details_address_dirty_{};
  doc::NodeId details_output_binding_{};
  bool focused_{};
  NumberField details_numbers_;
  NumberField settings_numbers_;
  NumberField canvas_numbers_;
  NumberField sync_numbers_;
  // The selected cable's region of its output.
  NumberField link_numbers_;
};

} // namespace mediaserver::editor
