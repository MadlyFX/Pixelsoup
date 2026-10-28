#pragma once

#include <mediaserver/editor/file_picker.hpp>
#include <mediaserver/core/frame_timing.hpp>
#include <mediaserver/core/render_conformance.hpp>

#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/assets/proxy_policy.hpp>
#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/library_panel.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/editor/selection.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/render/display_lut_cache.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mediaserver::assets {
struct AssetRegistry;
}

// The supporting surfaces around the editor's viewports and timeline.
//
// These panels deliberately keep document semantics out of the UI. The scene
// tree reports a selection request, the library reports the kind of item the
// operator wants to create, and the compositor/output monitor are presently
// rich visual models of the intended workflow. The shell remains responsible
// for turning either request into an authored command.
namespace mediaserver::editor {

enum class SceneSettingsTarget : core::u8 { None, Renderer, Environment };

// A scene-tree selection gesture. Shift range requests carry the exact
// filtered/expanded element-row order the operator saw; the shell remains the
// sole owner of canonical selection state.
struct SceneSelectionRequest final {
  doc::NodeId node{};
  SelectionAction action{SelectionAction::Replace};
  std::vector<doc::NodeId> visible_order;
  bool lock_changed{};
  bool locked{};
  // The eye: the element's own visibility, the same bit the Timeline's eye
  // writes.
  bool visibility_changed{};
  bool visible{true};
  enum class StructureAction : core::u8 {
    Group,
    Ungroup,
    // File `moved` under `destination` (zero: the top level), keeping each
    // element where it is on the stage. When `reorder_destination` is set the
    // moved rows land beside that sibling rather than at the front.
    MoveInto,
    // A new, empty group of `group_kind` under `destination` (zero: the top
    // level).
    Create,
  };
  std::optional<StructureAction> structure;
  doc::NodeId destination{};
  scene::GroupKind group_kind{scene::GroupKind::Folder};
  // The context menu's highlight colour, for every selected row: a
  // marker-palette index, or none to clear it.
  bool highlight_changed{};
  std::optional<core::u8> highlight;
  std::vector<doc::NodeId> moved;
  // A drag between two siblings: `node` lands before or after
  // `reorder_destination` in the front-to-back stack.
  doc::NodeId reorder_destination{};
  bool reorder_before{};
  // `node`'s new name, committed from the inline field.
  std::optional<std::string> rename;
  // The context menu's Delete: the shell deletes the current selection.
  bool delete_selection{};
  SceneSettingsTarget settings{SceneSettingsTarget::None};
  // `node` is an LED surface rather than a scene element: a plain click
  // selects it on Stage, and `lock_changed` sets its Feed Mapping lock.
  bool surface{};

  bool operator==(const SceneSelectionRequest &) const = default;
};

// The Scene outliner: the scene's real hierarchy - groups and Blender
// collections holding their children - with the same eye, padlock, icons and
// names the Timeline's layer list uses. Rows rename in place (double-click or
// F2), drag to reorder among siblings or onto a group to file under it, and
// carry a right-click menu with everything else a row can do: grouping, a
// highlight colour, and new empty groups inside a group. A right-click on the
// list's empty space creates groups at the top level. The stage's LED
// surfaces follow the scene's elements, as they do in the Timeline: physical
// geometry rather than scene nodes, so they carry a padlock but no eye and
// never file into the hierarchy.
class ScenePanel final {
public:
  explicit ScenePanel(gui::UiId id) noexcept
      : id_(id), rename_(id ^ 0x52454E414D455343ULL) {}

  [[nodiscard]] std::optional<SceneSelectionRequest>
  draw(gui::UiContext &ctx, const gui::Rect &bounds,
       std::span<const ElementInfo> elements,
       std::span<const doc::NodeId> selection, doc::NodeId primary = {},
       bool show_settings = false,
       SceneSettingsTarget settings_selection = SceneSettingsTarget::None,
       std::span<const output::ProjectionSurface> surfaces = {},
       doc::NodeId selected_surface = {});

  // Opens `element`'s name for editing, as F2 does.
  void begin_rename(gui::UiContext &ctx, const ElementInfo &element);
  [[nodiscard]] bool renaming() const noexcept { return rename_.active(); }

  [[nodiscard]] std::string_view search() const noexcept { return search_; }

private:
  gui::UiId id_{};
  std::string search_;
  gui::ScrollRegion scroll_;
  // Branches the operator closed. Everything starts open; selecting
  // something inside a closed branch opens it again.
  std::set<doc::NodeId> collapsed_;
  doc::NodeId revealed_{};
  InlineRename rename_;
  ClickTracker clicks_;
  // The outliner was the last panel clicked, so F2 is meant for it.
  bool focused_{};
  doc::NodeId drag_node_{};
  float drag_x_{};
  float drag_y_{};
  bool dragging_{};
  // The open context menu's row and entries, fixed when it opened so what
  // the operator reads is what a choice does.
  doc::NodeId menu_node_{};
  // A menu opened on the list's empty space rather than on a row.
  bool menu_background_{};
  std::vector<std::string> menu_entries_;
  std::vector<core::u8> menu_commands_;
  // The highlight colour menu, opened from the row menu at the same point.
  bool color_menu_{};
};

// A wire authored with a new node, in the same transaction. One end is the
// new node's own port, named by index because the node has no id yet; the
// other end is a port of a node that already exists.
struct CompositorAddLink final {
  // True when the new node is the wire's source: new.port -> existing.
  bool from_new{};
  core::u8 port{};
  compositor::EdgeEndpoint existing;
  // Where the wire sorts among the others into the same input: effects on a
  // layer run in this order.
  core::i64 order{};

  bool operator==(const CompositorAddLink &) const = default;
};

struct CompositorAddIntent final {
  compositor::NodeKind kind{compositor::NodeKind::Brightness};
  float editor_x{};
  float editor_y{};
  // SceneLayer binds the currently selected scene element; unused otherwise.
  doc::NodeId scene_node{};
  // Wires the new node arrives with, and existing wires the same undo step
  // removes - the one already on an occupied single input it is dropped on.
  std::vector<CompositorAddLink> links;
  std::vector<doc::NodeId> remove_edges;
  // Existing wires renumbered in the same step, to make room for a new one
  // between two of them.
  std::vector<std::pair<doc::NodeId, core::i64>> reorder;

  bool operator==(const CompositorAddIntent &) const = default;
};

// Nodes collapsed to their title and connected ports, or opened again.
struct CompositorCollapseIntent final {
  std::vector<doc::NodeId> nodes;
  bool collapsed{};

  bool operator==(const CompositorCollapseIntent &) const = default;
};

// Nodes passed through as their identity, or put back to work. Unlike
// collapsing, this changes the picture.
struct CompositorBypassIntent final {
  std::vector<doc::NodeId> nodes;
  bool bypassed{};

  bool operator==(const CompositorBypassIntent &) const = default;
};

// Wires given new places in the order their input takes them: (wire, order).
struct CompositorReorderIntent final {
  std::vector<std::pair<doc::NodeId, core::i64>> orders;

  bool operator==(const CompositorReorderIntent &) const = default;
};

struct CompositorWire final {
  compositor::EdgeEndpoint from;
  compositor::EdgeEndpoint to;

  bool operator==(const CompositorWire &) const = default;
};

// Wires removed and added as one undo step: breaking a port's wires, moving
// them to another port, splicing a node into a wire, or connecting a node
// that already exists to a port something was dropped on.
struct CompositorRewireIntent final {
  std::vector<doc::NodeId> remove_edges;
  std::vector<CompositorWire> add_edges;

  bool operator==(const CompositorRewireIntent &) const = default;
};

// A wire's complete list of reroute points, in graph units, replacing the
// list it had. Layout only: points never change what the graph computes.
struct CompositorWaypointIntent final {
  doc::NodeId edge{};
  std::vector<compositor::WirePoint> points;

  bool operator==(const CompositorWaypointIntent &) const = default;
};

struct CompositorMoveIntent final {
  doc::NodeId node{};
  float editor_x{};
  float editor_y{};

  bool operator==(const CompositorMoveIntent &) const = default;
};

struct CompositorConnectIntent final {
  compositor::EdgeEndpoint from;
  compositor::EdgeEndpoint to;
  // Dropping onto an occupied single-input port is an atomic rewire: erase
  // this edge in the same transaction before authoring the new connection.
  std::optional<doc::NodeId> replace_edge;

  bool operator==(const CompositorConnectIntent &) const = default;
};

enum class CompositorProperty : core::u8 {
  BrightnessAmount,
  SaturationAmount,
  LevelsMasterInputBlack,
  LevelsMasterInputWhite,
  LevelsMasterGamma,
  LevelsMasterOutputBlack,
  LevelsMasterOutputWhite,
  LevelsRedInputBlack,
  LevelsRedInputWhite,
  LevelsRedGamma,
  LevelsRedOutputBlack,
  LevelsRedOutputWhite,
  LevelsGreenInputBlack,
  LevelsGreenInputWhite,
  LevelsGreenGamma,
  LevelsGreenOutputBlack,
  LevelsGreenOutputWhite,
  LevelsBlueInputBlack,
  LevelsBlueInputWhite,
  LevelsBlueGamma,
  LevelsBlueOutputBlack,
  LevelsBlueOutputWhite,
  LiftGammaGainLiftR,
  LiftGammaGainLiftG,
  LiftGammaGainLiftB,
  LiftGammaGainGammaR,
  LiftGammaGainGammaG,
  LiftGammaGainGammaB,
  LiftGammaGainGainR,
  LiftGammaGainGainG,
  LiftGammaGainGainB,
  ContrastPivotContrast,
  ContrastPivotPivot,
  ColorMatrixM00,
  ColorMatrixM01,
  ColorMatrixM02,
  ColorMatrixM10,
  ColorMatrixM11,
  ColorMatrixM12,
  ColorMatrixM20,
  ColorMatrixM21,
  ColorMatrixM22,
  ColorMatrixOffsetR,
  ColorMatrixOffsetG,
  ColorMatrixOffsetB,
  WhiteBalanceTemperatureKelvin,
  WhiteBalanceTint,
  CurvesParameters,
  FilmGrainAmount,
  FilmGrainPixelScale,
  FilmGrainChromaRatio,
  FilmGrainSeed,
  FilmGrainMode,
  BlurRadius,
  BoxBlurRadius,
  BoxBlurPasses,
  DirectionalBlurLength,
  DirectionalBlurAngleTurns,
  RadialCount,
  RadialRadius,
  RadialStartAngleTurns,
  RadialCenterX,
  RadialCenterY,
  LedCarPositionX,
  LedCarPositionY,
  LedCarPositionZ,
  LedCarYawTurns,
  LedCarPitchTurns,
  LedCarRollTurns,
  LedCarPlateScale,
  LedCarArrayScale,
  LedCarReflectionHeight,
  LedCarReflectionScale,
  LedCarHorizontalBlend,
  ChromaKeyRed,
  ChromaKeyGreen,
  ChromaKeyBlue,
  ChromaKeyTolerance,
  ChromaKeySoftness,
  // Signed whole pixels: positive grows the matte, negative shrinks it.
  // Sent as a float and rounded.
  ChromaKeyEdgePx,
  ChromaKeySpill,
  ChromaKeyMix,
  LumaKeyLow,
  LumaKeyHigh,
  LumaKeyLowSoftness,
  LumaKeyHighSoftness,
  // Switches, sent as 0 or 1.
  LumaKeyInvert,
  LumaKeyMix,
  // 0 alpha, 1 luma.
  MatteMode,
  MatteInvert,
  MatteClampInput,
  // compositor::MergeBlendMode's value, sent as a whole number.
  MergeBlendMode,
  MergeOpacity,
  MergeValue,
  // compositor::BackgroundMode's value: 0 solid, 1 linear, 2 radial.
  BackgroundMode,
  BackgroundColorAR,
  BackgroundColorAG,
  BackgroundColorAB,
  BackgroundColorAA,
  BackgroundColorBR,
  BackgroundColorBG,
  BackgroundColorBB,
  BackgroundColorBA,
  BackgroundAngleTurns,
  BackgroundCenterX,
  BackgroundCenterY,
  BackgroundSize,
};

using CompositorPropertyValue =
    std::variant<float, core::u32, compositor::CurvesParameters>;

struct CompositorPropertyEditIntent final {
  doc::NodeId node{};
  CompositorProperty property{CompositorProperty::BrightnessAmount};
  CompositorPropertyValue value{0.0F};

  bool operator==(const CompositorPropertyEditIntent &) const = default;
};

enum class CompositorSelectionKind : core::u8 {
  None,
  Node,
  Edge,
  Comment,
};

struct CompositorSelection final {
  CompositorSelectionKind kind{CompositorSelectionKind::None};
  doc::NodeId node{};

  bool operator==(const CompositorSelection &) const = default;
};

// Edges are first-class document nodes, so disconnecting is the same compact
// remove intent as deleting a node. When a target is a node, the authoring
// boundary erases its incident edge nodes in the same transaction. Every
// target goes in one undo step.
struct CompositorDeleteIntent final {
  std::vector<CompositorSelection> targets;

  bool operator==(const CompositorDeleteIntent &) const = default;
};

// Copy keeps a snapshot of the chosen nodes, their wires and comment boxes
// with the Director; Paste places that snapshot's top-left at (x, y) in graph
// units; Duplicate copies the chosen nodes straight back, moved by (x, y),
// leaving the clipboard alone. Output, and a Scene Layer whose source already
// has one, are not placed.
enum class CompositorClipboardAction : core::u8 { Copy, Paste, Duplicate };

struct CompositorClipboardIntent final {
  CompositorClipboardAction action{CompositorClipboardAction::Copy};
  std::vector<doc::NodeId> nodes;
  float x{};
  float y{};

  bool operator==(const CompositorClipboardIntent &) const = default;
};

// A node's or comment box's new title. Empty gives a node back its kind's
// name.
struct CompositorRenameIntent final {
  doc::NodeId node{};
  std::string label;

  bool operator==(const CompositorRenameIntent &) const = default;
};

// A comment box created (no node yet) or changed: where it is, its title and
// its colour. Deleting one is an ordinary delete of a Comment selection.
struct CompositorCommentIntent final {
  doc::NodeId node{};
  float x{};
  float y{};
  float width{};
  float height{};
  std::string title;
  core::u8 color{};

  bool operator==(const CompositorCommentIntent &) const = default;
};

struct CompositorFrame final {
  // Set on every explicit node/edge selection press, including a re-click, so
  // the shell can make Compositor the active Delete-key domain.
  std::optional<CompositorSelection> selection;
  std::optional<CompositorAddIntent> add_node;
  // Every node a gesture moved: one for a drag, all of a dragged selection,
  // the contents of a dragged comment box, or what a layout tool placed.
  std::vector<CompositorMoveIntent> move_nodes;
  std::optional<CompositorConnectIntent> connect_ports;
  std::optional<CompositorRewireIntent> rewire;
  std::optional<CompositorWaypointIntent> waypoints;
  std::optional<CompositorPropertyEditIntent> property_edit;
  std::optional<NumericNudge> numeric_nudge;
  // Emitted by DELETE NODE / DISCONNECT in the graph toolbar.
  std::optional<CompositorDeleteIntent> remove;
  std::optional<CompositorClipboardIntent> clipboard;
  std::optional<CompositorRenameIntent> rename;
  std::optional<CompositorCommentIntent> comment;
  std::optional<CompositorCollapseIntent> collapse;
  std::optional<CompositorBypassIntent> bypass;
  std::optional<CompositorReorderIntent> reorder;
  // Delete was handled by this focused panel, so the shell must not also
  // delete the current scene selection.
  bool consume_delete{};
  // True while a numeric field is being scrubbed or typed, allowing the
  // authoring layer to group live values into one undo gesture.
  bool editing{};

  bool operator==(const CompositorFrame &) const = default;
};

// Operator-facing wording for model diagnostics. The graph model deliberately
// carries stable codes rather than UI copy; the panel owns this plain-language
// translation so authoring errors remain understandable without leaking UI
// concerns into evaluation.
[[nodiscard]] std::string_view compositor_diagnostic_message(
    const compositor::GraphDiagnostic &diagnostic) noexcept;
[[nodiscard]] std::string_view compositor_diagnostic_severity_label(
    const compositor::GraphDiagnostic &diagnostic) noexcept;

// Cross-panel drag feedback for Effects and Timeline rows. The graph canvas is
// a valid drop target while either source is lifted; the stronger hovered
// state makes the eventual add location explicit before release.
void draw_compositor_drop_target(gui::UiContext &ctx, const gui::Rect &bounds,
                                 std::string_view item_name);

// The Scene Layer node names its referenced scene element as Scene and
// Inspector do; keep the stable ID as a fallback if that element is absent.
[[nodiscard]] std::string compositor_scene_layer_detail(
    doc::NodeId scene_node, std::span<const ElementInfo> elements);

// Graph-space rows shared by every node, measured from the editor font so a
// node's title and port names always have the height their text needs.
struct CompositorNodeMetrics final {
  float header_height{};
  float row_height{};
  float footer_height{};
};

// An input that something dragged from another panel - a Timeline layer, a
// Library asset, an effect - would be connected to if let go now.
struct CompositorPortDrop final {
  compositor::EdgeEndpoint endpoint;
  std::string_view name;
  compositor::PortValueType type{compositor::PortValueType::Image};
  // The port's dot on screen.
  float x{};
  float y{};
  // The wire already on this input when it takes only one, which the drop
  // replaces in the same undo step.
  std::optional<doc::NodeId> occupied;
  // Whether the input takes more than one wire.
  bool multiple{};

  bool operator==(const CompositorPortDrop &) const = default;
};

// An effect wire something dragged from Effects would be spliced into if
// let go now: the new effect joins the same input, right after this wire's
// effect in the order that input applies them.
struct CompositorWireDrop final {
  doc::NodeId edge{};
  compositor::EdgeEndpoint input;
  // Where the drop lands on screen, on the wire.
  float x{};
  float y{};
  // The order for the new wire, and the renumbering of the input's other
  // wires that makes room for it.
  core::i64 order{};
  std::vector<std::pair<doc::NodeId, core::i64>> reorder;
  // The kind of effect it goes after, to name in the preview.
  std::string_view after;
  // Colour effects and filters each splice only into wires of their own.
  compositor::PortValueType type{compositor::PortValueType::Effect};

  bool operator==(const CompositorWireDrop &) const = default;
};

// A headless immediate-mode editor for the persistent compositor graph. The
// panel never mutates the evaluated projection: it reports compact intents for
// the shell/authoring layer to turn into ordinary document commands.
class CompositorPanel final {
public:
  explicit CompositorPanel(gui::UiId id) noexcept : id_(id) {}

  [[nodiscard]] CompositorFrame draw(gui::UiContext &ctx,
                                     const gui::Rect &bounds,
                                     const compositor::CompositorGraph &graph,
                                     std::span<const ElementInfo> elements = {});

  // Transitional read-only adapter for the current shell. It presents each
  // scene element as a Scene Layer node; new integrations should pass the
  // evaluated CompositorGraph to the overload above.
  void draw(gui::UiContext &ctx, const gui::Rect &bounds,
            std::span<const ElementInfo> elements,
            doc::NodeId primary = {}) const;

  // The primary selection: the one item the Parameters panel and a rename
  // act on.
  [[nodiscard]] CompositorSelection selection() const noexcept {
    return selection_;
  }
  // Every selected node, ascending, the primary among them. Empty when the
  // primary selection is a wire or a comment box.
  [[nodiscard]] std::span<const doc::NodeId> selected_nodes() const noexcept {
    return selected_nodes_;
  }
  // What Delete removes: every selected node but Output, or the selected
  // wire or comment box. Nothing when there is nothing it may remove.
  [[nodiscard]] std::optional<CompositorDeleteIntent>
  selection_delete(const compositor::CompositorGraph &graph) const;

  // Converts a screen-space drop into the graph-space top-left needed by a
  // newly created node, centring the node under the pointer. Effects and the
  // Timeline use this so cross-panel drops agree with the visible pan/zoom.
  [[nodiscard]] std::optional<gui::UiPoint>
  node_drop_position(float screen_x, float screen_y, float node_width,
                     float node_height) const noexcept {
    if (!last_canvas_.contains(screen_x, screen_y) || zoom_ <= 0.0F) {
      return std::nullopt;
    }
    return gui::UiPoint{
        (screen_x - last_canvas_.x - pan_x_) / zoom_ - node_width * 0.5F,
        (screen_y - last_canvas_.y - pan_y_) / zoom_ - node_height * 0.5F};
  }

  // Uses the same pan, zoom, dimensions and stacking order as the drawn nodes.
  [[nodiscard]] std::optional<std::pair<doc::NodeId, gui::Rect>>
  node_at(const compositor::CompositorGraph &graph, float x, float y) const noexcept;

  // The input port whose drop target - its whole row, dot and label - holds
  // the screen point, using the last drawn layout. A drop from another panel
  // asks this before node_at(), so a port wins over the node around it.
  [[nodiscard]] std::optional<compositor::EdgeEndpoint>
  input_port_at(const compositor::CompositorGraph &graph, float x,
                float y) const noexcept;

  // The compatible input a drop of `type` at the screen point would connect
  // to: the input whose row holds the point, or the nearest one whose dot is
  // within snapping reach. An input that already has a wire from the same
  // kind of source is still offered; one that takes a single wire reports
  // the wire the drop would replace.
  [[nodiscard]] std::optional<CompositorPortDrop>
  port_drop_target(const compositor::CompositorGraph &graph, float x, float y,
                   compositor::PortValueType type) const noexcept;

  // The wire of `type` - effects or filters - within reach of a
  // screen point, for an Effects drop.
  [[nodiscard]] std::optional<CompositorWireDrop>
  effect_wire_at(const compositor::CompositorGraph &graph, float x, float y,
                 compositor::PortValueType type) const;
  // Draws an effect wire drop's preview: the wire lit, and what will happen.
  void draw_wire_drop(gui::UiContext &ctx, const CompositorWireDrop &drop,
                      std::string_view what) const;

  // Brings the Scene Layer node for a scene element - or for the Track it
  // plays on - into the middle of the view, and outlines it for a moment.
  // Nothing happens when the graph has none. The view's zoom is kept.
  void reveal_element(doc::NodeId element, double now) noexcept {
    reveal_element_ = element;
    reveal_requested_at_ = now;
  }
  // The Scene Layer node most recently outlined this way, while it shows.
  [[nodiscard]] doc::NodeId revealed_node(double now) const noexcept {
    return now < reveal_until_ ? reveal_node_ : doc::NodeId{};
  }

  // Draws the drop's preview over the graph: the target port ringed, a wire
  // from the pointer to it, and a label naming what will happen.
  void draw_port_drop(gui::UiContext &ctx, const CompositorPortDrop &drop,
                      std::string_view what) const;

  // Where a new node of `kind` goes so that its first output faces the drop
  // target from the left, in graph units: the top-left to author.
  [[nodiscard]] gui::UiPoint
  node_position_feeding(compositor::NodeKind kind,
                        const CompositorPortDrop &drop) const noexcept;

  // The graph-space size a new node of `kind` is drawn at, so a drop can
  // centre it under the pointer. Measured from the editor font on each draw;
  // a Scene Layer can grow wider once it names its source.
  [[nodiscard]] gui::UiPoint new_node_size(compositor::NodeKind kind) const noexcept;

  // The Parameters panel's width beside the graph, in UI units. The operator
  // drags its edge; zero means the default. Machine-local, like dock sizes.
  [[nodiscard]] float parameter_panel_width() const noexcept {
    return parameter_width_;
  }
  void set_parameter_panel_width(float width) noexcept {
    parameter_width_ = std::isfinite(width) ? std::max(width, 0.0F) : 0.0F;
  }

  // Whether the minimap is shown in the canvas's corner; the toolbar's MAP
  // button toggles it.
  [[nodiscard]] bool minimap_shown() const noexcept { return minimap_; }
  void show_minimap(bool shown) noexcept { minimap_ = shown; }

  [[nodiscard]] float graph_zoom() const noexcept { return zoom_; }
  [[nodiscard]] gui::UiPoint graph_pan() const noexcept {
    return gui::UiPoint{pan_x_, pan_y_};
  }

  void reset_project_state() noexcept {
    selection_ = {};
    dragging_node_ = {};
    connecting_from_.reset();
    parameter_node_ = {};
    focused_ = false;
    add_menu_open_ = false;
    add_category_.reset();
    node_search_.clear();
    parameter_fields_ = NumberField{};
    pan_x_ = 0.0F;
    pan_y_ = 0.0F;
    zoom_ = 1.0F;
    panning_ = false;
    last_canvas_ = {};
    pending_moves_.clear();
    selected_nodes_.clear();
    drag_group_.clear();
    marquee_.reset();
    comment_drag_.reset();
    pending_comment_.reset();
    rename_.reset();
    context_menu_.reset();
    paste_baseline_.reset();
    notice_.clear();
    pending_points_.reset();
    pending_wires_.reset();
    pending_collapse_.clear();
    pending_bypass_.clear();
    pending_order_.clear();
    order_discs_.clear();
    order_drag_.reset();
    drawn_wires_.clear();
    reveal_element_ = {};
    reveal_node_ = {};
    reveal_until_ = 0.0;
    inline_fields_ = NumberField{};
    point_drag_.reset();
    relink_.reset();
    node_menu_.reset();
    clicks_.reset();
    node_sizes_.clear();
    parameter_scroll_ = 0.0F;
    parameter_content_height_ = 0.0F;
  }

private:
  // A released drag whose authored position has not reached the evaluated
  // graph yet. Until it does, the node keeps drawing where it was let go, so
  // the round trip through the Director never shows as a snap back.
  struct PendingMove final {
    doc::NodeId node{};
    float x{};
    float y{};
    // The graph's position when the drag was released. Any change from it
    // means the edit arrived (perhaps normalized) and the override ends.
    float from_x{};
    float from_y{};
    core::u32 frames{};
  };
  struct NodeSize final {
    doc::NodeId node{};
    float width{};
    float height{};
  };
  // Wires lifted off a port with Ctrl to be moved to another. Their free end
  // follows the pointer: the source end when they were lifted off an output,
  // the target end when lifted off an input.
  struct Relink final {
    std::vector<doc::NodeId> edges;
    bool moving_source{};
    compositor::EdgeEndpoint origin;
  };
  // The node search opened at the pointer - by Tab, by a right click on the
  // canvas, or by letting a wire go on empty canvas. In the last case the
  // node it adds arrives connected to that wire, and only nodes that can
  // take it are offered.
  struct NodeMenu final {
    float screen_x{};
    float screen_y{};
    std::optional<compositor::EdgeEndpoint> from;
    compositor::PortValueType type{compositor::PortValueType::Image};
    std::string search;
    std::size_t highlight{};
    bool focus_pending{true};
  };
  // A reroute point being dragged, and the wire's whole list as it stands.
  struct PointDrag final {
    doc::NodeId edge{};
    std::size_t index{};
    std::vector<compositor::WirePoint> points;
  };
  // Reroute points authored but not yet back from the Director, held like a
  // released node move so the wire never snaps back.
  struct PendingPoints final {
    doc::NodeId edge{};
    std::vector<compositor::WirePoint> points;
    std::vector<compositor::WirePoint> from;
    core::u32 frames{};
  };

  // Wires authored but not yet back from the Director: the ones to keep
  // drawing and the ones to stop drawing, so a connection never blinks.
  struct PendingWires final {
    std::vector<CompositorWire> added;
    std::vector<doc::NodeId> removed;
    core::u32 frames{};
  };

  // A node moving with the one being dragged: where it started.
  struct GroupMember final {
    doc::NodeId node{};
    float start_x{};
    float start_y{};
  };
  // A rubber-band selection: where it started on screen, and whether it adds
  // to the selection (Shift), takes from it (Ctrl), or replaces it.
  struct Marquee final {
    float start_x{};
    float start_y{};
    enum class Mode : core::u8 { Replace, Add, Subtract } mode{Mode::Replace};
  };
  // A comment box being moved by its title bar, carrying the nodes that sat
  // wholly inside it when it was picked up, or resized by its corner.
  struct CommentDrag final {
    doc::NodeId comment{};
    bool resizing{};
    float press_x{};
    float press_y{};
    compositor::GraphComment start;
    std::vector<GroupMember> carried;
    // How far the pointer has taken it, in graph units.
    float dx{};
    float dy{};
  };
  // A comment box's authored rectangle not yet back from the Director.
  struct PendingComment final {
    compositor::GraphComment held;
    compositor::GraphComment from;
    core::u32 frames{};
  };
  // A title being typed over its node or comment box.
  struct Rename final {
    doc::NodeId node{};
    std::string text;
    bool focus_pending{true};
  };
  // The right-click menu of a node selection, a wire, or a comment box.
  struct ContextMenu final {
    float screen_x{};
    float screen_y{};
    CompositorSelectionKind kind{CompositorSelectionKind::Node};
    doc::NodeId target{};
    // The graph point a wire was right-clicked at, for a reroute point.
    compositor::WirePoint at;
    std::size_t segment{};
  };
  // A paste or duplicate on its way: the nodes there were before it, so the
  // ones it adds can be selected when they arrive.
  struct PasteBaseline final {
    std::vector<doc::NodeId> nodes;
    core::u32 frames{};
  };

  [[nodiscard]] std::optional<gui::UiPoint>
  pending_position(doc::NodeId node) const noexcept;
  void select_only(doc::NodeId node);

  // A wire as last drawn, in screen space, for drops from other panels.
  struct DrawnWire final {
    doc::NodeId edge{};
    compositor::PortValueType type{compositor::PortValueType::Image};
    compositor::EdgeEndpoint input;
    std::vector<gui::UiPoint> points;
  };

  // Whether a node is drawn collapsed, honouring a toggle on its way.
  [[nodiscard]] bool collapsed(const compositor::GraphNode &node) const noexcept;
  // Whether a node is drawn bypassed, likewise.
  [[nodiscard]] bool bypassed(const compositor::GraphNode &node) const noexcept;
  // A wire's place in its input's order, honouring a move on its way.
  [[nodiscard]] core::i64 edge_order(const compositor::GraphEdge &edge) const noexcept;
  // The wires into one input, in the order it takes them.
  [[nodiscard]] std::vector<const compositor::GraphEdge *>
  input_chain(const compositor::CompositorGraph &graph,
              compositor::EdgeEndpoint input) const;

  // A wire's order number as last drawn. Dragged onto another number on the
  // same input, it moves its wire to that place.
  struct OrderDisc final {
    doc::NodeId edge{};
    compositor::EdgeEndpoint input;
    float x{};
    float y{};
    float radius{};
  };
  // The row a port sits on, or nothing when a collapsed node hides it.
  [[nodiscard]] std::optional<float>
  port_row(const compositor::CompositorGraph &graph,
           const compositor::GraphNode &node, core::u8 port,
           bool input) const;

  // A node's bounds on screen as last laid out, honouring a held move.
  [[nodiscard]] gui::Rect node_screen_bounds(
      const compositor::GraphNode &node) const noexcept;

  [[nodiscard]] gui::UiPoint node_size(const compositor::GraphNode &node) const noexcept;

  gui::UiId id_{};
  CompositorSelection selection_{};
  std::vector<PendingMove> pending_moves_;
  // Ascending; holds the primary when the primary is a node.
  std::vector<doc::NodeId> selected_nodes_;
  std::vector<GroupMember> drag_group_;
  bool drag_moved_{};
  bool press_was_selected_{};
  std::optional<Marquee> marquee_;
  std::optional<CommentDrag> comment_drag_;
  std::optional<PendingComment> pending_comment_;
  std::optional<Rename> rename_;
  std::optional<ContextMenu> context_menu_;
  std::optional<PasteBaseline> paste_baseline_;
  // A short note shown over the graph, and until when.
  std::string notice_;
  double notice_until_{};
  bool minimap_{};
  std::optional<PendingPoints> pending_points_;
  std::optional<PendingWires> pending_wires_;
  // Collapse toggles not yet back from the Director: (node, collapsed).
  std::vector<std::pair<doc::NodeId, bool>> pending_collapse_;
  // Bypass toggles not yet back from the Director: (node, bypassed).
  std::vector<std::pair<doc::NodeId, bool>> pending_bypass_;
  // Wire orders sent to the Director and not yet back: (wire, order).
  std::vector<std::pair<doc::NodeId, core::i64>> pending_order_;
  std::vector<OrderDisc> order_discs_;
  // The wire whose number is being dragged.
  std::optional<doc::NodeId> order_drag_;
  std::vector<DrawnWire> drawn_wires_;
  doc::NodeId reveal_element_{};
  double reveal_requested_at_{};
  doc::NodeId reveal_node_{};
  double reveal_until_{};
  // The value fields drawn on nodes themselves.
  NumberField inline_fields_;
  std::optional<PointDrag> point_drag_;
  std::optional<Relink> relink_;
  std::optional<NodeMenu> node_menu_;
  ClickTracker clicks_;
  // The last drawn size of each node, ascending by id, for the const queries.
  std::vector<NodeSize> node_sizes_;
  CompositorNodeMetrics metrics_{};
  // Indexed by NodeKind.
  std::array<gui::UiPoint, compositor::node_kind_count> new_node_sizes_{};
  doc::NodeId dragging_node_{};
  float drag_offset_x_{};
  float drag_offset_y_{};
  float drag_x_{};
  float drag_y_{};
  std::optional<compositor::EdgeEndpoint> connecting_from_;
  compositor::PortValueType connecting_type_{compositor::PortValueType::Image};
  doc::NodeId parameter_node_{};
  bool focused_{};
  bool add_menu_open_{};
  std::optional<core::u8> add_category_;
  std::string node_search_;
  NumberField parameter_fields_;
  float parameter_width_{};
  float parameter_height_{};
  float parameter_scroll_{};
  float parameter_content_height_{};
  float parameter_thumb_grab_{};
  // One bit per collapsible Parameters section; set means collapsed.
  core::u32 collapsed_sections_{};
  float pan_x_{};
  float pan_y_{};
  float zoom_{1.0F};
  bool panning_{};
  gui::Rect last_canvas_{};
};

struct OutputMonitorState final {
  core::u64 frame{};
  core::u32 width{1920U};
  core::u32 height{1080U};
  float frames_per_second{60.0F};
  bool playing{};
  struct Node final {
    enum class ContentStatus : core::u8 {
      NotRequired,
      Checking,
      Ready,
      Missing,
      Error,
    };

    // How this output is presenting physically. Every value is a transient
    // observation; none is ever written back to the project.
    enum class SyncStatus : core::u8 {
      // The project makes no physical timing claim.
      Unsynced,
      // Nothing has reported, or this build/platform cannot query the sync
      // device. Never treated as evidence of lock.
      Unverified,
      SyncDeviceMissing,
      ReferenceLost,
      ReferenceNotInUse,
      Unlocked,
      BarrierIncomplete,
      // A formerly joined client left, or the driver's own in-sync present
      // counter fell behind the present counter.
      BarrierLost,
      ShowClockReferenceUnavailable,
      ShowClockReferenceLost,
      // Locked and joined, but the evidence came from a simulated provider.
      Simulated,
      Locked,
    };

    doc::NodeId device{};
    std::string name;
    std::string endpoint;
    core::u32 node_index{};
    core::u32 width{1920U};
    core::u32 height{1080U};
    enum class Status : core::u8 {
      Disabled,
      InvalidConfiguration,
      DuplicateNodeIndex,
      Probing,
      Stopped,
      Starting,
      Running,
      Stopping,
      Faulted,
      Unreachable,
      AuthenticationFailed,
      ProtocolError,
      ConfigurationMismatch,
      Busy,
    } status{Status::Disabled};
    core::u64 generation{};
    std::string detail;
    // Transient content readiness reported by the machine-local lifecycle
    // agent for the currently required project manifest. None of these fields
    // are persisted in the project or allowed to rewrite authored assets.
    ContentStatus content{ContentStatus::NotRequired};
    core::u32 assets_required{};
    core::u32 assets_ready{};
    core::u32 assets_missing{};
    bool missing_media_only{};
    core::u32 assets_mismatched{};
    core::u32 assets_verification_failed{};
    std::string content_detail;
    enum class Connection : core::u8 { Disconnected, CatchingUp, Live };
    Connection connection{Connection::Disconnected};
    core::u64 committed_frame{};
    core::u64 target_frame{};
    SyncStatus sync{SyncStatus::Unsynced};
    core::u32 sync_locked_outputs{};
    core::u32 sync_expected_outputs{};
    core::u32 barrier_joined_outputs{};
    core::u32 barrier_expected_outputs{};
    core::u64 barrier_presents{};
    core::u64 barrier_presents_in_sync{};
    // The hardware source currently advancing this node's ShowClock. This is
    // deliberately shown beside barrier membership: a joined barrier alone
    // does not establish camera-reference timing.
    std::string_view show_clock_reference{"UNKNOWN"};
    core::u32 show_clock_reference_frame{};
    core::u64 missed_frames{};
    core::u64 late_frames{};
    core::u64 held_frames{};
    core::u64 repeated_frames{};
    core::u64 loss_of_sync_events{};
    std::string sync_detail;
    core::RenderConformance conformance{};
    core::FrameTiming frame_timing{};
    core::u32 frame_budget_us{};
    // Whether this node opened the physical outputs its bindings name. A node
    // that could not has no presenters, so everything downstream of it -
    // renderer evidence, asset readiness, the Program gate - reports the
    // honest default and reads as "still loading". This is the one field that
    // distinguishes the two, so it is shown on the row rather than inferred.
    //
    // Mirrors the runtime vocabulary rather than borrowing it, the way
    // SyncStatus above does: a panel describes what an operator sees, and the
    // editor does not depend on the session protocol.
    enum class OutputStatus : core::u8 {
      // Nothing reported. Never evidence that the outputs opened.
      Unknown,
      Deployed,
      InventoryUnavailable,
      InvalidMode,
      DisplayMissing,
      DisplayInactive,
      DisplayDuplicated,
      DisplayMirrored,
      DuplicateRenderServer,
      PresenterRefused,
    } output{OutputStatus::Unknown};

    [[nodiscard]] bool output_failed() const noexcept {
      return output != OutputStatus::Unknown &&
             output != OutputStatus::Deployed;
    }

    [[nodiscard]] bool content_ready() const noexcept {
      return content == ContentStatus::NotRequired ||
             content == ContentStatus::Ready || missing_media_only;
    }
  };
  // Borrowed only for draw(); the runtime coordinator owns the value-safe row
  // snapshot and the shell never retains it past the frame.
  std::span<const Node> nodes{};
  struct Cluster final {
    enum class Health : core::u8 {
      Unmonitored,
      Healthy,
      Degraded,
      Faulted,
    } health{Health::Unmonitored};
    core::u32 configured_nodes{};
    core::u32 live_sessions{};
    core::u32 catching_up_sessions{};
    core::u64 release_lag_frames{};
    core::u64 pacer_lag_frames{};
    // The Director is sealing no control frames, so edits are not applied.
    bool control_stalled{};
    core::u64 heartbeat_timeouts{};
    core::u64 session_failures{};
    core::u64 barrier_rejections{};
    // Nodes taken out of lockstep for holding every output back too long.
    core::u64 lockstep_demotions{};
    core::u64 present_failures{};
    bool diagnostics_recording{};
    // Render machines are controlled with no password: any Media Server on
    // the network could start and stop them too.
    bool control_unsecured{};
    // These remain false until an explicit platform/vendor implementation
    // measures the corresponding physical timing fact.
    bool external_time_sync_verified{};
    bool hardware_frame_lock_verified{};
    // The project synchronization policy and what the cluster can actually
    // prove about it. `sync_headline` is the exact operator wording produced
    // by the readiness gate - SYNC UNVERIFIED, REFERENCE LOST, SWAP BARRIER
    // INCOMPLETE - and is never replaced by a generic online state.
    std::string_view sync_mode{"UNSYNCED"};
    std::string_view sync_headline{"SYNC POLICY UNSYNCED"};
    bool sync_required{};
    bool sync_satisfied{true};
    bool sync_degraded{};
    core::u32 sync_groups{};
    core::u32 sync_locked_outputs{};
    core::u32 sync_expected_outputs{};
    core::u32 barrier_joined_outputs{};
    // Authored-topology problems that block a physical policy before any
    // measurement is consulted.
    core::u32 sync_configuration_issues{};

    // Renderer cohort truth is carried separately from physical sync. The
    // Output Monitor shows it on the existing control line, and only names a
    // node when that node is the first condition holding Program.
    std::string_view renderer_headline{"RENDERER INACTIVE"};
    bool renderer_active{};
    bool renderer_satisfied{true};
    bool renderer_fallback{};
    bool renderer_selected_advanced{};
    bool renderer_failure_local{};
    core::u32 renderer_failure_node{};
    bool conformance_active{};
    core::RenderConformance conformance{};
    core::FrameTiming frame_timing{};
    core::u32 frame_budget_us{};
    core::ConformanceComparison conformance_difference{};
    core::AssetCohort asset_cohort{};
    std::optional<core::u32> asset_waiting_node;

    bool operator==(const Cluster &) const = default;
  } cluster{};
};

struct StageAssetStatus final {
  bool loading{};
  std::string headline;
  std::string detail;
};

// Stage's own preparation evidence, independent of Program's asset cohort.
[[nodiscard]] StageAssetStatus stage_asset_status(
    const render::RenderStats& geometry, core::u64 pending_media,
    render::DisplayLutState display);

struct ProgramAssetStatus final {
  bool waiting{};
  std::string headline;
  std::string detail;
  // The ordinary hand-over every asset change goes through - loading, the
  // scheduled switch, outputs confirming it. Only a missed activation is not.
  // The Program monitor names routine waits in its footer and raises a
  // banner only once one lasts long enough to need the operator.
  bool routine{};
};

// Uses publication evidence, independently of renderer capability readiness.
[[nodiscard]] ProgramAssetStatus program_asset_status(
    core::AssetSetReadiness local, core::AssetCommit permit,
    core::u64 control_frame, bool required,
    const OutputMonitorState::Cluster& cluster, std::string_view loading_detail);

// Pure operator wording for one node's machine-local content readiness. Kept
// public so lifecycle tests can prove missing files, stale revisions, and local
// verification failures remain distinguishable without inspecting glyphs.
[[nodiscard]] std::string
output_node_content_label(const OutputMonitorState::Node &node);

[[nodiscard]] std::string_view
output_node_connection_label(const OutputMonitorState::Node &node) noexcept;

// Why this node is not driving the displays its bindings name, in the words
// an operator would use to go and fix it. Empty when nothing is wrong or
// nothing has been reported. Kept public so tests can prove a display fault
// stays distinguishable from an asset one without inspecting glyphs.
[[nodiscard]] std::string_view
output_node_deployment_label(const OutputMonitorState::Node &node) noexcept;

enum class OutputLifecycleAction : core::u8 {
  Status,
  Start,
  Restart,
  Stop,
};

struct OutputLifecycleRequest final {
  doc::NodeId device{};
  core::u32 node_index{};
  std::string endpoint;
  OutputLifecycleAction action{OutputLifecycleAction::Status};
  // A cluster action is expanded by the background controller from the latest
  // valid, unique render-server targets. No stale row list is captured here.
  bool all{};

  bool operator==(const OutputLifecycleRequest &) const = default;
};

struct OutputMonitorFrame final {
  bool check_render_conformance{};
  std::optional<OutputLifecycleRequest> lifecycle;
};

// The Program row is local render status. Render-server rows are explicit,
// document-bound lifecycle targets; drawing only emits one fixed action intent
// and never performs network work on the UI thread.
//
// Laid out as a table: a strip of status chips for the cluster, then one row
// per output - name, resolution, rate, a status pill, assets and sync where
// they apply, and its lifecycle buttons - with everything longer behind a
// click that opens the row's details.
class OutputMonitorPanel final {
public:
  explicit OutputMonitorPanel(gui::UiId id) noexcept : id_(id) {}

  [[nodiscard]] OutputMonitorFrame draw(gui::UiContext &ctx,
                                        const gui::Rect &bounds,
                                        const OutputMonitorState &state);

  // The table's fixed geometry, from the top of the panel.
  static constexpr float summary_height = 30.0F;
  static constexpr float header_height = 22.0F;
  static constexpr float row_height = 32.0F;
  static constexpr float footer_height = 32.0F;

private:
  bool conformance_expanded_{};
  bool timing_expanded_{};
  bool program_expanded_{};
  std::set<doc::NodeId> expanded_;
  gui::UiId id_{};
  gui::ScrollRegion scroll_;
};

// Machine-local settings the operator would otherwise pass on the command
// line. These are not document state: nothing here is saved into a project or
// replicated, so editing them authors no command.
//
// Every field takes effect when the process starts - except the render node
// password, which applies at once. The panel says which is which rather than
// implying a live change, and a command-line argument given this run still
// wins, so a scripted launch stays exactly reproducible.
struct EditorPreferences final {
  enum class TranscodePriority : core::u8 {
    Background,
    Balanced,
    Fast,
  };

  std::string director_listen;
  std::string show_id;
  std::string director_epoch;
  // Whether a render node password is set. The password itself never comes
  // back to the panel: it is kept only as the key it derives.
  bool render_node_password{};
  std::string wal_path;
  std::string proxy_cache_directory;
  bool proxy_cache_next_to_source{};
  core::u32 proxy_cache_gibibytes{8U};
  core::u32 output_buffer_frames{12U};
  core::u32 output_read_ahead_frames{12U};
  core::u32 playback_cache_mebibytes{2'048U};
  core::u32 playback_trail_frames{};
  TranscodePriority proxy_transcode_priority{TranscodePriority::Background};

  bool operator==(const EditorPreferences &) const = default;
};

// What the panel shows: the stored values it edits, what this run is actually
// using, and whether a command-line argument overrode each field.
struct EditorPreferencesState final {
  EditorPreferences stored;
  EditorPreferences active;
  bool director_listen_overridden{};
  bool show_id_overridden{};
  bool director_epoch_overridden{};
  // --lifecycle-password or --lifecycle-secret-file was given this run, so
  // the stored password is not the one in use until the next plain launch.
  bool render_node_password_overridden{};
  bool wal_path_overridden{};
  // Result of the last save attempt, shown in the panel. Empty means nothing
  // has been attempted this session.
  std::string message;
};

struct PreferencesFrame final {
  // Present only on the frame the operator pressed Save.
  std::optional<EditorPreferences> save;
  // With `save`, when the operator typed a new render node password or
  // cleared it: the new password, empty for none. Absent keeps the stored one.
  std::optional<std::string> render_node_password;
  // Clears the cache currently in use on this machine. This is intentionally
  // independent of Save so an unsaved draft can never redirect a deletion.
  bool clear_proxy_cache{};
};

// The document half of the same idea: how this project's effects are computed.
//
// Split from Preferences deliberately. Preferences describe the machine and are
// never replicated; these change the picture, so they are an ordinary document
// edit that undoes, saves, and reaches every render node. Nothing here belongs
// beside an individual effect in the Inspector - a kernel width is set once per
// show and then left alone, and putting it on every blur node would be five
// controls an operator scrolls past forever.
// What became of the authored environment map, from the renderer's side.
enum class EnvironmentStatus : core::u8 {
  // No map authored. The ordinary case, and not a fault: the hemisphere is a
  // real answer, so this must not read as an error.
  NotAuthored,
  // Authored and still being read or decoded on the residency worker.
  Loading,
  // Read or decode failed. Sticky until the path changes.
  Failed,
  // Authored, and the renderer this project selected cannot use it. Image-
  // based lighting is Advanced only, and a Basic project shows the hemisphere
  // however good the map is.
  BackendUnsupported,
  // There is no stage node to store it on, so Apply has nowhere to write and
  // the field reverts. Its own state because that is a project-structure
  // problem rather than anything about the map, and the two look identical
  // from the operator's side - the value simply does not stick.
  NoStage,
  // Lighting the scene.
  Ready,
};

[[nodiscard]] std::string_view
environment_status_text(EnvironmentStatus status) noexcept;

// What became of the authored colour, from the output transform's side.
//
// Every way it can fail is otherwise silent: a look file that does not exist,
// a table still baking, and a project running Basic - where colour is never
// applied because the display transform lives on the scene-linear path - all
// show the same picture. Naming the state is the difference between "the look
// does nothing" and one specific thing to fix.
enum class ColorStatus : core::u8 {
  // The table for the authored transform and look is still baking on the
  // residency worker. The previous table stays on the output meanwhile.
  Loading,
  // Baked and live on the output.
  Ready,
  // The look file could not be read or is not a 3D `.cube`. The transform is
  // live without it, so the wall is predictable while the path is fixed.
  Failed,
  // The project is running Basic, which is display-referred already: nothing
  // here reaches the wall until Advanced is selected and available.
  BackendUnsupported,
};

[[nodiscard]] std::string_view color_status_text(ColorStatus status) noexcept;
// The residency state, in the operator's terms.
[[nodiscard]] ColorStatus
color_status_for(render::DisplayLutState state) noexcept;

struct ProjectSettingsState final {
  scene::EffectSettings settings{};
  scene::RendererSettings renderer{};
  // The lighting environment, which lives on the stage node rather than in the
  // renderer policy: it is content, not a capability. Edited here because it is
  // one per scene and is selected through the Environment scene row.
  scene::Environment environment{};
  doc::NodeId stage_node{};
  // What the renderer is actually doing with the authored map.
  //
  // Reported because every way this can fail is otherwise silent: a path that
  // does not resolve, a file that is not Radiance, and a project still on the
  // Basic backend all produce exactly the same thing on screen - the
  // hemisphere - and an operator has no way to tell which. Naming the state is
  // the difference between "it does not work" and one specific thing to fix.
  EnvironmentStatus environment_status{EnvironmentStatus::NotAuthored};
  // The document node the values came from, or zero when the project has none
  // and the authoring boundary must create one.
  doc::NodeId node{};
  // Lens calibration is camera-owned document state, unlike the project-wide
  // renderer policy above. Project Settings edits the active stage camera when
  // one is available; a zero source deliberately keeps the controls read-only.
  std::optional<scene::LensProfile> lens_profile;
  doc::NodeId camera_node{};
  // Which authored passes the renderer actually ran on the last frame.
  //
  // Last in the struct because callers brace-initialise the fields above
  // positionally, and because it is observed rather than edited - Apply never
  // writes it back.
  render::RendererPassStatus passes{};
  // What the output transform did with the authored colour. Observed, like
  // the passes above, and filled by the editor shell that owns the transform.
  ColorStatus color_status{ColorStatus::Ready};
  bool bake_running{};
  std::string bake_status;
  doc::NodeId reflection_anchor{};
  // Program is a published revision drawn with renderer settings other than
  // these; the next PUBLISH carries these to Program.
  bool program_renderer_differs{};
};

struct ProjectSettingsValues final {
  scene::EffectSettings settings{};
  scene::RendererSettings renderer{};
  scene::Environment environment{};
  std::optional<scene::LensProfile> lens_profile;

  bool operator==(const ProjectSettingsValues &) const = default;
};

struct ProjectSettingsFrame final {
  std::optional<scene::BakedLighting> bake;
  std::string bake_destination;
  bool cancel_bake{};
  // A committed modal draft or an immediate edit from the scene Inspector.
  std::optional<ProjectSettingsValues> apply;
};

struct EnvironmentKeyEdit final {
  scene::PropertyChannel channel{};
  double value{};
  bool erase{};
};

struct SceneSettingsFrame final {
  std::optional<FilePickerRequest> file_picker;
  ProjectSettingsFrame project;
  std::vector<EnvironmentKeyEdit> keys;
};

// Persistent scene controls share normal Inspector layout and author each
// gesture immediately. Resource/topology policies remain discrete settings;
// environment values use the same channels and diamonds as object properties.
class SceneSettingsPanel final {
public:
  void set_bake_destination(std::string path) { bake_destination_ = std::move(path); }
  [[nodiscard]] SceneSettingsFrame draw(gui::UiContext& ctx, const gui::Rect& bounds,
      SceneSettingsTarget target, const ProjectSettingsState& state,
      std::span<const scene::ChannelKeyframes> keyframes, core::u64 show_frame,
      std::span<const ElementInfo> elements = {}, bool auto_key = true);
private:
  NumberField fields_;
  SceneSettingsTarget target_{SceneSettingsTarget::None};
  doc::NodeId stage_{};
  gui::ScrollRegion scroll_;
  std::array<bool, 10> sections_{{true,false,false,false,false,false,true,false,false,false}};
  std::string bake_destination_{"lighting"};
};

class ProjectSettingsPanel final {
public:
  explicit ProjectSettingsPanel(gui::UiId id) noexcept : id_(id) {}

  void open(const scene::EffectSettings &settings,
            const scene::RendererSettings &renderer,
            const std::optional<scene::LensProfile> &lens_profile = {},
            const scene::Environment &environment = {});
  void close() noexcept { open_ = false; }
  void adopt_bake(const scene::BakedLighting& baked) { environment_draft_.baked = baked; }
  [[nodiscard]] bool is_open() const noexcept { return open_; }

  // `bounds` is the whole window, as for Preferences: this is a modal card and
  // the caller must have denied the pointer to everything drawn before it.
  [[nodiscard]] ProjectSettingsFrame draw(gui::UiContext &ctx,
                                          const gui::Rect &bounds,
                                          const ProjectSettingsState &state);

private:
  enum class Category : core::u8 { Tracking, Lens };

  gui::UiId id_{};
  bool open_{};
  Category category_{Category::Tracking};
  scene::EffectSettings draft_{};
  scene::RendererSettings renderer_draft_{};
  scene::Environment environment_draft_{};
  std::optional<scene::LensProfile> lens_draft_;
  NumberField tracking_fields_;
  gui::ScrollRegion scroll_;
};

// The Preferences dialog: modal, editor-local, and authoring nothing.
//
// It owns its draft so cancelling really discards, and it dims and covers the
// whole window. The scrim is hit-tested after every control in the card,
// because gui::UiContext resolves a frame's pointer first-wins: a full-screen
// grab registered first would swallow the clicks meant for the card. Callers
// must also deny the pointer to everything drawn earlier in the frame - see
// gui::UiContext::push_pointer_block - or the workspace behind stays live.
class PreferencesPanel final {
public:
  explicit PreferencesPanel(gui::UiId id) noexcept : id_(id) {}

  void open(const EditorPreferences &stored);
  void close() noexcept;
  [[nodiscard]] bool is_open() const noexcept { return open_; }
  [[nodiscard]] const EditorPreferences &draft() const noexcept {
    return draft_;
  }

  // `bounds` is the whole window. Returns a save request when the operator
  // pressed Save; closing is reported through is_open().
  [[nodiscard]] PreferencesFrame draw(gui::UiContext &ctx,
                                      const gui::Rect &bounds,
                                      const EditorPreferencesState &state);

private:
  gui::UiId id_{};
  bool open_{};
  gui::ScrollRegion scroll_;
  EditorPreferences draft_;
  // The password being typed, and whether the operator touched it at all.
  std::string password_draft_;
  bool password_edited_{};
};

} // namespace mediaserver::editor
