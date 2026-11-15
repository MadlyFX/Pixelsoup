#include "../src/timeline/timeline_internal.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// The Timeline's row model on its own: the tree build_rows lists, and the
// summaries of closed parents.
namespace {

namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;
namespace timeline = mediaserver::editor::timeline;
using timeline::Row;
using timeline::RowList;
using timeline::SummaryKey;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define ROWS_CHECK(...)                                                        \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr doc::NodeId rig{100U};
constexpr doc::NodeId lamp{101U};
constexpr doc::NodeId plate{102U};
constexpr doc::NodeId inner{103U};
constexpr doc::NodeId bulb{104U};
constexpr doc::NodeId loner{105U};
constexpr doc::NodeId deck{200U};
constexpr doc::NodeId shot_one{201U};
constexpr doc::NodeId shot_two{202U};

[[nodiscard]] editor::ElementInfo element(doc::NodeId node,
                                          editor::ElementKind kind,
                                          std::string name,
                                          std::optional<doc::NodeId> parent) {
  editor::ElementInfo out{};
  out.node = node;
  out.kind = kind;
  out.display_name = std::move(name);
  out.parent_group = parent;
  return out;
}

// A group "Rig" holding a lamp, a plate placed from frame -12, and a nested
// group "Inner" holding a bulb; a loose "Loner" with nothing animated; and a
// track "Deck" with two clips.
struct Fixture final {
  std::vector<editor::ElementInfo> elements;
  std::vector<scene::TimelineLane> lanes{scene::TimelineLane{
      deck, scene::SceneId{1U}, 10, "Deck", false, false,
      scene::BlendMode::Normal, false}};
  std::vector<scene::ChannelKeyframes> keys{
      {lamp, scene::PropertyChannel::PositionX, {10, 40}, {0.0, 1.0}},
      {bulb, scene::PropertyChannel::Opacity, {40, 90}, {1.0, 0.0}},
      {bulb, scene::PropertyChannel::PositionX, {10, 90}, {2.0, 3.0}},
      {plate, scene::PropertyChannel::Opacity, {-5}, {0.5}},
      {rig, scene::PropertyChannel::RotationYaw, {40}, {0.25}},
      {shot_one, scene::PropertyChannel::PositionX, {35}, {1.0}},
  };
  timeline::RowsState state;
  std::string search;
  gui::Theme theme{};

  Fixture() {
    auto group = element(rig, editor::ElementKind::Group, "Rig", std::nullopt);
    group.group_order = 5;
    auto placed = element(plate, editor::ElementKind::Media, "Plate", rig);
    placed.timing = scene::ClipTiming{-12, 20, 0, 0, 0};
    auto first = element(shot_one, editor::ElementKind::Media, "Shot one",
                         std::nullopt);
    first.timeline_lane = deck;
    first.timing = scene::ClipTiming{30, 10, 0, 0, 0};
    auto second = element(shot_two, editor::ElementKind::Media, "Shot two",
                          std::nullopt);
    second.timeline_lane = deck;
    second.timing = scene::ClipTiming{50, 5, 0, 0, 0};
    elements = {group,
                element(lamp, editor::ElementKind::Object, "Lamp", rig),
                placed,
                element(inner, editor::ElementKind::Group, "Inner", rig),
                element(bulb, editor::ElementKind::Object, "Bulb", inner),
                element(loner, editor::ElementKind::Object, "Loner",
                        std::nullopt),
                first,
                second};
  }

  [[nodiscard]] RowList build() {
    return timeline::build_rows(
        state,
        timeline::RowsInput{elements, lanes, {}, {}, keys, 0U, search, {}},
        theme);
  }
};

[[nodiscard]] std::size_t find(const RowList &list, Row::Kind kind,
                               doc::NodeId node) {
  for (std::size_t index = 0U; index < list.rows.size(); ++index) {
    if (list.rows[index].kind == kind && list.rows[index].node == node) {
      return index;
    }
  }
  throw std::runtime_error{"row not listed: " + std::to_string(node.value())};
}

[[nodiscard]] bool listed(const RowList &list, doc::NodeId node) {
  for (const Row &row : list.rows) {
    if (row.node == node) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] const timeline::RowSummary *summary(const RowList &list,
                                                  std::size_t row) {
  const std::size_t index = list.rows[row].summary;
  return index == timeline::no_row ? nullptr : &list.summaries[index];
}

[[nodiscard]] std::vector<SummaryKey>
keys(const RowList &list, const timeline::RowSummary &summary) {
  const auto run = list.keys(summary);
  return {run.begin(), run.end()};
}

void rows_link_to_their_parents_through_groups_and_tracks() {
  Fixture f;
  f.state.expanded_hierarchies = {rig, inner};
  const RowList list = f.build();
  const std::size_t track = find(list, Row::Kind::LaneHeader, deck);
  const std::size_t one = find(list, Row::Kind::Layer, shot_one);
  const std::size_t two = find(list, Row::Kind::Layer, shot_two);
  const std::size_t group = find(list, Row::Kind::Layer, rig);
  const std::size_t group_properties =
      find(list, Row::Kind::PropertiesHeader, rig);
  const std::size_t lamp_row = find(list, Row::Kind::Layer, lamp);
  const std::size_t plate_row = find(list, Row::Kind::Layer, plate);
  const std::size_t nested = find(list, Row::Kind::Layer, inner);
  const std::size_t nested_properties =
      find(list, Row::Kind::PropertiesHeader, inner);
  const std::size_t bulb_row = find(list, Row::Kind::Layer, bulb);
  const std::size_t loose = find(list, Row::Kind::Layer, loner);

  ROWS_CHECK(list.rows[track].parent == timeline::no_row &&
             list.rows[track].depth == 0U);
  ROWS_CHECK(list.rows[one].parent == track && list.rows[one].depth == 1U);
  ROWS_CHECK(list.rows[two].parent == track && list.rows[two].depth == 1U);
  ROWS_CHECK(list.rows[track].end == two + 1U);

  ROWS_CHECK(list.rows[group].parent == timeline::no_row &&
             list.rows[group].depth == 0U);
  for (const std::size_t child :
       {group_properties, lamp_row, plate_row, nested}) {
    ROWS_CHECK(list.rows[child].parent == group &&
               list.rows[child].depth == 1U);
  }
  ROWS_CHECK(list.rows[nested_properties].parent == nested &&
             list.rows[nested_properties].depth == 2U);
  ROWS_CHECK(list.rows[bulb_row].parent == nested &&
             list.rows[bulb_row].depth == 2U);
  // Descendants are one contiguous run: the group's ends where Loner starts.
  ROWS_CHECK(list.rows[group].end == loose);
  ROWS_CHECK(list.rows[nested].end == bulb_row + 1U);
  ROWS_CHECK(list.rows[loose].parent == timeline::no_row &&
             list.rows[loose].end == loose + 1U);
}

void a_closed_group_spans_and_unions_everything_below_it() {
  Fixture f;
  const RowList list = f.build();
  ROWS_CHECK(!listed(list, lamp) && !listed(list, bulb));
  const timeline::RowSummary *group =
      summary(list, find(list, Row::Kind::Layer, rig));
  ROWS_CHECK(group != nullptr);
  // The plate's clip runs from -12 to (not including) 8; keys reach 90.
  ROWS_CHECK(group->first == -12 && group->end == 90);
  // Frame 10 has two Position X keys (lamp, bulb): counted, not repeated,
  // and still one channel. Frame 40 mixes lamp X, bulb Opacity and the
  // rig's own yaw; frame 90 the bulb's Opacity and X. A mixed frame records
  // its first channel in enumeration order, but is drawn neutral.
  using scene::PropertyChannel;
  ROWS_CHECK(keys(list, *group) ==
             (std::vector<SummaryKey>{
                 {-5, 1U, PropertyChannel::Opacity, false},
                 {10, 2U, PropertyChannel::PositionX, false},
                 {40, 3U, PropertyChannel::PositionX, true},
                 {90, 2U, PropertyChannel::PositionX, true}}));
}

void a_closed_track_summarises_its_clips() {
  Fixture f;
  f.state.collapsed_lanes = {deck};
  const RowList list = f.build();
  ROWS_CHECK(!listed(list, shot_one) && !listed(list, shot_two));
  const std::size_t track = find(list, Row::Kind::LaneHeader, deck);
  ROWS_CHECK(list.rows[track].expandable);
  ROWS_CHECK(list.rows[track].end == track + 1U);
  const timeline::RowSummary *closed = summary(list, track);
  ROWS_CHECK(closed != nullptr);
  ROWS_CHECK(closed->first == 30 && closed->end == 55);
  ROWS_CHECK(keys(list, *closed) == (std::vector<SummaryKey>{
                                 {35, 1U, scene::PropertyChannel::PositionX,
                                  false}}));

  // Open, the track lists its clips and summarises nothing itself.
  f.state.collapsed_lanes.clear();
  const RowList open = f.build();
  ROWS_CHECK(summary(open, find(open, Row::Kind::LaneHeader, deck)) == nullptr);
  ROWS_CHECK(listed(open, shot_one) && listed(open, shot_two));
}

void opening_a_parent_moves_its_summary_down_to_what_stays_folded() {
  Fixture f;
  f.state.expanded_hierarchies = {rig};
  const RowList list = f.build();
  ROWS_CHECK(summary(list, find(list, Row::Kind::Layer, rig)) == nullptr);
  // Its own folded properties: only the rig's yaw key.
  const timeline::RowSummary *own =
      summary(list, find(list, Row::Kind::PropertiesHeader, rig));
  ROWS_CHECK(own != nullptr && own->first == 40 && own->end == 40);
  ROWS_CHECK(keys(list, *own) == (std::vector<SummaryKey>{
                              {40, 1U, scene::PropertyChannel::RotationYaw,
                               false}}));
  // The still-closed Inner stands for itself and the bulb.
  const timeline::RowSummary *nested =
      summary(list, find(list, Row::Kind::Layer, inner));
  ROWS_CHECK(nested != nullptr && nested->first == 10 && nested->end == 90);
  // A plate whose properties are folded: its clip and its key.
  const timeline::RowSummary *placed =
      summary(list, find(list, Row::Kind::Layer, plate));
  ROWS_CHECK(placed != nullptr && placed->first == -12 && placed->end == 8);
  // Nothing animated, nothing placed: no summary, so no bar at all.
  ROWS_CHECK(summary(list, find(list, Row::Kind::Layer, loner)) == nullptr);

  // Opening the lamp's properties takes its summary away too.
  f.state.expanded_layers = {lamp.value()};
  const RowList opened = f.build();
  ROWS_CHECK(summary(opened, find(opened, Row::Kind::Layer, lamp)) == nullptr);
}

void a_search_keeps_ancestors_open_and_summarises_the_match() {
  Fixture f;
  f.search = "bulb";
  const RowList list = f.build();
  ROWS_CHECK(!listed(list, lamp) && !listed(list, plate) &&
             !listed(list, loner) && !listed(list, deck));
  const std::size_t group = find(list, Row::Kind::Layer, rig);
  const std::size_t nested = find(list, Row::Kind::Layer, inner);
  const std::size_t bulb_row = find(list, Row::Kind::Layer, bulb);
  ROWS_CHECK(list.rows[nested].parent == group &&
             list.rows[bulb_row].parent == nested);
  ROWS_CHECK(list.rows[bulb_row].depth == 2U);
  // The search opens the ancestors, so they summarise nothing themselves.
  ROWS_CHECK(summary(list, group) == nullptr &&
             summary(list, nested) == nullptr);
  const timeline::RowSummary *match = summary(list, bulb_row);
  ROWS_CHECK(match != nullptr && match->first == 10 && match->end == 90);
  ROWS_CHECK(keys(list, *match) ==
             (std::vector<SummaryKey>{
                 {10, 1U, scene::PropertyChannel::PositionX, false},
                 {40, 1U, scene::PropertyChannel::Opacity, false},
                 {90, 2U, scene::PropertyChannel::PositionX, true}}));
}

// A placed clip's row is as tall as the operator made it - alone, or with
// every other clip row - and the rows still stack as a running sum.
void clip_rows_take_their_own_height_or_every_clip_rows() {
  Fixture f;
  f.state.clip_row_height = 52.0F;
  f.state.clip_row_heights[shot_two] = 96.0F;
  const RowList list = f.build();
  ROWS_CHECK(list.rows[find(list, Row::Kind::Layer, shot_one)].height ==
             52.0F);
  ROWS_CHECK(list.rows[find(list, Row::Kind::Layer, shot_two)].height ==
             96.0F);
  ROWS_CHECK(list.rows[find(list, Row::Kind::LaneHeader, deck)].height ==
             timeline::row_height);
  ROWS_CHECK(list.rows[find(list, Row::Kind::Layer, loner)].height ==
             timeline::row_height);
  for (std::size_t index = 1U; index < list.rows.size(); ++index) {
    ROWS_CHECK(list.rows[index].top ==
               list.rows[index - 1U].top + list.rows[index - 1U].height);
  }

  // A height outside the range a row's edge drags through is held to it.
  f.state.clip_row_heights[shot_two] = 1'000.0F;
  f.state.clip_row_height = 3.0F;
  const RowList clamped = f.build();
  ROWS_CHECK(clamped.rows[find(clamped, Row::Kind::Layer, shot_two)].height ==
             timeline::clip_row_height_maximum);
  ROWS_CHECK(clamped.rows[find(clamped, Row::Kind::Layer, shot_one)].height ==
             timeline::row_height);
}

// Minor ticks are whole frames, as many as fit, and a label drops its hour
// only while the view stays inside the first one.
void a_particle_effect_lists_its_keyed_parameters_by_name() {
  constexpr doc::NodeId embers{300U};
  Fixture f;
  f.elements.push_back(element(embers, editor::ElementKind::ParticleEffect, "Embers", std::nullopt));
  const auto parameter = [&](std::string key, std::vector<mediaserver::core::i64> frames, std::vector<double> values) {
    scene::ChannelKeyframes channel{embers, scene::PropertyChannel::ParticleParameter,
                                    std::move(frames), std::move(values)};
    channel.parameter = std::move(key);
    return channel;
  };
  // A Float keyed alone, and two components of a Vec3, in key order.
  f.keys.push_back(parameter("rate.0", {0, 10}, {100.0, 200.0}));
  f.keys.push_back(parameter("wind.0", {5}, {0.5}));
  f.keys.push_back(parameter("wind.2", {5}, {-1.0}));
  f.state.expanded_layers.insert(embers.value());
  const RowList list = f.build();
  std::vector<const Row *> parameters;
  std::size_t groups = 0U;
  for (const Row &row : list.rows) {
    if (row.node != embers) continue;
    groups += row.kind == Row::Kind::Group ? 1U : 0U;
    if (row.kind == Row::Kind::Property) parameters.push_back(&row);
  }
  // Placed like anything else, with no Opacity row it cannot use.
  ROWS_CHECK(groups == 3U);
  ROWS_CHECK(parameters.size() == 3U);
  ROWS_CHECK(parameters[0]->label == "rate" && parameters[0]->parameter == "rate.0");
  ROWS_CHECK(parameters[0]->property == scene::PropertyChannel::ParticleParameter);
  ROWS_CHECK(parameters[0]->property_target == embers && parameters[0]->property_value == 100.0);
  ROWS_CHECK(parameters[1]->label == "wind X" && parameters[1]->parameter == "wind.0");
  ROWS_CHECK(parameters[2]->label == "wind Z" && parameters[2]->parameter == "wind.2");
  // A row names its one channel by key.
  const auto channels = timeline::row_channels(*parameters[2]);
  ROWS_CHECK(channels.has_value() && channels->parameter == "wind.2" && channels->count == 1U);
}

void ruler_ticks_divide_whole_frames_and_labels_drop_a_zero_hour() {
  // A second of 30 frames at 3 px a frame: two-frame ticks would be 6 px
  // apart, so it divides in tenths of three frames.
  ROWS_CHECK(timeline::ruler_subdivision(30, 3.0F, 7.0F) == 3);
  // 24 frames has no tenth; its sixths of four frames are the finest fit.
  ROWS_CHECK(timeline::ruler_subdivision(24, 3.0F, 7.0F) == 4);
  ROWS_CHECK(timeline::ruler_subdivision(1, 100.0F, 7.0F) == 0);
  ROWS_CHECK(timeline::ruler_subdivision(2, 2.0F, 7.0F) == 0);

  ROWS_CHECK(timeline::compact_timecode("00:04:20:13", true) == "04:20:13");
  ROWS_CHECK(timeline::compact_timecode("01:04:20:13", true) ==
             "01:04:20:13");
  ROWS_CHECK(timeline::compact_timecode("00:04:20:13", false) ==
             "00:04:20:13");
  ROWS_CHECK(timeline::compact_timecode("1200", true) == "1200");
}

// A car beside Deck: a vehicle track with one take on it (frames 100..149)
// and slot screens for the front, the left and the ceiling. The screens'
// own stack orders run the other way - ceiling, left, front - so the list
// has to put them in slot order itself. A fourth screen names a vehicle
// track that is gone.
constexpr doc::NodeId chicago{300U};
constexpr doc::NodeId car{301U};
constexpr doc::NodeId front_screen{302U};
constexpr doc::NodeId left_screen{303U};
constexpr doc::NodeId ceiling_screen{304U};
constexpr doc::NodeId orphan_screen{305U};
constexpr doc::NodeId take_one{311U};

void place_car(Fixture &f) {
  const auto track = [](doc::NodeId id, mediaserver::core::i64 order, std::string name) {
    return scene::TimelineLane{id, scene::SceneId{1U}, order, std::move(name),
                               false, false, scene::BlendMode::Normal, false};
  };
  const auto screen = [&](doc::NodeId id, mediaserver::core::i64 order, std::string name,
                          doc::NodeId owner, mediaserver::core::u8 slot) {
    scene::TimelineLane lane = track(id, order, std::move(name));
    lane.parent = owner;
    lane.slot = slot;
    return lane;
  };
  scene::TimelineLane vehicle = track(car, 20, "Vehicle");
  vehicle.vehicle = true;
  f.lanes.push_back(vehicle);
  f.lanes.push_back(screen(ceiling_screen, 21, "CEILING REFLECTION", car, 8U));
  f.lanes.push_back(screen(left_screen, 22, "LEFT", car, 6U));
  f.lanes.push_back(screen(front_screen, 23, "FRONT", car, 0U));
  f.lanes.push_back(
      screen(orphan_screen, 24, "RIGHT", doc::NodeId{999U}, 2U));
  auto take =
      element(take_one, editor::ElementKind::Media, "Chicago drive",
              std::nullopt);
  take.timeline_lane = car;
  take.timing = scene::ClipTiming{100, 50, 0, 0, 0};
  take.plateset = chicago;
  take.take = true;
  f.elements.push_back(take);
  for (const doc::NodeId wall :
       {front_screen, left_screen, ceiling_screen, orphan_screen}) {
    f.elements.push_back(
        element(wall, editor::ElementKind::Track, "", std::nullopt));
  }
}

[[nodiscard]] bool has_row(const RowList &list, Row::Kind kind,
                           doc::NodeId node) {
  for (const Row &row : list.rows) {
    if (row.kind == kind && row.node == node) {
      return true;
    }
  }
  return false;
}

void a_vehicle_track_lists_as_one_track_with_its_take() {
  Fixture f;
  place_car(f);
  const RowList list = f.build();
  // One track for the car, in front of Deck, with its take listed under it
  // like any clip under any track.
  const std::size_t track = find(list, Row::Kind::LaneHeader, car);
  const Row &header = list.rows[track];
  ROWS_CHECK(header.vehicle && !header.slot_screen && header.indent == 0U);
  ROWS_CHECK(header.label == "Vehicle");
  ROWS_CHECK(track < find(list, Row::Kind::LaneHeader, deck));
  const std::size_t take = find(list, Row::Kind::Layer, take_one);
  ROWS_CHECK(list.rows[take].parent == track && list.rows[take].depth == 1U);
  ROWS_CHECK(list.rows[take].label == "Chicago drive");
  // Deck is an ordinary track and says so.
  const Row &ordinary = list.rows[find(list, Row::Kind::LaneHeader, deck)];
  ROWS_CHECK(!ordinary.vehicle && !ordinary.slot_screen);
  // Its screens are not tracks of their own: unasked, none is listed.
  for (const doc::NodeId wall : {front_screen, left_screen, ceiling_screen}) {
    ROWS_CHECK(!listed(list, wall));
  }
  // A screen whose car is gone is all that is left of it, so it is listed
  // where it can be found and removed.
  const Row &stray = list.rows[find(list, Row::Kind::LaneHeader, orphan_screen)];
  ROWS_CHECK(stray.slot_screen && stray.indent == 0U);

  // Closed, the track summarises its take like any other.
  f.state.collapsed_lanes = {car};
  const RowList closed = f.build();
  ROWS_CHECK(!listed(closed, take_one));
  const timeline::RowSummary *span =
      summary(closed, find(closed, Row::Kind::LaneHeader, car));
  ROWS_CHECK(span != nullptr && span->first == 100 && span->end == 150);
}

// A row never offers keys the show would not play. A vehicle track owns no
// plane and a clip on a track plays on that track's screen, so neither lists
// a transform; both still fade. A wall and a loose layer keep theirs.
void rows_offer_no_transform_where_one_moves_nothing() {
  Fixture f;
  place_car(f);
  auto vehicle = element(car, editor::ElementKind::Track, "", std::nullopt);
  vehicle.vehicle = true;
  f.elements.push_back(vehicle);
  f.state.listed_screens = {car};
  f.state.open_screens = {front_screen};
  for (const doc::NodeId open :
       {car, take_one, front_screen, shot_one, loner}) {
    f.state.expanded_layers.insert(open.value());
  }
  const RowList list = f.build();
  const auto count = [&](doc::NodeId node, Row::Kind kind) {
    std::size_t found = 0U;
    for (const Row &row : list.rows) {
      found += row.node == node && row.kind == kind ? 1U : 0U;
    }
    return found;
  };
  const auto fades = [&](doc::NodeId node) {
    for (const Row &row : list.rows) {
      if (row.node == node && row.kind == Row::Kind::Property &&
          row.property == scene::PropertyChannel::Opacity) {
        return true;
      }
    }
    return false;
  };
  // The car and its take: no Position, Rotation or Scale, and an Opacity.
  ROWS_CHECK(count(car, Row::Kind::PropertiesHeader) == 1U);
  ROWS_CHECK(count(car, Row::Kind::Group) == 0U && fades(car));
  ROWS_CHECK(count(take_one, Row::Kind::Group) == 0U && fades(take_one));
  // An ordinary clip on an ordinary track is in the same position.
  ROWS_CHECK(count(shot_one, Row::Kind::Group) == 0U && fades(shot_one));
  // A wall is a screen with a place of its own, and a loose layer is
  // placed by its own transform.
  ROWS_CHECK(count(front_screen, Row::Kind::Group) == 3U);
  ROWS_CHECK(count(loner, Row::Kind::Group) == 3U);
}

void a_vehicle_tracks_screens_are_listed_under_it_on_request() {
  Fixture f;
  place_car(f);
  f.state.listed_screens = {car};
  const RowList list = f.build();
  const std::size_t track = find(list, Row::Kind::LaneHeader, car);
  const std::size_t take = find(list, Row::Kind::Layer, take_one);
  const std::size_t front = find(list, Row::Kind::LaneHeader, front_screen);
  const std::size_t left = find(list, Row::Kind::LaneHeader, left_screen);
  const std::size_t ceiling =
      find(list, Row::Kind::LaneHeader, ceiling_screen);
  // Under the car, after its take, front first and round to the ceiling -
  // slot order, whatever order the document happens to stack them in.
  ROWS_CHECK(track < take && take < front && front < left && left < ceiling);
  ROWS_CHECK(ceiling < find(list, Row::Kind::LaneHeader, deck));
  for (const std::size_t wall : {front, left, ceiling}) {
    ROWS_CHECK(list.rows[wall].slot_screen && !list.rows[wall].vehicle);
    ROWS_CHECK(list.rows[wall].indent == 1U);
  }
  ROWS_CHECK(list.rows[left].label == "LEFT");
  // A long name gives way to the slot's tag when the list is narrow.
  ROWS_CHECK(list.rows[ceiling].label == "CEILING REFLECTION");
  ROWS_CHECK(list.rows[ceiling].short_label == "TOP");
  ROWS_CHECK(list.rows[left].short_label == "L");
  // Each starts closed: nine walls open would bury the car under their own
  // Properties.
  for (const doc::NodeId wall : {front_screen, left_screen, ceiling_screen}) {
    ROWS_CHECK(!has_row(list, Row::Kind::PropertiesHeader, wall));
  }
  // One opened lists its Properties, to place and key that wall.
  f.state.open_screens = {left_screen};
  const RowList opened = f.build();
  ROWS_CHECK(has_row(opened, Row::Kind::PropertiesHeader, left_screen));
  ROWS_CHECK(!has_row(opened, Row::Kind::PropertiesHeader, front_screen));

  // The screens are listed under an open car only: closed, it is one row.
  f.state.collapsed_lanes = {car};
  const RowList closed = f.build();
  ROWS_CHECK(!listed(closed, front_screen) && !listed(closed, left_screen));
  f.state.collapsed_lanes.clear();

  // A search for a wall finds it under its car, asked for or not.
  f.state.listed_screens.clear();
  f.search = "ceiling";
  const RowList found = f.build();
  ROWS_CHECK(has_row(found, Row::Kind::LaneHeader, car));
  ROWS_CHECK(has_row(found, Row::Kind::LaneHeader, ceiling_screen));
  ROWS_CHECK(!listed(found, front_screen) && !listed(found, left_screen));
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 11U> cases{{
    {"a particle effect lists its keyed parameters by name",
     &a_particle_effect_lists_its_keyed_parameters_by_name},
    {"a vehicle track lists as one track with its take",
     &a_vehicle_track_lists_as_one_track_with_its_take},
    {"rows offer no transform where one moves nothing",
     &rows_offer_no_transform_where_one_moves_nothing},
    {"a vehicle track's screens are listed under it on request",
     &a_vehicle_tracks_screens_are_listed_under_it_on_request},
    {"clip rows take their own height or every clip row's",
     &clip_rows_take_their_own_height_or_every_clip_rows},
    {"ruler ticks divide whole frames and labels drop a zero hour",
     &ruler_ticks_divide_whole_frames_and_labels_drop_a_zero_hour},
    {"rows link to their parents through groups and tracks",
     &rows_link_to_their_parents_through_groups_and_tracks},
    {"a closed group spans and unions everything below it",
     &a_closed_group_spans_and_unions_everything_below_it},
    {"a closed track summarises its clips",
     &a_closed_track_summarises_its_clips},
    {"opening a parent moves its summary down to what stays folded",
     &opening_a_parent_moves_its_summary_down_to_what_stays_folded},
    {"a search keeps ancestors open and summarises the match",
     &a_search_keeps_ancestors_open_and_summarises_the_match},
}};

} // namespace

int main() {
  std::size_t failures = 0U;
  for (const Case &entry : cases) {
    try {
      entry.function();
      std::cout << "[pass] " << entry.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
