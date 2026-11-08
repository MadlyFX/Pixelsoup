// The canvas: pan and zoom, reveal, node and port layout, starting and
// releasing wires, the minimap and wire order numbers.

#include "compositor_frame.hpp"

#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/shapes.hpp>

namespace mediaserver::editor {

using namespace panel_helpers;
using namespace compositor_ui;

bool CompositorPanel::begin_canvas(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const compositor::CompositorGraph &compositor_graph = f.compositor_graph;
  const std::span<const ElementInfo> elements = f.elements;
  auto &canvas = f.canvas;
  auto &palette_bounds = f.palette_bounds;
  auto &menu_bounds = f.menu_bounds;
  auto &context_bounds = f.context_bounds;
  auto &palette_pointer_blocked = f.palette_pointer_blocked;
  palette_pointer_blocked =
      (add_menu_open_ &&
       palette_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) ||
      (node_menu_.has_value() &&
       menu_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) ||
      (context_menu_.has_value() &&
       context_bounds.contains(ctx.pointer_x(), ctx.pointer_y()));
  if (palette_pointer_blocked) {
    ctx.push_pointer_block();
  }

  if (canvas.width <= 0.0F || canvas.height <= 0.0F) {
    if (palette_pointer_blocked) {
      ctx.pop_pointer_block();
    }
    ctx.pop_clip();
    return false;
  }

  if (canvas.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      ctx.wheel() != 0.0F && dragging_node_ == doc::NodeId{} &&
      !connecting_from_.has_value() && !relink_.has_value() &&
      !point_drag_.has_value()) {
    f.set_zoom(view_.zoom * std::pow(compositor_zoom_step, ctx.wheel()),
             ctx.pointer_x(), ctx.pointer_y());
  }
  if (ctx.middle_pointer_pressed() &&
      canvas.contains(ctx.pointer_x(), ctx.pointer_y())) {
    panning_ = true;
  }
  if (panning_ && ctx.middle_pointer_down()) {
    view_.pan_x += ctx.pointer_dx();
    view_.pan_y += ctx.pointer_dy();
  }
  if (ctx.middle_pointer_released()) {
    panning_ = false;
  }

  // An element chosen in another panel brings its Scene Layer - or the one
  // for the Track it plays on - into view, outlined for a moment.
  if (reveal_element_ != doc::NodeId{}) {
    const auto layer_for =
        [&](doc::NodeId scene) -> const compositor::GraphNode * {
      const auto found = std::find_if(
          compositor_graph.nodes.begin(), compositor_graph.nodes.end(),
          [scene](const compositor::GraphNode &node) noexcept {
            const auto *parameters =
                std::get_if<compositor::SceneLayerParameters>(&node.parameters);
            return node.kind == compositor::NodeKind::SceneLayer &&
                   parameters != nullptr && parameters->scene_node == scene;
          });
      return found != compositor_graph.nodes.end() ? std::addressof(*found)
                                                   : nullptr;
    };
    const compositor::GraphNode *layer = layer_for(reveal_element_);
    if (layer == nullptr) {
      const auto element = std::find_if(
          elements.begin(), elements.end(),
          [this](const ElementInfo &info) noexcept {
            return info.node == reveal_element_;
          });
      if (element != elements.end() &&
          element->timeline_lane != doc::NodeId{}) {
        layer = layer_for(element->timeline_lane);
      }
    }
    if (layer != nullptr) {
      const gui::UiPoint size = node_size(*layer);
      const gui::UiPoint at =
          pending_position(layer->node)
              .value_or(gui::UiPoint{layer->editor_x, layer->editor_y});
      const gui::Rect placed{at.x, at.y, size.x, size.y};
      const gui::Rect shown = view_.to_screen(canvas, placed);
      // Already wholly in view, the view stays where the operator left it.
      if (shown.x < canvas.x || shown.y < canvas.y ||
          shown.right() > canvas.right() || shown.bottom() > canvas.bottom()) {
        view_.centre_on(canvas, placed.center_x(), placed.center_y());
      }
      reveal_node_ = layer->node;
      reveal_until_ = reveal_requested_at_ + 1.5;
    }
    reveal_element_ = {};
  }

  return true;
}

void CompositorPanel::Frame::build_visuals() {
  nodes.clear();
  inputs.clear();
  outputs.clear();
  for (const compositor::GraphNode &node : compositor_graph.nodes) {
    float editor_x = node.editor_x;
    float editor_y = node.editor_y;
    if (const std::optional<gui::UiPoint> held =
            panel.pending_position(node.node)) {
      editor_x = held->x;
      editor_y = held->y;
    }
    const auto member = std::find_if(
        panel.drag_group_.begin(), panel.drag_group_.end(),
        [&node](const GroupMember &entry) noexcept {
          return entry.node == node.node;
        });
    if (panel.dragging_node_ != doc::NodeId{} &&
        member != panel.drag_group_.end()) {
      const auto grabbed = std::find_if(
          panel.drag_group_.begin(), panel.drag_group_.end(),
          [this](const GroupMember &entry) noexcept {
            return entry.node == panel.dragging_node_;
          });
      if (grabbed != panel.drag_group_.end()) {
        editor_x = member->start_x + panel.drag_x_ - grabbed->start_x;
        editor_y = member->start_y + panel.drag_y_ - grabbed->start_y;
      }
    }
    if (panel.comment_drag_.has_value() && !panel.comment_drag_->resizing) {
      const auto carried =
          std::find_if(panel.comment_drag_->carried.begin(),
                       panel.comment_drag_->carried.end(),
                       [&node](const GroupMember &entry) noexcept {
                         return entry.node == node.node;
                       });
      if (carried != panel.comment_drag_->carried.end()) {
        editor_x = carried->start_x + panel.comment_drag_->dx;
        editor_y = carried->start_y + panel.comment_drag_->dy;
      }
    }
    const gui::UiPoint size = panel.node_size(node);
    const gui::Rect node_bounds = panel.view_.to_screen(
        canvas, gui::Rect{editor_x, editor_y, size.x, size.y});
    nodes.push_back(CompositorNodeVisual{std::addressof(node), node_bounds});

    // A port's target is its half of the row - dot and name - reaching a
    // little past the node edge so the dot itself is inside it too.
    const float reach = compositor_port_radius * panel.view_.zoom + 4.0F;
    const auto append_ports =
        [&](std::span<const compositor::PortDescriptor> descriptors,
            std::vector<CompositorPortVisual> &destination, bool input) {
          for (std::size_t index = 0U; index < descriptors.size(); ++index) {
            const compositor::PortDescriptor &descriptor = descriptors[index];
            const std::optional<float> place = panel.port_row(
                compositor_graph, node, descriptor.index, input);
            if (!place.has_value()) {
              continue;
            }
            const float row_top =
                node_bounds.y + (panel.metrics_.header_height +
                                 panel.metrics_.row_height * *place) *
                                    panel.view_.zoom;
            const float row_height =
                panel.metrics_.row_height * panel.view_.zoom;
            const float x =
                input ? node_bounds.x : node_bounds.right();
            const gui::Rect hit =
                input ? gui::Rect{node_bounds.x - reach, row_top,
                                  node_bounds.width * 0.5F + reach, row_height}
                      : gui::Rect{node_bounds.x + node_bounds.width * 0.5F,
                                  row_top, node_bounds.width * 0.5F + reach,
                                  row_height};
            destination.push_back(CompositorPortVisual{
                compositor::EdgeEndpoint{node.node, descriptor.index},
                std::addressof(descriptor), x, row_top + row_height * 0.5F,
                hit});
          }
        };
    append_ports(compositor::input_ports(node.kind), inputs, true);
    append_ports(compositor::output_ports(node.kind), outputs, false);
  }
}

gui::UiPoint
CompositorPanel::Frame::to_screen(compositor::WirePoint point) noexcept {
  return panel.view_.to_screen(canvas, point.x, point.y);
}

compositor::WirePoint CompositorPanel::Frame::to_graph(float x,
                                                       float y) noexcept {
  const gui::UiPoint at = panel.view_.to_graph(canvas, x, y);
  return compositor::WirePoint{at.x, at.y};
}

std::vector<doc::NodeId>
CompositorPanel::Frame::edges_at(compositor::EdgeEndpoint endpoint,
                                 bool source) {
  std::vector<doc::NodeId> found;
  for (const compositor::GraphEdge &edge : compositor_graph.edges) {
    if ((source ? edge.from : edge.to) == endpoint) {
      found.push_back(edge.node);
    }
  }
  return found;
}

// While a wire is being drawn, the input it would connect to: the
// compatible port whose row is under the pointer, or failing that the
// nearest compatible dot within snapping reach.
const CompositorPortVisual *CompositorPanel::Frame::snapped_input() {
  if (!panel.connecting_from_.has_value() ||
      !canvas.contains(ctx.pointer_x(), ctx.pointer_y())) {
    return nullptr;
  }
  const CompositorPortVisual *const from =
      find_port(outputs, *panel.connecting_from_);
  if (from == nullptr) {
    return nullptr;
  }
  const CompositorPortVisual *best = nullptr;
  float best_rank = std::numeric_limits<float>::infinity();
  for (const CompositorPortVisual &to : inputs) {
    if (!can_connect(compositor_graph, *from, to, lifted)) {
      continue;
    }
    const float distance =
        std::hypot(ctx.pointer_x() - to.x, ctx.pointer_y() - to.y);
    const bool over_row = to.hit.contains(ctx.pointer_x(), ctx.pointer_y());
    if (!over_row && distance > compositor_port_snap_radius) {
      continue;
    }
    // A row under the pointer outranks any dot that is merely near.
    const float rank = over_row ? distance - 1.0e6F : distance;
    if (rank < best_rank) {
      best = std::addressof(to);
      best_rank = rank;
    }
  }
  return best;
}

// While wires lifted off an output are being moved, the output they would
// move to: one every lifted wire's input could take.
const CompositorPortVisual *CompositorPanel::Frame::snapped_output() {
  if (!panel.relink_.has_value() || !panel.relink_->moving_source ||
      !canvas.contains(ctx.pointer_x(), ctx.pointer_y())) {
    return nullptr;
  }
  const CompositorPortVisual *best = nullptr;
  float best_rank = std::numeric_limits<float>::infinity();
  for (const CompositorPortVisual &from : outputs) {
    const bool accepts = std::all_of(
        panel.relink_->edges.begin(), panel.relink_->edges.end(),
        [&](doc::NodeId id) {
          const compositor::GraphEdge *const edge = compositor_graph.edge(id);
          const CompositorPortVisual *const to =
              edge != nullptr ? find_port(inputs, edge->to) : nullptr;
          return to != nullptr &&
                 (from.endpoint == panel.relink_->origin ||
                  can_connect(compositor_graph, from, *to, lifted));
        });
    if (!accepts) {
      continue;
    }
    const float distance =
        std::hypot(ctx.pointer_x() - from.x, ctx.pointer_y() - from.y);
    const bool over_row =
        from.hit.contains(ctx.pointer_x(), ctx.pointer_y());
    if (!over_row && distance > compositor_port_snap_radius) {
      continue;
    }
    const float rank = over_row ? distance - 1.0e6F : distance;
    if (rank < best_rank) {
      best = std::addressof(from);
      best_rank = rank;
    }
  }
  return best;
}

// A comment box as it is drawn: dragged, held after a release, or authored.
compositor::GraphComment
CompositorPanel::Frame::drawn_comment(const compositor::GraphComment &comment) {
  compositor::GraphComment drawn = comment;
  if (panel.pending_comment_.has_value() &&
      panel.pending_comment_->held.node == comment.node) {
    drawn = panel.pending_comment_->held;
  }
  if (panel.comment_drag_.has_value() &&
      panel.comment_drag_->comment == comment.node) {
    drawn = panel.comment_drag_->start;
    if (panel.comment_drag_->resizing) {
      drawn.width = std::max(drawn.width + panel.comment_drag_->dx,
                             compositor::min_comment_extent);
      drawn.height = std::max(drawn.height + panel.comment_drag_->dy,
                              compositor::min_comment_extent);
    } else {
      drawn.x += panel.comment_drag_->dx;
      drawn.y += panel.comment_drag_->dy;
    }
  }
  return drawn;
}

// The graph's extent in graph units: every node and comment box, or only
// the selected nodes.
std::optional<gui::Rect>
CompositorPanel::Frame::graph_extent(bool only_selected) {
  std::optional<gui::Rect> extent;
  for (const compositor::GraphNode &node : compositor_graph.nodes) {
    if (only_selected &&
        !std::binary_search(panel.selected_nodes_.begin(),
                            panel.selected_nodes_.end(), node.node)) {
      continue;
    }
    const std::optional<gui::UiPoint> held = panel.pending_position(node.node);
    const gui::UiPoint size = panel.node_size(node);
    const gui::Rect box{held ? held->x : node.editor_x,
                        held ? held->y : node.editor_y, size.x, size.y};
    extent = extent.has_value() ? gui::enclosing(*extent, box) : box;
  }
  if (!only_selected) {
    for (const compositor::GraphComment &comment : compositor_graph.comments) {
      const compositor::GraphComment drawn = drawn_comment(comment);
      const gui::Rect box{drawn.x, drawn.y, drawn.width, drawn.height};
      extent = extent.has_value() ? gui::enclosing(*extent, box) : box;
    }
  }
  return extent;
}

// A wire's order number is pressed to drag it, and let go on another
// number on the same input to move the wire to that place.
const CompositorPanel::OrderDisc *CompositorPanel::Frame::order_target(float x,
                                                                       float y) {
  if (!panel.order_drag_.has_value()) {
    return nullptr;
  }
  const auto dragged =
      std::find_if(panel.order_discs_.begin(), panel.order_discs_.end(),
                   [this](const OrderDisc &disc) {
                     return disc.edge == *panel.order_drag_;
                   });
  if (dragged == panel.order_discs_.end()) {
    return nullptr;
  }
  for (const OrderDisc &disc : panel.order_discs_) {
    if (disc.edge != dragged->edge && disc.input == dragged->input &&
        std::hypot(x - disc.x, y - disc.y) <= disc.radius + 6.0F) {
      return std::addressof(disc);
    }
  }
  return nullptr;
}

void CompositorPanel::interact_ports(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const compositor::CompositorGraph &compositor_graph = f.compositor_graph;
  CompositorFrame &frame = f.frame;
  auto &canvas = f.canvas;
  auto &nodes = f.nodes;
  auto &inputs = f.inputs;
  auto &outputs = f.outputs;
  auto &lifted = f.lifted;
  auto &selection_pressed = f.selection_pressed;
  auto &pointer_over_any_node = f.pointer_over_any_node;
  auto &minimap = f.minimap;
  auto &minimap_world = f.minimap_world;
  auto &minimap_scale = f.minimap_scale;
  nodes.reserve(compositor_graph.nodes.size());
  inputs.reserve(compositor_graph.nodes.size() * 2U);
  outputs.reserve(compositor_graph.nodes.size());

  f.build_visuals();

  // Wires lifted by a Ctrl drag do not count as already there while the
  // operator decides where they go.
  lifted =
      relink_.has_value() ? std::span<const doc::NodeId>{relink_->edges}
                          : std::span<const doc::NodeId>{};

  selection_pressed = false;
  pointer_over_any_node = std::any_of(
      nodes.begin(), nodes.end(), [&ctx](const CompositorNodeVisual &visual) {
        return visual.bounds.contains(ctx.pointer_x(), ctx.pointer_y());
      });
  if ((connecting_from_.has_value() || relink_.has_value()) &&
      ctx.pointer_released()) {
    if (relink_.has_value() && relink_->moving_source) {
      // Let go on another output: the wires move to it. On nothing: they
      // are disconnected. Back on their own output: nothing changes.
      const CompositorPortVisual *const target = f.snapped_output();
      if (target == nullptr || target->endpoint != relink_->origin) {
        CompositorRewireIntent rewire{relink_->edges, {}};
        if (target != nullptr) {
          for (const doc::NodeId id : relink_->edges) {
            if (const compositor::GraphEdge *const edge =
                    compositor_graph.edge(id)) {
              rewire.add_edges.push_back(
                  CompositorWire{target->endpoint, edge->to});
            }
          }
        }
        frame.rewire = std::move(rewire);
      }
    } else if (connecting_from_.has_value()) {
      const CompositorPortVisual *const from =
          find_port(outputs, *connecting_from_);
      const CompositorPortVisual *const to = f.snapped_input();
      std::optional<doc::NodeId> occupied;
      if (to != nullptr && to->descriptor != nullptr &&
          !to->descriptor->multiple) {
        const auto wired = std::find_if(
            compositor_graph.edges.begin(), compositor_graph.edges.end(),
            [to](const compositor::GraphEdge &edge) noexcept {
              return edge.to == to->endpoint;
            });
        if (wired != compositor_graph.edges.end()) {
          occupied = wired->node;
        }
      }
      if (relink_.has_value()) {
        // A wire lifted off an input: it lands on another input, or is
        // disconnected when let go on nothing; put back, nothing changes.
        if (to == nullptr || to->endpoint != relink_->origin) {
          CompositorRewireIntent rewire{relink_->edges, {}};
          if (occupied.has_value() &&
              std::find(lifted.begin(), lifted.end(), *occupied) ==
                  lifted.end()) {
            rewire.remove_edges.push_back(*occupied);
          }
          if (from != nullptr && to != nullptr) {
            rewire.add_edges.push_back(
                CompositorWire{from->endpoint, to->endpoint});
          }
          frame.rewire = std::move(rewire);
        }
      } else if (from != nullptr && to != nullptr) {
        frame.connect_ports =
            CompositorConnectIntent{from->endpoint, to->endpoint, occupied};
      } else if (from != nullptr &&
                 canvas.contains(ctx.pointer_x(), ctx.pointer_y()) &&
                 !pointer_over_any_node) {
        // Let go on empty canvas: offer the nodes that can take this wire,
        // to be created here already connected.
        node_menu_ = NodeMenu{ctx.pointer_x(), ctx.pointer_y(),
                              from->endpoint, from->descriptor->type};
      }
    }
    connecting_from_.reset();
    relink_.reset();
  }

  if (minimap_ && canvas.width >= 360.0F && canvas.height >= 220.0F) {
    const gui::Rect view = view_.visible(canvas);
    const std::optional<gui::Rect> extent = f.graph_extent(false);
    minimap_world = extent.has_value() ? gui::enclosing(view, *extent) : view;
    constexpr float apron = 60.0F;
    minimap_world = gui::Rect{minimap_world.x - apron, minimap_world.y - apron,
                              minimap_world.width + apron * 2.0F,
                              minimap_world.height + apron * 2.0F};
    minimap = gui::Rect{canvas.right() - 212.0F,
                        canvas.bottom() - 142.0F, 200.0F, 130.0F};
    minimap_scale = std::min(minimap.width / minimap_world.width,
                             minimap.height / minimap_world.height);
    const gui::Interaction interaction =
        clipped_grab(ctx, gui::ui_id(id_, "compositor.minimap"), minimap, canvas);
    if (interaction.pressed) {
      selection_pressed = true;
    }
    if (interaction.held) {
      const float graph_x =
          minimap_world.x + (ctx.pointer_x() - minimap.x) / minimap_scale;
      const float graph_y =
          minimap_world.y + (ctx.pointer_y() - minimap.y) / minimap_scale;
      view_.centre_on(canvas, graph_x, graph_y);
      f.build_visuals();
    }
  }

  for (const OrderDisc &disc : order_discs_) {
    const float reach = disc.radius + 2.0F;
    const gui::Interaction interaction = clipped_grab(
        ctx, gui::ui_id(id_, "compositor.order", disc.edge.value()),
        gui::Rect{disc.x - reach, disc.y - reach, reach * 2.0F, reach * 2.0F},
        canvas);
    ctx.cursor_for(interaction, gui::Cursor::Move);
    if (interaction.pressed) {
      order_drag_ = disc.edge;
      selection_ = CompositorSelection{CompositorSelectionKind::Edge, disc.edge};
      selected_nodes_.clear();
      frame.selection = selection_;
      selection_pressed = true;
    }
    if (interaction.released && order_drag_ == disc.edge) {
      if (const OrderDisc *const target =
              f.order_target(ctx.pointer_x(), ctx.pointer_y())) {
        f.move_in_order(disc.edge, target->edge);
      }
      order_drag_.reset();
    }
  }
  if (order_drag_.has_value() && !ctx.pointer_down() &&
      !ctx.pointer_released()) {
    order_drag_.reset();
  }
}

} // namespace mediaserver::editor
