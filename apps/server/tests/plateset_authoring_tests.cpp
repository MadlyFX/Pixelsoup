#include "plateset_authoring.hpp"

#include "director_compositor_authoring.hpp"

#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/assets/library_edit.hpp>
#include <mediaserver/compositor/compositor_schema.hpp>
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

[[nodiscard]] app::PlatesetPlacementPlan
place(const doc::Root &root,
      app::PlatesetRigChoice rig = app::PlatesetRigChoice::Auto,
      scene::MediaFrameRate rate = {24U, 1U}) {
  app::PlatesetPlacementRequest request{};
  request.scene = stage_id;
  request.plateset = doc::NodeId{20U};
  request.start = 100;
  request.show_rate = rate;
  request.rig = rig;
  auto plan = app::plan_plateset_placement(root, request, 1'000U);
  if (!plan) {
    throw std::runtime_error{"placement refused"};
  }
  return std::move(plan).value();
}

void a_placement_makes_a_track_and_clip_per_plate_and_wires_a_rig() {
  const app::PlatesetPlacementPlan plan = place(base_show());
  PLATESET_CHECK(plan.lanes.size() == 2U);
  PLATESET_CHECK(plan.clips.size() == 2U);
  const doc::Node &front = *plan.document.get(plan.clips[0]);
  const doc::Node &left = *plan.document.get(plan.clips[1]);
  // Both plates start together and last the span both cover: the front
  // skips 5 of its 1000 frames, so 995.
  for (const doc::Node *clip : {&front, &left}) {
    PLATESET_CHECK(integer(*clip, scene::schema::clip_start) == 100);
    PLATESET_CHECK(integer(*clip, scene::schema::clip_duration) == 995);
    PLATESET_CHECK(integer(*clip, scene::schema::plateset_take) ==
                   static_cast<std::int64_t>(plan.clips[0].value()));
    PLATESET_CHECK(integer(*clip, scene::schema::plateset_asset) == 20);
  }
  PLATESET_CHECK(integer(front, scene::schema::source_in) == 5);
  PLATESET_CHECK(integer(left, scene::schema::source_in) == 0);
  PLATESET_CHECK(integer(front, scene::schema::asset_id) == 11);
  PLATESET_CHECK(text(front, scene::schema::plateset_slot) == "front");
  PLATESET_CHECK(text(left, scene::schema::plateset_slot) == "left");
  PLATESET_CHECK(integer(front, scene::schema::lane_id) ==
                 static_cast<std::int64_t>(plan.lanes[0].value()));
  const doc::Node &track = *plan.document.get(plan.lanes[1]);
  PLATESET_CHECK(text(track, scene::schema::lane_name) == "LEFT / Take");
  PLATESET_CHECK(integer(track, scene::schema::lane_raster_width) == 1'920);

  // A new LED Car Process takes each track's Scene Layer on its slot's
  // socket and feeds a new Output.
  PLATESET_CHECK(plan.created_rig);
  PLATESET_CHECK(plan.document.get(plan.rig)->type ==
                 compositor::schema::led_car_process_type);
  PLATESET_CHECK(wired(plan.document, layer_for(plan.document, plan.lanes[0]),
                       plan.rig, 0));
  PLATESET_CHECK(wired(plan.document, layer_for(plan.document, plan.lanes[1]),
                       plan.rig, 6));
  const std::vector<doc::Node> outputs =
      of_type(plan.document, compositor::schema::output_type);
  PLATESET_CHECK(outputs.size() == 1U);
  PLATESET_CHECK(wired(plan.document, plan.rig, outputs.front().id, 0));
  PLATESET_CHECK(plan.skipped.empty());
}

void the_length_is_counted_in_show_frames() {
  doc::Root root = base_show();
  root = root.set(media(11U, "take_FC.mp4", 1'000, 25));
  root = root.set(media(12U, "take_SL.mp4", 1'000, 25));
  const app::PlatesetPlacementPlan plan =
      place(root, app::PlatesetRigChoice::Auto, {50U, 1U});
  PLATESET_CHECK(integer(*plan.document.get(plan.clips[0]),
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
  PLATESET_CHECK(wired(kept.document, layer_for(kept.document, kept.lanes[1]),
                       doc::NodeId{400U}, 6));

  const app::PlatesetPlacementPlan replaced =
      place(root, app::PlatesetRigChoice::Replace);
  PLATESET_CHECK(!replaced.document.contains(doc::NodeId{402U}));
  PLATESET_CHECK(wired(replaced.document,
                       layer_for(replaced.document, replaced.lanes[0]),
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
  app::PlatesetPlacementRequest request{};
  request.scene = stage_id;
  request.plateset = doc::NodeId{20U};
  const auto refused = app::plan_plateset_placement(root, request, 1'000U);
  PLATESET_CHECK(!refused);
  PLATESET_CHECK(refused.error() == app::PlatesetAuthoringError::NotReady);
}

void a_take_is_the_plates_sharing_its_start_and_length() {
  const app::PlatesetPlacementPlan plan = place(base_show());
  const std::vector<doc::NodeId> take =
      app::plateset_take_clips(plan.document, plan.clips[1]);
  PLATESET_CHECK(take.size() == 2U);
  // A plate moved on its own has left its take.
  doc::Node moved = *plan.document.get(plan.clips[1]);
  moved.properties.insert_or_assign(std::string{scene::schema::clip_start},
                                    doc::PropertyValue{std::int64_t{140}});
  const doc::Root detached = plan.document.set(moved);
  PLATESET_CHECK(app::plateset_take_clips(detached, plan.clips[0]) ==
                 std::vector<doc::NodeId>{plan.clips[0]});
}

void a_swap_plays_the_other_take_at_the_same_position() {
  doc::Root root = place(base_show()).document;
  root = root.set(media(13U, "night_FC.mp4"));
  root = root.set(media(14U, "night_SL.mp4"));
  root = with_set(root, 21U, "Night",
                  {{PlatesetSlot::Front, 13U, 2}, {PlatesetSlot::Left, 14U, 9}});
  const doc::NodeId front_clip{1'001U};
  const doc::Node &before = *root.get(front_clip);
  PLATESET_CHECK(text(before, scene::schema::plateset_slot) == "front");
  const auto swapped = app::plan_plateset_swap(root, front_clip, doc::NodeId{21U});
  PLATESET_CHECK(static_cast<bool>(swapped));
  const doc::Root &after = swapped.value().document;
  for (const doc::NodeId id : app::plateset_take_clips(after, front_clip)) {
    const doc::Node &clip = *after.get(id);
    PLATESET_CHECK(integer(clip, scene::schema::plateset_asset) == 21);
    PLATESET_CHECK(integer(clip, scene::schema::clip_start) == 100);
    if (text(clip, scene::schema::plateset_slot) == "front") {
      // It played 5 frames in on the old front (its offset); the new
      // front's offset is 2.
      PLATESET_CHECK(integer(clip, scene::schema::asset_id) == 13);
      PLATESET_CHECK(integer(clip, scene::schema::source_in) == 2);
      const auto lane = integer(clip, scene::schema::lane_id);
      PLATESET_CHECK(text(*after.get(doc::NodeId{static_cast<core::u64>(*lane)}),
                          scene::schema::lane_name) == "FRONT / Night");
    } else {
      PLATESET_CHECK(integer(clip, scene::schema::asset_id) == 14);
      PLATESET_CHECK(integer(clip, scene::schema::source_in) == 9);
    }
  }
  PLATESET_CHECK(swapped.value().unmatched.empty());

  // A set without a left plate leaves that plate as it was.
  root = root.set(media(15U, "dusk_FC.mp4"));
  root = with_set(root, 22U, "Dusk", {{PlatesetSlot::Front, 15U, 0}});
  const auto partial = app::plan_plateset_swap(root, front_clip, doc::NodeId{22U});
  PLATESET_CHECK((partial.value().unmatched ==
                  std::vector<PlatesetSlot>{PlatesetSlot::Left}));
  PLATESET_CHECK(!app::plan_plateset_swap(root, doc::NodeId{11U},
                                          doc::NodeId{22U}));
}

void placed_plates_follow_the_sets_offsets() {
  const doc::Root placed = place(base_show()).document;
  assets::LibraryEdit offsets{};
  offsets.kind = assets::LibraryEdit::Kind::SetPlatesetOffsets;
  offsets.target = doc::NodeId{20U};
  offsets.offsets[static_cast<std::size_t>(PlatesetSlot::Front)] = 8;
  offsets.offsets[static_cast<std::size_t>(PlatesetSlot::Left)] = 3;
  const doc::Root changed = *assets::plan_library_edit(placed, offsets);
  const doc::Root followed =
      app::follow_plateset_offsets(placed, changed, doc::NodeId{20U}, {24U, 1U});
  PLATESET_CHECK(integer(*followed.get(doc::NodeId{1'001U}),
                         scene::schema::source_in) == 8);
  PLATESET_CHECK(integer(*followed.get(doc::NodeId{1'003U}),
                         scene::schema::source_in) == 3);
  // The front now skips 8 of its 1000 frames, so the take - placed at 995
  // - is cut to the 992 it still has, on both plates.
  for (const core::u64 id : {1'001U, 1'003U}) {
    PLATESET_CHECK(integer(*followed.get(doc::NodeId{id}),
                           scene::schema::clip_duration) == 992);
  }

  // Pushing the front 900 frames in leaves it 100 to play, at a 48 fps
  // show twice that: the whole take is cut to them.
  offsets.offsets[static_cast<std::size_t>(PlatesetSlot::Front)] = 900;
  const doc::Root later = *assets::plan_library_edit(placed, offsets);
  const doc::Root shortened =
      app::follow_plateset_offsets(placed, later, doc::NodeId{20U}, {48U, 1U});
  PLATESET_CHECK(integer(*shortened.get(doc::NodeId{1'001U}),
                         scene::schema::source_in) == 900);
  for (const core::u64 id : {1'001U, 1'003U}) {
    PLATESET_CHECK(integer(*shortened.get(doc::NodeId{id}),
                           scene::schema::clip_duration) == 200);
  }
}

// The set's half size reaches every placed plate, now and when it changes.
void placed_plates_take_the_sets_decode_size() {
  doc::Root root = base_show();
  assets::LibraryEdit half{};
  half.kind = assets::LibraryEdit::Kind::SetPlatesetHalfResolution;
  half.target = doc::NodeId{20U};
  half.half = true;
  root = *assets::plan_library_edit(root, half);
  const doc::Root placed = place(root).document;
  for (const core::u64 id : {1'001U, 1'003U}) {
    PLATESET_CHECK(text(*placed.get(doc::NodeId{id}),
                        scene::schema::media_resolution) ==
                   scene::schema::media_resolution_half);
  }
  half.half = false;
  const doc::Root full = app::follow_plateset_resolution(
      *assets::plan_library_edit(placed, half), doc::NodeId{20U});
  for (const core::u64 id : {1'001U, 1'003U}) {
    PLATESET_CHECK(!full.get(doc::NodeId{id})->properties.contains(
        scene::schema::media_resolution));
  }
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 9U> cases{{
    {"placed plates take the set's decode size",
     &placed_plates_take_the_sets_decode_size},
    {"a placement makes a track and clip per plate and wires a rig",
     &a_placement_makes_a_track_and_clip_per_plate_and_wires_a_rig},
    {"the length is counted in show frames",
     &the_length_is_counted_in_show_frames},
    {"a new rig composites over what Output already shows",
     &a_new_rig_composites_over_what_output_already_shows},
    {"an existing rig is reused, replaced or joined by another",
     &an_existing_rig_is_reused_replaced_or_joined_by_another},
    {"a set that is not ready is refused", &a_set_that_is_not_ready_is_refused},
    {"a take is the plates sharing its start and length",
     &a_take_is_the_plates_sharing_its_start_and_length},
    {"a swap plays the other take at the same position",
     &a_swap_plays_the_other_take_at_the_same_position},
    {"placed plates follow the set's offsets",
     &placed_plates_follow_the_sets_offsets},
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
