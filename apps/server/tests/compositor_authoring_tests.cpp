#include "director_compositor_authoring.hpp"

#include <mediaserver/compositor/compositor_model.hpp>
#include <mediaserver/compositor/compositor_schema.hpp>
#include <mediaserver/doc/document.hpp>

#include <array>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace mediaserver;
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error{#__VA_ARGS__}; } while (false)

const doc::NodeId scene_id{3U};
constexpr cmd::UndoContextId graph_history{6U};

doc::Node graph_node(core::u64 id, std::string_view type) {
  doc::Node node{doc::NodeId{id}, std::string{type}, {}};
  node.properties.emplace(std::string{compositor::schema::scene_id},
                          core::i64{3});
  return node;
}

doc::Node layer(core::u64 id, core::u64 element) {
  doc::Node node = graph_node(id, compositor::schema::scene_layer_type);
  node.properties.emplace(std::string{compositor::schema::scene_node},
                          static_cast<core::i64>(element));
  return node;
}

compositor::EdgeEndpoint port(core::u64 node, core::u8 index) {
  return compositor::EdgeEndpoint{doc::NodeId{node}, index};
}

// The wire `id` runs from -> to.
bool wired(const doc::Root &root, core::u64 id, compositor::EdgeEndpoint from,
           compositor::EdgeEndpoint to) {
  const doc::Node *const edge = root.get(doc::NodeId{id});
  return edge != nullptr && *edge == app::make_compositor_edge(edge->id, from, to);
}

// Two documents hold the same nodes.
bool same(const doc::Root &left, const doc::Root &right) {
  return doc::diff(left, right).changes.empty();
}

// The starting graph: an element, its Scene Layer (5) wired by edge 7 into
// Output (6).
doc::Root starting_graph() {
  return doc::Root{}
      .set(doc::Node{doc::NodeId{4U}, "scene.media", {}})
      .set(layer(5U, 4U))
      .set(graph_node(6U, compositor::schema::output_type))
      .set(app::make_compositor_edge(doc::NodeId{7U}, port(5U, 0U),
                                     port(6U, 0U)));
}

// A node added with its wires - one in, one out - and the wire it displaces
// removed, is one undo step, undone and redone whole.
void a_linked_add_is_one_undo_step() {
  const doc::Root before = starting_graph();
  cmd::DirectorScheduler scheduler{before};
  editor::CompositorAddIntent request{compositor::NodeKind::Composite, 10.0F,
                                      20.0F};
  request.links = {{false, 0U, port(5U, 0U)}, {true, 0U, port(6U, 0U)}};
  request.remove_edges = {doc::NodeId{7U}};
  const std::array<doc::NodeId, 2U> ids{doc::NodeId{20U}, doc::NodeId{21U}};
  CHECK(app::schedule_compositor_add_linked(scheduler, request,
                                            doc::NodeId{10U}, scene_id, ids,
                                            300U, cmd::FrameNumber{1U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  const doc::Root &after = scheduler.sealed_document();
  CHECK(after.get(doc::NodeId{10U}) != nullptr &&
        after.get(doc::NodeId{10U})->type == compositor::schema::composite_type);
  CHECK(wired(after, 20U, port(5U, 0U), port(10U, 0U)));
  CHECK(wired(after, 21U, port(10U, 0U), port(6U, 0U)));
  CHECK(after.get(doc::NodeId{7U}) == nullptr);
  CHECK(scheduler.undo_depth(graph_history) == 1U);

  CHECK(scheduler.schedule_undo(graph_history, cmd::TransactionId{301U},
                                cmd::FrameNumber{2U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
  CHECK(same(scheduler.sealed_document(), before));
  CHECK(scheduler.schedule_redo(graph_history, cmd::TransactionId{302U},
                                cmd::FrameNumber{3U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{3U}));
  CHECK(wired(scheduler.sealed_document(), 21U, port(10U, 0U), port(6U, 0U)));
  CHECK(scheduler.sealed_document().get(doc::NodeId{7U}) == nullptr);
}

// A node that cannot be added - a Scene Layer naming nothing - takes its
// wires and removals with it: nothing changes.
void a_refused_add_changes_nothing() {
  const doc::Root before = starting_graph();
  cmd::DirectorScheduler scheduler{before};
  editor::CompositorAddIntent request{compositor::NodeKind::SceneLayer, 0.0F,
                                      0.0F};
  request.links = {{true, 0U, port(6U, 0U)}};
  request.remove_edges = {doc::NodeId{7U}};
  const std::array<doc::NodeId, 1U> ids{doc::NodeId{20U}};
  CHECK(app::schedule_compositor_add_linked(scheduler, request,
                                            doc::NodeId{10U}, scene_id, ids,
                                            300U, cmd::FrameNumber{1U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  CHECK(same(scheduler.sealed_document(), before));
  CHECK(scheduler.undo_depth(graph_history) == 0U);
}

// Wires removed and added together - a moved wire - are one undo step. A
// wire to a node that no longer exists is skipped, not authored dangling.
void a_rewire_is_one_undo_step() {
  doc::Root before = starting_graph().set(layer(8U, 4U));
  cmd::DirectorScheduler scheduler{before};
  const editor::CompositorRewireIntent request{
      {doc::NodeId{7U}},
      {{port(8U, 0U), port(6U, 0U)}, {port(99U, 0U), port(6U, 0U)}}};
  const std::array<doc::NodeId, 2U> ids{doc::NodeId{20U}, doc::NodeId{21U}};
  CHECK(app::schedule_compositor_rewire(scheduler, request, ids, 400U,
                                        cmd::FrameNumber{1U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  const doc::Root &after = scheduler.sealed_document();
  CHECK(after.get(doc::NodeId{7U}) == nullptr);
  CHECK(wired(after, 20U, port(8U, 0U), port(6U, 0U)));
  CHECK(after.get(doc::NodeId{21U}) == nullptr);
  CHECK(scheduler.undo_depth(graph_history) == 1U);
  CHECK(scheduler.schedule_undo(graph_history, cmd::TransactionId{401U},
                                cmd::FrameNumber{2U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
  CHECK(same(scheduler.sealed_document(), before));
}

// A wire's reroute points are written as its indexed point properties,
// replacing the list it had; a list with a bad point is ignored whole.
void reroute_points_replace_the_wires_list() {
  const doc::Root before = starting_graph();
  cmd::DirectorScheduler scheduler{before};
  CHECK(app::schedule_compositor_waypoints(
      scheduler,
      {doc::NodeId{7U}, {{120.5F, -40.0F}, {300.0F, 80.0F}}},
      500U, cmd::FrameNumber{1U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  const auto value = [&](std::string_view name) {
    const doc::Node *const edge =
        scheduler.sealed_document().get(doc::NodeId{7U});
    const auto found = edge->properties.find(name);
    return found == edge->properties.end()
               ? std::optional<double>{}
               : std::optional<double>{std::get<double>(found->second)};
  };
  CHECK(value("point_0_x") == 120.5 && value("point_0_y") == -40.0);
  CHECK(value("point_1_x") == 300.0 && value("point_1_y") == 80.0);
  // The wire itself is untouched.
  doc::Node unrouted = *scheduler.sealed_document().get(doc::NodeId{7U});
  std::erase_if(unrouted.properties, [](const auto &property) {
    return property.first.starts_with("point_");
  });
  CHECK(unrouted == *before.get(doc::NodeId{7U}));

  CHECK(app::schedule_compositor_waypoints(
      scheduler, {doc::NodeId{7U}, {{10.0F, 10.0F}}}, 501U,
      cmd::FrameNumber{2U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
  CHECK(value("point_0_x") == 10.0 && !value("point_1_x").has_value());

  CHECK(app::schedule_compositor_waypoints(
      scheduler, {doc::NodeId{7U}, {{10.0F, 10.0F}, {5.0e6F, 0.0F}}}, 502U,
      cmd::FrameNumber{3U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{3U}));
  CHECK(value("point_0_x") == 10.0 && !value("point_1_x").has_value());
  CHECK(scheduler.undo_depth(graph_history) == 2U);

  // Points never land on anything but a wire.
  CHECK(app::schedule_compositor_waypoints(
      scheduler, {doc::NodeId{6U}, {{1.0F, 1.0F}}}, 503U,
      cmd::FrameNumber{4U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{4U}));
  CHECK(*scheduler.sealed_document().get(doc::NodeId{6U}) ==
        *before.get(doc::NodeId{6U}));
}

// Renaming sets a node's or comment box's title, and an empty name gives a
// node back its kind's.
void renames_set_and_clear_titles() {
  doc::Node box = graph_node(9U, compositor::schema::comment_type);
  box.properties.emplace(std::string{compositor::schema::label},
                         std::string{"Plates"});
  const doc::Root before = starting_graph().set(box);
  cmd::DirectorScheduler scheduler{before};
  CHECK(app::schedule_compositor_rename(scheduler, {doc::NodeId{6U}, "Program"},
                                        600U, cmd::FrameNumber{1U}));
  CHECK(app::schedule_compositor_rename(
      scheduler, {doc::NodeId{9U}, "Car plates"}, 600U, cmd::FrameNumber{1U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  const auto label = [&](core::u64 id) {
    const doc::Node *const node = scheduler.sealed_document().get(doc::NodeId{id});
    const auto found = node->properties.find(compositor::schema::label);
    return found == node->properties.end()
               ? std::string{}
               : std::get<std::string>(found->second);
  };
  CHECK(label(6U) == "Program" && label(9U) == "Car plates");
  CHECK(scheduler.undo_depth(graph_history) == 1U);
  CHECK(app::schedule_compositor_rename(scheduler, {doc::NodeId{6U}, ""}, 601U,
                                        cmd::FrameNumber{2U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
  CHECK(label(6U).empty());
  // A wire has no title to rename.
  CHECK(app::schedule_compositor_rename(scheduler, {doc::NodeId{7U}, "Nope"},
                                        602U, cmd::FrameNumber{3U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{3U}));
  CHECK(*scheduler.sealed_document().get(doc::NodeId{7U}) ==
        *before.get(doc::NodeId{7U}));
}

// A comment box is created in the scene being authored, changed in place,
// and refused when it is too small or its colour unknown.
void comment_boxes_are_created_and_changed() {
  cmd::DirectorScheduler scheduler{starting_graph()};
  editor::CompositorCommentIntent request{
      {}, 10.0F, 20.0F, 300.0F, 200.0F, "Plates", 2U};
  CHECK(app::schedule_compositor_comment(scheduler, request, doc::NodeId{30U},
                                         scene_id, 700U, cmd::FrameNumber{1U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  const doc::Node *const created =
      scheduler.sealed_document().get(doc::NodeId{30U});
  CHECK(created != nullptr &&
        created->type == compositor::schema::comment_type);
  CHECK(std::get<core::i64>(created->properties.at(
            std::string{compositor::schema::scene_id})) == 3);
  CHECK(std::get<double>(created->properties.at(
            std::string{compositor::schema::comment_width})) == 300.0);

  request.node = doc::NodeId{30U};
  request.x = 50.0F;
  request.color = 4U;
  CHECK(app::schedule_compositor_comment(scheduler, request, doc::NodeId{30U},
                                         scene_id, 701U, cmd::FrameNumber{2U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
  const doc::Node *const moved =
      scheduler.sealed_document().get(doc::NodeId{30U});
  CHECK(std::get<double>(moved->properties.at(
            std::string{compositor::schema::editor_x})) == 50.0);
  CHECK(std::get<core::i64>(moved->properties.at(
            std::string{compositor::schema::comment_color})) == 4);

  editor::CompositorCommentIntent tiny = request;
  tiny.width = 5.0F;
  editor::CompositorCommentIntent unknown = request;
  unknown.color = 9U;
  CHECK(app::schedule_compositor_comment(scheduler, tiny, doc::NodeId{30U},
                                         scene_id, 702U, cmd::FrameNumber{3U}));
  CHECK(app::schedule_compositor_comment(scheduler, unknown, doc::NodeId{30U},
                                         scene_id, 702U, cmd::FrameNumber{3U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{3U}));
  CHECK(*scheduler.sealed_document().get(doc::NodeId{30U}) == *moved);
  CHECK(scheduler.undo_depth(graph_history) == 2U);
}

// Deleting several selected items is one undo step: nodes with their wires,
// a comment box, but never Output.
void several_items_delete_in_one_step() {
  doc::Node box = graph_node(9U, compositor::schema::comment_type);
  const doc::Root before =
      starting_graph().set(layer(8U, 4U)).set(box).set(
          app::make_compositor_edge(doc::NodeId{12U}, port(8U, 0U),
                                    port(5U, 0U)));
  cmd::DirectorScheduler scheduler{before};
  const editor::CompositorDeleteIntent request{
      {{editor::CompositorSelectionKind::Node, doc::NodeId{5U}},
       {editor::CompositorSelectionKind::Node, doc::NodeId{6U}},
       {editor::CompositorSelectionKind::Comment, doc::NodeId{9U}}}};
  CHECK(app::schedule_compositor_delete(scheduler, request, 800U,
                                        cmd::FrameNumber{1U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  const doc::Root &after = scheduler.sealed_document();
  CHECK(after.get(doc::NodeId{5U}) == nullptr);
  CHECK(after.get(doc::NodeId{7U}) == nullptr);
  CHECK(after.get(doc::NodeId{12U}) == nullptr);
  CHECK(after.get(doc::NodeId{9U}) == nullptr);
  CHECK(after.get(doc::NodeId{6U}) != nullptr);
  CHECK(after.get(doc::NodeId{8U}) != nullptr);
  CHECK(scheduler.undo_depth(graph_history) == 1U);
  CHECK(scheduler.schedule_undo(graph_history, cmd::TransactionId{801U},
                                cmd::FrameNumber{2U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
  CHECK(same(scheduler.sealed_document(), before));
}

// A paste lands as one undo step, and the ids it used are never handed out
// again.
void a_paste_is_one_undo_step() {
  const doc::Root before = starting_graph();
  cmd::DirectorScheduler scheduler{before};
  doc::Node copy = graph_node(40U, compositor::schema::composite_type);
  doc::Node box = graph_node(41U, compositor::schema::comment_type);
  const std::array<doc::Node, 2U> puts{copy, box};
  core::u64 next_id = 10U;
  CHECK(app::schedule_compositor_paste(scheduler, puts, 900U,
                                       cmd::FrameNumber{1U}, next_id));
  CHECK(next_id == 42U);
  CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  CHECK(scheduler.sealed_document().get(doc::NodeId{40U}) != nullptr);
  CHECK(scheduler.undo_depth(graph_history) == 1U);
  CHECK(scheduler.schedule_undo(graph_history, cmd::TransactionId{901U},
                                cmd::FrameNumber{2U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
  CHECK(same(scheduler.sealed_document(), before));

  const std::array<doc::Node, 3U> snapshot{
      [] {
        doc::Node one = graph_node(1U, compositor::schema::composite_type);
        one.properties.emplace(std::string{compositor::schema::editor_x}, 300.0);
        one.properties.emplace(std::string{compositor::schema::editor_y}, 80.0);
        return one;
      }(),
      [] {
        doc::Node one = graph_node(2U, compositor::schema::comment_type);
        one.properties.emplace(std::string{compositor::schema::editor_x}, 120.0);
        one.properties.emplace(std::string{compositor::schema::editor_y}, 150.0);
        return one;
      }(),
      app::make_compositor_edge(doc::NodeId{3U}, port(1U, 0U), port(1U, 0U))};
  CHECK(app::compositor_snapshot_origin(snapshot) ==
        (std::pair<float, float>{120.0F, 80.0F}));
}

// An effect spliced in between two others on one input takes the order it
// was given, and the wires it makes room among are renumbered in the same
// undo step.
void a_spliced_add_renumbers_its_neighbours() {
  doc::Node edge_8 =
      app::make_compositor_edge(doc::NodeId{8U}, port(12U, 0U), port(5U, 0U));
  doc::Node edge_11 =
      app::make_compositor_edge(doc::NodeId{11U}, port(13U, 0U), port(5U, 0U));
  const doc::Root before =
      starting_graph()
          .set(graph_node(12U, compositor::schema::brightness_type))
          .set(graph_node(13U, compositor::schema::saturation_type))
          .set(edge_8)
          .set(edge_11);
  cmd::DirectorScheduler scheduler{before};
  editor::CompositorAddIntent request{compositor::NodeKind::Blur, 10.0F,
                                      20.0F};
  request.links = {{true, 0U, port(5U, 0U), 1}};
  request.reorder = {{doc::NodeId{11U}, 2}, {doc::NodeId{99U}, 4}};
  const std::array<doc::NodeId, 1U> ids{doc::NodeId{20U}};
  CHECK(app::schedule_compositor_add_linked(scheduler, request,
                                            doc::NodeId{30U}, scene_id, ids,
                                            800U, cmd::FrameNumber{1U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  const doc::Root &after = scheduler.sealed_document();
  const auto order_of = [&](core::u64 id) {
    return std::get<core::i64>(after.get(doc::NodeId{id})->properties.at(
        std::string{compositor::schema::order}));
  };
  CHECK(order_of(20U) == 1);
  CHECK(order_of(11U) == 2);
  CHECK(order_of(8U) == 0);
  // A renumbered wire that is not there is not invented.
  CHECK(after.get(doc::NodeId{99U}) == nullptr);
  CHECK(scheduler.undo_depth(graph_history) == 1U);
  CHECK(scheduler.schedule_undo(graph_history, cmd::TransactionId{801U},
                                cmd::FrameNumber{2U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
  CHECK(same(scheduler.sealed_document(), before));
}

// Collapsing writes the flag on each node - and nothing on a wire - and
// opening takes it away again; each is one undo step.
void collapsing_marks_nodes_and_opening_clears_them() {
  const doc::Root before = starting_graph();
  cmd::DirectorScheduler scheduler{before};
  CHECK(app::schedule_compositor_collapse(
      scheduler,
      editor::CompositorCollapseIntent{
          {doc::NodeId{5U}, doc::NodeId{6U}, doc::NodeId{7U}}, true},
      900U, cmd::FrameNumber{1U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  const auto folded = [&](core::u64 id) {
    const doc::Node *const node = scheduler.sealed_document().get(doc::NodeId{id});
    const auto found = node->properties.find(compositor::schema::collapsed);
    return found != node->properties.end() && std::get<bool>(found->second);
  };
  CHECK(folded(5U) && folded(6U));
  CHECK(*scheduler.sealed_document().get(doc::NodeId{7U}) ==
        *before.get(doc::NodeId{7U}));
  CHECK(scheduler.undo_depth(graph_history) == 1U);

  CHECK(app::schedule_compositor_collapse(
      scheduler, editor::CompositorCollapseIntent{{doc::NodeId{6U}}, false},
      901U, cmd::FrameNumber{2U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
  CHECK(folded(5U) && !folded(6U));
  CHECK(*scheduler.sealed_document().get(doc::NodeId{6U}) ==
        *before.get(doc::NodeId{6U}));
  CHECK(scheduler.undo_depth(graph_history) == 2U);
}

// Bypass is written on nodes that can pass through and nowhere else, and
// taken away again; each is one undo step.
void bypass_marks_only_bypassable_nodes() {
  const doc::Root before =
      starting_graph().set(graph_node(12U, compositor::schema::blur_type));
  cmd::DirectorScheduler scheduler{before};
  CHECK(app::schedule_compositor_bypass(
      scheduler,
      editor::CompositorBypassIntent{
          {doc::NodeId{5U}, doc::NodeId{6U}, doc::NodeId{7U},
           doc::NodeId{12U}},
          true},
      950U, cmd::FrameNumber{1U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  const auto passed = [&](core::u64 id) {
    const doc::Node *const node = scheduler.sealed_document().get(doc::NodeId{id});
    const auto found = node->properties.find(compositor::schema::bypassed);
    return found != node->properties.end() && std::get<bool>(found->second);
  };
  CHECK(passed(12U));
  // The Scene Layer, Output and the wire are left exactly as they were.
  for (const core::u64 id : {5U, 6U, 7U}) {
    CHECK(*scheduler.sealed_document().get(doc::NodeId{id}) ==
          *before.get(doc::NodeId{id}));
  }
  CHECK(scheduler.undo_depth(graph_history) == 1U);
  CHECK(app::schedule_compositor_bypass(
      scheduler, editor::CompositorBypassIntent{{doc::NodeId{12U}}, false},
      951U, cmd::FrameNumber{2U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
  CHECK(same(scheduler.sealed_document(), before));
  CHECK(scheduler.undo_depth(graph_history) == 2U);
}

// Keyer and Matte values are written in the document's own forms: a signed
// whole edge, switches as booleans, Matte's mode by name. A Luma Key range
// that would turn over is refused.
void keyer_and_matte_values_are_written_typed() {
  const doc::Root before =
      starting_graph()
          .set(graph_node(12U, compositor::schema::chroma_key_type))
          .set(graph_node(13U, compositor::schema::luma_key_type))
          .set(graph_node(14U, compositor::schema::matte_type));
  cmd::DirectorScheduler scheduler{before};
  using P = editor::CompositorProperty;
  const auto author = [&](core::u64 id, P property,
                          editor::CompositorPropertyValue value,
                          core::u64 frame) {
    CHECK(app::schedule_compositor_property(
        scheduler, {doc::NodeId{id}, property, value}, 960U + frame,
        cmd::FrameNumber{frame}, nullptr));
    CHECK(scheduler.seal_frame(cmd::FrameNumber{frame}));
  };
  const auto value = [&](core::u64 id, std::string_view name) {
    return scheduler.sealed_document().get(doc::NodeId{id})->properties.at(
        std::string{name});
  };
  author(12U, P::ChromaKeyEdgePx, editor::CompositorPropertyValue{-3.0F}, 1U);
  CHECK(std::get<std::int64_t>(value(
            12U, compositor::schema::chroma_key_morphology_px)) == -3);
  author(12U, P::ChromaKeyBlue, editor::CompositorPropertyValue{0.25F}, 2U);
  CHECK(std::get<double>(value(12U, compositor::schema::chroma_key_color_b)) ==
        0.25);
  author(13U, P::LumaKeyInvert,
         editor::CompositorPropertyValue{core::u32{1U}}, 3U);
  CHECK(std::get<bool>(value(13U, compositor::schema::luma_key_invert)));
  author(14U, P::MatteMode, editor::CompositorPropertyValue{core::u32{1U}},
         4U);
  CHECK(std::get<std::string>(value(14U, compositor::schema::matte_mode)) ==
        compositor::schema::matte_luma_mode);
  author(14U, P::MatteClampInput,
         editor::CompositorPropertyValue{core::u32{0U}}, 5U);
  CHECK(!std::get<bool>(value(14U, compositor::schema::matte_clamp_input)));

  // Low above the default high of one: the node is left as it was.
  author(13U, P::LumaKeyHigh, editor::CompositorPropertyValue{0.5F}, 6U);
  const doc::Node kept = *scheduler.sealed_document().get(doc::NodeId{13U});
  author(13U, P::LumaKeyLow, editor::CompositorPropertyValue{0.75F}, 7U);
  CHECK(*scheduler.sealed_document().get(doc::NodeId{13U}) == kept);
}

// A new Merge or Background is complete as added - the graph reads it with
// no refusals - and a Merge with only BASE wired leaves the picture alone.
// Their named choices are written as the words the graph reads; a choice
// past the end of the list writes nothing.
void merge_and_background_are_added_whole_and_choices_written_as_words() {
  cmd::DirectorScheduler scheduler{starting_graph()};
  const auto add = [&](compositor::NodeKind kind, core::u64 id,
                       core::u64 frame) {
    CHECK(app::schedule_compositor_add(
        scheduler, editor::CompositorAddIntent{kind, 0.0F, 0.0F},
        doc::NodeId{id}, scene_id, 970U + frame, cmd::FrameNumber{frame}));
    CHECK(scheduler.seal_frame(cmd::FrameNumber{frame}));
  };
  add(compositor::NodeKind::Merge, 30U, 1U);
  add(compositor::NodeKind::Background, 31U, 2U);
  compositor::CompositorModel model;
  model.reset(scheduler.sealed_document());
  CHECK(model.counters().rejected == 0U);
  const compositor::CompositorGraph added = model.evaluate();
  const compositor::GraphNode *const merge = added.node(doc::NodeId{30U});
  const compositor::GraphNode *const background = added.node(doc::NodeId{31U});
  CHECK(merge != nullptr && background != nullptr);
  const auto &merge_parameters =
      std::get<compositor::MergeParameters>(merge->parameters);
  CHECK(merge_parameters == compositor::MergeParameters{});
  CHECK(compositor::merge_constant_is_identity(merge_parameters));
  CHECK(std::get<compositor::BackgroundParameters>(background->parameters) ==
        compositor::BackgroundParameters{});

  using P = editor::CompositorProperty;
  const auto author = [&](core::u64 id, P property,
                          editor::CompositorPropertyValue value,
                          core::u64 frame) {
    CHECK(app::schedule_compositor_property(
        scheduler, {doc::NodeId{id}, property, value}, 980U + frame,
        cmd::FrameNumber{frame}, nullptr));
    CHECK(scheduler.seal_frame(cmd::FrameNumber{frame}));
  };
  const auto value = [&](core::u64 id, std::string_view name) {
    return scheduler.sealed_document().get(doc::NodeId{id})->properties.at(
        std::string{name});
  };
  author(30U, P::MergeBlendMode,
         editor::CompositorPropertyValue{core::u32{2U}}, 3U);
  CHECK(std::get<std::string>(value(30U, compositor::schema::blend_mode)) ==
        compositor::schema::multiply_blend);
  author(30U, P::MergeBlendMode,
         editor::CompositorPropertyValue{
             core::u32{compositor::merge_blend_mode_count}},
         4U);
  CHECK(std::get<std::string>(value(30U, compositor::schema::blend_mode)) ==
        compositor::schema::multiply_blend);
  author(30U, P::MergeValue, editor::CompositorPropertyValue{0.5F}, 5U);
  CHECK(std::get<double>(value(30U, compositor::schema::value)) == 0.5);
  author(31U, P::BackgroundMode,
         editor::CompositorPropertyValue{core::u32{2U}}, 6U);
  CHECK(std::get<std::string>(value(
            31U, compositor::schema::background_mode)) ==
        compositor::schema::radial_background);
  author(31U, P::BackgroundColorBA, editor::CompositorPropertyValue{0.25F},
         7U);
  CHECK(std::get<double>(value(31U, compositor::schema::color_b_a)) == 0.25);
  author(31U, P::BackgroundSize, editor::CompositorPropertyValue{1.5F}, 8U);
  CHECK(std::get<double>(value(31U, compositor::schema::gradient_size)) ==
        1.5);

  model.reset(scheduler.sealed_document());
  CHECK(model.counters().rejected == 0U);
  const compositor::CompositorGraph edited = model.evaluate();
  const auto &scaled = std::get<compositor::MergeParameters>(
      edited.node(doc::NodeId{30U})->parameters);
  CHECK(scaled.blend == compositor::MergeBlendMode::Multiply &&
        scaled.value == 0.5F);
  const auto &radial = std::get<compositor::BackgroundParameters>(
      edited.node(doc::NodeId{31U})->parameters);
  CHECK(radial.mode == compositor::BackgroundMode::Radial &&
        radial.color_b.a == 0.25F && radial.size == 1.5F);

  // A Merge can be bypassed to its BASE; a Background has nothing to pass.
  CHECK(app::schedule_compositor_bypass(
      scheduler,
      editor::CompositorBypassIntent{{doc::NodeId{30U}, doc::NodeId{31U}},
                                     true},
      990U, cmd::FrameNumber{9U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{9U}));
  CHECK(std::get<bool>(value(30U, compositor::schema::bypassed)));
  CHECK(!scheduler.sealed_document()
             .get(doc::NodeId{31U})
             ->properties.contains(std::string{compositor::schema::bypassed}));
}

// Wires are renumbered in place as one undo step; anything else named is
// left alone.
void a_reorder_renumbers_wires_in_one_step() {
  const doc::Root before =
      starting_graph()
          .set(graph_node(12U, compositor::schema::brightness_type))
          .set(app::make_compositor_edge(doc::NodeId{8U}, port(12U, 0U),
                                         port(5U, 0U)));
  cmd::DirectorScheduler scheduler{before};
  CHECK(app::schedule_compositor_reorder(
      scheduler,
      editor::CompositorReorderIntent{
          {{doc::NodeId{8U}, 3}, {doc::NodeId{12U}, 9}}},
      970U, cmd::FrameNumber{1U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  CHECK(std::get<core::i64>(
            scheduler.sealed_document().get(doc::NodeId{8U})->properties.at(
                std::string{compositor::schema::order})) == 3);
  CHECK(*scheduler.sealed_document().get(doc::NodeId{12U}) ==
        *before.get(doc::NodeId{12U}));
  CHECK(scheduler.undo_depth(graph_history) == 1U);
  CHECK(scheduler.schedule_undo(graph_history, cmd::TransactionId{971U},
                                cmd::FrameNumber{2U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
  CHECK(same(scheduler.sealed_document(), before));
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 14U> cases{{
    {"Merge and Background are added whole and choices written as words",
     &merge_and_background_are_added_whole_and_choices_written_as_words},
    {"keyer and Matte values are written typed",
     &keyer_and_matte_values_are_written_typed},
    {"a reorder renumbers wires in one step",
     &a_reorder_renumbers_wires_in_one_step},
    {"bypass marks only bypassable nodes",
     &bypass_marks_only_bypassable_nodes},
    {"a spliced add renumbers its neighbours",
     &a_spliced_add_renumbers_its_neighbours},
    {"collapsing marks nodes and opening clears them",
     &collapsing_marks_nodes_and_opening_clears_them},
    {"renames set and clear titles", &renames_set_and_clear_titles},
    {"comment boxes are created and changed",
     &comment_boxes_are_created_and_changed},
    {"several items delete in one step",
     &several_items_delete_in_one_step},
    {"a paste is one undo step", &a_paste_is_one_undo_step},
    {"a linked add is one undo step", &a_linked_add_is_one_undo_step},
    {"a refused add changes nothing", &a_refused_add_changes_nothing},
    {"a rewire is one undo step", &a_rewire_is_one_undo_step},
    {"reroute points replace the wire's list",
     &reroute_points_replace_the_wires_list},
}};

} // namespace

int main() {
  std::size_t failures{};
  for (const Case &test : cases) {
    try {
      test.function();
      std::cout << "[pass] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
