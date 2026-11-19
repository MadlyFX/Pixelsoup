// Drawing the canvas: grid, guidance, comment boxes, wires, and what
// floats over the nodes.

#include "compositor_frame.hpp"

#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/shapes.hpp>

namespace mediaserver::editor {

using namespace panel_helpers;
using namespace compositor_ui;

void CompositorPanel::draw_backdrop(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const compositor::CompositorGraph &compositor_graph = f.compositor_graph;
  auto &canvas = f.canvas;
  auto &comment_header = f.comment_header;
  ctx.push_clip(canvas);
  ctx.panel(canvas, scale(ctx.theme().background, 0.84F));
  // A quiet drafting-grid texture identifies this as a spatial graph surface
  // without competing with wires or implying rigid row/column alignment.
  const float dot_spacing = 32.0F * view_.zoom;
  constexpr float dot_size = 1.5F;
  const gui::UiColor dot_color = with_alpha(ctx.theme().divider, 0.42F);
  float first_dot_x =
      canvas.x + std::fmod(view_.pan_x + dot_spacing * 0.5F, dot_spacing);
  float first_dot_y =
      canvas.y + std::fmod(view_.pan_y + dot_spacing * 0.5F, dot_spacing);
  if (first_dot_x < canvas.x) {
    first_dot_x += dot_spacing;
  }
  if (first_dot_y < canvas.y) {
    first_dot_y += dot_spacing;
  }
  for (float y = first_dot_y; y < canvas.bottom(); y += dot_spacing) {
    for (float x = first_dot_x; x < canvas.right(); x += dot_spacing) {
      ctx.draw_list().add_rect(x - dot_size * 0.5F, y - dot_size * 0.5F,
                               dot_size, dot_size, dot_color);
    }
  }

  // Guidance sits behind graph content: it remains available in empty/error
  // states, while nodes and wires always paint over it and therefore never
  // become obscured by instructional chrome.
  const auto first_problem =
      std::find_if(compositor_graph.diagnostics.begin(),
                   compositor_graph.diagnostics.end(), is_problem);
  const bool graph_problem = first_problem != compositor_graph.diagnostics.end();
  // The Output is waiting for its image, so the graph composites nothing and
  // Program shows the scene as authored.
  const auto output_node = std::find_if(
      compositor_graph.nodes.begin(), compositor_graph.nodes.end(),
      [](const compositor::GraphNode &node) noexcept {
        return node.kind == compositor::NodeKind::Output;
      });
  const bool composites_nothing =
      output_node != compositor_graph.nodes.end() &&
      node_disabled(compositor_graph, output_node->node);
  if (compositor_graph.nodes.empty() || graph_problem || composites_nothing) {
    const bool diagnostic = graph_problem;
    gui::Rect top = gui::inset(canvas, 8.0F);
    gui::Rect guide = gui::cut_top(top, 28.0F);
    guide.width = std::min(guide.width, 660.0F);
    ctx.panel(guide, with_alpha(scale(ctx.theme().header, 0.92F), 0.94F));
    ctx.outline(guide,
                diagnostic ? with_alpha(ctx.theme().warning, 0.72F)
                      : with_alpha(ctx.theme().accent, 0.42F));
    std::string title;
    if (diagnostic) {
      title = first_problem->severity == compositor::DiagnosticSeverity::Error
                  ? "GRAPH ERROR: "
                  : "GRAPH WARNING: ";
    } else {
    }
    title += diagnostic ? compositor_diagnostic_message(*first_problem)
             : compositor_graph.nodes.empty()
                 ? std::string_view{"Empty graph"}
                 : std::string_view{"Output not wired"};
    const std::string_view next_step =
        diagnostic ? std::string_view{}
        : compositor_graph.nodes.empty()
            ? std::string_view{"Add a Layer for each scene object. "}
            : std::string_view{"Until Layers are wired into Output, Program "
                               "shows the scene as authored. "};
    ctx.label(gui::inset(guide, 8.0F, 3.0F), title,
              diagnostic ? ctx.theme().warning : ctx.theme().text_muted);
    ctx.tooltip(guide, std::string{next_step} +
                           "Connect the output on the right of a node to an "
                           "input on the left. Layers feed Composite, then "
                           "Output.");
  }

  for (const compositor::GraphComment &comment : compositor_graph.comments) {
    const compositor::GraphComment drawn = f.drawn_comment(comment);
    const gui::Rect box = view_.to_screen(
        canvas, gui::Rect{drawn.x, drawn.y, drawn.width, drawn.height});
    const bool chosen = selection_ == CompositorSelection{
                                          CompositorSelectionKind::Comment,
                                          comment.node};
    const gui::UiColor color = comment_color(drawn.color);
    ctx.panel(box, with_alpha(color, 0.07F));
    gui::Rect below_title = box;
    const gui::Rect title = gui::cut_top(below_title, comment_header);
    ctx.panel(title, with_alpha(blend_color(ctx.theme().header, color, 0.35F),
                                0.92F));
    ctx.outline(box, chosen ? ctx.theme().accent : with_alpha(color, 0.55F),
                chosen ? 2.0F : 1.0F);
    static_cast<void>(compositor_label(
        ctx,
        gui::Rect{title.x + 8.0F * view_.zoom, title.y,
                  std::max(title.width - 16.0F * view_.zoom, 0.0F), title.height},
        drawn.title, ctx.theme().text, compositor_title_text, view_.zoom));
    // The corner grip it resizes by.
    const float grip_x = box.right() - 4.0F;
    const float grip_y = box.bottom() - 4.0F;
    for (float offset = 4.0F; offset <= 10.0F; offset += 3.0F) {
      ctx.draw_list().add_line(grip_x - offset, grip_y, grip_x, grip_y - offset,
                               1.0F, with_alpha(color, 0.8F));
    }
  }
}

// A small numbered disc on a wire, or a problem mark.
gui::Rect CompositorPanel::Frame::wire_disc(gui::UiPoint at,
                                            std::string_view text,
                                            gui::UiColor ring,
                                            gui::UiColor fill) {
  const float radius = std::max(6.0F, 7.0F * panel.view_.zoom);
  gui::add_disc(ctx.draw_list(), gui::UiPoint{at.x, at.y},
                radius + 1.0F, ring);
  gui::add_disc(ctx.draw_list(), gui::UiPoint{at.x, at.y},
                radius, fill);
  const gui::Rect disc = gui::around(at.x, at.y, radius * 2.0F, radius * 2.0F);
  ctx.label(gui::centered(disc, ctx.font_width(text, 0.75F) + 2.0F,
                          disc.height),
            text, ctx.theme().text, 0.75F);
  return disc;
}

void CompositorPanel::draw_wires(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const compositor::CompositorGraph &compositor_graph = f.compositor_graph;
  auto &inputs = f.inputs;
  auto &outputs = f.outputs;
  auto &lifted = f.lifted;
  auto &chrome_readable = f.chrome_readable;
  auto &edges = f.edges;
  auto &insert_edge = f.insert_edge;
  auto &point_radius = f.point_radius;
  auto &hovered_edge = f.hovered_edge;
  auto &wire_width = f.wire_width;
  auto &dragged_number = f.dragged_number;
  // Wires scale with the graph but never thin below what reads on screen.
  wire_width = std::clamp(2.0F * view_.zoom, 1.5F, 3.5F);
  // A selected node's flow stands out: every wire feeding it, directly or
  // further upstream, and every wire it feeds on its way to Output.
  std::set<doc::NodeId> upstream;
  std::set<doc::NodeId> downstream;
  if (selection_.kind == CompositorSelectionKind::Node) {
    const auto walk = [&](std::set<doc::NodeId> &reached, bool forward) {
      std::vector<doc::NodeId> pending{selection_.node};
      while (!pending.empty()) {
        const doc::NodeId node = pending.back();
        pending.pop_back();
        if (!reached.insert(node).second) {
          continue;
        }
        for (const compositor::GraphEdge &edge : compositor_graph.edges) {
          if ((forward ? edge.from.node : edge.to.node) == node) {
            pending.push_back(forward ? edge.to.node : edge.from.node);
          }
        }
      }
    };
    walk(upstream, false);
    walk(downstream, true);
  }
  const bool wires_held = pending_wires_.has_value();
  drawn_wires_.clear();
  order_discs_.clear();
  const OrderDisc *const order_drop =
      f.order_target(ctx.pointer_x(), ctx.pointer_y());
  // The point a distance back along a wire from its input end.
  const auto back_along = [](std::span<const gui::UiPoint> points,
                             float distance) {
    for (std::size_t index = points.size(); index > 1U; --index) {
      const gui::UiPoint a = points[index - 2U];
      const gui::UiPoint b = points[index - 1U];
      const float length = std::hypot(b.x - a.x, b.y - a.y);
      if (length >= distance && length > 0.0F) {
        const float t = distance / length;
        return gui::UiPoint{b.x + (a.x - b.x) * t, b.y + (a.y - b.y) * t};
      }
      distance -= length;
    }
    return points.empty() ? gui::UiPoint{} : points.front();
  };
  for (const EdgeVisual &visual : edges) {
    DrawnWire &drawn = drawn_wires_.emplace_back(
        DrawnWire{visual.edge->node, visual.from->descriptor->type,
                  visual.edge->to, {}});
    for (const WirePath &segment : visual.segments) {
      const std::span<const gui::UiPoint> run = segment.view();
      drawn.points.insert(drawn.points.end(),
                          run.begin() + (drawn.points.empty() || run.empty()
                                             ? 0
                                             : 1),
                          run.end());
    }
    if (std::find(lifted.begin(), lifted.end(), visual.edge->node) !=
        lifted.end()) {
      continue; // drawn following the pointer below
    }
    if (wires_held &&
        std::find(pending_wires_->removed.begin(),
                  pending_wires_->removed.end(),
                  visual.edge->node) != pending_wires_->removed.end()) {
      continue; // removed, and on its way out of the graph
    }
    const bool selected =
        selection_ ==
        CompositorSelection{CompositorSelectionKind::Edge, visual.edge->node};
    const bool in_flow = (upstream.contains(visual.edge->from.node) &&
                          upstream.contains(visual.edge->to.node)) ||
                         (downstream.contains(visual.edge->from.node) &&
                          downstream.contains(visual.edge->to.node));
    const bool hovered = std::addressof(visual) == hovered_edge;
    const bool splice = std::addressof(visual) == insert_edge;
    const gui::UiColor color = port_color(ctx, visual.from->descriptor->type);
    // A bypassed or disabled node's wire carries nothing, and says so by
    // fading.
    const compositor::GraphNode *const wire_source =
        compositor_graph.node(visual.edge->from.node);
    const bool carries_nothing =
        wire_source != nullptr &&
        (bypassed(*wire_source) ||
         node_disabled(compositor_graph, wire_source->node));
    gui::UiColor stroke = with_alpha(color, 0.78F);
    float width = wire_width;
    if (selected || splice) {
      stroke = ctx.theme().text;
      width = wire_width + 1.5F;
    } else if (carries_nothing) {
      stroke = with_alpha(color, hovered ? 0.55F : 0.3F);
    } else if (hovered || in_flow) {
      stroke = blend_color(color, ctx.theme().text, hovered ? 0.45F : 0.2F);
      width = wire_width + (hovered ? 1.0F : 0.5F);
    }
    for (const WirePath &segment : visual.segments) {
      ctx.draw_list().add_polyline(segment.view(), width, stroke);
    }
    // Reroute points are small handles on the wire itself.
    for (const compositor::WirePoint point : visual.points) {
      const gui::UiPoint centre = f.to_screen(point);
      gui::add_disc(ctx.draw_list(), gui::UiPoint{centre.x, centre.y},
                    point_radius + 1.0F, scale(ctx.theme().panel, 0.8F));
      gui::add_disc(ctx.draw_list(), gui::UiPoint{centre.x, centre.y},
                    point_radius,
                    selected || hovered ? ctx.theme().text : color);
    }
    if (!chrome_readable || drawn.points.size() < 2U) {
      continue;
    }
    // Where several wires feed one input, each is numbered in the order the
    // input takes them: effects on a layer run first to last.
    const std::vector<const compositor::GraphEdge *> chain =
        input_chain(compositor_graph, visual.edge->to);
    if (chain.size() >= 2U) {
      const auto rank = static_cast<std::size_t>(
          std::distance(chain.begin(),
                        std::find(chain.begin(), chain.end(), visual.edge))) +
                        1U;
      const std::string number = std::to_string(rank);
      const gui::UiPoint at =
          back_along(drawn.points, std::max(30.0F * view_.zoom, 22.0F));
      const float radius = std::max(6.0F, 7.0F * view_.zoom);
      order_discs_.push_back(
          OrderDisc{visual.edge->node, visual.edge->to, at.x, at.y, radius});
      if (order_drag_ == visual.edge->node) {
        // Drawn after the nodes, at the pointer.
        dragged_number = std::pair{number, stroke};
      } else {
        const bool target = order_drop != nullptr &&
                            order_drop->edge == visual.edge->node;
        static_cast<void>(f.wire_disc(at, number,
                                    target ? ctx.theme().text : stroke,
                                    target ? ctx.theme().accent
                                           : scale(ctx.theme().panel, 1.18F)));
      }
    }
    // A problem with the wire itself is marked halfway along it.
    std::string problems;
    bool error = false;
    for (const compositor::GraphDiagnostic &diagnostic :
         compositor_graph.diagnostics) {
      if (diagnostic.subject != visual.edge->node) {
        continue;
      }
      if (!problems.empty()) {
        problems += " | ";
      }
      problems += compositor_diagnostic_message(diagnostic);
      error = error ||
              diagnostic.severity == compositor::DiagnosticSeverity::Error;
    }
    if (!problems.empty()) {
      const gui::Rect mark = f.wire_disc(
          drawn.points[drawn.points.size() / 2U], "!", ctx.theme().warning,
          error ? ctx.theme().warning : scale(ctx.theme().panel, 1.18F));
      ctx.tooltip(mark, problems);
    }
  }
  if (wires_held) {
    for (const CompositorWire &held : pending_wires_->added) {
      const bool present = std::any_of(
          compositor_graph.edges.begin(), compositor_graph.edges.end(),
          [&held](const compositor::GraphEdge &edge) noexcept {
            return edge.from == held.from && edge.to == held.to;
          });
      const CompositorPortVisual *const from = find_port(outputs, held.from);
      const CompositorPortVisual *const to = find_port(inputs, held.to);
      if (!present && from != nullptr && to != nullptr) {
        wire(ctx, from->x, from->y, to->x, to->y,
             with_alpha(port_color(ctx, from->descriptor->type), 0.78F),
             wire_width, view_.zoom);
      }
    }
  }
}

void CompositorPanel::draw_overlays(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const compositor::CompositorGraph &compositor_graph = f.compositor_graph;
  auto &canvas = f.canvas;
  auto &nodes = f.nodes;
  auto &inputs = f.inputs;
  auto &lifted = f.lifted;
  auto &minimap = f.minimap;
  auto &minimap_world = f.minimap_world;
  auto &minimap_scale = f.minimap_scale;
  auto &dragged_number = f.dragged_number;
  auto &snapped = f.snapped;
  auto &connecting_source = f.connecting_source;
  // An order number being dragged rides on the pointer, over the nodes.
  if (dragged_number.has_value()) {
    static_cast<void>(
        f.wire_disc(gui::UiPoint{ctx.pointer_x(), ctx.pointer_y()},
                  dragged_number->first, ctx.theme().text,
                  dragged_number->second));
  }
  if (connecting_source != nullptr && snapped == nullptr) {
    const auto under = std::find_if(
        inputs.begin(), inputs.end(), [&ctx](const CompositorPortVisual &port) {
          return port.hit.contains(ctx.pointer_x(), ctx.pointer_y());
        });
    const std::string reason =
        under != inputs.end()
            ? connection_refusal(compositor_graph, *connecting_source, *under,
                                 lifted)
            : std::string{};
    if (!reason.empty()) {
      const float width = std::ceil(ctx.font_width(reason)) + 16.0F;
      const float height = ctx.line_height() + 8.0F;
      const gui::Rect badge{ctx.pointer_x() + 14.0F, ctx.pointer_y() + 10.0F,
                            width, height};
      ctx.panel(badge, with_alpha(ctx.theme().background, 0.94F));
      ctx.outline(badge, ctx.theme().warning);
      ctx.label(gui::Rect{badge.x + 8.0F, badge.y, badge.width - 8.0F,
                          badge.height},
                reason, ctx.theme().warning);
    }
  }
  if (marquee_.has_value() && ctx.pointer_down()) {
    const gui::Rect band{std::min(marquee_->start_x, ctx.pointer_x()),
                         std::min(marquee_->start_y, ctx.pointer_y()),
                         std::abs(ctx.pointer_x() - marquee_->start_x),
                         std::abs(ctx.pointer_y() - marquee_->start_y)};
    ctx.panel(band, with_alpha(ctx.theme().accent, 0.10F));
    ctx.outline(band, with_alpha(ctx.theme().accent, 0.75F));
  }
  if (minimap.width > 0.0F && minimap_scale > 0.0F) {
    const auto map = [&](float x, float y, float width, float height) {
      return gui::Rect{minimap.x + (x - minimap_world.x) * minimap_scale,
                       minimap.y + (y - minimap_world.y) * minimap_scale,
                       std::max(width * minimap_scale, 1.0F),
                       std::max(height * minimap_scale, 1.0F)};
    };
    ctx.panel(minimap, with_alpha(ctx.theme().background, 0.92F));
    for (const compositor::GraphComment &comment : compositor_graph.comments) {
      const compositor::GraphComment drawn = f.drawn_comment(comment);
      ctx.outline(map(drawn.x, drawn.y, drawn.width, drawn.height),
                  with_alpha(comment_color(drawn.color), 0.7F));
    }
    for (const CompositorNodeVisual &visual : nodes) {
      const gui::UiPoint at =
          view_.to_graph(canvas, visual.bounds.x, visual.bounds.y);
      ctx.panel(map(at.x, at.y, visual.bounds.width / view_.zoom,
                    visual.bounds.height / view_.zoom),
                with_alpha(compositor_node_color(ctx, visual.node->kind), 0.8F));
    }
    const gui::Rect seen = view_.visible(canvas);
    ctx.outline(map(seen.x, seen.y, seen.width, seen.height), ctx.theme().text);
    ctx.outline(minimap, ctx.theme().border);
  }
  if (ctx.time_seconds() < notice_until_ && !notice_.empty()) {
    const float width = std::ceil(ctx.font_width(notice_)) + 20.0F;
    const float height = ctx.line_height() + 10.0F;
    const gui::Rect badge{canvas.x + 10.0F,
                          canvas.bottom() - height - 10.0F, width,
                          height};
    ctx.panel(badge, with_alpha(ctx.theme().background, 0.94F));
    ctx.outline(badge, ctx.theme().border);
    ctx.label(gui::Rect{badge.x + 10.0F, badge.y, badge.width - 10.0F,
                        badge.height},
              notice_, ctx.theme().text);
  }
  ctx.pop_clip();
}

} // namespace mediaserver::editor
