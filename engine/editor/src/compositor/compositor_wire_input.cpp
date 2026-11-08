// Picking wires, splicing a dragged node into one, reroute points, the
// right click and the rubber band.

#include "compositor_frame.hpp"

#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/shapes.hpp>

namespace mediaserver::editor {

using namespace panel_helpers;
using namespace compositor_ui;

std::vector<compositor::WirePoint>
CompositorPanel::Frame::edge_points(const compositor::GraphEdge &edge) {
  if (panel.point_drag_.has_value() && panel.point_drag_->edge == edge.node) {
    return panel.point_drag_->points;
  }
  if (panel.pending_points_.has_value() &&
      panel.pending_points_->edge == edge.node) {
    return panel.pending_points_->points;
  }
  return edge.points;
}

void CompositorPanel::Frame::emit_points(doc::NodeId edge,
                                         std::vector<compositor::WirePoint> points,
                                         const std::vector<compositor::WirePoint> &from) {
  frame.waypoints = CompositorWaypointIntent{edge, points};
  panel.pending_points_ = PendingPoints{edge, std::move(points), from, 0U};
}

void CompositorPanel::interact_wires(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const compositor::CompositorGraph &compositor_graph = f.compositor_graph;
  CompositorFrame &frame = f.frame;
  auto &canvas = f.canvas;
  auto &parameter_resize_pressed = f.parameter_resize_pressed;
  auto &nodes = f.nodes;
  auto &inputs = f.inputs;
  auto &outputs = f.outputs;
  auto &selection_pressed = f.selection_pressed;
  auto &pointer_over_any_node = f.pointer_over_any_node;
  auto &minimap = f.minimap;
  auto &released_node = f.released_node;
  auto &comment_header = f.comment_header;
  auto &edges = f.edges;
  auto &insert_edge = f.insert_edge;
  auto &point_radius = f.point_radius;
  auto &hovered_edge = f.hovered_edge;
  auto &wiring = f.wiring;
  edges.reserve(compositor_graph.edges.size());
  for (const compositor::GraphEdge &edge : compositor_graph.edges) {
    const CompositorPortVisual *const from = find_port(outputs, edge.from);
    const CompositorPortVisual *const to = find_port(inputs, edge.to);
    if (from == nullptr || to == nullptr || from->descriptor == nullptr ||
        to->descriptor == nullptr ||
        from->descriptor->type != to->descriptor->type) {
      continue;
    }
    std::vector<compositor::WirePoint> points = f.edge_points(edge);
    std::vector<gui::UiPoint> screen;
    screen.reserve(points.size());
    for (const compositor::WirePoint point : points) {
      screen.push_back(f.to_screen(point));
    }
    edges.push_back(EdgeVisual{std::addressof(edge), from, to,
                               std::move(points),
                               routed_wire(gui::UiPoint{from->x, from->y},
                                           screen,
                                           gui::UiPoint{to->x, to->y}, view_.zoom)});
  }
  // Distance from the pointer to a wire, and the stretch it is nearest.
  const auto nearest_segment = [&](const EdgeVisual &visual) {
    float nearest = std::numeric_limits<float>::infinity();
    std::size_t segment{};
    for (std::size_t index = 0U; index < visual.segments.size(); ++index) {
      const float distance = wire_distance(visual.segments[index],
                                           ctx.pointer_x(), ctx.pointer_y());
      if (distance < nearest) {
        nearest = distance;
        segment = index;
      }
    }
    return std::pair{nearest, segment};
  };

  // A node being dragged that is free - no wires of its own - and has an
  // input and an output of the same kind can be dropped onto a wire of that
  // kind to splice itself in.
  insert_edge = nullptr;
  std::optional<std::pair<core::u8, core::u8>> insert_ports;
  if (dragging_node_ != doc::NodeId{} && drag_group_.size() == 1U) {
    const compositor::GraphNode *const dragged =
        compositor_graph.node(dragging_node_);
    const bool free =
        dragged != nullptr &&
        std::none_of(compositor_graph.edges.begin(),
                     compositor_graph.edges.end(),
                     [&](const compositor::GraphEdge &edge) noexcept {
                       return edge.from.node == dragging_node_ ||
                              edge.to.node == dragging_node_;
                     });
    float nearest = 12.0F;
    for (const EdgeVisual &visual : edges) {
      if (!free) {
        break;
      }
      const compositor::PortValueType type = visual.from->descriptor->type;
      const auto in = std::find_if(
          compositor::input_ports(dragged->kind).begin(),
          compositor::input_ports(dragged->kind).end(),
          [type](const compositor::PortDescriptor &port) noexcept {
            return port.type == type;
          });
      const auto out = std::find_if(
          compositor::output_ports(dragged->kind).begin(),
          compositor::output_ports(dragged->kind).end(),
          [type](const compositor::PortDescriptor &port) noexcept {
            return port.type == type;
          });
      if (in == compositor::input_ports(dragged->kind).end() ||
          out == compositor::output_ports(dragged->kind).end()) {
        continue;
      }
      const float distance = nearest_segment(visual).first;
      if (distance <= nearest) {
        nearest = distance;
        insert_edge = std::addressof(visual);
        insert_ports = std::pair{in->index, out->index};
      }
    }
  }
  if (released_node != doc::NodeId{} && insert_edge != nullptr &&
      insert_ports.has_value()) {
    frame.rewire = CompositorRewireIntent{
        {insert_edge->edge->node},
        {CompositorWire{insert_edge->edge->from,
                        compositor::EdgeEndpoint{released_node,
                                                 insert_ports->first}},
         CompositorWire{compositor::EdgeEndpoint{released_node,
                                                 insert_ports->second},
                        insert_edge->edge->to}}};
  }

  // Reroute points: drag to move, double-click to remove. Each is claimed
  // before the wires so a press on one never selects the wire under it.
  point_radius = std::clamp(5.0F * view_.zoom, 4.0F, 7.0F);
  for (const EdgeVisual &visual : edges) {
    for (std::size_t index = 0U; index < visual.points.size(); ++index) {
      const gui::UiPoint centre = f.to_screen(visual.points[index]);
      const gui::Rect handle{centre.x - point_radius - 2.0F,
                             centre.y - point_radius - 2.0F,
                             (point_radius + 2.0F) * 2.0F,
                             (point_radius + 2.0F) * 2.0F};
      const gui::Interaction interaction = clipped_grab(
          ctx,
          gui::ui_id(id_, "compositor.wire_point",
              visual.edge->node.value() * 32U + index),
          handle, canvas);
      ctx.cursor_for(interaction, gui::Cursor::Move);
      if (interaction.pressed) {
        selection_ =
            CompositorSelection{CompositorSelectionKind::Edge, visual.edge->node};
        selected_nodes_.clear();
        frame.selection = selection_;
        selection_pressed = true;
        if (clicks_.double_click(visual.edge->node.value() * 64U + 33U + index,
                                 ctx.time_seconds())) {
          std::vector<compositor::WirePoint> remaining = visual.points;
          remaining.erase(remaining.begin() +
                          static_cast<std::ptrdiff_t>(index));
          f.emit_points(visual.edge->node, std::move(remaining),
                      visual.edge->points);
        } else {
          point_drag_ = PointDrag{visual.edge->node, index, visual.points};
        }
      }
      if (interaction.held && point_drag_.has_value() &&
          point_drag_->edge == visual.edge->node &&
          point_drag_->index == index) {
        point_drag_->points[index] = f.to_graph(ctx.pointer_x(), ctx.pointer_y());
        if (interaction.released) {
          if (point_drag_->points != visual.edge->points) {
            f.emit_points(visual.edge->node, point_drag_->points,
                        visual.edge->points);
          }
          point_drag_.reset();
        }
      }
    }
  }

  // The wire nearest the pointer, within reach and not under a node, which
  // draws over it. Every wire still takes part in the interaction each
  // frame, so the one that was pressed always sees its release.
  hovered_edge = nullptr;
  std::size_t hovered_segment{};
  wiring = connecting_from_.has_value() || relink_.has_value();
  if (!wiring && dragging_node_ == doc::NodeId{} && !point_drag_.has_value() &&
      !pointer_over_any_node &&
      canvas.contains(ctx.pointer_x(), ctx.pointer_y())) {
    float nearest = compositor_wire_hit_radius;
    for (const EdgeVisual &visual : edges) {
      const auto [distance, segment] = nearest_segment(visual);
      if (distance <= nearest) {
        nearest = distance;
        hovered_edge = std::addressof(visual);
        hovered_segment = segment;
      }
    }
  }
  for (const EdgeVisual &visual : edges) {
    const bool hovered = std::addressof(visual) == hovered_edge;
    const gui::Rect pick =
        hovered ? gui::Rect{ctx.pointer_x() - 1.0F, ctx.pointer_y() - 1.0F,
                            2.0F, 2.0F}
                : gui::Rect{};
    const gui::Interaction interaction = ctx.grab(
        gui::ui_id(id_, "compositor.edge", visual.edge->node.value()), pick);
    ctx.cursor_for(interaction, gui::Cursor::Hand);
    if (!interaction.pressed) {
      continue;
    }
    selection_ =
        CompositorSelection{CompositorSelectionKind::Edge, visual.edge->node};
    selected_nodes_.clear();
    frame.selection = selection_;
    selection_pressed = true;
    if (ctx.alt_held()) {
      // Alt-click a wire to break it.
      frame.rewire = CompositorRewireIntent{{visual.edge->node}, {}};
    } else if (clicks_.double_click(visual.edge->node.value() * 64U,
                                    ctx.time_seconds()) &&
               visual.points.size() < compositor::max_wire_points) {
      // Double-click a wire to add a reroute point where it was clicked, in
      // the stretch it was clicked on.
      std::vector<compositor::WirePoint> points = visual.points;
      points.insert(points.begin() +
                        static_cast<std::ptrdiff_t>(hovered_segment),
                    f.to_graph(ctx.pointer_x(), ctx.pointer_y()));
      f.emit_points(visual.edge->node, std::move(points), visual.edge->points);
    }
  }
  // A right click on a node, a wire or a comment box's title opens its menu;
  // anywhere else on the canvas, like Tab, it opens the node search.
  if (!wiring && canvas.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      ctx.secondary_pointer_pressed() && !minimap.contains(ctx.pointer_x(),
                                                           ctx.pointer_y())) {
    node_menu_.reset();
    const auto node_under = std::find_if(
        nodes.rbegin(), nodes.rend(), [&ctx](const CompositorNodeVisual &visual) {
          return visual.bounds.contains(ctx.pointer_x(), ctx.pointer_y());
        });
    const auto comment_under = std::find_if(
        compositor_graph.comments.rbegin(), compositor_graph.comments.rend(),
        [&](const compositor::GraphComment &comment) {
          const compositor::GraphComment drawn = f.drawn_comment(comment);
          // Its title strip, which keeps its height at any zoom.
          gui::Rect title = view_.to_screen(
              canvas, gui::Rect{drawn.x, drawn.y, drawn.width, drawn.height});
          title.height = comment_header;
          return title.contains(ctx.pointer_x(), ctx.pointer_y());
        });
    if (node_under != nodes.rend()) {
      if (!f.is_selected(node_under->node->node)) {
        select_only(node_under->node->node);
      }
      selection_ = CompositorSelection{CompositorSelectionKind::Node,
                                       node_under->node->node};
      frame.selection = selection_;
      context_menu_ = ContextMenu{ctx.pointer_x(), ctx.pointer_y(),
                                  CompositorSelectionKind::Node,
                                  node_under->node->node};
    } else if (hovered_edge != nullptr) {
      selection_ = CompositorSelection{CompositorSelectionKind::Edge,
                                       hovered_edge->edge->node};
      selected_nodes_.clear();
      frame.selection = selection_;
      context_menu_ = ContextMenu{ctx.pointer_x(), ctx.pointer_y(),
                                  CompositorSelectionKind::Edge,
                                  hovered_edge->edge->node,
                                  f.to_graph(ctx.pointer_x(), ctx.pointer_y()),
                                  hovered_segment};
    } else if (comment_under != compositor_graph.comments.rend()) {
      selection_ = CompositorSelection{CompositorSelectionKind::Comment,
                                       comment_under->node};
      selected_nodes_.clear();
      frame.selection = selection_;
      context_menu_ = ContextMenu{ctx.pointer_x(), ctx.pointer_y(),
                                  CompositorSelectionKind::Comment,
                                  comment_under->node};
    } else {
      context_menu_.reset();
      node_menu_ = NodeMenu{ctx.pointer_x(), ctx.pointer_y()};
    }
  }
  if (!wiring && canvas.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      focused_ && !ctx.typing() && ctx.key_pressed(gui::Key::Tab)) {
    node_menu_ = NodeMenu{ctx.pointer_x(), ctx.pointer_y()};
  }

  // A press on empty canvas clears the selection and starts a rubber band;
  // with Shift the band adds to the selection, with Ctrl it takes from it.
  if (ctx.pointer_pressed() &&
      canvas.contains(ctx.pointer_x(), ctx.pointer_y()) && !selection_pressed &&
      !parameter_resize_pressed) {
    const Marquee::Mode mode = ctx.shift_held()  ? Marquee::Mode::Add
                               : ctx.ctrl_held() ? Marquee::Mode::Subtract
                                                 : Marquee::Mode::Replace;
    if (mode == Marquee::Mode::Replace) {
      selection_ = CompositorSelection{};
      selected_nodes_.clear();
      frame.selection = selection_;
    }
    marquee_ = Marquee{ctx.pointer_x(), ctx.pointer_y(), mode};
  }
  if (marquee_.has_value() && !ctx.pointer_down()) {
    // Dragged rightwards, the band takes the nodes it wholly contains;
    // leftwards, every node it touches.
    const float left = std::min(marquee_->start_x, ctx.pointer_x());
    const float top = std::min(marquee_->start_y, ctx.pointer_y());
    const gui::Rect band{left, top,
                         std::abs(ctx.pointer_x() - marquee_->start_x),
                         std::abs(ctx.pointer_y() - marquee_->start_y)};
    const bool containing = ctx.pointer_x() >= marquee_->start_x;
    if (band.width > 3.0F || band.height > 3.0F) {
      for (const CompositorNodeVisual &visual : nodes) {
        const gui::Rect &b = visual.bounds;
        const bool inside =
            containing
                ? b.x >= band.x && b.y >= band.y &&
                      b.right() <= band.right() &&
                      b.bottom() <= band.bottom()
                : b.x < band.right() && band.x < b.right() &&
                      b.y < band.bottom() && band.y < b.bottom();
        if (!inside) {
          continue;
        }
        if (marquee_->mode == Marquee::Mode::Subtract) {
          std::erase(selected_nodes_, visual.node->node);
        } else {
          f.add_to_selection(visual.node->node);
        }
      }
      if (selection_.kind != CompositorSelectionKind::Node ||
          !f.is_selected(selection_.node)) {
        selection_ = selected_nodes_.empty()
                         ? CompositorSelection{}
                         : CompositorSelection{CompositorSelectionKind::Node,
                                               selected_nodes_.front()};
      }
      frame.selection = selection_;
    }
    marquee_.reset();
  }
}

} // namespace mediaserver::editor
