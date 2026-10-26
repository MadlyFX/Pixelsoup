#include "director_compositor_authoring.hpp"

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

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 4U> cases{{
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
