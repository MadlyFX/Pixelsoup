#include "../src/timeline/timeline_internal.hpp"

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

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 5U> cases{{
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
