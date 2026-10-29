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

// A placed plateset beside Deck: FRONT and LEFT tracks holding one take (7)
// from frame 100, razored at 150 so both plates carry a second take from
// there to 170. FRONT is behind
// LEFT, so LEFT is listed first. A second placement of the same set, on its
// own track, is take 8; a track holding a plate and an ordinary clip is an
// ordinary track.
constexpr doc::NodeId chicago{300U};
constexpr doc::NodeId front_lane{301U};
constexpr doc::NodeId left_lane{302U};
constexpr doc::NodeId again_lane{303U};
constexpr doc::NodeId mixed_lane{304U};
constexpr doc::NodeId front_plate{311U};
constexpr doc::NodeId left_plate{312U};
constexpr doc::NodeId front_tail{313U};
constexpr doc::NodeId left_tail{314U};
constexpr doc::NodeId again_plate{315U};
constexpr doc::NodeId mixed_plate{316U};
constexpr doc::NodeId mixed_clip{317U};

void place_plateset(Fixture &f) {
  const auto lane = [](doc::NodeId id, mediaserver::core::i64 order,
                       std::string name) {
    return scene::TimelineLane{id, scene::SceneId{1U}, order, std::move(name),
                               false, false, scene::BlendMode::Normal, false};
  };
  f.lanes.push_back(lane(front_lane, 20, "FRONT / Chicago drive"));
  f.lanes.push_back(lane(left_lane, 30, "LEFT / Chicago drive"));
  f.lanes.push_back(lane(again_lane, 40, "FRONT / Chicago drive"));
  f.lanes.push_back(lane(mixed_lane, 50, "Mixed"));
  using mediaserver::core::i64;
  const auto place = [&](doc::NodeId node, doc::NodeId on, std::string slot,
                         i64 take, i64 start, i64 duration) {
    auto out = element(node, editor::ElementKind::Media, slot, std::nullopt);
    out.timeline_lane = on;
    out.plateset = chicago;
    out.plateset_slot = std::move(slot);
    out.plateset_take = take;
    out.timing = scene::ClipTiming{start, duration, 0, 0, 0};
    f.elements.push_back(out);
  };
  place(front_plate, front_lane, "front", 7, 100, 50);
  place(left_plate, left_lane, "left", 7, 100, 50);
  place(front_tail, front_lane, "front", 7, 150, 20);
  place(left_tail, left_lane, "left", 7, 150, 20);
  place(again_plate, again_lane, "front", 8, 300, 10);
  place(mixed_plate, mixed_lane, "front", 9, 400, 10);
  auto ordinary =
      element(mixed_clip, editor::ElementKind::Media, "Other", std::nullopt);
  ordinary.timeline_lane = mixed_lane;
  ordinary.timing = scene::ClipTiming{410, 10, 0, 0, 0};
  f.elements.push_back(ordinary);
  for (const doc::NodeId track : {front_lane, left_lane, again_lane,
                                  mixed_lane}) {
    f.elements.push_back(
        element(track, editor::ElementKind::Track, "", std::nullopt));
  }
}

void a_placed_platesets_tracks_fold_into_one_take_row() {
  Fixture f;
  place_plateset(f);
  const RowList list = f.build();
  // Two placements, two rows; the mixed track stays a track.
  ROWS_CHECK(list.platesets.size() == 2U);
  const std::size_t take_row = find(list, Row::Kind::PlatesetHeader, front_lane);
  const std::size_t again_row =
      find(list, Row::Kind::PlatesetHeader, again_lane);
  const std::size_t mixed = find(list, Row::Kind::LaneHeader, mixed_lane);
  ROWS_CHECK(list.rows[mixed].indent == 0U);
  ROWS_CHECK(listed(list, mixed_plate) && listed(list, mixed_clip));
  // Listed where its frontmost track would be: after the later placement and
  // the mixed track, before Deck, which keeps its own header and clips.
  ROWS_CHECK(mixed < again_row && again_row < take_row);
  ROWS_CHECK(take_row < find(list, Row::Kind::LaneHeader, deck));
  ROWS_CHECK(listed(list, shot_one));

  // Closed by default: the row alone, named after what its tracks share.
  const Row &row = list.rows[take_row];
  ROWS_CHECK(row.label == "Chicago drive");
  ROWS_CHECK(row.expandable && row.indent == 0U);
  ROWS_CHECK(row.end == take_row + 1U);
  ROWS_CHECK(!listed(list, left_lane) && !listed(list, front_plate) &&
             !listed(list, left_tail));
  // Its group: both tracks, LEFT first, and the two takes the razor left,
  // each led by its front plate.
  const timeline::PlatesetGroup &group = list.platesets[row.plateset];
  ROWS_CHECK(group.plateset == chicago && group.key == front_lane);
  ROWS_CHECK(group.lanes == (std::vector<doc::NodeId>{left_lane, front_lane}));
  ROWS_CHECK(group.takes.size() == 2U);
  ROWS_CHECK(group.takes[0].start == 100 && group.takes[0].duration == 50 &&
             group.takes[0].lead == front_plate &&
             group.takes[0].clips ==
                 (std::vector<doc::NodeId>{left_plate, front_plate}));
  ROWS_CHECK(group.takes[1].start == 150 && group.takes[1].lead == front_tail);
  ROWS_CHECK(!group.takes[0].locked);
  // Closed, it summarises every plate.
  const timeline::RowSummary *closed = summary(list, take_row);
  ROWS_CHECK(closed != nullptr && closed->first == 100 && closed->end == 170);

  // The Library's name wins over the tracks'.
  const std::array<editor::TimelinePlateset, 1U> names{
      {{chicago, "Chicago drive, take 3"}}};
  const RowList named = timeline::build_rows(
      f.state,
      timeline::RowsInput{f.elements, f.lanes, {}, {}, f.keys, 0U, f.search,
                          {}, names},
      f.theme);
  ROWS_CHECK(named.rows[find(named, Row::Kind::PlatesetHeader, front_lane)]
                 .label == "Chicago drive, take 3");

  // Open, its tracks follow it one indent in, each still a track with its
  // Properties and its clips; Deck is unaffected.
  f.state.expanded_platesets = {front_lane};
  const RowList open = f.build();
  const std::size_t header = find(open, Row::Kind::PlatesetHeader, front_lane);
  const std::size_t left = find(open, Row::Kind::LaneHeader, left_lane);
  const std::size_t front = find(open, Row::Kind::LaneHeader, front_lane);
  ROWS_CHECK(left == header + 1U && left < front);
  ROWS_CHECK(open.rows[left].indent == 1U && open.rows[left].parent == header);
  ROWS_CHECK(open.rows[front].parent == header);
  const std::size_t plate_row = find(open, Row::Kind::Layer, front_plate);
  ROWS_CHECK(open.rows[plate_row].parent == front &&
             open.rows[plate_row].indent == 2U &&
             open.rows[plate_row].lane_owner == front_lane);
  ROWS_CHECK(open.rows[header].end == find(open, Row::Kind::LaneHeader, deck));
  ROWS_CHECK(summary(open, header) == nullptr);
  ROWS_CHECK(open.rows[find(open, Row::Kind::LaneHeader, deck)].indent == 0U);
  // The row is not a selectable layer or track itself.
  for (const doc::NodeId node : open.visible_order) {
    ROWS_CHECK(node != again_lane);
  }

  // A locked plate locks its take.
  for (editor::ElementInfo &held : f.elements) {
    held.locked = held.locked || held.node == front_plate;
  }
  f.state.expanded_platesets.clear();
  const RowList locked = f.build();
  ROWS_CHECK(locked.platesets[locked.rows[find(locked,
                                               Row::Kind::PlatesetHeader,
                                               front_lane)]
                                  .plateset]
                 .takes[0]
                 .locked);

  // A search for the set lists its tracks under the row, as a search for a
  // track's name lists the track.
  f.search = "chicago";
  const RowList found = f.build();
  const std::size_t searched =
      find(found, Row::Kind::PlatesetHeader, front_lane);
  ROWS_CHECK(found.rows[find(found, Row::Kind::LaneHeader, left_lane)].parent ==
             searched);
  ROWS_CHECK(found.rows[find(found, Row::Kind::LaneHeader, front_lane)]
                 .parent == searched);
  ROWS_CHECK(!listed(found, shot_one));
}

// Open, a plate's track shows its slot alone under the row that names the
// set. A track sitting between the set's tracks parts the row in two, so
// the list keeps the stacking order; the row's switch sets every track of
// its own run that is not there yet.
void a_take_row_parts_around_a_track_between_its_own() {
  Fixture f;
  place_plateset(f);
  f.state.expanded_platesets = {front_lane};
  const RowList open = f.build();
  const Row &left = open.rows[find(open, Row::Kind::LaneHeader, left_lane)];
  ROWS_CHECK(left.shown_label == "LEFT" && left.short_label == "L");
  ROWS_CHECK(left.label == "LEFT / Chicago drive");
  ROWS_CHECK(open.rows[find(open, Row::Kind::LaneHeader, mixed_lane)]
                 .shown_label.empty());

  for (scene::TimelineLane &lane : f.lanes) {
    lane.muted = lane.id == left_lane;
  }
  const timeline::PlatesetGroup &whole =
      open.platesets[open.rows[find(open, Row::Kind::PlatesetHeader,
                                    front_lane)]
                         .plateset];
  ROWS_CHECK(timeline::plateset_switch_edits(
                 whole, f.lanes, editor::TimelineLaneAction::ToggleMute,
                 true) ==
             (std::vector<editor::TimelineLaneEdit>{
                 {editor::TimelineLaneAction::ToggleMute, front_lane, {}}}));
  ROWS_CHECK(timeline::plateset_switch_edits(
                 whole, f.lanes, editor::TimelineLaneAction::ToggleMute,
                 false) ==
             (std::vector<editor::TimelineLaneEdit>{
                 {editor::TimelineLaneAction::ToggleMute, left_lane, {}}}));

  constexpr doc::NodeId between{305U};
  // Lanes arrive in stack order, back first.
  f.lanes.insert(
      std::find_if(f.lanes.begin(), f.lanes.end(),
                   [](const scene::TimelineLane &lane) {
                     return lane.id == left_lane;
                   }),
      scene::TimelineLane{between, scene::SceneId{1U}, 25, "Between", false,
                          false, scene::BlendMode::Normal, false});
  f.state.expanded_platesets.clear();
  const RowList parted = f.build();
  ROWS_CHECK(parted.platesets.size() == 3U);
  const std::size_t left_row =
      find(parted, Row::Kind::PlatesetHeader, left_lane);
  const std::size_t between_row =
      find(parted, Row::Kind::LaneHeader, between);
  const std::size_t front_row =
      find(parted, Row::Kind::PlatesetHeader, front_lane);
  ROWS_CHECK(left_row < between_row && between_row < front_row);
  const timeline::PlatesetGroup &upper =
      parted.platesets[parted.rows[left_row].plateset];
  const timeline::PlatesetGroup &lower =
      parted.platesets[parted.rows[front_row].plateset];
  ROWS_CHECK(upper.lanes == std::vector<doc::NodeId>{left_lane});
  ROWS_CHECK(lower.lanes == std::vector<doc::NodeId>{front_lane});
  ROWS_CHECK(upper.takes.size() == 2U && upper.takes[0].lead == left_plate &&
             upper.takes[0].clips == std::vector<doc::NodeId>{left_plate});
  ROWS_CHECK(lower.takes[0].lead == front_plate);
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 9U> cases{{
    {"a take row parts around a track between its own",
     &a_take_row_parts_around_a_track_between_its_own},
    {"a placed plateset's tracks fold into one take row",
     &a_placed_platesets_tracks_fold_into_one_take_row},
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
