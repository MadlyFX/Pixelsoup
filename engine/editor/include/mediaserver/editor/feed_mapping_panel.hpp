#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/canvas_view.hpp>
#include <mediaserver/gui/form_layout.hpp>
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

// The Feed Mapping panel: which part of the canvas or which surface each
// output window shows. It returns edit intents like the Hardware Mapping
// panel does.

namespace mediaserver::editor {


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

// List housekeeping for viewports and surfaces: names, order and copies.
// One request type because all three are the same kind of edit - they change
// how the operator finds an item, never what it calibrates - and they reach
// the document through the same authoring boundary.
enum class FeedOrganizeAction : core::u8 {
  // Sets `name` on the viewport or surface `node`.
  Rename,
  // `order` is the complete list in its new order: every viewport of one
  // window, or every surface.
  Reorder,
  // Copies `node` and places the copy directly after it.
  Duplicate,
};

struct FeedOrganizeRequest final {
  FeedOrganizeAction action{FeedOrganizeAction::Rename};
  doc::NodeId node{};
  std::string name;
  std::vector<doc::NodeId> order;
  core::u64 gesture{};

  bool operator==(const FeedOrganizeRequest &) const = default;
};

// Build from hardware: the canvas and every viewport of the windows the
// plan lays out, replacing what those windows had, as one undoable edit.
struct FeedLayoutRequest final {
  output::FeedLayoutPlan plan;
  core::u64 gesture{};

  bool operator==(const FeedLayoutRequest &) const = default;
};

// The project's stage: whether its surfaces stand in the world or travel with
// the scene camera, and where the viewer stands in it. The complete value, so
// a scrub's newest request is the whole answer.
struct FeedStageEditRequest final {
  output::StagePlacement stage{};
  core::u64 gesture{};

  bool operator==(const FeedStageEditRequest &) const = default;
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
  std::optional<FeedOrganizeRequest> organize;
  std::optional<FeedLayoutRequest> layout;
  std::optional<FeedStageEditRequest> stage_edit;
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
                                           nullptr,
                                       const topology::HardwareRuntimeSnapshot
                                           *runtime = nullptr);

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
    rectangle_numbers_ = NumberField{};
    rectangle_mesh_domain_ = false;
    stage_ = output::StagePlacement{};
    stage_numbers_ = NumberField{};
    focused_ = false;
    rename_.reset();
    clicks_.reset();
    row_drag_node_ = {};
    row_drag_moved_ = false;
    rename_row_ = {};
  }

private:
  enum class DragKind : core::u8 {
    None,
    Move,
    Resize,
  };

  // One draw as an object, and its phases in the order draw() runs them.
  // See src/mapping/feed_mapping_frame.hpp.
  struct Frame;
  [[nodiscard]] bool begin_frame(Frame &f);
  void draw_viewport_list(Frame &f);
  void draw_surface_list(Frame &f);
  void draw_stage_setup(Frame &f);
  void draw_work_area(Frame &f);
  void draw_rectangle_controls(Frame &f);
  void draw_details(Frame &f);
  void draw_surface_calibration(Frame &f, const gui::Rect &details,
                                gui::FormLayout &details_column);
  void resolve_surface_drop(Frame &f);
  void draw_build_notes(Frame &f);

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
  // A viewport row dragged within its window's rows to reorder it.
  doc::NodeId row_drag_node_{};
  float row_drag_start_y_{};
  bool row_drag_moved_{};
  // Double-click or F2 on a viewport or surface row names it in place.
  InlineRename rename_{gui::ui_id("feed.mapping.rename")};
  ClickTracker clicks_;
  // The viewport or surface row clicked last: what F2 names.
  doc::NodeId rename_row_{};
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
  NumberField rectangle_numbers_;
  bool rectangle_mesh_domain_{};
  // The stage as shown. It follows the document except while an eye field is
  // mid-scrub, so a scrub is never re-based on a value still in flight.
  output::StagePlacement stage_{};
  NumberField stage_numbers_;
  bool focused_{};
  // What the last Build from hardware did, shown until this time.
  std::vector<std::string> build_notes_;
  double build_notes_until_{};

  friend bool detail::wait_for_feed_mapping_projection_cache_for_test(
      FeedMappingPanel &panel) noexcept;
};

} // namespace mediaserver::editor
