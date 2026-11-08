#pragma once

// The compositor graph editor: the panel and the intents it reports.

#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/library_panel.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/editor/selection.hpp>
#include <mediaserver/gui/canvas_view.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mediaserver::editor {

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
    if (!last_canvas_.contains(screen_x, screen_y) || view_.zoom <= 0.0F) {
      return std::nullopt;
    }
    const gui::UiPoint at = view_.to_graph(last_canvas_, screen_x, screen_y);
    return gui::UiPoint{at.x - node_width * 0.5F, at.y - node_height * 0.5F};
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

  [[nodiscard]] float graph_zoom() const noexcept { return view_.zoom; }
  [[nodiscard]] gui::UiPoint graph_pan() const noexcept {
    return gui::UiPoint{view_.pan_x, view_.pan_y};
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
    view_ = gui::CanvasView{};
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
  // A colour parameter's swatch as a colour well. What the picker chose
  // reaches the graph one channel a frame, since a frame carries one edit.
  void colour_well_field(gui::UiContext &ctx, const gui::Rect &well,
                         doc::NodeId node, core::u64 slot,
                         gui::UiColor current,
                         const std::array<CompositorProperty, 3U> &channels,
                         CompositorFrame &frame);
  struct ColourTarget final {
    gui::UiId well{};
    doc::NodeId node{};
    std::array<CompositorProperty, 3U> channels{};
    gui::UiColor color{};
  };
  std::optional<ColourTarget> colour_target_;
  bool colour_editing_{};
  float parameter_width_{};
  float parameter_height_{};
  float parameter_scroll_{};
  float parameter_content_height_{};
  float parameter_thumb_grab_{};
  // One bit per collapsible Parameters section; set means collapsed.
  core::u32 collapsed_sections_{};
  // The graph's pan and zoom over the canvas.
  gui::CanvasView view_{};
  bool panning_{};
  gui::Rect last_canvas_{};
};

} // namespace mediaserver::editor
