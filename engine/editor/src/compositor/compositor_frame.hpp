#pragma once

// One CompositorPanel::draw as an object. draw() builds a Frame from its
// arguments and runs the panel's phases in order; state one phase hands to
// another lives here, everything else stays local to its phase. Helpers
// several phases call are Frame methods. Internal to the editor library.

#include "compositor_support.hpp"

namespace mediaserver::editor {

struct CompositorPanel::Frame final {
  CompositorPanel &panel;
  gui::UiContext &ctx;
  const gui::Rect &bounds;
  const compositor::CompositorGraph &compositor_graph;
  std::span<const ElementInfo> elements;
  CompositorFrame frame{};

  // Set by begin_frame(): the toolbar strip, everything under it, the
  // graph canvas, and the node whose Parameters show beside it.
  gui::Rect toolbar{};
  gui::Rect content{};
  const compositor::GraphNode *parameter_node{};
  gui::Rect canvas{};
  gui::Rect parameter_bounds{};
  bool parameter_resize_pressed{};

  // Set by draw_toolbar().
  std::size_t output_count{};
  gui::Rect palette_bounds{};

  // Set by build_menus(): what the node search and right-click menu offer
  // and where they sit, claimed before the canvas sees the pointer.
  std::vector<compositor_ui::MenuItem> menu_items{};
  gui::Rect menu_bounds{};
  std::vector<compositor_ui::ContextItem> context_items{};
  gui::Rect context_bounds{};

  // Set by begin_canvas().
  bool palette_pointer_blocked{};

  // Set by interact_ports(): every node and port as laid out this frame.
  std::vector<compositor_ui::CompositorNodeVisual> nodes{};
  std::vector<compositor_ui::CompositorPortVisual> inputs{};
  std::vector<compositor_ui::CompositorPortVisual> outputs{};
  std::span<const doc::NodeId> lifted{};
  bool selection_pressed{};
  bool pointer_over_any_node{};
  // The minimap: the whole graph and the visible part of it, bottom-right of
  // the canvas. Pressing or dragging in it centres the view there.
  gui::Rect minimap{};
  gui::Rect minimap_world{};
  float minimap_scale{};

  // Set by interact_nodes().
  bool drag_released{};
  doc::NodeId released_node{};
  bool chrome_readable{};
  bool inline_editable{};
  float comment_header{};

  // Set by interact_wires(): each wire's curve, for picking and drawing.
  std::vector<compositor_ui::EdgeVisual> edges{};
  const compositor_ui::EdgeVisual *insert_edge{};
  float point_radius{};
  const compositor_ui::EdgeVisual *hovered_edge{};
  bool wiring{};

  // Set by draw_wires().
  float wire_width{};
  std::optional<std::pair<std::string, gui::UiColor>> dragged_number{};

  // Set by draw_nodes(): the input a wire being drawn would snap to.
  const compositor_ui::CompositorPortVisual *snapped{};
  const compositor_ui::CompositorPortVisual *connecting_source{};

  // Helpers beside begin_frame().
  [[nodiscard]] const compositor::GraphComment *comment_of(doc::NodeId id);
  void set_zoom(float requested, float anchor_x, float anchor_y) noexcept;

  // Whether a palette entry matches the search.
  [[nodiscard]] bool
  palette_matches(const compositor_ui::CompositorPaletteItem &item);
  [[nodiscard]] bool
  palette_matches_text(const compositor_ui::CompositorPaletteItem &item,
                       std::string_view search);
  [[nodiscard]] bool
  palette_direct_matches(const compositor_ui::CompositorPaletteItem &item);

  // Wire order and bypass, shared by the keyboard, the right-click menu
  // and Parameters.
  // Moves a wire to `target`'s place in their input's order; the wires
  // between shift up or down one. The whole input is numbered afresh.
  void move_in_order(doc::NodeId moved, doc::NodeId target);
  // The wire before or after one in its input's order, if any.
  [[nodiscard]] std::optional<doc::NodeId> order_neighbour(doc::NodeId id,
                                                           int step);
  // The selected nodes that can be bypassed, and whether all of them are.
  [[nodiscard]] std::pair<std::vector<doc::NodeId>, bool> bypass_targets();
  // Bypasses the selection's bypassable nodes, or restores them when every
  // one already is.
  void toggle_bypass();

  // Layout, snapping and comment-box queries over this frame's nodes.
  void build_visuals();
  [[nodiscard]] gui::UiPoint to_screen(compositor::WirePoint point) noexcept;
  [[nodiscard]] compositor::WirePoint to_graph(float x, float y) noexcept;
  [[nodiscard]] std::vector<doc::NodeId>
  edges_at(compositor::EdgeEndpoint endpoint, bool source);
  // While a wire is being drawn, the input it would connect to: the
  // compatible port whose row is under the pointer, or failing that the
  // nearest compatible dot within snapping reach.
  [[nodiscard]] const compositor_ui::CompositorPortVisual *snapped_input();
  // While wires lifted off an output are being moved, the output they would
  // move to: one every lifted wire's input could take.
  [[nodiscard]] const compositor_ui::CompositorPortVisual *snapped_output();
  // A comment box as it is drawn: dragged, held after a release, or authored.
  [[nodiscard]] compositor::GraphComment
  drawn_comment(const compositor::GraphComment &comment);
  // The graph's extent in graph units: every node and comment box, or only
  // the selected nodes.
  [[nodiscard]] std::optional<gui::Rect> graph_extent(bool only_selected);
  // A wire's order number is pressed to drag it, and let go on another
  // number on the same input to move the wire to that place.
  [[nodiscard]] const OrderDisc *order_target(float x, float y);

  // Selection, collapse and the parts of a node that take input.
  [[nodiscard]] bool is_selected(doc::NodeId node);
  void add_to_selection(doc::NodeId node);
  // Where a node is now in graph units: held, if a released move is still
  // on its way, or as authored.
  [[nodiscard]] gui::UiPoint graph_position(const compositor::GraphNode &node);
  // Collapses or opens nodes: the selection, when the toggled node is in it.
  void toggle_collapse(doc::NodeId node, bool fold);
  // The toggle sits at the right end of the title line.
  [[nodiscard]] gui::Rect
  toggle_bounds(const compositor_ui::CompositorNodeVisual &visual);
  // An inline value's row - below the port rows - and its field.
  [[nodiscard]] gui::Rect
  inline_row(const compositor_ui::CompositorNodeVisual &visual,
             std::size_t index);
  [[nodiscard]] gui::Rect inline_field(const gui::Rect &row);
  [[nodiscard]] bool inside_canvas(const gui::Rect &rect) noexcept;

  // Reroute points.
  [[nodiscard]] std::vector<compositor::WirePoint>
  edge_points(const compositor::GraphEdge &edge);
  void emit_points(doc::NodeId edge, std::vector<compositor::WirePoint> points,
                   const std::vector<compositor::WirePoint> &from);

  // Layout tools, shared by the keyboard and the right-click menu.
  // The selected nodes' boxes in graph units.
  [[nodiscard]] std::vector<compositor_ui::LayoutBox> selected_boxes();
  // Authors moves as one step, held on screen until they arrive.
  void apply_moves(const std::vector<CompositorMoveIntent> &moves);
  // Fits a graph-space rectangle in the canvas, no closer than 150%.
  void frame_view(const gui::Rect &target);
  // Straighten Connection: each selected node moves up or down so the wire
  // into its first connected input runs level; a node with no input wire
  // levels the wire out of its first connected output instead. Left to
  // right, so a straightened chain stays straight.
  [[nodiscard]] std::vector<CompositorMoveIntent> straightened();
  // Arrange: columns by how many wires stand between a node and the end of
  // its chain, right to left toward Output, each column ordered to follow
  // what it feeds. The selection's top-right corner stays put. With fewer
  // than two nodes selected it arranges the whole graph.
  [[nodiscard]] std::vector<CompositorMoveIntent> arranged();
  // A comment box around the selected nodes, with room for its title.
  void comment_selection();
  // Copy, paste and duplicate: the Director keeps the copied nodes; the ones
  // a paste or duplicate adds are selected when they arrive.
  void expect_new_nodes();
  void say(std::string text);
  [[nodiscard]] std::ptrdiff_t unplaceable();
  void copy_selection();
  void paste_here();
  void duplicate_selection();
  void start_rename();

  // Drawing beside the wires.
  // A small numbered disc on a wire, or a problem mark.
  [[nodiscard]] gui::Rect wire_disc(gui::UiPoint at, std::string_view text,
                                    gui::UiColor ring, gui::UiColor fill);
};

} // namespace mediaserver::editor
