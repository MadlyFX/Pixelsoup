#include "plateset_authoring.hpp"

#include "director_compositor_authoring.hpp"
#include "director_scene_authoring.hpp"
#include "timeline_lane_authoring.hpp"

#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/assets/library_edit.hpp>
#include <mediaserver/assets/plateset.hpp>
#include <mediaserver/compositor/compositor_schema.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <tuple>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace assets = mediaserver::assets;
namespace compositor = mediaserver::compositor;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace scene = mediaserver::scene;
using assets::PlatesetSlot;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define PLATESET_CHECK(...)                                                    \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr scene::SceneId stage_id{1U};

[[nodiscard]] std::optional<std::int64_t> integer(const doc::Node &node,
                                                  std::string_view name) {
  const auto found = node.properties.find(name);
  if (found == node.properties.end()) {
    return std::nullopt;
  }
  const auto *const value = std::get_if<std::int64_t>(&found->second);
  return value != nullptr ? std::optional<std::int64_t>{*value}
                          : std::nullopt;
}

[[nodiscard]] std::string text(const doc::Node &node, std::string_view name) {
  const auto found = node.properties.find(name);
  return found != node.properties.end() &&
                 std::holds_alternative<std::string>(found->second)
             ? std::get<std::string>(found->second)
             : std::string{};
}

[[nodiscard]] doc::Node media(core::u64 id, std::string name,
                              std::int64_t frames = 1'000,
                              std::int64_t rate = 24) {
  doc::Node node{};
  node.id = doc::NodeId{id};
  node.type = std::string{assets::schema::media_type};
  const auto put = [&node](std::string_view key, doc::PropertyValue value) {
    node.properties.insert_or_assign(std::string{key}, std::move(value));
  };
  put(assets::schema::display_name, name);
  put(assets::schema::source, "D:\\plates\\" + name);
  put(assets::schema::media_width, std::int64_t{1'920});
  put(assets::schema::media_height, std::int64_t{1'080});
  put(assets::schema::media_frame_count, frames);
  put(assets::schema::media_rate_numerator, rate);
  put(assets::schema::media_rate_denominator, std::int64_t{1});
  return node;
}

[[nodiscard]] doc::Root with_set(
    doc::Root root, core::u64 id, std::string name,
    std::initializer_list<std::tuple<PlatesetSlot, core::u64, core::i64>>
        slots) {
  assets::LibraryEdit create{};
  create.kind = assets::LibraryEdit::Kind::CreatePlateset;
  create.name = std::move(name);
  for (const auto &[slot, plate, offset] : slots) {
    static_cast<void>(offset);
    create.slots[static_cast<std::size_t>(slot)] = doc::NodeId{plate};
  }
  root = *assets::plan_library_edit(root, create, doc::NodeId{id});
  assets::LibraryEdit offsets{};
  offsets.kind = assets::LibraryEdit::Kind::SetPlatesetOffsets;
  offsets.target = doc::NodeId{id};
  for (const auto &[slot, plate, offset] : slots) {
    static_cast<void>(plate);
    offsets.offsets[static_cast<std::size_t>(slot)] = offset;
  }
  if (auto changed = assets::plan_library_edit(root, offsets)) {
    root = std::move(*changed);
  }
  return root;
}

[[nodiscard]] doc::Root base_show() {
  doc::Root root;
  root = root.set(doc::Node{doc::NodeId{static_cast<core::u64>(stage_id.value())},
                            std::string{scene::schema::stage_type},
                            {}});
  root = root.set(media(11U, "take_FC.mp4"));
  root = root.set(media(12U, "take_SL.mp4"));
  root = with_set(root, 20U, "Take",
                  {{PlatesetSlot::Front, 11U, 5}, {PlatesetSlot::Left, 12U, 0}});
  return root;
}

[[nodiscard]] std::vector<doc::Node> of_type(const doc::Root &root,
                                             std::string_view type) {
  std::vector<doc::Node> found;
  for (const doc::NodeChange &change : doc::diff(doc::Root{}, root).changes) {
    if (change.after && change.after->type == type) {
      found.push_back(*change.after);
    }
  }
  return found;
}

[[nodiscard]] bool wired(const doc::Root &root, doc::NodeId from,
                         doc::NodeId to, std::int64_t port) {
  for (const doc::Node &edge : of_type(root, compositor::schema::edge_type)) {
    if (integer(edge, compositor::schema::from_node) ==
            static_cast<std::int64_t>(from.value()) &&
        integer(edge, compositor::schema::to_node) ==
            static_cast<std::int64_t>(to.value()) &&
        integer(edge, compositor::schema::to_port) == port) {
      return true;
    }
  }
  return false;
}

// The Scene Layer naming `lane`, as the placement made it.
[[nodiscard]] doc::NodeId layer_for(const doc::Root &root, doc::NodeId lane) {
  for (const doc::Node &node :
       of_type(root, compositor::schema::scene_layer_type)) {
    if (integer(node, compositor::schema::scene_node) ==
        static_cast<std::int64_t>(lane.value())) {
      return node.id;
    }
  }
  return {};
}

constexpr std::size_t front = static_cast<std::size_t>(PlatesetSlot::Front);
constexpr std::size_t right = static_cast<std::size_t>(PlatesetSlot::Right);
constexpr std::size_t left = static_cast<std::size_t>(PlatesetSlot::Left);

// A take's property for one plate: its `_source`, `_asset`, `_count` or
// `_offset`.
[[nodiscard]] std::string plate(std::size_t slot, std::string_view suffix) {
  return scene::schema::plate_property(slot, suffix);
}

[[nodiscard]] core::Result<app::PlatesetPlacementPlan,
                           app::PlatesetAuthoringError>
try_place(const doc::Root &root, core::u64 set = 20U,
          app::PlatesetRigChoice rig = app::PlatesetRigChoice::Auto,
          scene::MediaFrameRate rate = {24U, 1U}, core::i64 start = 100,
          doc::NodeId track = {}) {
  app::PlatesetPlacementRequest request{};
  request.scene = stage_id;
  request.plateset = doc::NodeId{set};
  request.start = start;
  request.show_rate = rate;
  request.rig = rig;
  request.track = track;
  return app::plan_plateset_placement(root, request, 1'000U);
}

[[nodiscard]] app::PlatesetPlacementPlan
place(const doc::Root &root,
      app::PlatesetRigChoice rig = app::PlatesetRigChoice::Auto,
      scene::MediaFrameRate rate = {24U, 1U}) {
  auto plan = try_place(root, 20U, rig, rate);
  if (!plan) {
    throw std::runtime_error{"placement refused"};
  }
  return std::move(plan).value();
}

// What the scene draws at `frame` of a 24 fps show.
[[nodiscard]] scene::Scene evaluated(const doc::Root &root, core::u64 frame) {
  scene::SceneModel model;
  model.reset(root);
  PLATESET_CHECK(model.counters().rejected == 0U);
  return model.evaluate(doc::NodeId{stage_id.value()},
                        scene::SceneTime{frame, 24U, 1U});
}

[[nodiscard]] const scene::MediaInstance *on_screen(const scene::Scene &scene,
                                                    doc::NodeId screen) {
  const auto found = std::find_if(
      scene.media.begin(), scene.media.end(),
      [screen](const scene::MediaInstance &value) noexcept {
        return value.timeline_lane == screen;
      });
  return found == scene.media.end() ? nullptr : std::addressof(*found);
}

void the_library_and_the_scene_call_the_slots_the_same() {
  // A take's plate goes to the screen whose slot key the scene reads, so the
  // two vocabularies - neither library links the other - must not drift.
  PLATESET_CHECK(assets::all_plateset_slots.size() ==
                 scene::schema::vehicle_slot_keys.size());
  for (const PlatesetSlot slot : assets::all_plateset_slots) {
    PLATESET_CHECK(
        assets::plateset_slot_key(slot) ==
        scene::schema::vehicle_slot_keys[static_cast<std::size_t>(slot)]);
  }
}

void a_placement_makes_one_take_on_a_vehicle_track_and_wires_its_screens() {
  const app::PlatesetPlacementPlan plan = place(base_show());
  // One track for the car, a screen for each of the two plates, one clip.
  PLATESET_CHECK(plan.created_track);
  PLATESET_CHECK(plan.screens.size() == 2U);
  const doc::Node &track = *plan.document.get(plan.track);
  PLATESET_CHECK(track.type == scene::schema::timeline_lane_type);
  PLATESET_CHECK(text(track, scene::schema::lane_kind) ==
                 scene::schema::lane_kind_vehicle);
  PLATESET_CHECK(text(track, scene::schema::lane_name) == "Vehicle");
  PLATESET_CHECK(
      of_type(plan.document, scene::schema::timeline_lane_type).size() == 3U);
  PLATESET_CHECK(of_type(plan.document, scene::schema::media_type).size() ==
                 1U);

  // The take is an ordinary clip of the front plate: it starts where it
  // was dropped, lasts the span both plates cover - the front skips 5 of its
  // 1000 frames, so 995 - and begins on the front's first common frame.
  const doc::Node &take = *plan.document.get(plan.clip);
  PLATESET_CHECK(integer(take, scene::schema::lane_id) ==
                 static_cast<std::int64_t>(plan.track.value()));
  PLATESET_CHECK(integer(take, scene::schema::clip_start) == 100);
  PLATESET_CHECK(integer(take, scene::schema::clip_duration) == 995);
  PLATESET_CHECK(integer(take, scene::schema::source_in) == 5);
  PLATESET_CHECK(integer(take, scene::schema::asset_id) == 11);
  PLATESET_CHECK(text(take, scene::schema::source) ==
                 "D:\\plates\\take_FC.mp4");
  PLATESET_CHECK(integer(take, scene::schema::sequence_count) == 1'000);
  PLATESET_CHECK(integer(take, scene::schema::plateset_asset) == 20);
  // And it names each plate the set fills, with its sync offset.
  PLATESET_CHECK(text(take, plate(front, scene::schema::plate_source_suffix)) ==
                 "D:\\plates\\take_FC.mp4");
  PLATESET_CHECK(integer(take, plate(front, scene::schema::plate_offset_suffix)) ==
                 5);
  PLATESET_CHECK(text(take, plate(left, scene::schema::plate_source_suffix)) ==
                 "D:\\plates\\take_SL.mp4");
  PLATESET_CHECK(integer(take, plate(left, scene::schema::plate_asset_suffix)) ==
                 12);
  PLATESET_CHECK(integer(take, plate(left, scene::schema::plate_count_suffix)) ==
                 1'000);
  PLATESET_CHECK(integer(take, plate(left, scene::schema::plate_offset_suffix)) ==
                 0);
  PLATESET_CHECK(!take.properties.contains(
      plate(right, scene::schema::plate_source_suffix)));

  // Each screen belongs to the track, is its slot, and takes its plate's
  // raster.
  const doc::Node &front_screen = *plan.document.get(plan.screens[0]);
  const doc::Node &left_screen = *plan.document.get(plan.screens[1]);
  for (const doc::Node *screen : {&front_screen, &left_screen}) {
    PLATESET_CHECK(integer(*screen, scene::schema::lane_parent) ==
                   static_cast<std::int64_t>(plan.track.value()));
    PLATESET_CHECK(integer(*screen, scene::schema::lane_raster_width) == 1'920);
  }
  PLATESET_CHECK(text(front_screen, scene::schema::lane_slot) == "front");
  PLATESET_CHECK(text(left_screen, scene::schema::lane_slot) == "left");
  PLATESET_CHECK(text(left_screen, scene::schema::lane_name) ==
                 assets::plateset_slot_label(PlatesetSlot::Left));

  // A new LED Car Process takes each screen's Scene Layer on its slot's
  // socket and feeds a new Output.
  PLATESET_CHECK(plan.created_rig);
  PLATESET_CHECK(plan.document.get(plan.rig)->type ==
                 compositor::schema::led_car_process_type);
  PLATESET_CHECK(wired(plan.document,
                       layer_for(plan.document, plan.screens[0]), plan.rig, 0));
  PLATESET_CHECK(wired(plan.document,
                       layer_for(plan.document, plan.screens[1]), plan.rig, 6));
  const std::vector<doc::Node> outputs =
      of_type(plan.document, compositor::schema::output_type);
  PLATESET_CHECK(outputs.size() == 1U);
  PLATESET_CHECK(wired(plan.document, plan.rig, outputs.front().id, 0));
  PLATESET_CHECK(plan.skipped.empty());

  // What was authored is what plays: ten frames in, each plate on its own
  // screen, the front ten past its skip and the left ten past its own.
  const scene::Scene playing = evaluated(plan.document, 110U);
  PLATESET_CHECK(playing.media.size() == 2U);
  const scene::MediaInstance *const front_plate =
      on_screen(playing, plan.screens[0]);
  const scene::MediaInstance *const left_plate =
      on_screen(playing, plan.screens[1]);
  PLATESET_CHECK(front_plate != nullptr && left_plate != nullptr);
  PLATESET_CHECK(front_plate->source == "D:\\plates\\take_FC.mp4");
  PLATESET_CHECK(front_plate->frame_index == 15U);
  PLATESET_CHECK(left_plate->source == "D:\\plates\\take_SL.mp4");
  PLATESET_CHECK(left_plate->frame_index == 10U);
  PLATESET_CHECK(front_plate->placement == plan.clip);
  PLATESET_CHECK(left_plate->placement == plan.clip);
}

void the_length_is_counted_in_show_frames() {
  doc::Root root = base_show();
  root = root.set(media(11U, "take_FC.mp4", 1'000, 25));
  root = root.set(media(12U, "take_SL.mp4", 1'000, 25));
  const app::PlatesetPlacementPlan plan =
      place(root, app::PlatesetRigChoice::Auto, {50U, 1U});
  PLATESET_CHECK(integer(*plan.document.get(plan.clip),
                         scene::schema::clip_duration) == 1'990);
  PLATESET_CHECK(app::plateset_show_frames(10, {30'000U, 1'001U}, {24U, 1U}) ==
                 9);
}

void a_new_rig_composites_over_what_output_already_shows() {
  doc::Root root = base_show();
  editor::CompositorAddIntent add{};
  add.kind = compositor::NodeKind::Background;
  root = root.set(app::make_compositor_node(doc::NodeId{300U}, add,
                                            doc::NodeId{stage_id.value()}));
  add.kind = compositor::NodeKind::Output;
  root = root.set(app::make_compositor_node(doc::NodeId{301U}, add,
                                            doc::NodeId{stage_id.value()}));
  root = root.set(app::make_compositor_edge(
      doc::NodeId{302U}, compositor::EdgeEndpoint{doc::NodeId{300U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{301U}, 0U}));
  const app::PlatesetPlacementPlan plan = place(root);
  PLATESET_CHECK(!plan.document.contains(doc::NodeId{302U}));
  const std::vector<doc::Node> composites =
      of_type(plan.document, compositor::schema::composite_type);
  PLATESET_CHECK(composites.size() == 1U);
  const doc::NodeId composite = composites.front().id;
  PLATESET_CHECK(wired(plan.document, doc::NodeId{300U}, composite, 0));
  PLATESET_CHECK(wired(plan.document, plan.rig, composite, 0));
  PLATESET_CHECK(wired(plan.document, composite, doc::NodeId{301U}, 0));
  PLATESET_CHECK(of_type(plan.document, compositor::schema::output_type).size() ==
                 1U);
}

void an_existing_rig_is_reused_replaced_or_joined_by_another() {
  doc::Root root = base_show();
  editor::CompositorAddIntent add{};
  add.kind = compositor::NodeKind::LedCarProcess;
  root = root.set(app::make_compositor_node(doc::NodeId{400U}, add,
                                            doc::NodeId{stage_id.value()}));
  add.kind = compositor::NodeKind::SceneLayer;
  add.scene_node = doc::NodeId{999U};
  root = root.set(app::make_compositor_node(doc::NodeId{401U}, add,
                                            doc::NodeId{stage_id.value()}));
  // The old rig already plays something on FRONT.
  root = root.set(app::make_compositor_edge(
      doc::NodeId{402U}, compositor::EdgeEndpoint{doc::NodeId{401U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{400U}, 0U}));
  PLATESET_CHECK(app::plateset_rig_busy(root, stage_id, doc::NodeId{20U}));

  const app::PlatesetPlacementPlan kept = place(root);
  PLATESET_CHECK(!kept.created_rig);
  PLATESET_CHECK(kept.rig == doc::NodeId{400U});
  PLATESET_CHECK((kept.skipped == std::vector<PlatesetSlot>{PlatesetSlot::Front}));
  PLATESET_CHECK(kept.document.contains(doc::NodeId{402U}));
  PLATESET_CHECK(wired(kept.document, layer_for(kept.document, kept.screens[1]),
                       doc::NodeId{400U}, 6));
  // Once the scene has a vehicle track there is nothing left to ask: the
  // next take goes on it.
  PLATESET_CHECK(!app::plateset_rig_busy(kept.document, stage_id,
                                         doc::NodeId{20U}));

  const app::PlatesetPlacementPlan replaced =
      place(root, app::PlatesetRigChoice::Replace);
  PLATESET_CHECK(!replaced.document.contains(doc::NodeId{402U}));
  PLATESET_CHECK(wired(replaced.document,
                       layer_for(replaced.document, replaced.screens[0]),
                       doc::NodeId{400U}, 0));
  PLATESET_CHECK(replaced.skipped.empty());

  const app::PlatesetPlacementPlan second =
      place(root, app::PlatesetRigChoice::NewRig);
  PLATESET_CHECK(second.created_rig);
  PLATESET_CHECK(second.rig != doc::NodeId{400U});
  PLATESET_CHECK(second.document.contains(doc::NodeId{402U}));
  PLATESET_CHECK(of_type(second.document,
                         compositor::schema::led_car_process_type)
                     .size() == 2U);
}

void a_set_that_is_not_ready_is_refused() {
  doc::Root root = base_show();
  assets::LibraryEdit clear{};
  clear.kind = assets::LibraryEdit::Kind::SetPlatesetSlot;
  clear.target = doc::NodeId{20U};
  clear.slot = PlatesetSlot::Front;
  root = *assets::plan_library_edit(root, clear);
  const auto refused = try_place(root);
  PLATESET_CHECK(!refused);
  PLATESET_CHECK(refused.error() == app::PlatesetAuthoringError::NotReady);
}

void another_take_goes_on_the_same_track() {
  const app::PlatesetPlacementPlan first = place(base_show());
  doc::Root root = first.document;
  root = root.set(media(13U, "night_FC.mp4"));
  root = root.set(media(14U, "night_SL.mp4"));
  root = root.set(media(15U, "night_SR.mp4"));
  root = with_set(root, 21U, "Night",
                  {{PlatesetSlot::Front, 13U, 0},
                   {PlatesetSlot::Left, 14U, 0},
                   {PlatesetSlot::Right, 15U, 0}});

  // The same set again, later: one more clip and nothing else. No track, no
  // screen, no rig, no wire.
  const auto again = try_place(root, 20U, app::PlatesetRigChoice::Auto,
                               {24U, 1U}, 2'000);
  PLATESET_CHECK(static_cast<bool>(again));
  PLATESET_CHECK(!again.value().created_track);
  PLATESET_CHECK(again.value().track == first.track);
  PLATESET_CHECK(again.value().screens.empty());
  PLATESET_CHECK(!again.value().created_rig);
  PLATESET_CHECK(again.value().rig == first.rig);
  const doc::Delta added = doc::diff(root, again.value().document);
  PLATESET_CHECK(added.changes.size() == 1U);
  PLATESET_CHECK(added.changes.front().id == again.value().clip);
  // It is in front of the take already there.
  PLATESET_CHECK(
      integer(*again.value().document.get(again.value().clip),
              scene::schema::clip_order) >
      integer(*root.get(first.clip), scene::schema::clip_order));

  // A set with a plate the car has no screen for yet gains that one screen,
  // on the rig the others already feed.
  const auto wider = try_place(root, 21U, app::PlatesetRigChoice::Auto,
                               {24U, 1U}, 2'000);
  PLATESET_CHECK(static_cast<bool>(wider));
  PLATESET_CHECK(wider.value().track == first.track);
  PLATESET_CHECK(wider.value().screens.size() == 1U);
  const doc::Node &right_screen =
      *wider.value().document.get(wider.value().screens.front());
  PLATESET_CHECK(text(right_screen, scene::schema::lane_slot) == "right");
  PLATESET_CHECK(!wider.value().created_rig);
  PLATESET_CHECK(wired(wider.value().document,
                       layer_for(wider.value().document, right_screen.id),
                       first.rig, 2));
  PLATESET_CHECK(of_type(wider.value().document,
                         compositor::schema::led_car_process_type)
                     .size() == 1U);

  // Dropped on the track by name it goes there; dropped on something that
  // is not a vehicle track it is refused.
  const auto chosen = try_place(root, 20U, app::PlatesetRigChoice::Auto,
                                {24U, 1U}, 2'000, first.track);
  PLATESET_CHECK(static_cast<bool>(chosen));
  PLATESET_CHECK(chosen.value().track == first.track);
  const auto on_a_screen = try_place(root, 20U, app::PlatesetRigChoice::Auto,
                                     {24U, 1U}, 2'000, first.screens[0]);
  PLATESET_CHECK(!on_a_screen);
  PLATESET_CHECK(on_a_screen.error() ==
                 app::PlatesetAuthoringError::NotAVehicleTrack);

  // A second car is asked for by name: its own track, screens and rig.
  const auto second_car = try_place(root, 20U, app::PlatesetRigChoice::NewRig);
  PLATESET_CHECK(static_cast<bool>(second_car));
  PLATESET_CHECK(second_car.value().created_track);
  PLATESET_CHECK(second_car.value().track != first.track);
  PLATESET_CHECK(text(*second_car.value().document.get(second_car.value().track),
                      scene::schema::lane_name) == "Vehicle 2");
  PLATESET_CHECK(second_car.value().screens.size() == 2U);
  PLATESET_CHECK(second_car.value().created_rig);
}

void a_swap_plays_the_other_take_at_the_same_position() {
  const app::PlatesetPlacementPlan placed = place(base_show());
  doc::Root root = placed.document;
  root = root.set(media(13U, "night_FC.mp4", 500));
  root = root.set(media(14U, "night_SL.mp4", 500));
  root = with_set(root, 21U, "Night",
                  {{PlatesetSlot::Front, 13U, 2}, {PlatesetSlot::Left, 14U, 9}});
  // The operator has trimmed the take forty frames in, faded it, and nudged
  // the left wall: none of which is the footage's.
  {
    doc::Node take = *root.get(placed.clip);
    take.properties.insert_or_assign(std::string{scene::schema::source_in},
                                     doc::PropertyValue{std::int64_t{45}});
    take.properties.insert_or_assign(std::string{scene::schema::clip_duration},
                                     doc::PropertyValue{std::int64_t{300}});
    take.properties.insert_or_assign(std::string{scene::schema::fade_in},
                                     doc::PropertyValue{std::int64_t{12}});
    root = root.set(take);
    doc::Node wall = *root.get(placed.screens[1]);
    wall.properties.insert_or_assign(std::string{scene::schema::position_x},
                                     doc::PropertyValue{0.25});
    wall.properties.insert_or_assign(
        std::string{scene::schema::lane_time_offset},
        doc::PropertyValue{std::int64_t{-3}});
    root = root.set(wall);
  }

  const auto swapped = app::plan_plateset_swap(root, placed.clip,
                                               doc::NodeId{21U}, {24U, 1U},
                                               5'000U);
  PLATESET_CHECK(static_cast<bool>(swapped));
  const doc::Root &after = swapped.value().document;
  // One node changed: the clip. The track, its screens and their wiring are
  // exactly as they were.
  const doc::Delta changed = doc::diff(root, after);
  PLATESET_CHECK(changed.changes.size() == 1U);
  PLATESET_CHECK(changed.changes.front().id == placed.clip);
  PLATESET_CHECK(swapped.value().unmatched.empty());
  PLATESET_CHECK(swapped.value().screens.empty());

  const doc::Node &take = *after.get(placed.clip);
  PLATESET_CHECK(integer(take, scene::schema::plateset_asset) == 21);
  PLATESET_CHECK(integer(take, scene::schema::asset_id) == 13);
  PLATESET_CHECK(text(take, scene::schema::source) ==
                 "D:\\plates\\night_FC.mp4");
  PLATESET_CHECK(integer(take, scene::schema::sequence_count) == 500);
  PLATESET_CHECK(integer(take, plate(left, scene::schema::plate_asset_suffix)) ==
                 14);
  PLATESET_CHECK(integer(take, plate(left, scene::schema::plate_offset_suffix)) ==
                 9);
  // Where it is on the Timeline, and how it fades, are the operator's.
  PLATESET_CHECK(integer(take, scene::schema::clip_start) == 100);
  PLATESET_CHECK(integer(take, scene::schema::clip_duration) == 300);
  PLATESET_CHECK(integer(take, scene::schema::fade_in) == 12);
  // It was forty frames into the old take (trim 45, the old front skipping
  // 5); the new front skips 2, so forty frames into this one is 42.
  PLATESET_CHECK(integer(take, scene::schema::source_in) == 42);

  // And it plays: the left wall where the operator put it, three frames
  // behind its take as they set it, on the new take's own sync.
  const scene::Scene playing = evaluated(after, 100U);
  const scene::MediaInstance *const left_plate =
      on_screen(playing, placed.screens[1]);
  PLATESET_CHECK(left_plate != nullptr);
  PLATESET_CHECK(left_plate->source == "D:\\plates\\night_SL.mp4");
  // 42 on the front is 42 + (9 - 2) on the left, less the wall's three.
  PLATESET_CHECK(left_plate->frame_index == 46U);
  PLATESET_CHECK(left_plate->world.at(0U, 3U) == 0.25F);

  // A take longer than the new footage is cut to what every plate still
  // has: the left starts 49 frames in and has 500, so 451.
  doc::Node longer = *root.get(placed.clip);
  longer.properties.insert_or_assign(std::string{scene::schema::clip_duration},
                                     doc::PropertyValue{std::int64_t{900}});
  const auto shortened = app::plan_plateset_swap(
      root.set(longer), placed.clip, doc::NodeId{21U}, {24U, 1U}, 5'000U);
  PLATESET_CHECK(integer(*shortened.value().document.get(placed.clip),
                         scene::schema::clip_duration) == 451);

  // A set with no left plate leaves that screen standing with nothing on
  // it, and says so.
  root = root.set(media(15U, "dusk_FC.mp4"));
  root = with_set(root, 22U, "Dusk", {{PlatesetSlot::Front, 15U, 0}});
  const auto partial = app::plan_plateset_swap(root, placed.clip,
                                               doc::NodeId{22U}, {24U, 1U},
                                               5'000U);
  PLATESET_CHECK((partial.value().unmatched ==
                  std::vector<PlatesetSlot>{PlatesetSlot::Left}));
  PLATESET_CHECK(!partial.value().document.get(placed.clip)->properties.contains(
      plate(left, scene::schema::plate_source_suffix)));
  PLATESET_CHECK(partial.value().document.contains(placed.screens[1]));
  const scene::Scene dusk = evaluated(partial.value().document, 100U);
  PLATESET_CHECK(dusk.media.size() == 1U);
  PLATESET_CHECK(on_screen(dusk, placed.screens[0]) != nullptr);

  // A set with a plate the car has no screen for gains the screen, wired
  // to the car's rig.
  root = root.set(media(16U, "rain_FC.mp4"));
  root = root.set(media(17U, "rain_SR.mp4"));
  root = with_set(root, 23U, "Rain",
                  {{PlatesetSlot::Front, 16U, 0}, {PlatesetSlot::Right, 17U, 0}});
  const auto wider = app::plan_plateset_swap(root, placed.clip,
                                             doc::NodeId{23U}, {24U, 1U},
                                             5'000U);
  PLATESET_CHECK(wider.value().screens.size() == 1U);
  PLATESET_CHECK(wired(wider.value().document,
                       layer_for(wider.value().document,
                                 wider.value().screens.front()),
                       placed.rig, 2));

  // Only a take can be swapped, and not for the set it already plays.
  const auto not_a_take = app::plan_plateset_swap(
      root, doc::NodeId{11U}, doc::NodeId{22U}, {24U, 1U}, 5'000U);
  PLATESET_CHECK(!not_a_take);
  PLATESET_CHECK(not_a_take.error() == app::PlatesetAuthoringError::NotATake);
  const auto same = app::plan_plateset_swap(placed.document, placed.clip,
                                            doc::NodeId{20U}, {24U, 1U},
                                            5'000U);
  PLATESET_CHECK(!same);
  PLATESET_CHECK(same.error() == app::PlatesetAuthoringError::NothingToSwap);
}

void placed_takes_follow_the_sets_offsets() {
  const app::PlatesetPlacementPlan plan = place(base_show());
  const doc::Root &placed = plan.document;
  assets::LibraryEdit offsets{};
  offsets.kind = assets::LibraryEdit::Kind::SetPlatesetOffsets;
  offsets.target = doc::NodeId{20U};
  offsets.offsets[front] = 8;
  offsets.offsets[left] = 3;
  const doc::Root followed = app::follow_plateset(
      *assets::plan_library_edit(placed, offsets), doc::NodeId{20U},
      {24U, 1U});
  const doc::Node &take = *followed.get(plan.clip);
  // The take stays where it was in the footage: its trim moves with the
  // front's skip, and each plate carries its new offset.
  PLATESET_CHECK(integer(take, scene::schema::source_in) == 8);
  PLATESET_CHECK(integer(take, plate(front, scene::schema::plate_offset_suffix)) ==
                 8);
  PLATESET_CHECK(integer(take, plate(left, scene::schema::plate_offset_suffix)) ==
                 3);
  // The front now skips 8 of its 1000 frames, so the take - placed at 995
  // - is cut to the 992 it still has.
  PLATESET_CHECK(integer(take, scene::schema::clip_duration) == 992);
  // And the left plays three frames in when the take opens.
  const scene::Scene opening = evaluated(followed, 100U);
  const scene::MediaInstance *const left_plate =
      on_screen(opening, plan.screens[1]);
  PLATESET_CHECK(left_plate != nullptr && left_plate->frame_index == 3U);

  // Pushing the front 900 frames in leaves it 100 to play, at a 48 fps
  // show twice that: the take is cut to them.
  offsets.offsets[front] = 900;
  const doc::Root shortened = app::follow_plateset(
      *assets::plan_library_edit(placed, offsets), doc::NodeId{20U},
      {48U, 1U});
  PLATESET_CHECK(integer(*shortened.get(plan.clip),
                         scene::schema::source_in) == 900);
  PLATESET_CHECK(integer(*shortened.get(plan.clip),
                         scene::schema::clip_duration) == 200);

  // A plate replaced in the set is replaced in the take.
  doc::Root library = placed.set(media(13U, "retake_SL.mp4", 800));
  assets::LibraryEdit slot{};
  slot.kind = assets::LibraryEdit::Kind::SetPlatesetSlot;
  slot.target = doc::NodeId{20U};
  slot.slot = PlatesetSlot::Left;
  slot.media = doc::NodeId{13U};
  const doc::Root replaced = app::follow_plateset(
      *assets::plan_library_edit(library, slot), doc::NodeId{20U}, {24U, 1U});
  PLATESET_CHECK(integer(*replaced.get(plan.clip),
                         plate(left, scene::schema::plate_asset_suffix)) == 13);
  PLATESET_CHECK(integer(*replaced.get(plan.clip),
                         plate(left, scene::schema::plate_count_suffix)) == 800);
  PLATESET_CHECK(integer(*replaced.get(plan.clip),
                         scene::schema::clip_duration) == 800);

  // A set that can no longer be placed leaves its takes as they were.
  slot.slot = PlatesetSlot::Front;
  slot.media = doc::NodeId{};
  const doc::Root broken = *assets::plan_library_edit(placed, slot);
  PLATESET_CHECK(
      doc::diff(broken,
                app::follow_plateset(broken, doc::NodeId{20U}, {24U, 1U}))
          .changes.empty());
}

// The set's half size reaches every placed take, now and when it changes.
void placed_takes_take_the_sets_decode_size() {
  doc::Root root = base_show();
  assets::LibraryEdit half{};
  half.kind = assets::LibraryEdit::Kind::SetPlatesetHalfResolution;
  half.target = doc::NodeId{20U};
  half.half = true;
  root = *assets::plan_library_edit(root, half);
  const app::PlatesetPlacementPlan plan = place(root);
  PLATESET_CHECK(text(*plan.document.get(plan.clip),
                      scene::schema::media_resolution) ==
                 scene::schema::media_resolution_half);
  for (const scene::MediaInstance &plate_drawn :
       evaluated(plan.document, 100U).media) {
    PLATESET_CHECK(plate_drawn.resolution == scene::MediaResolution::Half);
  }
  half.half = false;
  const doc::Root full = app::follow_plateset(
      *assets::plan_library_edit(plan.document, half), doc::NodeId{20U},
      {24U, 1U});
  PLATESET_CHECK(!full.get(plan.clip)->properties.contains(
      scene::schema::media_resolution));
}

void a_vehicle_track_is_deleted_with_its_screens_and_never_split() {
  const app::PlatesetPlacementPlan plan = place(base_show());
  app::LaneAuthorRequest request{};
  request.scene = stage_id;
  request.lane = plan.track;

  // The car is one thing: half of it is not a track.
  request.action = app::LaneAuthorAction::Split;
  request.frame = 400;
  request.show_rate = {24U, 1U};
  const auto split =
      app::plan_lane_authoring(plan.document, request, plan.next_node);
  PLATESET_CHECK(!split);
  PLATESET_CHECK(split.error() == app::LaneAuthorError::VehicleTrack);
  request.lane = plan.screens[0];
  const auto split_screen =
      app::plan_lane_authoring(plan.document, request, plan.next_node);
  PLATESET_CHECK(!split_screen);
  PLATESET_CHECK(split_screen.error() == app::LaneAuthorError::VehicleTrack);

  // With a take on it, it is not empty.
  request.lane = plan.track;
  request.action = app::LaneAuthorAction::DeleteEmpty;
  const auto kept =
      app::plan_lane_authoring(plan.document, request, plan.next_node);
  PLATESET_CHECK(!kept);
  PLATESET_CHECK(kept.error() == app::LaneAuthorError::NonEmptyLane);

  // Deleted with its clips, its screens go with it and nothing of the car
  // is left on the Timeline.
  request.action = app::LaneAuthorAction::DeleteClips;
  const auto deleted =
      app::plan_lane_authoring(plan.document, request, plan.next_node);
  PLATESET_CHECK(static_cast<bool>(deleted));
  const doc::Root &after = deleted.value().document;
  PLATESET_CHECK(!after.contains(plan.track));
  PLATESET_CHECK(!after.contains(plan.screens[0]));
  PLATESET_CHECK(!after.contains(plan.screens[1]));
  PLATESET_CHECK(!after.contains(plan.clip));
  PLATESET_CHECK(of_type(after, scene::schema::timeline_lane_type).empty());
  PLATESET_CHECK(deleted.value().erased.size() == 4U);

  // An ordinary track beside it is untouched by any of it.
  app::LaneAuthorRequest create{};
  create.scene = stage_id;
  create.action = app::LaneAuthorAction::Create;
  create.name = "Wall";
  const auto with_wall =
      app::plan_lane_authoring(plan.document, create, plan.next_node);
  PLATESET_CHECK(static_cast<bool>(with_wall));
  const auto beside = app::plan_lane_authoring(
      with_wall.value().document, request, with_wall.value().next_node);
  PLATESET_CHECK(static_cast<bool>(beside));
  PLATESET_CHECK(beside.value().document.contains(with_wall.value().created));
  PLATESET_CHECK(of_type(beside.value().document,
                         scene::schema::timeline_lane_type)
                     .size() == 1U);
}

// The Inspector's Time offset field on a slot screen: whole source frames,
// that screen's plate alone, and nothing on a track that is nobody's wall.
void a_slot_screens_time_offset_is_authored_in_whole_frames() {
  const app::PlatesetPlacementPlan plan = place(base_show());
  mediaserver::cmd::DirectorScheduler scheduler{plan.document};
  const auto author = [&](doc::NodeId node, double value, core::u64 frame) {
    editor::GenericEdit edit{};
    edit.node = node;
    edit.property = std::string{scene::schema::lane_time_offset};
    edit.value = value;
    edit.gesture = 900U + frame;
    PLATESET_CHECK(app::schedule_generic_edit(
        scheduler, edit, mediaserver::cmd::FrameNumber{frame}));
    PLATESET_CHECK(scheduler.seal_frame(mediaserver::cmd::FrameNumber{frame}));
  };
  // Ten frames into the take the left wall plays its tenth frame.
  {
    const scene::Scene before = evaluated(plan.document, 110U);
    PLATESET_CHECK(on_screen(before, plan.screens[1])->frame_index == 10U);
  }

  // 3.6 frames is four: the left wall plays four frames ahead of its take,
  // and the front wall is where it was.
  author(plan.screens[1], 3.6, 1U);
  PLATESET_CHECK(integer(*scheduler.sealed_document().get(plan.screens[1]),
                         scene::schema::lane_time_offset) == 4);
  const scene::Scene ahead = evaluated(scheduler.sealed_document(), 110U);
  PLATESET_CHECK(on_screen(ahead, plan.screens[1])->frame_index == 14U);
  PLATESET_CHECK(on_screen(ahead, plan.screens[0])->frame_index == 15U);

  // Back to none leaves no property behind.
  author(plan.screens[1], 0.0, 2U);
  PLATESET_CHECK(!scheduler.sealed_document()
                      .get(plan.screens[1])
                      ->properties.contains(scene::schema::lane_time_offset));

  // The vehicle track is nobody's wall: an offset on it would make the
  // scene refuse the track, so it is not authored at all.
  author(plan.track, 5.0, 3U);
  PLATESET_CHECK(!scheduler.sealed_document()
                      .get(plan.track)
                      ->properties.contains(scene::schema::lane_time_offset));
  PLATESET_CHECK(evaluated(scheduler.sealed_document(), 110U).media.size() ==
                 2U);
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 12U> cases{{
    {"a slot screen's time offset is authored in whole frames",
     &a_slot_screens_time_offset_is_authored_in_whole_frames},
    {"the Library and the scene call the slots the same",
     &the_library_and_the_scene_call_the_slots_the_same},
    {"placed takes take the set's decode size",
     &placed_takes_take_the_sets_decode_size},
    {"a placement makes one take on a vehicle track and wires its screens",
     &a_placement_makes_one_take_on_a_vehicle_track_and_wires_its_screens},
    {"the length is counted in show frames",
     &the_length_is_counted_in_show_frames},
    {"a new rig composites over what Output already shows",
     &a_new_rig_composites_over_what_output_already_shows},
    {"an existing rig is reused, replaced or joined by another",
     &an_existing_rig_is_reused_replaced_or_joined_by_another},
    {"a set that is not ready is refused", &a_set_that_is_not_ready_is_refused},
    {"another take goes on the same track",
     &another_take_goes_on_the_same_track},
    {"a swap plays the other take at the same position",
     &a_swap_plays_the_other_take_at_the_same_position},
    {"placed takes follow the set's offsets",
     &placed_takes_follow_the_sets_offsets},
    {"a vehicle track is deleted with its screens and never split",
     &a_vehicle_track_is_deleted_with_its_screens_and_never_split},
}};

} // namespace

int main() {
  std::size_t failures = 0U;
  for (const Case &entry : cases) {
    try {
      entry.function();
      std::cout << "[pass] " << entry.name << std::endl;
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
