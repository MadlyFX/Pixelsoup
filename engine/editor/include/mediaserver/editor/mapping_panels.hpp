#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/render/mapped_scene.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/sync/sync_policy.hpp>
#include <mediaserver/topology/hardware_map.hpp>
#include <mediaserver/topology/hardware_runtime.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// Authoring surfaces for the document-backed hardware and output maps.
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

// The MODE cycle button of the SYNC POLICY section in the palette rail.
// Exposed as a pure presentation query, like the palette item bounds above,
// so a headless test can drive the control without duplicating the panel
// layout constants.
[[nodiscard]] gui::Rect
hardware_sync_mode_button_bounds(const gui::Rect &bounds) noexcept;

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

struct HardwarePlaceRequest final {
  topology::HardwareDeviceKind kind{topology::HardwareDeviceKind::RenderServer};
  core::i32 grid_x{};
  core::i32 grid_y{};
  core::u64 gesture{};

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

  bool operator==(const HardwareLinkRequest &) const = default;
};

// Immediate authoring feedback for the cable gesture. Invalid drop states are
// kept distinct so an occupied input is not misreported as a signal mismatch.
enum class HardwareLinkFeedback : core::u8 {
  None,
  AwaitingTarget,
  Dragging,
  Compatible,
  Incompatible,
  Occupied,
  Missed,
};

// One committed edit from the selected device's details rail. The property is
// drawn from topology::schema and the value keeps the document's native scalar
// type, so names/addresses stay strings while raster/timing fields stay exact
// integers.
struct HardwarePropertyEdit final {
  doc::NodeId device{};
  std::string property;
  doc::PropertyValue value;
  core::u64 gesture{};

  bool operator==(const HardwarePropertyEdit &) const = default;
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
  std::optional<HardwareControlPortEdit> control_port_edit;
  std::optional<HardwareSyncPolicyEdit> sync_policy_edit;
  std::optional<HardwareOutputBindingEdit> output_binding_edit;
  std::optional<HardwareOutputBindingDelete> output_binding_delete;
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

  [[nodiscard]] HardwareSelection selection() const noexcept {
    return selection_;
  }
  [[nodiscard]] std::optional<topology::HardwareEndpoint>
  pending_link() const noexcept {
    return pending_link_;
  }
  // The panel-local view transform. The standalone projection helper remains
  // the reset/default view for existing callers and authored tests.
  [[nodiscard]] IsometricProjection
  view_projection(const gui::Rect &bounds) const noexcept;
  [[nodiscard]] float view_zoom() const noexcept { return view_zoom_; }

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
    sync_numbers_ = NumberField{};
  }

private:
  enum class DragKind : core::u8 {
    None,
    Palette,
    Device,
  };

  gui::UiId id_{};
  HardwareSelection selection_{};
  std::optional<topology::HardwareEndpoint> pending_link_;
  DragKind drag_kind_{DragKind::None};
  topology::HardwareDeviceKind palette_kind_{
      topology::HardwareDeviceKind::RenderServer};
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

  float view_zoom_{1.0F};
  float view_pan_x_{};
  float view_pan_y_{};
  bool panning_{};

  doc::NodeId details_node_{};
  std::string details_name_;
  std::string details_address_;
  bool details_name_dirty_{};
  bool details_address_dirty_{};
  doc::NodeId details_output_binding_{};
  bool focused_{};
  NumberField details_numbers_;
  NumberField settings_numbers_;
  NumberField sync_numbers_;
};

// The canvas-sized rectangle fitted into the visual editor area. A zero canvas
// returns an empty rectangle at the centre of the area.
[[nodiscard]] gui::Rect fit_feed_canvas(const gui::Rect &area,
                                        scene::Canvas canvas) noexcept;

// The fitted canvas used by FeedMappingPanel for these outer bounds.
[[nodiscard]] gui::Rect feed_mapping_canvas(const gui::Rect &bounds,
                                            scene::Canvas canvas) noexcept;

[[nodiscard]] gui::Rect feed_map_screen_rect(const gui::Rect &fitted_canvas,
                                             scene::Canvas canvas,
                                             scene::CanvasRect source) noexcept;

// The explicit operator identity used for a feed-map row and its projection
// pane. FEED is deliberately one-based for people; FEED INDEX is the exact
// zero-based value selected by a persistent Hardware output binding. Legacy
// projects still select the same value directly from process --node-index.
[[nodiscard]] std::string feed_output_identity(core::u32 node_index);

// Physical ownership is assigned in Hardware Mapping instead of inferred from
// this feed's numeric index.
[[nodiscard]] std::string feed_runtime_binding(core::u32 node_index);

struct FeedMapRectEdit final {
  doc::NodeId node{};
  // Destination viewport inside this feed's fixed application window. Source
  // cropping/projection is a separate FeedProjectionEdit concern.
  scene::CanvasRect viewport{};
  core::u64 gesture{};

  bool operator==(const FeedMapRectEdit &) const = default;
};

// A persistent viewport row to create inside one logical output window. The
// authoring boundary allocates its stable document identity and undo gesture.
struct FeedMapAddRequest final {
  core::u32 node_index{};
  scene::CanvasRect source{};
  scene::CanvasRect viewport{};
  core::u64 gesture{};

  bool operator==(const FeedMapAddRequest &) const = default;
};

// A document-backed viewport row to erase by stable identity.
struct FeedMapDeleteRequest final {
  doc::NodeId node{};
  core::u64 gesture{};

  bool operator==(const FeedMapDeleteRequest &) const = default;
};

// Changes which image-space an output region belongs to. Canvas regions crop
// the shared scene camera; physical regions partition a reusable calibrated
// LED surface and derive an off-axis view from it at presentation time.
struct FeedProjectionEdit final {
  doc::NodeId node{};
  output::FeedProjection projection{output::FeedProjection::CanvasSlice};
  doc::NodeId surface{};
  scene::CanvasRect source{};
  core::u64 gesture{};

  bool operator==(const FeedProjectionEdit &) const = default;
};

struct FeedSurfaceAddRequest final {
  std::string name;
  output::PhysicalSurface surface{};
  core::u64 gesture{};
  output::ProjectionSurfaceShape shape{output::ProjectionSurfaceShape::Planar};
  float horizontal_arc_turns{0.25F};
  core::u32 mesh_columns{32U};
  std::string mesh_source;
  float mesh_scale{1.0F};
  // Imported patches are normally one file per feed. When present, playback
  // creates the surface and assigns this existing output map in the same undo
  // transaction. An unselected import remains a reusable unassigned surface.
  std::optional<doc::NodeId> assign_map_node;

  bool operator==(const FeedSurfaceAddRequest &) const = default;
};

// Which authored transform components a surface edit is allowed to replace.
// A partial mask is important once a surface is animated: the evaluated value
// of an animated axis must never be baked into another axis's authored base.
enum class FeedSurfaceTransform : core::u8 {
  None = 0U,
  PositionX = 1U << 0U,
  PositionY = 1U << 1U,
  PositionZ = 1U << 2U,
  Yaw = 1U << 3U,
  All = (1U << 0U) | (1U << 1U) | (1U << 2U) | (1U << 3U),
};

[[nodiscard]] constexpr FeedSurfaceTransform
operator|(FeedSurfaceTransform left, FeedSurfaceTransform right) noexcept {
  return static_cast<FeedSurfaceTransform>(static_cast<core::u8>(left) |
                                           static_cast<core::u8>(right));
}

constexpr FeedSurfaceTransform &operator|=(FeedSurfaceTransform &left,
                                            FeedSurfaceTransform right) noexcept {
  left = left | right;
  return left;
}

[[nodiscard]] constexpr bool has_transform(
    FeedSurfaceTransform mask, FeedSurfaceTransform component) noexcept {
  return (static_cast<core::u8>(mask) & static_cast<core::u8>(component)) != 0U;
}

struct FeedSurfaceEditRequest final {
  doc::NodeId node{};
  std::string name;
  output::PhysicalSurface surface{};
  core::u64 gesture{};
  output::ProjectionSurfaceShape shape{output::ProjectionSurfaceShape::Planar};
  float horizontal_arc_turns{0.25F};
  core::u32 mesh_columns{32U};
  std::string mesh_source;
  float mesh_scale{1.0F};
  // All keeps the legacy full-edit contract. Transform-only controls set the
  // exact changed bits; non-transform controls set None so playback preserves
  // every authored centre/orientation property while updating calibration.
  FeedSurfaceTransform transform_components{FeedSurfaceTransform::All};
  std::optional<scene::WallColorSettings> color;

  bool operator==(const FeedSurfaceEditRequest &) const = default;
};

// Surface removal deliberately does not carry output regions. The document
// keeps their references dangling so the mapping panel can identify and repair
// a missing calibration instead of silently changing what a connector shows.
struct FeedSurfaceDeleteRequest final {
  doc::NodeId node{};
  core::u64 gesture{};

  bool operator==(const FeedSurfaceDeleteRequest &) const = default;
};

struct FeedSurfaceLockRequest final {
  doc::NodeId node{};
  bool locked{};

  bool operator==(const FeedSurfaceLockRequest &) const = default;
};

struct FeedMappingFrame final {
  // Set on every explicit row or rectangle selection, including a re-click,
  // so the shell can make Feed Mapping the active Delete-key domain.
  std::optional<doc::NodeId> selection;
  // Explicit surface focus, separate from the panel's automatic details-row
  // fallback. This lets the Stage viewport mirror operator selection without
  // stealing scene-object focus merely because surfaces exist.
  std::optional<doc::NodeId> surface_selection;
  std::optional<FeedMapAddRequest> add;
  // A move or resize is authored once, on pointer release.
  std::optional<FeedMapRectEdit> edit;
  std::optional<FeedMapDeleteRequest> remove;
  std::optional<FeedProjectionEdit> projection_edit;
  std::optional<FeedSurfaceAddRequest> surface_add;
  std::optional<FeedSurfaceEditRequest> surface_edit;
  // A request to choose or replace the selected surface's UV mesh. The panel
  // cannot own a native dialog; EditorShell validates the chosen file and
  // turns this full value into a regular surface edit only on success.
  std::optional<FeedSurfaceEditRequest> mesh_browse;
  std::optional<FeedSurfaceDeleteRequest> surface_remove;
  std::optional<FeedSurfaceLockRequest> surface_lock;
  // Delete was handled by this focused panel, so the shell must not also
  // delete the current scene selection.
  bool consume_delete{};
  bool dragging{};
  bool editing{};
};

class FeedMappingPanel;

namespace detail {

// Deterministic test seam for the selected UV-preview cache. Production draw
// never waits for background asset work.
[[nodiscard]] bool wait_for_feed_mapping_projection_cache_for_test(
    FeedMappingPanel &panel) noexcept;

} // namespace detail

class FeedMappingPanel final {
public:
  explicit FeedMappingPanel(gui::UiId id) noexcept : id_(id) {}

  [[nodiscard]] FeedMappingFrame draw(gui::UiContext &ctx,
                                       const gui::Rect &bounds,
                                       scene::Canvas canvas,
                                       const output::FeedMap &feed_map,
                                       core::u64 show_frame = 0U,
                                       const topology::HardwareMap *hardware =
                                           nullptr);

  [[nodiscard]] std::optional<doc::NodeId> selected_map_node() const noexcept {
    return selected_map_node_;
  }

  [[nodiscard]] std::optional<doc::NodeId>
  selected_surface_node() const noexcept {
    return selected_surface_node_;
  }

  // The Stage viewport and this setup panel are two views of the same
  // calibration selection. The document is still the authority; these only
  // keep both views focused on the same surface while a live gesture is
  // waiting for its authored value to evaluate back through the model.
  void select_surface(doc::NodeId node) noexcept {
    selected_surface_node_ = node == doc::NodeId{}
                                 ? std::optional<doc::NodeId>{}
                                 : std::optional<doc::NodeId>{node};
  }

  void preview_surface_geometry(doc::NodeId node,
                                output::PhysicalSurface geometry) noexcept {
    selected_surface_node_ = node;
    if (surface_details_node_ == node) {
      surface_geometry_ = geometry;
    }
  }

  void reset_project_state() noexcept {
    selected_map_node_.reset();
    selected_surface_node_.reset();
    drag_kind_ = DragKind::None;
    surface_drag_node_ = {};
    surface_drag_moved_ = false;
    surface_details_node_ = {};
    surface_details_show_frame_ = 0U;
    surface_name_.clear();
    surface_name_dirty_ = false;
    surface_shape_ = output::ProjectionSurfaceShape::Planar;
    surface_horizontal_arc_turns_ = 0.25F;
    surface_mesh_columns_ = 32U;
    surface_mesh_source_.clear();
    surface_mesh_scale_ = 1.0F;
    preview_mesh_.reset();
    preview_projection_cache_.reset();
    mesh_browse_pending_ = false;
    surface_numbers_ = NumberField{};
    focused_ = false;
  }

private:
  enum class DragKind : core::u8 {
    None,
    Move,
    Resize,
  };

  gui::UiId id_{};
  std::optional<doc::NodeId> selected_map_node_;
  std::optional<doc::NodeId> selected_surface_node_;
  DragKind drag_kind_{DragKind::None};
  doc::NodeId drag_node_{};
  scene::CanvasRect drag_original_{};
  scene::CanvasRect preview_{};
  float drag_start_x_{};
  float drag_start_y_{};
  // A surface dragged out of the list onto the output it drives. Assignment is
  // that gesture, so the details rail carries no stepper to cycle surfaces.
  doc::NodeId surface_drag_node_{};
  float surface_drag_start_x_{};
  float surface_drag_start_y_{};
  bool surface_drag_moved_{};
  doc::NodeId surface_details_node_{};
  core::u64 surface_details_show_frame_{};
  std::string surface_name_;
  output::PhysicalSurface surface_geometry_{};
  output::ProjectionSurfaceShape surface_shape_{
      output::ProjectionSurfaceShape::Planar};
  float surface_horizontal_arc_turns_{0.25F};
  core::u32 surface_mesh_columns_{32U};
  std::string surface_mesh_source_;
  float surface_mesh_scale_{1.0F};
  scene::WallColorSettings surface_color_;
  scene::WallColorSettings surface_color_observed_;
  bool surface_color_page_{};
  render::OutputProjectionCache preview_projection_cache_;
  std::shared_ptr<const output::ProjectionMesh> preview_mesh_;
  bool mesh_browse_pending_{};
  bool surface_name_dirty_{};
  NumberField surface_numbers_;
  bool focused_{};

  friend bool detail::wait_for_feed_mapping_projection_cache_for_test(
      FeedMappingPanel &panel) noexcept;
};

} // namespace mediaserver::editor
