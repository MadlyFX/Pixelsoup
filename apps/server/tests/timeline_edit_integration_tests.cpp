#include "timeline_edit_integration.hpp"
#include "timeline_edit_authoring.hpp"
#include "clip_timing_authoring.hpp"
#include "source_range_edit.hpp"

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
namespace assets = mediaserver::assets;
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

// A second track keeps time with a ripple on the first while it is sync
// locked, and is left alone again once it is also locked: lock wins.
void a_sync_locked_track_keeps_time_with_a_ripple_unless_it_is_locked() {
  doc::Node second = lane();
  second.id = doc::NodeId{21U};
  second.properties.insert_or_assign(std::string{scene::schema::lane_order},
                                     whole(10));
  second.properties.insert_or_assign(
      std::string{scene::schema::lane_sync_locked}, true);
  doc::Node follower = media(12U, 20, 10);
  follower.properties.insert_or_assign(std::string{scene::schema::lane_id},
                                       whole(21));
  doc::Root root;
  root = root.set(stage());
  root = root.set(lane());
  root = root.set(second);
  root = root.set(media(10U, 0, 20));
  root = root.set(media(11U, 20, 10));
  root = root.set(follower);

  app::ClipClipboard clipboard;
  const editor::TimelineEditIntent request{
      scene_id,
      editor::TimelineRippleTrimRequest{
          doc::NodeId{10U}, scene::TimelineTrimEdge::Tail, 5},
      show_60, 0U, 7U};
  const auto synced = app::plan_timeline_edit_integration(
      root, placements(root), request, clipboard, 100U);
  EDIT_CHECK(synced.has_value());
  EDIT_CHECK(integer(*synced.value().document.get(doc::NodeId{11U}),
                     scene::schema::clip_start) == 25);
  EDIT_CHECK(integer(*synced.value().document.get(doc::NodeId{12U}),
                     scene::schema::clip_start) == 25);

  second.properties.insert_or_assign(std::string{scene::schema::lane_locked},
                                     true);
  root = root.set(second);
  const auto held = app::plan_timeline_edit_integration(
      root, placements(root), request, clipboard, 100U);
  EDIT_CHECK(held.has_value());
  EDIT_CHECK(integer(*held.value().document.get(doc::NodeId{11U}),
                     scene::schema::clip_start) == 25);
  EDIT_CHECK(integer(*held.value().document.get(doc::NodeId{12U}),
                     scene::schema::clip_start) == 20);
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

void overwrite_lays_the_paste_over_the_track_and_cuts_nothing() {
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
  EDIT_CHECK(plan.timeline.span_start == 5);
  EDIT_CHECK(plan.timeline.span_duration == 5);
  // Clip 11 runs under the paste and keeps all of itself, its effect and its
  // key channel; the track still ends at 16.
  for (const core::u64 kept : {10U, 11U, 30U, 40U}) {
    EDIT_CHECK(*plan.document.get(doc::NodeId{kept}) ==
               *root.get(doc::NodeId{kept}));
  }
  EDIT_CHECK(plan.changed.empty() && plan.erased.empty());
  EDIT_CHECK(app::source_append_frame(placements(plan.document), scene_id,
                                      lane_id) == 16U);
  // The paste is the only new clip, on the track and in front of clips 10
  // and 11 (orders 10 and 11).
  EDIT_CHECK(plan.created.size() == 1U);
  const doc::Node *const pasted = created_media_at(plan, 5);
  EDIT_CHECK(pasted != nullptr);
  EDIT_CHECK(integer(*pasted, scene::schema::asset_id) == 501);
  EDIT_CHECK(integer(*pasted, scene::schema::lane_id) == 20);
  EDIT_CHECK(integer(*pasted, scene::schema::clip_order) == 12);

  cmd::DirectorScheduler scheduler{root};
  auto scheduled = scheduler.schedule_document_replace(
      plan.document, cmd::UndoContextId{2U},
      cmd::TransactionId{request.transaction}, cmd::FrameNumber{1U});
  EDIT_CHECK(scheduled.has_value());
  EDIT_CHECK(scheduler.undo_depth(cmd::UndoContextId{2U}) == 1U);

  // Several pasted clips keep their own stacking - here clip 10 in front of
  // clip 11, against their id order - above everything on the track.
  doc::Node front = media(10U, 0, 5, 0, 501U);
  front.properties.insert_or_assign(std::string{scene::schema::clip_order},
                                    whole(30));
  const auto stacked = root.set(front);
  app::ClipClipboard both;
  constexpr std::array pair{doc::NodeId{10U}, doc::NodeId{11U}};
  EDIT_CHECK(both.copy(stacked, pair));
  const editor::TimelineEditIntent twice{
      scene_id, editor::TimelineOverwriteRequest{lane_id}, show_60, 2U, 22U};
  const auto layered = app::plan_timeline_edit_integration(
      stacked, placements(stacked), twice, both, 100U);
  EDIT_CHECK(layered.has_value());
  const doc::Node *const first = created_media_at(layered.value(), 2);
  const doc::Node *const second = created_media_at(layered.value(), 6);
  EDIT_CHECK(first != nullptr && second != nullptr);
  EDIT_CHECK(integer(*first, scene::schema::asset_id) == 501);
  EDIT_CHECK(integer(*second, scene::schema::asset_id) == 777);
  EDIT_CHECK(integer(*second, scene::schema::clip_order) == 31);
  EDIT_CHECK(integer(*first, scene::schema::clip_order) == 32);
  // The pasted effect follows the pasted clip 11.
  EDIT_CHECK(std::any_of(
      layered.value().created.begin(), layered.value().created.end(),
      [&](const doc::Node &node) {
        return node.type == scene::schema::effect_type &&
               integer(node, scene::schema::effect_target) ==
                   static_cast<std::int64_t>(second->id.value());
      }));
  for (const core::u64 kept : {10U, 11U, 30U, 40U}) {
    EDIT_CHECK(*layered.value().document.get(doc::NodeId{kept}) ==
               *stacked.get(doc::NodeId{kept}));
  }
}

// With no track chosen the editor aims a paste outside every track: the
// pasted clip leaves its track, and only the clips filed under no track make
// room for it or stack under it.
void a_paste_outside_every_track_leaves_the_tracks_alone() {
  const auto filed_under_no_track = [](core::u64 id, core::i64 start,
                                       core::i64 duration, std::int64_t order) {
    doc::Node node = media(id, start, duration, 0, 900U + id);
    node.properties.erase(std::string{scene::schema::lane_id});
    node.properties.erase(std::string{scene::schema::clip_order});
    node.properties.emplace(std::string{scene::schema::group_order},
                            whole(order));
    return node;
  };
  doc::Root root;
  root = root.set(stage());
  root = root.set(lane());
  root = root.set(media(10U, 0, 5, 0, 501U));
  root = root.set(media(11U, 6, 9, 0, 777U));
  root = root.set(filed_under_no_track(12U, 8, 12, 4));
  root = root.set(filed_under_no_track(13U, 0, 3, 9));

  app::ClipClipboard clipboard;
  constexpr std::array copied{doc::NodeId{10U}};
  EDIT_CHECK(clipboard.copy(root, copied));

  // Insert moves the later untracked clip, 12, along by the paste and leaves
  // clip 11 on the track where it is, though it too starts after the playhead.
  const editor::TimelineEditIntent insert{
      scene_id, editor::TimelineInsertRequest{doc::NodeId{}}, show_60, 6U, 51U};
  const auto inserted = app::plan_timeline_edit_integration(
      root, placements(root), insert, clipboard, 100U);
  EDIT_CHECK(inserted.has_value());
  EDIT_CHECK(integer(*inserted.value().document.get(doc::NodeId{12U}),
                     scene::schema::clip_start) == 13);
  for (const core::u64 kept : {10U, 11U, 13U}) {
    EDIT_CHECK(*inserted.value().document.get(doc::NodeId{kept}) ==
               *root.get(doc::NodeId{kept}));
  }
  const doc::Node *const put = created_media_at(inserted.value(), 6);
  EDIT_CHECK(put != nullptr);
  EDIT_CHECK(integer(*put, scene::schema::asset_id) == 501);
  EDIT_CHECK(!put->properties.contains(scene::schema::lane_id));

  // Overwrite moves nothing and stacks the paste in front of the untracked
  // clips (group orders 4 and 9), whatever the track's clips are.
  const editor::TimelineEditIntent overwrite{
      scene_id, editor::TimelineOverwriteRequest{doc::NodeId{}}, show_60, 6U,
      52U};
  const auto laid = app::plan_timeline_edit_integration(
      root, placements(root), overwrite, clipboard, 100U);
  EDIT_CHECK(laid.has_value());
  EDIT_CHECK(laid.value().changed.empty() && laid.value().erased.empty());
  for (const core::u64 kept : {10U, 11U, 12U, 13U}) {
    EDIT_CHECK(*laid.value().document.get(doc::NodeId{kept}) ==
               *root.get(doc::NodeId{kept}));
  }
  const doc::Node *const over = created_media_at(laid.value(), 6);
  EDIT_CHECK(over != nullptr);
  EDIT_CHECK(!over->properties.contains(scene::schema::lane_id));
  EDIT_CHECK(integer(*over, scene::schema::group_order) == 10);
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

// ----------------------------------------------------------- Asset Viewer

// A 200-frame 24 fps source, cut into a 25 fps show.
constexpr scene::MediaFrameRate show_25{25U, 1U};

[[nodiscard]] assets::MediaAsset viewer_source() {
  assets::MediaAsset asset{};
  asset.id = doc::NodeId{501U};
  asset.display_name = "Plate";
  asset.source = "clip.mov";
  asset.frame_count = 200U;
  asset.frame_rate = assets::MediaFrameRate{24U, 1U};
  asset.dimensions = assets::PixelDimensions{1920U, 1080U};
  return asset;
}

// What a Library add of `asset` authors: the whole source from frame zero.
[[nodiscard]] doc::Node whole_source(const assets::MediaAsset &asset,
                                     core::i64 duration = 209) {
  doc::Node node{};
  node.id = doc::NodeId{1U};
  node.type = std::string{scene::schema::media_type};
  node.properties.emplace(std::string{scene::schema::source}, asset.source);
  node.properties.emplace(std::string{scene::schema::sequence_start}, whole(0));
  node.properties.emplace(
      std::string{scene::schema::sequence_count},
      whole(static_cast<std::int64_t>(asset.frame_count.value_or(1U))));
  if (asset.frame_rate.has_value()) {
    node.properties.emplace(
        std::string{scene::schema::source_rate_numerator},
        whole(static_cast<std::int64_t>(asset.frame_rate->numerator)));
    node.properties.emplace(
        std::string{scene::schema::source_rate_denominator},
        whole(static_cast<std::int64_t>(asset.frame_rate->denominator)));
  }
  node.properties.emplace(std::string{scene::schema::clip_start}, whole(0));
  node.properties.emplace(std::string{scene::schema::clip_duration},
                          whole(duration));
  node.properties.emplace(std::string{scene::schema::scene_id}, whole(1));
  node.properties.emplace(
      std::string{scene::schema::asset_id},
      whole(static_cast<std::int64_t>(asset.id.value())));
  return node;
}

[[nodiscard]] editor::SourceRangeEditIntent
viewer_range(editor::SourceEditMode mode, core::u64 source_in,
             core::u64 frames, core::u64 start) {
  editor::SourceRangeEditIntent request{};
  request.scene = scene_id;
  request.asset = doc::NodeId{501U};
  request.source_in = source_in;
  request.source_frames = frames;
  request.mode = mode;
  request.lane = lane_id;
  request.start = start;
  request.show_rate = show_25;
  request.transaction = 77U;
  return request;
}

void a_viewer_insert_places_the_exact_range_and_moves_later_clips() {
  const auto root = doc::Root{}
                        .set(stage())
                        .set(lane())
                        .set(media(10U, 0, 20))
                        .set(media(11U, 20, 10));
  const assets::MediaAsset asset = viewer_source();
  // Source frames 48..71: one second of 24 fps, which plays for exactly 25
  // frames of a 25 fps show.
  const auto plan = app::plan_source_range_edit(
      root, placements(root), asset,
      viewer_range(editor::SourceEditMode::Insert, 48U, 24U, 20U),
      whole_source(asset), 300U);
  EDIT_CHECK(plan.has_value());
  const doc::Node *const cut = created_media_at(plan.value(), 20);
  EDIT_CHECK(cut != nullptr);
  EDIT_CHECK(cut->id.value() >= 300U);
  EDIT_CHECK(integer(*cut, scene::schema::clip_duration) == 25);
  EDIT_CHECK(integer(*cut, scene::schema::source_in) == 48);
  EDIT_CHECK(integer(*cut, scene::schema::lane_id) == 20);
  EDIT_CHECK(integer(*cut, scene::schema::asset_id) == 501);
  EDIT_CHECK(integer(*cut, scene::schema::scene_id) == 1);
  // The clip already at the edit point moves along by the cut's length; the
  // one before it stays.
  EDIT_CHECK(integer(*plan.value().document.get(doc::NodeId{11U}),
                     scene::schema::clip_start) == 45);
  EDIT_CHECK(integer(*plan.value().document.get(doc::NodeId{10U}),
                     scene::schema::clip_start) == 0);
  // The first and last show frames play the first and last marked frames.
  scene::SceneModel model;
  model.reset(plan.value().document);
  const auto clips = model.clips(scene_id);
  const auto placed = std::find_if(
      clips.begin(), clips.end(),
      [&](const scene::ClipPlacement &clip) { return clip.node == cut->id; });
  EDIT_CHECK(placed != clips.end());
  const scene::SceneTime time{0U, 25U, 1U};
  EDIT_CHECK(scene::clip_source_frame(*placed, time, 20U) == 48U);
  EDIT_CHECK(scene::clip_source_frame(*placed, time, 44U) == 71U);
  EDIT_CHECK(placed->timing.last_frame() == 44);
}

void a_viewer_insert_inside_a_clip_cuts_it_and_moves_its_second_half() {
  const auto root =
      doc::Root{}.set(stage()).set(lane()).set(media(10U, 0, 100, 7));
  const assets::MediaAsset asset = viewer_source();
  const auto plan = app::plan_source_range_edit(
      root, placements(root), asset,
      viewer_range(editor::SourceEditMode::Insert, 0U, 24U, 40U),
      whole_source(asset), 300U);
  EDIT_CHECK(plan.has_value());
  // The first half keeps its place and its id, cut at the edit point.
  const doc::Node *const head = plan.value().document.get(doc::NodeId{10U});
  EDIT_CHECK(head != nullptr);
  EDIT_CHECK(integer(*head, scene::schema::clip_start) == 0);
  EDIT_CHECK(integer(*head, scene::schema::clip_duration) == 40);
  // The new clip plays from the edit point for 25 frames...
  const doc::Node *const cut = created_media_at(plan.value(), 40);
  EDIT_CHECK(cut != nullptr);
  EDIT_CHECK(integer(*cut, scene::schema::clip_duration) == 25);
  // ...and the second half follows it, still showing the source frames it
  // showed before: 7 + 40 onwards, at the image sequence's one-to-one rate.
  scene::SceneModel model;
  model.reset(plan.value().document);
  const auto clips = model.clips(scene_id);
  const auto tail = std::find_if(
      clips.begin(), clips.end(), [](const scene::ClipPlacement &clip) {
        return clip.timing.start == 65;
      });
  EDIT_CHECK(tail != clips.end());
  EDIT_CHECK(tail->node != doc::NodeId{10U} && tail->node != cut->id);
  EDIT_CHECK(tail->timing.duration == 60);
  EDIT_CHECK(tail->timing.source_in == 47);
  // The whole edit is one document replacement.
  EDIT_CHECK(clips.size() == 3U);
}

void a_viewer_overwrite_lays_the_range_over_the_track_and_keeps_its_length() {
  const auto root = doc::Root{}
                        .set(stage())
                        .set(lane())
                        .set(media(10U, 0, 100, 3))
                        .set(media(11U, 100, 40));
  const assets::MediaAsset asset = viewer_source();
  const auto plan = app::plan_source_range_edit(
      root, placements(root), asset,
      viewer_range(editor::SourceEditMode::Overwrite, 36U, 24U, 10U),
      whole_source(asset), 300U);
  EDIT_CHECK(plan.has_value());
  // The clips already on the track are neither cut nor moved: the track
  // still ends at 140.
  EDIT_CHECK(*plan.value().document.get(doc::NodeId{10U}) ==
             *root.get(doc::NodeId{10U}));
  EDIT_CHECK(*plan.value().document.get(doc::NodeId{11U}) ==
             *root.get(doc::NodeId{11U}));
  EDIT_CHECK(app::source_append_frame(placements(plan.value().document),
                                      scene_id, lane_id) == 140U);
  // The range is the one new clip, on the track at the playhead and in
  // front of both clips there (their orders are 10 and 11).
  EDIT_CHECK(plan.value().created.size() == 1U);
  const doc::Node *const cut = created_media_at(plan.value(), 10);
  EDIT_CHECK(cut != nullptr);
  EDIT_CHECK(integer(*cut, scene::schema::clip_duration) == 25);
  EDIT_CHECK(integer(*cut, scene::schema::source_in) == 36);
  EDIT_CHECK(integer(*cut, scene::schema::lane_id) == 20);
  EDIT_CHECK(integer(*cut, scene::schema::clip_order) == 12);
  EDIT_CHECK(placements(plan.value().document).size() == 3U);

  // Clips filed under no track stack by group order instead.
  doc::Node loose = media(12U, 0, 50);
  loose.properties.erase(std::string{scene::schema::lane_id});
  loose.properties.erase(std::string{scene::schema::clip_order});
  loose.properties.emplace(std::string{scene::schema::group_order}, whole(6));
  const auto untracked_root = doc::Root{}.set(stage()).set(loose);
  editor::SourceRangeEditIntent untracked =
      viewer_range(editor::SourceEditMode::Overwrite, 0U, 24U, 5U);
  untracked.lane = doc::NodeId{};
  const auto over_loose = app::plan_source_range_edit(
      untracked_root, placements(untracked_root), asset, untracked,
      whole_source(asset), 300U);
  EDIT_CHECK(over_loose.has_value());
  EDIT_CHECK(*over_loose.value().document.get(doc::NodeId{12U}) == loose);
  const doc::Node *const laid = created_media_at(over_loose.value(), 5);
  EDIT_CHECK(laid != nullptr);
  EDIT_CHECK(!laid->properties.contains(scene::schema::lane_id));
  EDIT_CHECK(integer(*laid, scene::schema::group_order) == 7);
}

void a_viewer_append_lands_after_the_last_clip_and_moves_nothing() {
  const auto root = doc::Root{}
                        .set(stage())
                        .set(lane())
                        .set(media(10U, 0, 20))
                        .set(media(11U, 20, 10));
  EDIT_CHECK(app::source_append_frame(placements(root), scene_id, lane_id) ==
             30U);
  const assets::MediaAsset asset = viewer_source();
  // The request's start is ignored: Append goes after the track's last clip.
  const auto plan = app::plan_source_range_edit(
      root, placements(root), asset,
      viewer_range(editor::SourceEditMode::Append, 12U, 48U, 3U),
      whole_source(asset), 300U);
  EDIT_CHECK(plan.has_value());
  const doc::Node *const cut = created_media_at(plan.value(), 30);
  EDIT_CHECK(cut != nullptr);
  EDIT_CHECK(integer(*cut, scene::schema::clip_duration) == 50);
  EDIT_CHECK(integer(*cut, scene::schema::source_in) == 12);
  EDIT_CHECK(*plan.value().document.get(doc::NodeId{10U}) ==
             *root.get(doc::NodeId{10U}));
  EDIT_CHECK(*plan.value().document.get(doc::NodeId{11U}) ==
             *root.get(doc::NodeId{11U}));
}

void a_viewer_still_keeps_its_placed_duration() {
  const auto root = doc::Root{}.set(stage()).set(lane());
  assets::MediaAsset still = viewer_source();
  still.frame_count = 1U;
  still.frame_rate.reset();
  const auto plan = app::plan_source_range_edit(
      root, placements(root), still,
      viewer_range(editor::SourceEditMode::Insert, 0U, 1U, 7U),
      whole_source(still, 125), 300U);
  EDIT_CHECK(plan.has_value());
  const doc::Node *const cut = created_media_at(plan.value(), 7);
  EDIT_CHECK(cut != nullptr);
  EDIT_CHECK(integer(*cut, scene::schema::clip_duration) == 125);
}

void a_viewer_range_is_refused_with_its_reason_and_nothing_changes() {
  const auto root =
      doc::Root{}.set(stage()).set(lane()).set(media(10U, 0, 20));
  assets::MediaAsset unprobed = viewer_source();
  unprobed.frame_count.reset();
  const auto not_probed = app::plan_source_range_edit(
      root, placements(root), unprobed,
      viewer_range(editor::SourceEditMode::Insert, 0U, 10U, 0U),
      whole_source(unprobed), 300U);
  EDIT_CHECK(!not_probed);
  EDIT_CHECK(not_probed.error() == app::SourceRangeEditError::NotProbed);

  const assets::MediaAsset asset = viewer_source();
  const auto past_end = app::plan_source_range_edit(
      root, placements(root), asset,
      viewer_range(editor::SourceEditMode::Insert, 190U, 11U, 0U),
      whole_source(asset), 300U);
  EDIT_CHECK(!past_end);
  EDIT_CHECK(past_end.error() ==
             app::SourceRangeEditError::RangeOutsideSource);
  const auto empty = app::plan_source_range_edit(
      root, placements(root), asset,
      viewer_range(editor::SourceEditMode::Insert, 4U, 0U, 0U),
      whole_source(asset), 300U);
  EDIT_CHECK(!empty);
  EDIT_CHECK(empty.error() == app::SourceRangeEditError::RangeOutsideSource);

  const auto locked_root =
      doc::Root{}.set(stage()).set(lane(true)).set(media(10U, 0, 20));
  const auto locked = app::plan_source_range_edit(
      locked_root, placements(locked_root), asset,
      viewer_range(editor::SourceEditMode::Overwrite, 0U, 10U, 0U),
      whole_source(asset), 300U);
  EDIT_CHECK(!locked);
  EDIT_CHECK(locked.error() ==
             app::SourceRangeEditError::TargetLaneUnavailable);
}

void a_viewer_cut_is_one_undo_step() {
  const auto root = doc::Root{}
                        .set(stage())
                        .set(lane())
                        .set(media(10U, 0, 20))
                        .set(media(11U, 20, 10));
  const assets::MediaAsset asset = viewer_source();
  const auto plan = app::plan_source_range_edit(
      root, placements(root), asset,
      viewer_range(editor::SourceEditMode::Insert, 48U, 24U, 20U),
      whole_source(asset), 300U);
  EDIT_CHECK(plan.has_value());
  cmd::DirectorScheduler scheduler{root};
  constexpr cmd::UndoContextId history{2U};
  EDIT_CHECK(scheduler.schedule_document_replace(
      plan.value().document, history, cmd::TransactionId{77U},
      cmd::FrameNumber{1U}));
  EDIT_CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
  EDIT_CHECK(scheduler.undo_depth(history) == 1U);
  EDIT_CHECK(scheduler.schedule_undo(history, cmd::TransactionId{78U},
                                     cmd::FrameNumber{2U}));
  EDIT_CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
  EDIT_CHECK(doc::diff(root, scheduler.sealed_document()).changes.empty());
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 16U> cases{{
    {"a viewer insert inside a clip cuts it and moves its second half",
     &a_viewer_insert_inside_a_clip_cuts_it_and_moves_its_second_half},
    {"a viewer insert places the exact range and moves later clips",
     &a_viewer_insert_places_the_exact_range_and_moves_later_clips},
    {"a viewer overwrite lays the range over the track and keeps its length",
     &a_viewer_overwrite_lays_the_range_over_the_track_and_keeps_its_length},
    {"a viewer append lands after the last clip and moves nothing",
     &a_viewer_append_lands_after_the_last_clip_and_moves_nothing},
    {"a viewer still keeps its placed duration",
     &a_viewer_still_keeps_its_placed_duration},
    {"a viewer range is refused with its reason and nothing changes",
     &a_viewer_range_is_refused_with_its_reason_and_nothing_changes},
    {"a viewer cut is one undo step", &a_viewer_cut_is_one_undo_step},
    {"a sync-locked track keeps time with a ripple unless it is locked",
     &a_sync_locked_track_keeps_time_with_a_ripple_unless_it_is_locked},
    {"negative placement and loop Slip commit with undo and redo",
     &negative_placement_and_loop_slip_commit_with_undo_and_redo},
    {"Razor uses playback rounding and wraps looping sources",
     &razor_uses_native_floor_at_cut_and_wraps_looping_sources},
    {"fractional slip routes through the pure planner",
     &fractional_slip_routes_through_the_pure_planner},
    {"insert is one replacement with lane mapped clipboard subgraph",
     &insert_is_one_replacement_with_lane_mapped_clipboard_subgraph},
    {"overwrite lays the paste over the track and cuts nothing",
     &overwrite_lays_the_paste_over_the_track_and_cuts_nothing},
    {"a paste outside every track leaves the tracks alone",
     &a_paste_outside_every_track_leaves_the_tracks_alone},
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
