#include "timeline_edit_integration.hpp"
#include "timeline_edit_authoring.hpp"
#include "clip_timing_authoring.hpp"

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/scene/key_edit.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace scene = mediaserver::scene;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define EDIT_CHECK(...)                                                        \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr scene::SceneId scene_id{1U};
constexpr doc::NodeId lane_id{20U};
constexpr scene::MediaFrameRate show_60{60U, 1U};

[[nodiscard]] doc::PropertyValue whole(std::int64_t value) {
  return doc::PropertyValue{value};
}

[[nodiscard]] doc::Node stage() {
  return doc::Node{scene_id, std::string{scene::schema::stage_type}, {}};
}

[[nodiscard]] doc::Node lane(bool locked = false) {
  doc::Node node{};
  node.id = lane_id;
  node.type = std::string{scene::schema::timeline_lane_type};
  node.properties.emplace(std::string{scene::schema::scene_id}, whole(1));
  node.properties.emplace(std::string{scene::schema::lane_order}, whole(0));
  node.properties.emplace(std::string{scene::schema::lane_name},
                          std::string{"Video"});
  node.properties.emplace(std::string{scene::schema::lane_locked}, locked);
  return node;
}

[[nodiscard]] doc::Node media(core::u64 id, core::i64 start, core::i64 duration,
                              core::i64 source_in = 0, core::u64 asset = 501U,
                              bool native_rate = false) {
  doc::Node node{};
  node.id = doc::NodeId{id};
  node.type = std::string{scene::schema::media_type};
  node.properties.emplace(std::string{scene::schema::scene_id}, whole(1));
  node.properties.emplace(std::string{scene::schema::lane_id}, whole(20));
  node.properties.emplace(std::string{scene::schema::clip_order},
                          whole(static_cast<std::int64_t>(id)));
  node.properties.emplace(std::string{scene::schema::clip_start}, whole(start));
  node.properties.emplace(std::string{scene::schema::clip_duration},
                          whole(duration));
  node.properties.emplace(std::string{scene::schema::source_in},
                          whole(source_in));
  node.properties.emplace(std::string{scene::schema::sequence_start}, whole(0));
  node.properties.emplace(std::string{scene::schema::sequence_count},
                          whole(200));
  node.properties.emplace(std::string{scene::schema::asset_id},
                          whole(static_cast<std::int64_t>(asset)));
  node.properties.emplace(std::string{scene::schema::source},
                          std::string{"clip.mov"});
  if (native_rate) {
    node.properties.emplace(std::string{scene::schema::source_rate_numerator},
                            whole(30'000));
    node.properties.emplace(std::string{scene::schema::source_rate_denominator},
                            whole(1'001));
  }
  node.properties.emplace("proxy_identity", std::string{"preserved"});
  return node;
}

[[nodiscard]] doc::Node effect(core::u64 id, core::u64 target) {
  doc::Node node{};
  node.id = doc::NodeId{id};
  node.type = std::string{scene::schema::effect_type};
  node.properties.emplace(std::string{scene::schema::scene_id}, whole(1));
  node.properties.emplace(std::string{scene::schema::effect_target},
                          whole(static_cast<std::int64_t>(target)));
  node.properties.emplace(std::string{scene::schema::effect_kind},
                          std::string{scene::schema::brightness_kind});
  node.properties.emplace(std::string{scene::schema::brightness}, 1.2);
  return node;
}

[[nodiscard]] doc::Node channel(core::u64 id, core::u64 target) {
  doc::Node node{};
  node.id = doc::NodeId{id};
  node.type = std::string{scene::schema::channel_type};
  node.properties.emplace(std::string{scene::schema::scene_id}, whole(1));
  node.properties.emplace(std::string{scene::schema::channel_target},
                          whole(static_cast<std::int64_t>(target)));
  node.properties.emplace(std::string{scene::schema::channel_component},
                          std::string{scene::schema::brightness_kind});
  node.properties.emplace("key0_frame", whole(0));
  node.properties.emplace("key0_value", 1.0);
  node.properties.emplace("key0_interp", std::string{"linear"});
  return node;
}

[[nodiscard]] std::vector<scene::ClipPlacement>
placements(const doc::Root &document) {
  scene::SceneModel model;
  model.reset(document);
  return model.clips(scene_id);
}

[[nodiscard]] std::int64_t integer(const doc::Node &node,
                                   std::string_view property) {
  const auto found = node.properties.find(property);
  EDIT_CHECK(found != node.properties.end());
  const auto *const value = std::get_if<std::int64_t>(&found->second);
  EDIT_CHECK(value != nullptr);
  return *value;
}

[[nodiscard]] const doc::Node *
created_media_at(const app::TimelineEditIntegrationPlan &plan,
                 core::i64 start) {
  const auto found = std::find_if(
      plan.created.begin(), plan.created.end(), [start](const doc::Node &node) {
        if (node.type != scene::schema::media_type) {
          return false;
        }
        const auto held = node.properties.find(scene::schema::clip_start);
        const auto *const value =
            held == node.properties.end()
                ? nullptr
                : std::get_if<std::int64_t>(&held->second);
        return value != nullptr && *value == start;
      });
  return found == plan.created.end() ? nullptr : &*found;
}

void fractional_slip_routes_through_the_pure_planner() {
  doc::Root root;
  root = root.set(stage());
  root = root.set(lane());
  root = root.set(media(10U, 0, 20, 5, 501U, true));

  app::ClipClipboard clipboard;
  const editor::TimelineEditIntent request{
      scene_id, editor::TimelineSlipRequest{doc::NodeId{10U}, 2}, show_60, 0U,
      7U};
  const auto plan = app::plan_timeline_edit_integration(
      root, placements(root), request, clipboard, 100U);
  EDIT_CHECK(plan.has_value());
  EDIT_CHECK(plan.value().timeline.applied_delta == 2);
  const doc::Node *const slipped = plan.value().document.get(doc::NodeId{10U});
  EDIT_CHECK(slipped != nullptr);
  // Two 60 Hz show frames map to one 30000/1001 source ordinal.
  EDIT_CHECK(integer(*slipped, scene::schema::source_in) == 6);
  EDIT_CHECK(integer(*slipped, scene::schema::clip_start) == 0);
  EDIT_CHECK(integer(*slipped, scene::schema::clip_duration) == 20);
}

void negative_placement_and_loop_slip_commit_with_undo_and_redo() {
  auto root = doc::Root{}.set(stage()).set(lane()).set(media(10U, 10, 800, 190));
  const auto normalized = app::normalise_clip_timing(
      scene::ClipTiming{-48, 800, 790, 12, 20}, 200U);
  EDIT_CHECK(normalized.has_value());
  EDIT_CHECK(normalized->start == -48 && normalized->source_in == 190);
  auto moved = placements(root).front();
  moved.timing = *normalized;
  scene::TimelineEditPlan placement{};
  placement.updates.push_back(moved);
  const auto authored = app::plan_timeline_edit_authoring(root, placement, 100U);
  EDIT_CHECK(authored.has_value());
  EDIT_CHECK(integer(*authored.value().document.get(moved.node), scene::schema::clip_start) == -48);
  app::ClipClipboard clipboard;
  const editor::TimelineEditIntent request{
      scene_id, editor::TimelineSlipRequest{moved.node, 27}, show_60, 0U, 91U};
  const auto slipped = app::plan_timeline_edit_integration(authored.value().document,
      placements(authored.value().document), request, clipboard, 100U);
  EDIT_CHECK(slipped.has_value());
  const auto& edited = *slipped.value().document.get(moved.node);
  EDIT_CHECK(integer(edited, scene::schema::clip_start) == -48);
  EDIT_CHECK(integer(edited, scene::schema::clip_duration) == 800);
  EDIT_CHECK(integer(edited, scene::schema::source_in) == 17);
  cmd::DirectorScheduler scheduler{root};
  constexpr cmd::UndoContextId history{2U};
  EDIT_CHECK(scheduler.schedule_document_replace(slipped.value().document, history,
      cmd::TransactionId{91U}, cmd::FrameNumber{1U}));
  EDIT_CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  EDIT_CHECK(*scheduler.sealed_document().get(moved.node) == edited);
  EDIT_CHECK(scheduler.undo_depth(history) == 1U);
  EDIT_CHECK(scheduler.schedule_undo(history, cmd::TransactionId{92U}, cmd::FrameNumber{2U}));
  EDIT_CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
  EDIT_CHECK(doc::diff(root, scheduler.sealed_document()).changes.empty());
  EDIT_CHECK(scheduler.schedule_redo(history, cmd::TransactionId{93U}, cmd::FrameNumber{3U}));
  EDIT_CHECK(scheduler.seal_frame(cmd::FrameNumber{3U}));
  EDIT_CHECK(*scheduler.sealed_document().get(moved.node) == edited);

  const auto before_zero = doc::Root{}.set(stage()).set(lane()).set(media(10U, -40, 100));
  const auto overwrite = scene::plan_overwrite(placements(before_zero),
      scene::TimelineLaneRef{scene_id, lane_id}, -20, 10, show_60, doc::NodeId{100U});
  EDIT_CHECK(overwrite.has_value());
  const auto split_authored = app::plan_timeline_edit_authoring(before_zero, overwrite.value(), 100U);
  EDIT_CHECK(split_authored.has_value());
  EDIT_CHECK(integer(*split_authored.value().document.get(doc::NodeId{10U}), scene::schema::clip_start) == -40);
  EDIT_CHECK(integer(*split_authored.value().document.get(doc::NodeId{100U}), scene::schema::clip_start) == -10);

  EDIT_CHECK(!app::normalise_clip_timing(scene::ClipTiming{
      std::numeric_limits<core::i64>::max(), 1, 0, 0, 0}, 200U));
  EDIT_CHECK(!app::split_clip_timing(scene::ClipTiming{
      std::numeric_limits<core::i64>::max() - 1, 8, 0, 0, 0},
      std::numeric_limits<core::i64>::max(), 200U, show_60, std::nullopt));
}

void razor_uses_native_floor_at_cut_and_wraps_looping_sources() {
  const scene::ClipTiming original{-1, 600, 199, 1, 8};
  const auto cut = app::split_clip_timing(original, 2, 200U, show_60,
      scene::MediaFrameRate{30'000U, 1'001U});
  EDIT_CHECK(cut.has_value());
  // Three show frames are 1.4985 source frames: playback floors to one,
  // rather than rounding to two or advancing by three show frames.
  EDIT_CHECK(cut->left.start == -1 && cut->left.duration == 3);
  EDIT_CHECK(cut->right.start == 2 && cut->right.source_in == 0);
  EDIT_CHECK(cut->left.fade_out == 0 && cut->right.fade_in == 0);
  EDIT_CHECK(cut->right.fade_out == 8);
  scene::SceneModel before;
  before.reset(doc::Root{}.set(stage()).set(lane()).set(media(10U, -1, 600, 199, 501U, true)));
  scene::SceneModel after;
  after.reset(doc::Root{}.set(stage()).set(lane()).set(media(11U,
      cut->right.start, cut->right.duration, cut->right.source_in, 501U, true)));
  const scene::SceneTime time{2U, 60U, 1U};
  EDIT_CHECK(before.evaluate(scene_id, time).media.front().frame_index == 0U);
  EDIT_CHECK(after.evaluate(scene_id, time).media.front().frame_index == 0U);

  const auto negative = app::split_clip_timing(
      scene::ClipTiming{-20, 620, 190, 0, 0}, -5, 200U, show_60, std::nullopt);
  EDIT_CHECK(negative.has_value());
  EDIT_CHECK(negative->right.start == -5 && negative->right.source_in == 5);
  before.reset(doc::Root{}.set(stage()).set(lane()).set(media(10U, -20, 620, 190)));
  after.reset(doc::Root{}.set(stage()).set(lane()).set(media(11U,
      negative->right.start, negative->right.duration, negative->right.source_in)));
  for (core::u64 frame = 0U; frame < 600U; ++frame) {
    const scene::SceneTime sample{frame, 60U, 1U};
    const auto expected = (frame + 210U) % 200U;
    EDIT_CHECK(before.evaluate(scene_id, sample).media.front().frame_index == expected);
    EDIT_CHECK(after.evaluate(scene_id, sample).media.front().frame_index == expected);
  }
}

void insert_is_one_replacement_with_lane_mapped_clipboard_subgraph() {
  doc::Root root;
  root = root.set(stage());
  root = root.set(lane());
  root = root.set(media(10U, 0, 5, 0, 501U, true));
  root = root.set(media(11U, 5, 5, 0, 777U));
  root = root.set(effect(30U, 10U));
  root = root.set(channel(40U, 30U));

  app::ClipClipboard clipboard;
  constexpr std::array copied{doc::NodeId{10U}};
  EDIT_CHECK(clipboard.copy(root, copied));
  EDIT_CHECK(clipboard.span_duration() == 5);
  EDIT_CHECK(clipboard.single_source_lane());

  const editor::TimelineEditIntent request{
      scene_id, editor::TimelineInsertRequest{lane_id}, show_60, 5U, 11U};
  EDIT_CHECK(std::get<editor::TimelineInsertRequest>(request.request).lane ==
             lane_id);
  EDIT_CHECK(root.get(lane_id) != nullptr);
  EDIT_CHECK(root.get(lane_id)->type == scene::schema::timeline_lane_type);
  EDIT_CHECK(integer(*root.get(lane_id), scene::schema::scene_id) == 1);
  const auto planned = app::plan_timeline_edit_integration(
      root, placements(root), request, clipboard, 100U);
  if (!planned) {
    throw std::runtime_error{
        "insert integration error " +
        std::to_string(static_cast<unsigned>(planned.error()))};
  }
  const auto &plan = planned.value();
  EDIT_CHECK(plan.timeline.applied_delta == 5);
  EDIT_CHECK(plan.timeline.span_start == 5);
  EDIT_CHECK(plan.timeline.span_duration == 5);
  EDIT_CHECK(integer(*plan.document.get(doc::NodeId{11U}),
                     scene::schema::clip_start) == 10);

  const doc::Node *const pasted = created_media_at(plan, 5);
  EDIT_CHECK(pasted != nullptr);
  EDIT_CHECK(integer(*pasted, scene::schema::asset_id) == 501);
  EDIT_CHECK(integer(*pasted, scene::schema::lane_id) == 20);
  EDIT_CHECK(integer(*pasted, scene::schema::source_rate_numerator) == 30'000);
  const auto proxy = pasted->properties.find("proxy_identity");
  EDIT_CHECK(proxy != pasted->properties.end());
  EDIT_CHECK(std::get<std::string>(proxy->second) == "preserved");

  const auto cloned_effect =
      std::find_if(plan.created.begin(), plan.created.end(),
                   [pasted](const doc::Node &node) {
                     return node.type == scene::schema::effect_type &&
                            integer(node, scene::schema::effect_target) ==
                                static_cast<std::int64_t>(pasted->id.value());
                   });
  EDIT_CHECK(cloned_effect != plan.created.end());
  const auto cloned_channel = std::find_if(
      plan.created.begin(), plan.created.end(), [&](const doc::Node &node) {
        return node.type == scene::schema::channel_type &&
               integer(node, scene::schema::channel_target) ==
                   static_cast<std::int64_t>(cloned_effect->id.value());
      });
  EDIT_CHECK(cloned_channel != plan.created.end());

  cmd::DirectorScheduler scheduler{root};
  auto scheduled = scheduler.schedule_document_replace(
      plan.document, cmd::UndoContextId{2U},
      cmd::TransactionId{request.transaction}, cmd::FrameNumber{1U});
  EDIT_CHECK(scheduled.has_value());
  EDIT_CHECK(scheduler.undo_depth(cmd::UndoContextId{2U}) == 1U);
  auto undone = scheduler.schedule_undo(
      cmd::UndoContextId{2U}, cmd::TransactionId{12U}, cmd::FrameNumber{1U});
  EDIT_CHECK(undone.has_value());
  EDIT_CHECK(doc::diff(scheduler.projected_document(), root).changes.empty());
  auto redone = scheduler.schedule_redo(
      cmd::UndoContextId{2U}, cmd::TransactionId{13U}, cmd::FrameNumber{1U});
  EDIT_CHECK(redone.has_value());
  EDIT_CHECK(
      doc::diff(scheduler.projected_document(), plan.document).changes.empty());
}

void overwrite_preserves_both_old_remainders_and_adds_the_paste_atomically() {
  doc::Root root;
  root = root.set(stage());
  root = root.set(lane());
  root = root.set(media(10U, 0, 5, 0, 501U));
  root = root.set(media(11U, 4, 12, 20, 777U, true));
  root = root.set(effect(30U, 11U));
  root = root.set(channel(40U, 30U));

  app::ClipClipboard clipboard;
  constexpr std::array copied{doc::NodeId{10U}};
  EDIT_CHECK(clipboard.copy(root, copied));
  const editor::TimelineEditIntent request{
      scene_id, editor::TimelineOverwriteRequest{lane_id}, show_60, 5U, 21U};
  const auto planned = app::plan_timeline_edit_integration(
      root, placements(root), request, clipboard, 100U);
  if (!planned) {
    throw std::runtime_error{
        "overwrite integration error " +
        std::to_string(static_cast<unsigned>(planned.error()))};
  }
  const auto &plan = planned.value();
  EDIT_CHECK(plan.timeline.applied_delta == 0);
  EDIT_CHECK(integer(*plan.document.get(doc::NodeId{11U}),
                     scene::schema::clip_start) == 4);
  EDIT_CHECK(integer(*plan.document.get(doc::NodeId{11U}),
                     scene::schema::clip_duration) == 1);

  const doc::Node *const pasted = created_media_at(plan, 5);
  const doc::Node *const right = created_media_at(plan, 10);
  EDIT_CHECK(pasted != nullptr);
  EDIT_CHECK(right != nullptr);
  EDIT_CHECK(integer(*pasted, scene::schema::asset_id) == 501);
  EDIT_CHECK(integer(*right, scene::schema::asset_id) == 777);
  EDIT_CHECK(integer(*right, scene::schema::clip_duration) == 6);
  // Native-rate split: six show frames advance three source ordinals.
  EDIT_CHECK(integer(*right, scene::schema::source_in) == 23);

  cmd::DirectorScheduler scheduler{root};
  auto scheduled = scheduler.schedule_document_replace(
      plan.document, cmd::UndoContextId{2U},
      cmd::TransactionId{request.transaction}, cmd::FrameNumber{1U});
  EDIT_CHECK(scheduled.has_value());
  EDIT_CHECK(scheduler.undo_depth(cmd::UndoContextId{2U}) == 1U);
}

void locked_or_ambiguous_clipboard_targets_fail_without_a_document() {
  doc::Root locked;
  locked = locked.set(stage());
  locked = locked.set(lane(true));
  locked = locked.set(media(10U, 0, 5));
  app::ClipClipboard clipboard;
  constexpr std::array copied{doc::NodeId{10U}};
  EDIT_CHECK(clipboard.copy(locked, copied));
  const editor::TimelineEditIntent request{
      scene_id, editor::TimelineInsertRequest{lane_id}, show_60, 5U, 31U};
  const auto rejected = app::plan_timeline_edit_integration(
      locked, placements(locked), request, clipboard, 100U);
  EDIT_CHECK(!rejected.has_value());
  EDIT_CHECK(rejected.error() ==
             app::TimelineEditIntegrationError::TargetLaneUnavailable);

  app::ClipClipboard empty;
  const auto no_clipboard = app::plan_timeline_edit_integration(
      locked, placements(locked), request, empty, 100U);
  EDIT_CHECK(!no_clipboard.has_value());
  EDIT_CHECK(no_clipboard.error() ==
             app::TimelineEditIntegrationError::ClipboardIncompatible);

  doc::Root unlocked;
  unlocked = unlocked.set(stage());
  unlocked = unlocked.set(lane());
  unlocked = unlocked.set(media(10U, 0, 5));
  app::ClipClipboard full;
  EDIT_CHECK(full.copy(unlocked, copied));
  const auto exhausted = app::plan_timeline_edit_integration(
      unlocked, placements(unlocked), request, full,
      std::numeric_limits<core::u64>::max());
  EDIT_CHECK(!exhausted.has_value());
  EDIT_CHECK(exhausted.error() ==
             app::TimelineEditIntegrationError::PasteRejected);
}

void key_scale_replacement_is_one_scene_undo_transaction() {
  doc::Root root;
  root = root.set(stage());
  root = root.set(lane());
  root = root.set(media(10U, 0, 20));
  root = root.set(effect(30U, 10U));
  doc::Node authored = channel(40U, 30U);
  authored.properties["key0_frame"] = whole(10);
  authored.properties["key0_value"] = 1.0;
  authored.properties["key1_frame"] = whole(20);
  authored.properties["key1_value"] = 2.0;
  authored.properties["key1_interp"] = std::string{"cubic"};
  root = root.set(std::move(authored));

  constexpr std::array selection{
      scene::KeyAddress{doc::NodeId{30U}, scene::PropertyChannel::Brightness,
                        10},
      scene::KeyAddress{doc::NodeId{30U}, scene::PropertyChannel::Brightness,
                        20},
  };
  const auto planned = scene::plan_key_scale(
      root, selection,
      scene::KeyTimeScale{2, 1, scene::KeyTimeAnchor::FirstSelected, 0, 0});
  EDIT_CHECK(planned.has_value());
  EDIT_CHECK(planned.value().report.applied == 2U);
  EDIT_CHECK(planned.value().report.collisions.empty());
  EDIT_CHECK(planned.value().selection_after.size() == 2U);
  EDIT_CHECK(planned.value().selection_after[0].address.frame == 10);
  EDIT_CHECK(planned.value().selection_after[1].address.frame == 30);

  cmd::DirectorScheduler scheduler{root};
  constexpr cmd::UndoContextId scene_history{2U};
  auto scheduled = scheduler.schedule_document_replace(
      planned.value().after, scene_history, cmd::TransactionId{71U},
      cmd::FrameNumber{1U});
  EDIT_CHECK(scheduled.has_value());
  EDIT_CHECK(scheduler.undo_depth(scene_history) == 1U);
  const doc::Node *const changed =
      scheduler.projected_document().get(doc::NodeId{40U});
  EDIT_CHECK(changed != nullptr);
  EDIT_CHECK(integer(*changed, "key0_frame") == 10);
  EDIT_CHECK(integer(*changed, "key1_frame") == 30);

  auto undone = scheduler.schedule_undo(scene_history, cmd::TransactionId{72U},
                                        cmd::FrameNumber{1U});
  EDIT_CHECK(undone.has_value());
  EDIT_CHECK(doc::diff(scheduler.projected_document(), root).changes.empty());
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 7U> cases{{
    {"negative placement and loop Slip commit with undo and redo",
     &negative_placement_and_loop_slip_commit_with_undo_and_redo},
    {"Razor uses playback rounding and wraps looping sources",
     &razor_uses_native_floor_at_cut_and_wraps_looping_sources},
    {"fractional slip routes through the pure planner",
     &fractional_slip_routes_through_the_pure_planner},
    {"insert is one replacement with lane mapped clipboard subgraph",
     &insert_is_one_replacement_with_lane_mapped_clipboard_subgraph},
    {"overwrite preserves both old remainders and adds paste atomically",
     &overwrite_preserves_both_old_remainders_and_adds_the_paste_atomically},
    {"locked or ambiguous clipboard targets fail without a document",
     &locked_or_ambiguous_clipboard_targets_fail_without_a_document},
    {"key scale replacement is one scene undo transaction",
     &key_scale_replacement_is_one_scene_undo_transaction},
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
