// CompositorPanel::draw and the queries outside it; the frame's first
// phase, the title being typed and the frame's close.

#include "compositor_frame.hpp"

#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/shapes.hpp>

namespace mediaserver::editor {

using namespace panel_helpers;
using namespace compositor_ui;

std::string compositor_scene_layer_detail(
    doc::NodeId scene_node, std::span<const ElementInfo> elements) {
  const auto element = std::find_if(
      elements.begin(), elements.end(),
      [scene_node](const ElementInfo &candidate) {
        return candidate.node == scene_node;
      });
  if (element != elements.end()) {
    return element_display_name(*element);
  }
  return "OBJECT #" + std::to_string(scene_node.value());
}

std::string_view compositor_diagnostic_message(
    const compositor::GraphDiagnostic &diagnostic) noexcept {
  switch (diagnostic.code) {
  case compositor::DiagnosticCode::NodesOverCapacity:
    return "Too many nodes; remove a node";
  case compositor::DiagnosticCode::EdgesOverCapacity:
    return "Too many connections; disconnect a wire";
  case compositor::DiagnosticCode::DanglingNode:
    return "A wire references a missing node";
  case compositor::DiagnosticCode::DanglingSceneReference:
    return "A Layer references a missing scene object";
  case compositor::DiagnosticCode::UnknownPort:
    return "A wire references a missing port";
  case compositor::DiagnosticCode::IncompatiblePorts:
    return "A wire joins different kinds of port; filters go into FILTERS";
  case compositor::DiagnosticCode::DuplicateInput:
    return "An input already has a connection";
  case compositor::DiagnosticCode::MissingOutput:
    return "Add and connect one Output node";
  case compositor::DiagnosticCode::DuplicateOutput:
    return "Keep only one Output node";
  case compositor::DiagnosticCode::DuplicateSceneLayerReference:
    return "One active Scene Layer per source is supported";
  case compositor::DiagnosticCode::Cycle:
    return "Cycle detected; wires must flow toward Output";
  case compositor::DiagnosticCode::DeadBranch:
    return "This branch does not reach Output";
  case compositor::DiagnosticCode::PassBudgetExceeded:
    return "Graph exceeds the authored pass budget";
  case compositor::DiagnosticCode::TextureBudgetExceeded:
    return "Graph exceeds the offscreen texture budget";
  case compositor::DiagnosticCode::TapBudgetExceeded:
    return "Graph exceeds the blur tap budget";
  case compositor::DiagnosticCode::MergeNestingTooDeep:
    return "Merges are nested more than 4 deep; flatten one";
  case compositor::DiagnosticCode::UnsupportedMatteTopology:
    return "Matte needs a Scene Layer source and a Scene Layer or Background "
           "matte";
  case compositor::DiagnosticCode::UnsupportedMatteGroupSource:
    return "A Matte cannot use a group";
  case compositor::DiagnosticCode::MatteBranchAlsoOutputReachable:
    return "A Matte's matte layer cannot also be shown";
  }
  return "Graph is not ready";
}

std::string_view compositor_diagnostic_severity_label(
    const compositor::GraphDiagnostic &diagnostic) noexcept {
  return compositor::diagnostic_severity_name(diagnostic.severity);
}

void draw_compositor_drop_target(gui::UiContext &ctx, const gui::Rect &bounds,
                                 std::string_view item_name) {
  gui::Rect canvas = bounds;
  static_cast<void>(gui::cut_top(canvas, compositor_toolbar_height));
  const bool hovered = canvas.contains(ctx.pointer_x(), ctx.pointer_y());
  std::string label{hovered ? "RELEASE TO ADD NODE  ·  "
                            : "DROP IN NODE GRAPH  ·  "};
  label += item_name.empty() ? "ITEM" : item_name;
  draw_drop_target(ctx, canvas, label, hovered, 360.0F);
}

std::optional<gui::UiPoint>
CompositorPanel::pending_position(doc::NodeId node) const noexcept {
  const auto found = std::find_if(
      pending_moves_.begin(), pending_moves_.end(),
      [node](const PendingMove &move) noexcept { return move.node == node; });
  return found == pending_moves_.end()
             ? std::nullopt
             : std::optional<gui::UiPoint>{gui::UiPoint{found->x, found->y}};
}

void CompositorPanel::colour_well_field(
    gui::UiContext &ctx, const gui::Rect &well, doc::NodeId node,
    core::u64 slot, gui::UiColor current,
    const std::array<CompositorProperty, 3U> &channels,
    CompositorFrame &frame) {
  const gui::UiId id = gui::ui_id(id_, "compositor.colour", node.value() * 8U + slot);
  gui::UiColor picked = current;
  if (ctx.color_well(id, well, picked)) {
    colour_target_ = ColourTarget{id, node, channels, picked};
  }
  if (ctx.color_open(id)) {
    colour_editing_ = true;
  }
  if (!colour_target_.has_value() || colour_target_->well != id ||
      colour_target_->node != node) {
    return;
  }
  // A frame carries one property, so the picked colour lands a channel a
  // frame, inside the one gesture the open picker holds.
  const std::array<float, 3U> now{current.r, current.g, current.b};
  const std::array<float, 3U> want{colour_target_->color.r,
                                   colour_target_->color.g,
                                   colour_target_->color.b};
  for (std::size_t index = 0U; index < 3U; ++index) {
    if (std::abs(now[index] - want[index]) <= 1.0e-6F) {
      continue;
    }
    if (!frame.property_edit.has_value()) {
      frame.property_edit = CompositorPropertyEditIntent{
          node, colour_target_->channels[index],
          CompositorPropertyValue{want[index]}};
    }
    return;
  }
  colour_target_.reset();
}

void CompositorPanel::select_only(doc::NodeId node) {
  selection_ = CompositorSelection{CompositorSelectionKind::Node, node};
  selected_nodes_.assign(1U, node);
}

std::optional<CompositorDeleteIntent> CompositorPanel::selection_delete(
    const compositor::CompositorGraph &graph) const {
  CompositorDeleteIntent remove;
  if (selection_.kind == CompositorSelectionKind::Edge ||
      selection_.kind == CompositorSelectionKind::Comment) {
    remove.targets.push_back(selection_);
  }
  for (const doc::NodeId node : selected_nodes_) {
    const compositor::GraphNode *const found = graph.node(node);
    // Output is structural: the graph keeps its one.
    if (found != nullptr && found->kind != compositor::NodeKind::Output) {
      remove.targets.push_back(
          CompositorSelection{CompositorSelectionKind::Node, node});
    }
  }
  return remove.targets.empty() ? std::nullopt
                                : std::optional<CompositorDeleteIntent>{
                                      std::move(remove)};
}

bool CompositorPanel::collapsed(
    const compositor::GraphNode &node) const noexcept {
  const auto held = std::find_if(
      pending_collapse_.begin(), pending_collapse_.end(),
      [&node](const auto &entry) noexcept { return entry.first == node.node; });
  return held != pending_collapse_.end() ? held->second : node.collapsed;
}

bool CompositorPanel::bypassed(
    const compositor::GraphNode &node) const noexcept {
  const auto held = std::find_if(
      pending_bypass_.begin(), pending_bypass_.end(),
      [&node](const auto &entry) noexcept { return entry.first == node.node; });
  return held != pending_bypass_.end() ? held->second : node.bypassed;
}

core::i64 CompositorPanel::edge_order(
    const compositor::GraphEdge &edge) const noexcept {
  const auto held = std::find_if(
      pending_order_.begin(), pending_order_.end(),
      [&edge](const auto &entry) noexcept { return entry.first == edge.node; });
  return held != pending_order_.end() ? held->second : edge.order;
}

std::vector<const compositor::GraphEdge *>
CompositorPanel::input_chain(const compositor::CompositorGraph &graph,
                             compositor::EdgeEndpoint input) const {
  std::vector<const compositor::GraphEdge *> chain;
  for (const compositor::GraphEdge &edge : graph.edges) {
    if (edge.to == input) {
      chain.push_back(std::addressof(edge));
    }
  }
  std::sort(chain.begin(), chain.end(),
            [this](const compositor::GraphEdge *left,
                   const compositor::GraphEdge *right) {
              const core::i64 a = edge_order(*left);
              const core::i64 b = edge_order(*right);
              return a != b ? a < b : left->node < right->node;
            });
  return chain;
}

std::optional<float>
CompositorPanel::port_row(const compositor::CompositorGraph &graph,
                          const compositor::GraphNode &node, core::u8 port,
                          bool input) const {
  const auto ports = input ? compositor::input_ports(node.kind)
                           : compositor::output_ports(node.kind);
  const bool packed = collapsed(node);
  std::size_t row{};
  for (const compositor::PortDescriptor &descriptor : ports) {
    const compositor::EdgeEndpoint endpoint{node.node, descriptor.index};
    // A collapsed node keeps only its connected ports, packed together.
    const bool shown =
        !packed ||
        std::any_of(graph.edges.begin(), graph.edges.end(),
                    [&](const compositor::GraphEdge &edge) noexcept {
                      return (input ? edge.to : edge.from) == endpoint;
                    });
    if (descriptor.index == port) {
      return shown ? std::optional<float>{static_cast<float>(row)}
                   : std::nullopt;
    }
    if (shown) {
      ++row;
    }
  }
  return std::nullopt;
}

std::optional<CompositorWireDrop>
CompositorPanel::effect_wire_at(const compositor::CompositorGraph &graph,
                                float x, float y,
                                compositor::PortValueType type) const {
  if (!last_canvas_.contains(x, y)) {
    return std::nullopt;
  }
  const DrawnWire *best = nullptr;
  float nearest = 8.0F;
  gui::UiPoint at{};
  for (const DrawnWire &drawn : drawn_wires_) {
    if (drawn.type != type) {
      continue;
    }
    for (std::size_t index = 0U; index + 1U < drawn.points.size(); ++index) {
      const gui::UiPoint a = drawn.points[index];
      const gui::UiPoint b = drawn.points[index + 1U];
      const float sx = b.x - a.x;
      const float sy = b.y - a.y;
      const float length = sx * sx + sy * sy;
      const float t =
          length > 0.0F
              ? std::clamp(((x - a.x) * sx + (y - a.y) * sy) / length, 0.0F,
                           1.0F)
              : 0.0F;
      const gui::UiPoint closest{a.x + sx * t, a.y + sy * t};
      const float distance = std::hypot(x - closest.x, y - closest.y);
      if (distance <= nearest) {
        nearest = distance;
        best = std::addressof(drawn);
        at = closest;
      }
    }
  }
  const compositor::GraphEdge *const edge =
      best != nullptr ? graph.edge(best->edge) : nullptr;
  if (edge == nullptr) {
    return std::nullopt;
  }
  // The input's wires in the order it applies them; the new one goes right
  // after this one, and the rest are spaced out to make room.
  const std::vector<const compositor::GraphEdge *> chain =
      input_chain(graph, edge->to);
  CompositorWireDrop drop{edge->node, edge->to, at.x, at.y};
  drop.type = type;
  for (std::size_t index = 0U; index < chain.size(); ++index) {
    const core::i64 spaced = static_cast<core::i64>(index) * 2;
    if (edge_order(*chain[index]) != spaced) {
      drop.reorder.emplace_back(chain[index]->node, spaced);
    }
    if (chain[index]->node == edge->node) {
      drop.order = spaced + 1;
    }
  }
  if (const compositor::GraphNode *const source = graph.node(edge->from.node)) {
    drop.after = compositor::node_kind_name(source->kind);
  }
  return drop;
}

void CompositorPanel::draw_wire_drop(gui::UiContext &ctx,
                                     const CompositorWireDrop &drop,
                                     std::string_view what) const {
  ctx.push_clip(last_canvas_);
  for (const DrawnWire &drawn : drawn_wires_) {
    if (drawn.edge == drop.edge) {
      ctx.draw_list().add_polyline(drawn.points,
                                   std::clamp(4.0F * view_.zoom, 3.0F, 6.0F),
                                   ctx.theme().text);
    }
  }
  draw_port_shape(ctx, drop.type, drop.x, drop.y,
                  std::max(4.0F, 6.0F * view_.zoom), ctx.theme().text);
  std::string label{what};
  label += "  -  INSERT AFTER ";
  label += drop.after;
  const float width = std::ceil(ctx.font_width(label)) + 16.0F;
  const float height = ctx.line_height() + 8.0F;
  const gui::Rect badge{drop.x + 12.0F, drop.y + 10.0F, width, height};
  ctx.panel(badge, with_alpha(ctx.theme().background, 0.92F));
  ctx.outline(badge, port_color(ctx, drop.type));
  ctx.label(gui::Rect{badge.x + 8.0F, badge.y, badge.width - 8.0F,
                      badge.height},
            label, ctx.theme().text);
  ctx.pop_clip();
}

gui::Rect CompositorPanel::node_screen_bounds(
    const compositor::GraphNode &node) const noexcept {
  const std::optional<gui::UiPoint> pending = pending_position(node.node);
  const gui::UiPoint size = node_size(node);
  return view_.to_screen(
      last_canvas_, gui::Rect{pending ? pending->x : node.editor_x,
                              pending ? pending->y : node.editor_y, size.x,
                              size.y});
}

std::optional<std::pair<doc::NodeId, gui::Rect>>
CompositorPanel::node_at(const compositor::CompositorGraph &graph,
                         float x, float y) const noexcept {
  if (!last_canvas_.contains(x, y)) return std::nullopt;
  for (auto it = graph.nodes.rbegin(); it != graph.nodes.rend(); ++it) {
    const gui::Rect bounds = node_screen_bounds(*it);
    if (bounds.contains(x, y)) return std::pair{it->node, gui::intersection(bounds, last_canvas_)};
  }
  return std::nullopt;
}

gui::UiPoint
CompositorPanel::node_size(const compositor::GraphNode &node) const noexcept {
  const auto found = std::lower_bound(
      node_sizes_.begin(), node_sizes_.end(), node.node,
      [](const NodeSize &size, doc::NodeId id) noexcept {
        return size.node < id;
      });
  if (found != node_sizes_.end() && found->node == node.node) {
    return gui::UiPoint{found->width, found->height};
  }
  return new_node_size(node.kind);
}

gui::UiPoint
CompositorPanel::new_node_size(compositor::NodeKind kind) const noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index < new_node_sizes_.size() && new_node_sizes_[index].x > 0.0F) {
    return new_node_sizes_[index];
  }
  // Not drawn yet: the minimum width at the nominal 14-unit font.
  const CompositorNodeMetrics nominal{35.0F, 22.0F, 7.0F};
  return gui::UiPoint{compositor_node_min_width,
                      compositor_node_height(metrics_.row_height > 0.0F
                                                 ? metrics_
                                                 : nominal,
                                             kind)};
}

std::optional<compositor::EdgeEndpoint>
CompositorPanel::input_port_at(const compositor::CompositorGraph &graph,
                               float x, float y) const noexcept {
  if (!last_canvas_.contains(x, y)) return std::nullopt;
  // Topmost node first, as node_at() does, so the answer is the port the
  // operator can see.
  for (auto it = graph.nodes.rbegin(); it != graph.nodes.rend(); ++it) {
    const gui::Rect bounds = node_screen_bounds(*it);
    const float reach = compositor_port_radius * view_.zoom + 4.0F;
    const gui::Rect with_dots{bounds.x - reach, bounds.y,
                              bounds.width + reach, bounds.height};
    if (!with_dots.contains(x, y)) {
      continue;
    }
    const auto ports = compositor::input_ports(it->kind);
    for (std::size_t index = 0U; index < ports.size(); ++index) {
      const std::optional<float> place =
          port_row(graph, *it, ports[index].index, true);
      if (!place.has_value()) {
        continue;
      }
      const float row_top =
          bounds.y +
          (metrics_.header_height + metrics_.row_height * *place) * view_.zoom;
      const gui::Rect row{bounds.x - reach, row_top,
                          bounds.width * 0.5F + reach,
                          metrics_.row_height * view_.zoom};
      if (row.contains(x, y)) {
        return compositor::EdgeEndpoint{it->node, ports[index].index};
      }
    }
    if (bounds.contains(x, y)) {
      return std::nullopt; // over this node, but not over one of its inputs
    }
  }
  return std::nullopt;
}

std::optional<CompositorPortDrop> CompositorPanel::port_drop_target(
    const compositor::CompositorGraph &graph, float x, float y,
    compositor::PortValueType type) const noexcept {
  if (!last_canvas_.contains(x, y)) return std::nullopt;
  std::optional<CompositorPortDrop> best;
  float best_rank = std::numeric_limits<float>::infinity();
  // Later nodes draw on top, so on a tie the topmost wins.
  for (auto it = graph.nodes.rbegin(); it != graph.nodes.rend(); ++it) {
    const gui::Rect bounds = node_screen_bounds(*it);
    const float reach = compositor_port_radius * view_.zoom + 4.0F;
    const auto ports = compositor::input_ports(it->kind);
    for (std::size_t index = 0U; index < ports.size(); ++index) {
      const compositor::PortDescriptor &port = ports[index];
      const std::optional<float> place =
          port_row(graph, *it, port.index, true);
      if (port.type != type || !place.has_value()) {
        continue;
      }
      const float row_top =
          bounds.y +
          (metrics_.header_height + metrics_.row_height * *place) * view_.zoom;
      const float row_height = metrics_.row_height * view_.zoom;
      const gui::Rect row{bounds.x - reach, row_top,
                          bounds.width * 0.5F + reach, row_height};
      const float dot_y = row_top + row_height * 0.5F;
      const float distance = std::hypot(x - bounds.x, y - dot_y);
      const bool over_row = row.contains(x, y);
      if (!over_row && distance > compositor_port_snap_radius) {
        continue;
      }
      const float rank = over_row ? distance - 1.0e6F : distance;
      if (rank >= best_rank) {
        continue;
      }
      best_rank = rank;
      const compositor::EdgeEndpoint endpoint{it->node, port.index};
      std::optional<doc::NodeId> occupied;
      if (!port.multiple) {
        const auto wired = std::find_if(
            graph.edges.begin(), graph.edges.end(),
            [endpoint](const compositor::GraphEdge &edge) noexcept {
              return edge.to == endpoint;
            });
        if (wired != graph.edges.end()) {
          occupied = wired->node;
        }
      }
      best = CompositorPortDrop{endpoint, port.name, port.type, bounds.x,
                                dot_y, occupied, port.multiple};
    }
  }
  return best;
}

void CompositorPanel::draw_port_drop(gui::UiContext &ctx,
                                     const CompositorPortDrop &drop,
                                     std::string_view what) const {
  ctx.push_clip(last_canvas_);
  const gui::UiColor color = port_color(ctx, drop.type);
  wire(ctx, ctx.pointer_x(), ctx.pointer_y(), drop.x, drop.y,
       with_alpha(color, 0.9F), std::clamp(2.5F * view_.zoom, 2.0F, 4.0F), view_.zoom);
  const float radius = std::max(2.5F, compositor_port_radius * view_.zoom) + 3.0F;
  draw_port_shape(ctx, drop.type, drop.x, drop.y, radius + 1.5F,
                  ctx.theme().text);
  draw_port_shape(ctx, drop.type, drop.x, drop.y, radius, color);
  // The label sits left of the port, where the new node will arrive.
  std::string label{what};
  label += drop.occupied.has_value() ? "  -  REPLACE " : "  -  CONNECT TO ";
  label += drop.name;
  const float width = std::ceil(ctx.font_width(label)) + 16.0F;
  const float height = ctx.line_height() + 8.0F;
  const gui::Rect badge{drop.x - radius - 10.0F - width,
                        drop.y - height * 0.5F, width, height};
  ctx.panel(badge, with_alpha(ctx.theme().background, 0.92F));
  ctx.outline(badge, color);
  ctx.label(gui::Rect{badge.x + 8.0F, badge.y, badge.width - 8.0F,
                      badge.height},
            label, ctx.theme().text);
  ctx.pop_clip();
}

gui::UiPoint CompositorPanel::node_position_feeding(
    compositor::NodeKind kind, const CompositorPortDrop &drop) const noexcept {
  const gui::UiPoint size = new_node_size(kind);
  // A clear gap between the new node's output and the input it feeds.
  constexpr float gap = 70.0F;
  const gui::UiPoint at = view_.to_graph(last_canvas_, drop.x, drop.y);
  return gui::UiPoint{at.x - gap - size.x,
                      at.y - metrics_.header_height -
                          metrics_.row_height * 0.5F};
}

CompositorFrame
CompositorPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                      const compositor::CompositorGraph &compositor_graph,
                      std::span<const ElementInfo> elements) {
  Frame f{*this, ctx, bounds, compositor_graph, elements};
  if (!begin_frame(f)) {
    return std::move(f.frame);
  }
  draw_toolbar(f);
  build_menus(f);
  if (!begin_canvas(f)) {
    return std::move(f.frame);
  }
  interact_ports(f);
  interact_nodes(f);
  interact_wires(f);
  handle_keyboard(f);
  draw_backdrop(f);
  draw_wires(f);
  draw_nodes(f);
  draw_overlays(f);
  draw_parameters(f);
  draw_add_palette(f);
  draw_node_search(f);
  draw_context_menu(f);
  draw_rename(f);
  finish_frame(f);
  return std::move(f.frame);
}

const compositor::GraphComment *
CompositorPanel::Frame::comment_of(doc::NodeId id) {
  const auto found = std::find_if(
      compositor_graph.comments.begin(), compositor_graph.comments.end(),
      [id](const compositor::GraphComment &comment) noexcept {
        return comment.node == id;
      });
  return found == compositor_graph.comments.end() ? nullptr
                                                  : std::addressof(*found);
}

void CompositorPanel::Frame::set_zoom(float requested, float anchor_x,
                                      float anchor_y) noexcept {
  const float next =
      std::clamp(requested, compositor_minimum_zoom, compositor_maximum_zoom);
  if (next == panel.view_.zoom || canvas.width <= 0.0F ||
      canvas.height <= 0.0F) {
    return;
  }
  panel.view_.zoom_about(canvas, anchor_x, anchor_y, next);
}

bool CompositorPanel::begin_frame(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const gui::Rect &bounds = f.bounds;
  const compositor::CompositorGraph &compositor_graph = f.compositor_graph;
  const std::span<const ElementInfo> elements = f.elements;
  auto &toolbar = f.toolbar;
  auto &content = f.content;
  auto &parameter_node = f.parameter_node;
  auto &canvas = f.canvas;
  auto &parameter_bounds = f.parameter_bounds;
  auto &parameter_resize_pressed = f.parameter_resize_pressed;
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return false;
  }
  if (ctx.pointer_pressed()) {
    focused_ = bounds.contains(ctx.pointer_x(), ctx.pointer_y());
  }
  std::erase_if(selected_nodes_, [&](doc::NodeId node) {
    return compositor_graph.node(node) == nullptr;
  });
  if ((selection_.kind == CompositorSelectionKind::Node &&
       compositor_graph.node(selection_.node) == nullptr) ||
      (selection_.kind == CompositorSelectionKind::Edge &&
       compositor_graph.edge(selection_.node) == nullptr) ||
      (selection_.kind == CompositorSelectionKind::Comment &&
       f.comment_of(selection_.node) == nullptr)) {
    // The primary went away: another selected node takes its place.
    selection_ = selected_nodes_.empty()
                     ? CompositorSelection{}
                     : CompositorSelection{CompositorSelectionKind::Node,
                                           selected_nodes_.front()};
  }
  // A paste or duplicate selects what it added once that arrives.
  if (paste_baseline_.has_value()) {
    std::vector<doc::NodeId> added;
    for (const compositor::GraphNode &node : compositor_graph.nodes) {
      if (!std::binary_search(paste_baseline_->nodes.begin(),
                              paste_baseline_->nodes.end(), node.node)) {
        added.push_back(node.node);
      }
    }
    if (!added.empty()) {
      selected_nodes_ = std::move(added);
      selection_ = CompositorSelection{CompositorSelectionKind::Node,
                                       selected_nodes_.front()};
      paste_baseline_.reset();
    } else if (++paste_baseline_->frames > compositor_pending_move_frames) {
      paste_baseline_.reset();
    }
  }

  toolbar = gui::Rect{bounds.x, bounds.y, bounds.width,
                          std::min(bounds.height, compositor_toolbar_height)};
  content = gui::Rect{bounds.x, bounds.y + toolbar.height, bounds.width,
                          std::max(bounds.height - toolbar.height, 0.0F)};

  parameter_node =
      selection_.kind == CompositorSelectionKind::Node
          ? compositor_graph.node(selection_.node)
          : nullptr;
  if (parameter_node != nullptr &&
      !has_editable_parameters(parameter_node->kind)) {
    parameter_node = nullptr;
  }
  if (parameter_node == nullptr) {
    if (parameter_node_.value() != 0U) {
      parameter_fields_.cancel(ctx);
      parameter_node_ = {};
    }
  } else if (parameter_node_ != parameter_node->node) {
    parameter_fields_.cancel(ctx);
    parameter_node_ = parameter_node->node;
    parameter_scroll_ = 0.0F;
    parameter_content_height_ = 0.0F;
  }

  // Collapse and bypass toggles end once the graph shows them.
  std::erase_if(pending_collapse_, [&](const auto &held) {
    const compositor::GraphNode *const node = compositor_graph.node(held.first);
    return node == nullptr || node->collapsed == held.second;
  });
  std::erase_if(pending_bypass_, [&](const auto &held) {
    const compositor::GraphNode *const node = compositor_graph.node(held.first);
    return node == nullptr || node->bypassed == held.second;
  });
  std::erase_if(pending_order_, [&](const auto &held) {
    const compositor::GraphEdge *const edge = compositor_graph.edge(held.first);
    return edge == nullptr || edge->order == held.second;
  });
  // Every node's size this frame, from the font it is drawn in.
  metrics_ = compositor_node_metrics(ctx);
  for (const CompositorPaletteItem &item : compositor_palette_items) {
    new_node_sizes_[static_cast<std::size_t>(item.kind)] = gui::UiPoint{
        compositor_node_width(ctx, item.kind,
                              compositor::node_kind_name(item.kind), {}),
        compositor_node_height(metrics_, item.kind)};
  }
  node_sizes_.clear();
  node_sizes_.reserve(compositor_graph.nodes.size());
  for (const compositor::GraphNode &node : compositor_graph.nodes) {
    const std::string_view title =
        node.label.empty() ? compositor::node_kind_name(node.kind)
                           : std::string_view{node.label};
    const std::string source =
        node.kind == compositor::NodeKind::SceneLayer
            ? compositor_node_detail(node, elements)
            : std::string{};
    float height = compositor_node_height(metrics_, node.kind);
    if (collapsed(node)) {
      std::size_t inputs_shown{};
      std::size_t outputs_shown{};
      for (const auto &port : compositor::input_ports(node.kind)) {
        inputs_shown += port_row(compositor_graph, node, port.index, true)
                            .has_value()
                            ? 1U
                            : 0U;
      }
      for (const auto &port : compositor::output_ports(node.kind)) {
        outputs_shown += port_row(compositor_graph, node, port.index, false)
                             .has_value()
                             ? 1U
                             : 0U;
      }
      height = metrics_.header_height +
               static_cast<float>(std::max(inputs_shown, outputs_shown)) *
                   metrics_.row_height +
               metrics_.footer_height;
    }
    node_sizes_.push_back(NodeSize{
        node.node, compositor_node_width(ctx, node.kind, title, source),
        height});
  }
  std::sort(node_sizes_.begin(), node_sizes_.end(),
            [](const NodeSize &left, const NodeSize &right) noexcept {
              return left.node < right.node;
            });

  // A released move keeps its position until the graph reports the edit.
  std::erase_if(pending_moves_, [&](PendingMove &held) {
    const compositor::GraphNode *const moved = compositor_graph.node(held.node);
    return moved == nullptr || moved->editor_x != held.from_x ||
           moved->editor_y != held.from_y ||
           ++held.frames > compositor_pending_move_frames;
  });
  if (pending_comment_.has_value()) {
    const compositor::GraphComment *const box =
        f.comment_of(pending_comment_->held.node);
    if (box == nullptr || *box != pending_comment_->from ||
        ++pending_comment_->frames > compositor_pending_move_frames) {
      pending_comment_.reset();
    }
  }

  if (pending_points_.has_value()) {
    const compositor::GraphEdge *const routed =
        compositor_graph.edge(pending_points_->edge);
    if (routed == nullptr || routed->points != pending_points_->from ||
        ++pending_points_->frames > compositor_pending_move_frames) {
      pending_points_.reset();
    }
  }
  if (point_drag_.has_value() &&
      compositor_graph.edge(point_drag_->edge) == nullptr) {
    point_drag_.reset();
  }
  if (pending_wires_.has_value()) {
    const bool arrived =
        std::all_of(pending_wires_->added.begin(), pending_wires_->added.end(),
                    [&](const CompositorWire &wire) {
                      return std::any_of(
                          compositor_graph.edges.begin(),
                          compositor_graph.edges.end(),
                          [&](const compositor::GraphEdge &edge) noexcept {
                            return edge.from == wire.from && edge.to == wire.to;
                          });
                    }) &&
        std::none_of(pending_wires_->removed.begin(),
                     pending_wires_->removed.end(), [&](doc::NodeId id) {
                       return compositor_graph.edge(id) != nullptr;
                     });
    if (arrived || ++pending_wires_->frames > compositor_pending_move_frames) {
      pending_wires_.reset();
    }
  }

  canvas = content;
  // The seam the operator drags to resize Parameters: its left edge beside
  // the graph, or its top edge when the panel is too narrow for both.
  gui::Rect parameter_splitter{};
  bool parameter_side = false;
  parameter_resize_pressed = false;
  constexpr float minimum_graph_width = 256.0F;
  constexpr float minimum_graph_height = 72.0F;
  constexpr float minimum_parameter_width = 200.0F;
  constexpr float default_parameter_width = 280.0F;
  constexpr float maximum_parameter_width = 640.0F;
  constexpr float minimum_parameter_height = 96.0F;
  constexpr float splitter_reach = 3.0F;
  if (parameter_node != nullptr) {
    parameter_side =
        content.width >= minimum_graph_width + minimum_parameter_width;
    if (parameter_side) {
      const float requested =
          parameter_width_ > 0.0F ? parameter_width_ : default_parameter_width;
      const float width = std::clamp(
          requested, minimum_parameter_width,
          std::clamp(content.width - minimum_graph_width,
                     minimum_parameter_width, maximum_parameter_width));
      canvas.width = std::max(content.width - width, 0.0F);
      parameter_bounds =
          gui::Rect{canvas.right(), content.y, width, content.height};
      parameter_splitter =
          gui::Rect{parameter_bounds.x - splitter_reach, parameter_bounds.y,
                    splitter_reach * 2.0F, parameter_bounds.height};
    } else {
      const float available =
          std::max(content.height - minimum_graph_height, 0.0F);
      const float requested = parameter_height_ > 0.0F
                                  ? parameter_height_
                                  : parameter_panel_height(*parameter_node);
      const float height = std::min(
          std::max(requested, std::min(minimum_parameter_height, available)),
          available);
      canvas.height = std::max(content.height - height, 0.0F);
      parameter_bounds =
          gui::Rect{content.x, canvas.bottom(), content.width, height};
      parameter_splitter =
          gui::Rect{parameter_bounds.x, parameter_bounds.y - splitter_reach,
                    parameter_bounds.width, splitter_reach * 2.0F};
    }
    // Claimed before the graph so a press on the seam resizes rather than
    // starting a node drag under it.
    const gui::Interaction resize = clipped_grab(
        ctx, gui::ui_id(id_, "compositor.parameters.resize"), parameter_splitter,
        content);
    parameter_resize_pressed = resize.pressed;
    ctx.cursor_for(resize, parameter_side ? gui::Cursor::ResizeHorizontal
                                          : gui::Cursor::ResizeVertical);
    if (resize.held) {
      if (parameter_side) {
        parameter_width_ = std::clamp(
            content.right() - ctx.pointer_x(),
            minimum_parameter_width,
            std::clamp(content.width - minimum_graph_width,
                       minimum_parameter_width, maximum_parameter_width));
      } else {
        parameter_height_ = std::clamp(
            content.bottom() - ctx.pointer_y(),
            std::min(minimum_parameter_height,
                     std::max(content.height - minimum_graph_height, 0.0F)),
            std::max(content.height - minimum_graph_height, 0.0F));
      }
    }
  }
  last_canvas_ = canvas;

  return true;
}

void CompositorPanel::draw_rename(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  CompositorFrame &frame = f.frame;
  auto &canvas = f.canvas;
  auto &nodes = f.nodes;
  // A title being typed over its node or comment box: Enter or a click
  // elsewhere keeps it, Escape abandons it.
  if (rename_.has_value()) {
    std::optional<gui::Rect> field;
    if (const auto visual = std::find_if(
            nodes.begin(), nodes.end(),
            [this](const CompositorNodeVisual &entry) {
              return entry.node->node == rename_->node;
            });
        visual != nodes.end()) {
      field = gui::Rect{visual->bounds.x + 4.0F, visual->bounds.y + 3.0F,
                        std::max(visual->bounds.width - 8.0F, 40.0F),
                        std::max(ctx.line_height() + 6.0F, 22.0F)};
    } else if (const compositor::GraphComment *const comment =
                   f.comment_of(rename_->node)) {
      const compositor::GraphComment drawn = f.drawn_comment(*comment);
      const gui::Rect box = view_.to_screen(
          canvas, gui::Rect{drawn.x, drawn.y, drawn.width, drawn.height});
      field = gui::Rect{box.x + 4.0F, box.y + 2.0F,
                        std::max(box.width - 8.0F, 60.0F),
                        std::max(ctx.line_height() + 6.0F, 22.0F)};
    }
    const gui::UiId rename_id = gui::ui_id(id_, "compositor.rename");
    if (!field.has_value() || ctx.key_pressed(gui::Key::Escape)) {
      rename_.reset();
      ctx.clear_focus();
    } else {
      const bool commit =
          ctx.key_pressed(gui::Key::Enter) ||
          (!rename_->focus_pending && ctx.focus() != rename_id);
      if (rename_->focus_pending) {
        ctx.focus_text_field_selected(rename_id, rename_->text);
        rename_->focus_pending = false;
      }
      ctx.push_clip(canvas);
      ctx.panel(*field, ctx.theme().background);
      static_cast<void>(ctx.text_field(rename_id, *field, rename_->text));
      ctx.outline(*field, ctx.theme().accent);
      ctx.pop_clip();
      if (commit) {
        frame.rename = CompositorRenameIntent{rename_->node, rename_->text};
        rename_.reset();
        ctx.clear_focus();
      }
    }
  }
}

void CompositorPanel::finish_frame(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  CompositorFrame &frame = f.frame;
  auto &drag_released = f.drag_released;
  // Hold what this frame authored on screen until the graph reports it.
  if (frame.connect_ports.has_value() || frame.rewire.has_value()) {
    PendingWires held;
    if (frame.connect_ports.has_value()) {
      held.added.push_back(
          CompositorWire{frame.connect_ports->from, frame.connect_ports->to});
      if (frame.connect_ports->replace_edge.has_value()) {
        held.removed.push_back(*frame.connect_ports->replace_edge);
      }
    }
    if (frame.rewire.has_value()) {
      held.added.insert(held.added.end(), frame.rewire->add_edges.begin(),
                        frame.rewire->add_edges.end());
      held.removed.insert(held.removed.end(),
                          frame.rewire->remove_edges.begin(),
                          frame.rewire->remove_edges.end());
    }
    pending_wires_ = std::move(held);
  }

  frame.editing =
      parameter_fields_.live() || inline_fields_.live() || colour_editing_;
  colour_editing_ = false;
  ctx.pop_clip();
  if (drag_released) {
    dragging_node_ = {};
  }
}

} // namespace mediaserver::editor
