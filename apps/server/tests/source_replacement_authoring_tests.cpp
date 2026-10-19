#include "source_replacement_authoring.hpp"
#include <mediaserver/assets/asset_schema.hpp>

#include <iostream>
#include <stdexcept>

namespace {
using namespace mediaserver;
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error{#__VA_ARGS__}; } while (false)
const scene::SceneId scene_id{31U};

doc::Node placement(doc::NodeId id, std::string_view type) {
  doc::Node node{id, std::string{type}, {}};
  node.properties.emplace(std::string{scene::schema::scene_id}, core::i64{31});
  node.properties.emplace(std::string{scene::schema::source}, std::string{"T:/original.mov"});
  node.properties.emplace(std::string{scene::schema::asset_id}, core::i64{21});
  node.properties.emplace(std::string{scene::schema::position_x}, 4.75);
  node.properties.emplace(std::string{scene::schema::clip_start}, core::i64{19});
  node.properties.emplace(std::string{scene::schema::clip_duration}, core::i64{83});
  node.properties.emplace(std::string{scene::schema::source_in}, core::i64{7});
  node.properties.emplace(std::string{scene::schema::sequence_count}, core::i64{220});
  node.properties.emplace(std::string{scene::schema::source_rate_numerator}, core::i64{24});
  node.properties.emplace(std::string{scene::schema::source_rate_denominator}, core::i64{1});
  return node;
}
doc::Node asset(std::string_view type) {
  doc::Node node{doc::NodeId{91U}, std::string{type}, {}};
  node.properties.emplace(std::string{assets::schema::source}, std::string{"T:/replacement.mov"});
  node.properties.emplace(std::string{assets::schema::display_name}, std::string{"Replacement"});
  if (type == assets::schema::media_type) {
    node.properties.emplace(std::string{assets::schema::media_frame_count}, core::i64{637});
    node.properties.emplace(std::string{assets::schema::media_rate_numerator}, core::i64{30000});
    node.properties.emplace(std::string{assets::schema::media_rate_denominator}, core::i64{1001});
  }
  return node;
}
void asset_replacement() {
  for (bool media : {true, false}) {
    const auto original = placement(doc::NodeId{47U}, media ? scene::schema::media_type : scene::schema::model_type);
    auto imported = asset(media ? assets::schema::media_type : assets::schema::model_type);
    cmd::DirectorScheduler scheduler{doc::Root{}.set(original).set(imported)};
    const editor::SourceReplacementIntent request{original.id, imported.id, {}, scene_id, 101U};
    CHECK(app::schedule_source_replacement(scheduler, request, cmd::FrameNumber{1U}));
    CHECK(*scheduler.sealed_document().get(original.id) == original);
    CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
    auto expected = original;
    expected.properties[std::string{scene::schema::source}] = std::string{"T:/replacement.mov"};
    expected.properties[std::string{scene::schema::asset_id}] = core::i64{91};
    if (media) {
      expected.properties[std::string{scene::schema::sequence_start}] = core::i64{0};
      expected.properties[std::string{scene::schema::sequence_count}] = core::i64{637};
      expected.properties[std::string{scene::schema::source_rate_numerator}] = core::i64{30000};
      expected.properties[std::string{scene::schema::source_rate_denominator}] = core::i64{1001};
    }
    CHECK(*scheduler.sealed_document().get(original.id) == expected);
    CHECK(*scheduler.sealed_document().get(imported.id) == imported);
    CHECK(scheduler.undo_depth(cmd::UndoContextId{2U}) == 1U);
    CHECK(scheduler.schedule_undo(cmd::UndoContextId{2U}, cmd::TransactionId{102U}, cmd::FrameNumber{2U}));
    CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
    CHECK(*scheduler.sealed_document().get(original.id) == original);
    CHECK(scheduler.schedule_redo(cmd::UndoContextId{2U}, cmd::TransactionId{103U}, cmd::FrameNumber{3U}));
    CHECK(scheduler.seal_frame(cmd::FrameNumber{3U}));
    CHECK(*scheduler.sealed_document().get(original.id) == expected);
  }
}
void graph_replacement() {
  const auto original = placement(doc::NodeId{47U}, scene::schema::model_type);
  const auto source = placement(doc::NodeId{53U}, scene::schema::model_type);
  doc::Node graph{doc::NodeId{101U}, std::string{compositor::schema::scene_layer_type}, {}};
  graph.properties.emplace(std::string{compositor::schema::scene_node}, core::i64{47});
  graph.properties.emplace(std::string{compositor::schema::editor_x}, 123.75);
  graph.properties.emplace(std::string{compositor::schema::label}, std::string{"Keep my input label"});
  cmd::DirectorScheduler scheduler{doc::Root{}.set(original).set(source).set(graph)};
  CHECK(app::schedule_source_replacement(scheduler, {graph.id, {}, source.id, scene_id, 201U}, cmd::FrameNumber{1U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  auto expected = graph;
  expected.properties[std::string{compositor::schema::scene_node}] = core::i64{53};
  CHECK(*scheduler.sealed_document().get(graph.id) == expected);
  CHECK(*scheduler.sealed_document().get(original.id) == original);
  CHECK(*scheduler.sealed_document().get(source.id) == source);
  CHECK(scheduler.undo_depth(cmd::UndoContextId{6U}) == 1U);
  CHECK(scheduler.schedule_undo(cmd::UndoContextId{6U}, cmd::TransactionId{202U}, cmd::FrameNumber{2U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
  CHECK(*scheduler.sealed_document().get(graph.id) == graph);
}
void still_source_clears_old_cadence() {
  const auto target = placement(doc::NodeId{47U}, scene::schema::media_type);
  auto source = asset(assets::schema::media_type);
  source.properties.erase(std::string{assets::schema::media_rate_numerator});
  source.properties.erase(std::string{assets::schema::media_rate_denominator});
  source.properties.erase(std::string{assets::schema::media_frame_count});
  cmd::DirectorScheduler scheduler{doc::Root{}.set(target).set(source)};
  CHECK(app::schedule_source_replacement(scheduler, {target.id, source.id, {}, scene_id, 401U}, cmd::FrameNumber{1U}));
  CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  const auto &properties = scheduler.sealed_document().get(target.id)->properties;
  CHECK(!properties.contains(scene::schema::source_rate_numerator));
  CHECK(!properties.contains(scene::schema::source_rate_denominator));
  CHECK(std::get<core::i64>(properties.at(std::string{scene::schema::sequence_count})) == 1);
}
void refuse_invalid_graph_replacements() {
  for (int condition = 0; condition < 8; ++condition) {
    auto original = placement(doc::NodeId{47U}, scene::schema::model_type);
    auto source = placement(doc::NodeId{53U}, scene::schema::model_type);
    doc::Node graph{doc::NodeId{101U}, std::string{compositor::schema::scene_layer_type}, {}};
    graph.properties.emplace(std::string{compositor::schema::scene_node}, core::i64{47});
    if (condition == 0) source.type = std::string{scene::schema::media_type};
    if (condition == 1) source.properties[std::string{scene::schema::locked}] = true;
    if (condition == 2) original.properties[std::string{scene::schema::locked}] = true;
    if (condition == 3) graph.type = std::string{compositor::schema::brightness_type};
    if (condition == 4) graph.properties[std::string{compositor::schema::scene_node}] = core::i64{999};
    if (condition == 5) source.properties[std::string{scene::schema::scene_id}] = core::i64{32};
    if (condition == 6) graph.properties[std::string{scene::schema::scene_id}] = core::i64{32};
    if (condition == 7) source.id = original.id;
    auto root = doc::Root{}.set(original).set(graph);
    if (condition != 7) root = root.set(source);
    cmd::DirectorScheduler scheduler{root};
    CHECK(app::schedule_source_replacement(scheduler, {graph.id, {}, source.id, scene_id, 501U}, cmd::FrameNumber{1U}));
    CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
    CHECK(*scheduler.sealed_document().get(graph.id) == graph);
    CHECK(scheduler.undo_depth(cmd::UndoContextId{6U}) == 0U);
  }
}

void refuse_invalid_replacements() {
  for (int condition = 0; condition < 8; ++condition) {
    auto target = placement(doc::NodeId{47U}, scene::schema::model_type);
    auto source = asset(assets::schema::model_type);
    editor::SourceReplacementIntent request{target.id, source.id, {}, scene_id, 301U};
    if (condition == 0) source = asset(assets::schema::media_type);
    if (condition == 1) target.properties[std::string{scene::schema::locked}] = true;
    if (condition == 2) target.properties[std::string{scene::link_schema::instance}] = core::i64{71};
    if (condition == 3) request.target = doc::NodeId{999U};
    if (condition == 4) request.asset = doc::NodeId{999U};
    if (condition == 5) request.layer = doc::NodeId{53U};
    if (condition == 6) source.type = std::string{assets::schema::projection_mesh_type};
    if (condition == 7) request.scene = scene::SceneId{32U};
    const auto root = doc::Root{}.set(target).set(source);
    cmd::DirectorScheduler scheduler{root};
    CHECK(app::schedule_source_replacement(scheduler, request, cmd::FrameNumber{1U}));
    CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
    CHECK(*scheduler.sealed_document().get(target.id) == target);
    CHECK(scheduler.undo_depth(cmd::UndoContextId{2U}) == 0U);
  }
}
} // namespace
int main() {
  try {
    asset_replacement(); graph_replacement(); still_source_clears_old_cadence();
    refuse_invalid_replacements(); refuse_invalid_graph_replacements();
    std::cout << "[pass] replacement source metadata, identity, placement preservation, guards, undo and redo\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "[fail] " << error.what() << '\n'; return 1;
  }
}
