#include "plateset_authoring.hpp"

#include "director_compositor_authoring.hpp"
#include "timeline_lane_authoring.hpp"

#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/assets/plateset.hpp>
#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/compositor/compositor_schema.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <new>
#include <set>
#include <string_view>
#include <utility>

namespace mediaserver::app {
namespace {

using assets::PlatesetSlot;

// A Library plateset's slots and a vehicle track's are the same nine, in the
// same order: a plate goes to the screen with its own index.
static_assert(assets::plateset_slot_count == scene::schema::vehicle_slot_count);

// Wall plates this wide, on the rig's default 4 m radius, meet at their
// edges: the side of the octagon the eight sockets stand on.
constexpr double plate_width = 3.32;

constexpr std::size_t front_plate = 0U;

[[nodiscard]] std::optional<std::int64_t>
integer(const doc::Node &node, std::string_view name) noexcept {
  const auto found = node.properties.find(name);
  if (found == node.properties.end()) {
    return std::nullopt;
  }
  const auto *const value = std::get_if<std::int64_t>(&found->second);
  return value != nullptr ? std::optional<std::int64_t>{*value}
                          : std::nullopt;
}

[[nodiscard]] std::string_view text(const doc::Node &node,
                                    std::string_view name) noexcept {
  const auto found = node.properties.find(name);
  if (found == node.properties.end()) {
    return {};
  }
  const auto *const value = std::get_if<std::string>(&found->second);
  return value != nullptr ? std::string_view{*value} : std::string_view{};
}

void put(doc::Node &node, std::string_view name, doc::PropertyValue value) {
  node.properties.insert_or_assign(std::string{name}, std::move(value));
}

[[nodiscard]] doc::PropertyValue whole(std::int64_t value) {
  return doc::PropertyValue{value};
}

// Every node, and the scene a node without an owner belongs to: the lowest
// stage, as evaluation resolves legacy content.
struct Nodes final {
  std::vector<doc::Node> all;
  scene::SceneId fallback{};
};

[[nodiscard]] Nodes nodes_of(const doc::Root &document) {
  Nodes result;
  const doc::Delta contents = doc::diff(doc::Root{}, document);
  result.all.reserve(contents.changes.size());
  for (const doc::NodeChange &change : contents.changes) {
    if (change.after) {
      result.all.push_back(*change.after);
    }
  }
  for (const doc::Node &node : result.all) {
    if (node.type == scene::schema::stage_type &&
        (result.fallback == scene::SceneId{} ||
         node.id.value() < result.fallback.value())) {
      result.fallback = scene::SceneId{node.id.value()};
    }
  }
  return result;
}

[[nodiscard]] bool in_scene(const doc::Node &node, scene::SceneId wanted,
                            scene::SceneId fallback) noexcept {
  const std::optional<std::int64_t> owner =
      integer(node, scene::schema::scene_id);
  const scene::SceneId owned =
      owner.has_value() && *owner > 0
          ? scene::SceneId{static_cast<core::u64>(*owner)}
          : fallback;
  return owned == wanted;
}

[[nodiscard]] assets::AssetRegistry registry_of(const doc::Root &document) {
  assets::AssetModel model;
  model.reset(document);
  return model.evaluate();
}

[[nodiscard]] const assets::PlatesetAsset *
plateset_in(const assets::AssetRegistry &registry, doc::NodeId id) noexcept {
  const assets::Asset *const asset = registry.asset(id);
  return asset != nullptr ? std::get_if<assets::PlatesetAsset>(asset)
                          : nullptr;
}

[[nodiscard]] const assets::MediaAsset *
media_in(const assets::AssetRegistry &registry, doc::NodeId id) noexcept {
  const assets::Asset *const asset = registry.asset(id);
  return asset != nullptr ? std::get_if<assets::MediaAsset>(asset) : nullptr;
}

// The plates of a set a take can play, by slot: media that is in the Library
// with a rate and a length. Null where the slot is empty or its plate is not
// playable.
using Plates =
    std::array<const assets::MediaAsset *, assets::plateset_slot_count>;

[[nodiscard]] Plates plates_of(const assets::PlatesetAsset &set,
                               const assets::AssetRegistry &registry) {
  Plates plates{};
  for (std::size_t slot = 0U; slot < plates.size(); ++slot) {
    const assets::MediaAsset *const media =
        media_in(registry, set.slots[slot].media);
    if (media != nullptr && media->frame_rate.has_value() &&
        media->frame_count.has_value() && *media->frame_count > 0U) {
      plates[slot] = media;
    }
  }
  return plates;
}

[[nodiscard]] std::string plate_name(std::size_t slot,
                                     std::string_view suffix) {
  return scene::schema::plate_property(slot, suffix);
}

// A take: a clip that came from a plateset and names a front plate.
[[nodiscard]] bool is_take(const doc::Node &node) {
  return node.type == scene::schema::media_type &&
         integer(node, scene::schema::plateset_asset).value_or(0) > 0 &&
         node.properties.contains(
             plate_name(front_plate, scene::schema::plate_source_suffix));
}

[[nodiscard]] bool has_plate(const doc::Node &take, std::size_t slot) {
  return take.properties.contains(
      plate_name(slot, scene::schema::plate_source_suffix));
}

[[nodiscard]] core::i64 plate_offset(const doc::Node &take, std::size_t slot) {
  return integer(take, plate_name(slot, scene::schema::plate_offset_suffix))
      .value_or(0);
}

// Makes `clip` a take of `set`: an ordinary clip of the front plate that
// also names every plate the set can play. Placement, trim, fades, effects
// and everything else on the clip are left alone.
void put_take(doc::Node &clip, const assets::PlatesetAsset &set,
              const Plates &plates) {
  const assets::MediaAsset &front = *plates[front_plate];
  put(clip, scene::schema::source, doc::PropertyValue{front.source});
  put(clip, scene::schema::asset_id,
      whole(static_cast<std::int64_t>(front.id.value())));
  put(clip, scene::schema::sequence_start, whole(0));
  put(clip, scene::schema::sequence_count,
      whole(static_cast<std::int64_t>(*front.frame_count)));
  put(clip, scene::schema::source_rate_numerator,
      whole(front.frame_rate->numerator));
  put(clip, scene::schema::source_rate_denominator,
      whole(front.frame_rate->denominator));
  put(clip, scene::schema::plateset_asset,
      whole(static_cast<std::int64_t>(set.id.value())));
  if (set.half_resolution) {
    put(clip, scene::schema::media_resolution,
        doc::PropertyValue{std::string{scene::schema::media_resolution_half}});
  } else {
    clip.properties.erase(std::string{scene::schema::media_resolution});
  }
  for (std::size_t slot = 0U; slot < plates.size(); ++slot) {
    for (const std::string_view suffix :
         {scene::schema::plate_source_suffix, scene::schema::plate_asset_suffix,
          scene::schema::plate_count_suffix,
          scene::schema::plate_offset_suffix}) {
      clip.properties.erase(plate_name(slot, suffix));
    }
    const assets::MediaAsset *const plate = plates[slot];
    if (plate == nullptr) {
      continue;
    }
    put(clip, plate_name(slot, scene::schema::plate_source_suffix),
        doc::PropertyValue{plate->source});
    put(clip, plate_name(slot, scene::schema::plate_asset_suffix),
        whole(static_cast<std::int64_t>(plate->id.value())));
    put(clip, plate_name(slot, scene::schema::plate_count_suffix),
        whole(static_cast<std::int64_t>(*plate->frame_count)));
    put(clip, plate_name(slot, scene::schema::plate_offset_suffix),
        whole(set.slots[slot].offset));
  }
}

// The show frames a take can play from its trim before one of its plates
// runs out. Nothing when the take has no rate to count them at.
[[nodiscard]] std::optional<core::i64>
take_playable(const doc::Node &take, scene::MediaFrameRate show_rate) {
  const core::i64 numerator =
      integer(take, scene::schema::source_rate_numerator).value_or(0);
  const core::i64 denominator =
      integer(take, scene::schema::source_rate_denominator).value_or(0);
  if (numerator <= 0 || denominator <= 0 || show_rate.numerator == 0U ||
      show_rate.denominator == 0U) {
    return std::nullopt;
  }
  const core::i64 source_in =
      integer(take, scene::schema::source_in).value_or(0);
  const core::i64 front_offset = plate_offset(take, front_plate);
  std::optional<core::i64> shortest;
  for (std::size_t slot = 0U; slot < assets::plateset_slot_count; ++slot) {
    if (!has_plate(take, slot)) {
      continue;
    }
    const core::i64 frames =
        integer(take, plate_name(slot, scene::schema::plate_count_suffix))
            .value_or(0);
    // Where this plate is when the take's first frame shows, as evaluation
    // places it: as far from the front's trim as its offset is from the
    // front's.
    const core::i64 plate_in = std::max<core::i64>(
        0, source_in + plate_offset(take, slot) - front_offset);
    const core::i64 remaining = frames - plate_in;
    const core::i64 playable =
        remaining <= 0
            ? 0
            : remaining * static_cast<core::i64>(show_rate.numerator) *
                  denominator /
                  (static_cast<core::i64>(show_rate.denominator) * numerator);
    shortest = shortest.has_value() ? std::min(*shortest, playable) : playable;
  }
  return shortest;
}

// `clip` plays `set` from here on, at the position in the take it was at:
// its trim moves by as much as the front plate's sync offset did, and it is
// shortened if a plate would now run out before it ends. Never to nothing:
// a take with no frame left in common keeps one, and readiness says why.
void retake(doc::Node &clip, const assets::PlatesetAsset &set,
            const Plates &plates, scene::MediaFrameRate show_rate) {
  const core::i64 old_front = is_take(clip) ? plate_offset(clip, front_plate) : 0;
  const core::i64 source_in =
      integer(clip, scene::schema::source_in).value_or(0);
  put_take(clip, set, plates);
  put(clip, scene::schema::source_in,
      whole(std::max<core::i64>(
          0, source_in - old_front + set.slots[front_plate].offset)));
  const core::i64 duration =
      integer(clip, scene::schema::clip_duration).value_or(0);
  const std::optional<core::i64> playable = take_playable(clip, show_rate);
  if (duration > 0 && playable.has_value() && *playable < duration) {
    put(clip, scene::schema::clip_duration,
        whole(std::max<core::i64>(*playable, 1)));
  }
}

[[nodiscard]] bool is_vehicle_track(const doc::Node &node) noexcept {
  return node.type == scene::schema::timeline_lane_type &&
         text(node, scene::schema::lane_kind) ==
             scene::schema::lane_kind_vehicle;
}

// The scene's vehicle tracks, front of the list first.
[[nodiscard]] std::vector<doc::NodeId> vehicle_tracks(const Nodes &nodes,
                                                      scene::SceneId scene) {
  std::vector<std::pair<std::int64_t, doc::NodeId>> found;
  for (const doc::Node &node : nodes.all) {
    if (is_vehicle_track(node) && in_scene(node, scene, nodes.fallback)) {
      found.emplace_back(
          integer(node, scene::schema::lane_order).value_or(0), node.id);
    }
  }
  std::sort(found.begin(), found.end());
  std::vector<doc::NodeId> tracks;
  tracks.reserve(found.size());
  for (const auto &entry : found) {
    tracks.push_back(entry.second);
  }
  return tracks;
}

// A vehicle track's slot screens, by slot; zero where it has none. Two
// screens claiming one slot leave it to the lower node id, as evaluation
// does.
using Screens = std::array<doc::NodeId, assets::plateset_slot_count>;

[[nodiscard]] Screens screens_of(const Nodes &nodes, doc::NodeId track) {
  Screens screens{};
  for (const doc::Node &node : nodes.all) {
    if (node.type != scene::schema::timeline_lane_type ||
        integer(node, scene::schema::lane_parent) !=
            static_cast<std::int64_t>(track.value())) {
      continue;
    }
    const auto key = std::find(scene::schema::vehicle_slot_keys.begin(),
                               scene::schema::vehicle_slot_keys.end(),
                               text(node, scene::schema::lane_slot));
    if (key == scene::schema::vehicle_slot_keys.end()) {
      continue;
    }
    doc::NodeId &screen = screens[static_cast<std::size_t>(
        key - scene::schema::vehicle_slot_keys.begin())];
    if (screen == doc::NodeId{} || node.id < screen) {
      screen = node.id;
    }
  }
  return screens;
}

// The scene's LED Car Process nodes in id order, and which of their image
// sockets already hold a wire (edge id per port).
struct Rig final {
  doc::NodeId node{};
  std::array<doc::NodeId, assets::plateset_slot_count> wired{};
};

[[nodiscard]] std::vector<Rig> rigs_in(const Nodes &nodes,
                                       scene::SceneId scene) {
  std::vector<Rig> rigs;
  for (const doc::Node &node : nodes.all) {
    if (node.type == compositor::schema::led_car_process_type &&
        in_scene(node, scene, nodes.fallback)) {
      rigs.push_back(Rig{node.id, {}});
    }
  }
  std::sort(rigs.begin(), rigs.end(),
            [](const Rig &left, const Rig &right) noexcept {
              return left.node < right.node;
            });
  for (const doc::Node &edge : nodes.all) {
    if (edge.type != compositor::schema::edge_type) {
      continue;
    }
    const auto to = integer(edge, compositor::schema::to_node);
    const auto port = integer(edge, compositor::schema::to_port);
    if (!to.has_value() || !port.has_value() || *port < 0 ||
        *port >= static_cast<std::int64_t>(assets::plateset_slot_count)) {
      continue;
    }
    for (Rig &rig : rigs) {
      if (rig.node.value() == static_cast<core::u64>(*to)) {
        rig.wired[static_cast<std::size_t>(*port)] = edge.id;
      }
    }
  }
  return rigs;
}

// The LED Car Process a vehicle track's screens already feed: the one a
// screen it gains later should join. Zero when none of them is wired to one.
[[nodiscard]] doc::NodeId rig_fed_by(const Nodes &nodes,
                                     const Screens &screens) {
  std::set<std::int64_t> layers;
  std::set<std::int64_t> rigs;
  for (const doc::Node &node : nodes.all) {
    if (node.type == compositor::schema::led_car_process_type) {
      rigs.insert(static_cast<std::int64_t>(node.id.value()));
    } else if (node.type == compositor::schema::scene_layer_type) {
      const auto named = integer(node, compositor::schema::scene_node);
      if (named.has_value() && *named > 0 &&
          std::find(screens.begin(), screens.end(),
                    doc::NodeId{static_cast<core::u64>(*named)}) !=
              screens.end()) {
        layers.insert(static_cast<std::int64_t>(node.id.value()));
      }
    }
  }
  doc::NodeId fed{};
  for (const doc::Node &edge : nodes.all) {
    if (edge.type != compositor::schema::edge_type) {
      continue;
    }
    const auto from = integer(edge, compositor::schema::from_node);
    const auto to = integer(edge, compositor::schema::to_node);
    if (!from.has_value() || !to.has_value() || !layers.contains(*from) ||
        !rigs.contains(*to)) {
      continue;
    }
    const doc::NodeId rig{static_cast<core::u64>(*to)};
    if (fed == doc::NodeId{} || rig < fed) {
      fed = rig;
    }
  }
  return fed;
}

[[nodiscard]] doc::Node graph_node(doc::NodeId id, compositor::NodeKind kind,
                                   float x, float y, scene::SceneId scene,
                                   doc::NodeId scene_node = {}) {
  editor::CompositorAddIntent add{};
  add.kind = kind;
  add.editor_x = x;
  add.editor_y = y;
  add.scene_node = scene_node;
  return make_compositor_node(id, add, doc::NodeId{scene.value()});
}

struct NewScreen final {
  PlatesetSlot slot{PlatesetSlot::Front};
  doc::NodeId lane{};
};

struct Wiring final {
  doc::NodeId rig{};
  bool created{};
  std::vector<PlatesetSlot> skipped;
};

// Gives each new screen a Scene Layer wired to its slot's socket of an LED
// Car Process: `join` when the track's other screens already feed one,
// otherwise the scene's first unless `choice` asks for a new one. A socket
// that already holds a wire keeps it unless `choice` is Replace. A rig made
// here reaches the picture: straight into Output when it is free, otherwise
// composited over what Output already shows.
[[nodiscard]] Wiring wire_screens(doc::Root &document, core::u64 &next_node,
                                  scene::SceneId scene,
                                  const std::vector<NewScreen> &screens,
                                  PlatesetRigChoice choice, doc::NodeId join) {
  Wiring wiring{};
  wiring.rig = join;
  if (screens.empty()) {
    return wiring;
  }
  const auto allocate = [&document, &next_node]() {
    while (document.contains(doc::NodeId{next_node})) {
      ++next_node;
    }
    return doc::NodeId{next_node++};
  };
  const Nodes nodes = nodes_of(document);
  std::vector<Rig> rigs = rigs_in(nodes, scene);
  float right_edge = 0.0F;
  for (const doc::Node &node : nodes.all) {
    if (compositor::schema::is_node_type(node.type) &&
        in_scene(node, scene, nodes.fallback)) {
      const auto found = node.properties.find(compositor::schema::editor_x);
      if (found != node.properties.end()) {
        if (const auto *const x = std::get_if<double>(&found->second)) {
          right_edge = std::max(right_edge, static_cast<float>(*x));
        }
      }
    }
  }
  Rig rig{};
  const auto joined =
      std::find_if(rigs.begin(), rigs.end(), [join](const Rig &candidate) {
        return candidate.node == join;
      });
  const bool reuse = joined != rigs.end() ||
                     (!rigs.empty() && choice != PlatesetRigChoice::NewRig);
  if (joined != rigs.end()) {
    rig = *joined;
  } else if (reuse) {
    rig = rigs.front();
  } else {
    rig.node = allocate();
    wiring.created = true;
  }
  wiring.rig = rig.node;
  // A new rig stands left of the Output it feeds, its Scene Layers left of
  // it, so the picture reads left to right; with no Output yet, to the
  // right of everything already in the graph.
  const auto position_of = [](const doc::Node &node, std::string_view axis) {
    const auto found = node.properties.find(axis);
    const auto *const value = found != node.properties.end()
                                  ? std::get_if<double>(&found->second)
                                  : nullptr;
    return value != nullptr ? static_cast<float>(*value) : 0.0F;
  };
  const doc::Node *existing_output = nullptr;
  for (const doc::Node &node : nodes.all) {
    if (node.type == compositor::schema::output_type &&
        in_scene(node, scene, nodes.fallback) &&
        (existing_output == nullptr || node.id < existing_output->id)) {
      existing_output = std::addressof(node);
    }
  }
  float rig_x = right_edge + 560.0F;
  float rig_y = 120.0F;
  if (existing_output != nullptr) {
    rig_x = position_of(*existing_output, compositor::schema::editor_x) -
            340.0F;
    rig_y = position_of(*existing_output, compositor::schema::editor_y);
  }
  const float layers_x = reuse ? right_edge + 280.0F : rig_x - 300.0F;
  // Scene Layer nodes stand about 110 units tall with their ports.
  constexpr float layer_spacing = 130.0F;
  const float layers_top =
      rig_y - layer_spacing * 0.5F * static_cast<float>(screens.size() - 1U);
  if (!reuse) {
    document = document.set(graph_node(
        rig.node, compositor::NodeKind::LedCarProcess, rig_x, rig_y, scene));
  }
  const bool replace = choice == PlatesetRigChoice::Replace;
  for (std::size_t index = 0U; index < screens.size(); ++index) {
    const PlatesetSlot slot = screens[index].slot;
    const auto port = static_cast<core::u8>(slot);
    const doc::NodeId occupied = rig.wired[port];
    if (occupied != doc::NodeId{}) {
      if (!replace) {
        wiring.skipped.push_back(slot);
        continue;
      }
      if (auto erased = document.erase(occupied)) {
        document = std::move(*erased);
      }
    }
    const doc::NodeId layer = allocate();
    document = document.set(graph_node(
        layer, compositor::NodeKind::SceneLayer, layers_x,
        layers_top + layer_spacing * static_cast<float>(index), scene,
        screens[index].lane));
    document = document.set(make_compositor_edge(
        allocate(), compositor::EdgeEndpoint{layer, 0U},
        compositor::EdgeEndpoint{rig.node, port}));
  }
  if (reuse) {
    return wiring;
  }

  doc::NodeId output_id{};
  if (existing_output == nullptr) {
    output_id = allocate();
    document = document.set(graph_node(output_id, compositor::NodeKind::Output,
                                       rig_x + 340.0F, rig_y, scene));
  } else {
    output_id = existing_output->id;
  }
  const doc::Node *feeding = nullptr;
  for (const doc::Node &edge : nodes.all) {
    if (edge.type == compositor::schema::edge_type &&
        integer(edge, compositor::schema::to_node) ==
            static_cast<std::int64_t>(output_id.value())) {
      feeding = std::addressof(edge);
    }
  }
  const compositor::EdgeEndpoint rig_out{rig.node, 0U};
  if (feeding == nullptr) {
    document = document.set(make_compositor_edge(
        allocate(), rig_out, compositor::EdgeEndpoint{output_id, 0U}));
    return wiring;
  }
  const auto from = integer(*feeding, compositor::schema::from_node);
  const doc::Node *const source =
      from.has_value()
          ? document.get(doc::NodeId{static_cast<core::u64>(*from)})
          : nullptr;
  if (source != nullptr && source->type == compositor::schema::composite_type) {
    doc::Node edge = make_compositor_edge(
        allocate(), rig_out, compositor::EdgeEndpoint{source->id, 0U});
    put(edge, compositor::schema::order, whole(1'000'000));
    document = document.set(std::move(edge));
  } else if (from.has_value()) {
    const doc::NodeId composite = allocate();
    document = document.set(
        graph_node(composite, compositor::NodeKind::Composite, rig_x + 170.0F,
                   rig_y + 140.0F, scene));
    if (auto erased = document.erase(feeding->id)) {
      document = std::move(*erased);
    }
    const auto from_port =
        integer(*feeding, compositor::schema::from_port).value_or(0);
    document = document.set(make_compositor_edge(
        allocate(),
        compositor::EdgeEndpoint{doc::NodeId{static_cast<core::u64>(*from)},
                                 static_cast<core::u8>(from_port)},
        compositor::EdgeEndpoint{composite, 0U}));
    doc::Node over = make_compositor_edge(
        allocate(), rig_out, compositor::EdgeEndpoint{composite, 0U});
    put(over, compositor::schema::order, whole(1));
    document = document.set(std::move(over));
    document = document.set(make_compositor_edge(
        allocate(), compositor::EdgeEndpoint{composite, 0U},
        compositor::EdgeEndpoint{output_id, 0U}));
  }
  return wiring;
}

// A new track in `scene`, named `name`, with `extra` written onto it.
template <typename Author>
[[nodiscard]] std::optional<doc::NodeId>
make_track(doc::Root &document, core::u64 &next_node, scene::SceneId scene,
           std::string name, Author &&author) {
  LaneAuthorRequest request{};
  request.action = LaneAuthorAction::Create;
  request.scene = scene;
  request.name = std::move(name);
  while (document.contains(doc::NodeId{next_node})) {
    ++next_node;
  }
  auto planned = plan_lane_authoring(document, request, next_node);
  if (!planned) {
    return std::nullopt;
  }
  document = std::move(planned.value().document);
  next_node = planned.value().next_node;
  const doc::NodeId id = planned.value().created;
  const doc::Node *const created = document.get(id);
  if (created == nullptr) {
    return std::nullopt;
  }
  doc::Node track = *created;
  author(track);
  document = document.set(std::move(track));
  return id;
}

// A slot screen on `track` for each plate it has none for: named after the
// slot, in the plate's own raster, and as wide as closes the octagon on the
// rig's default radius. Nothing when the scene cannot take another track.
[[nodiscard]] std::optional<std::vector<NewScreen>>
make_missing_screens(doc::Root &document, core::u64 &next_node,
                     scene::SceneId scene, doc::NodeId track,
                     const Plates &plates) {
  const Screens existing = screens_of(nodes_of(document), track);
  std::vector<NewScreen> made;
  for (std::size_t slot = 0U; slot < plates.size(); ++slot) {
    const assets::MediaAsset *const plate = plates[slot];
    if (plate == nullptr || existing[slot] != doc::NodeId{}) {
      continue;
    }
    const auto role = static_cast<PlatesetSlot>(slot);
    const std::optional<doc::NodeId> screen = make_track(
        document, next_node, scene,
        std::string{assets::plateset_slot_label(role)},
        [&](doc::Node &lane) {
          put(lane, scene::schema::lane_parent,
              whole(static_cast<std::int64_t>(track.value())));
          put(lane, scene::schema::lane_slot,
              doc::PropertyValue{
                  std::string{scene::schema::vehicle_slot_keys[slot]}});
          if (plate->dimensions.has_value()) {
            put(lane, scene::schema::lane_raster_width,
                whole(plate->dimensions->width));
            put(lane, scene::schema::lane_raster_height,
                whole(plate->dimensions->height));
          }
          put(lane, scene::schema::scale_x, doc::PropertyValue{plate_width});
        });
    if (!screen.has_value()) {
      return std::nullopt;
    }
    made.push_back(NewScreen{role, *screen});
  }
  return made;
}

[[nodiscard]] std::vector<doc::NodeId>
lanes_of(const std::vector<NewScreen> &screens) {
  std::vector<doc::NodeId> lanes;
  lanes.reserve(screens.size());
  for (const NewScreen &screen : screens) {
    lanes.push_back(screen.lane);
  }
  return lanes;
}

} // namespace

core::i64 plateset_show_frames(core::i64 source_frames,
                               assets::MediaFrameRate source,
                               scene::MediaFrameRate show_rate) {
  if (source_frames <= 0 || source.numerator == 0U ||
      source.denominator == 0U || show_rate.numerator == 0U ||
      show_rate.denominator == 0U) {
    return 0;
  }
  // A day of 120 fps plates at a 1001-denominator show rate is still far
  // inside 64 bits.
  const auto numerator = static_cast<std::int64_t>(source_frames) *
                         show_rate.numerator * source.denominator;
  const auto denominator =
      static_cast<std::int64_t>(show_rate.denominator) * source.numerator;
  return static_cast<core::i64>((numerator + denominator - 1) / denominator);
}

bool plateset_rig_busy(const doc::Root &document, scene::SceneId scene,
                       doc::NodeId plateset) {
  const assets::AssetRegistry registry = registry_of(document);
  const assets::PlatesetAsset *const set = plateset_in(registry, plateset);
  if (set == nullptr) {
    return false;
  }
  const Nodes nodes = nodes_of(document);
  // A scene with a vehicle track has nothing to ask: the take goes on it.
  if (!vehicle_tracks(nodes, scene).empty()) {
    return false;
  }
  const std::vector<Rig> rigs = rigs_in(nodes, scene);
  if (rigs.empty()) {
    return false;
  }
  for (const PlatesetSlot slot : assets::all_plateset_slots) {
    const std::size_t index = static_cast<std::size_t>(slot);
    if (set->slots[index].media != doc::NodeId{} &&
        rigs.front().wired[index] != doc::NodeId{}) {
      return true;
    }
  }
  return false;
}

core::Result<PlatesetPlacementPlan, PlatesetAuthoringError>
plan_plateset_placement(const doc::Root &document,
                        const PlatesetPlacementRequest &request,
                        core::u64 next_node) {
  using Result = core::Result<PlatesetPlacementPlan, PlatesetAuthoringError>;
  try {
    const assets::AssetRegistry registry = registry_of(document);
    const assets::PlatesetAsset *const set =
        plateset_in(registry, request.plateset);
    if (set == nullptr) {
      return Result::failure(PlatesetAuthoringError::NotFound);
    }
    const assets::PlatesetReadiness readiness =
        assets::evaluate_plateset(*set, registry);
    const Plates plates = plates_of(*set, registry);
    if (!readiness.ready() || !readiness.rate.has_value() ||
        readiness.common_frames <= 0 || plates[front_plate] == nullptr) {
      return Result::failure(PlatesetAuthoringError::NotReady);
    }
    const core::i64 duration = plateset_show_frames(
        readiness.common_frames, *readiness.rate, request.show_rate);
    if (duration <= 0) {
      return Result::failure(PlatesetAuthoringError::NotReady);
    }

    PlatesetPlacementPlan plan;
    plan.document = document;
    plan.next_node = next_node;
    const Nodes nodes = nodes_of(document);

    // The track: the one the operator let go on, else the scene's own
    // unless a second car was asked for, else a new one.
    if (request.track != doc::NodeId{}) {
      const doc::Node *const chosen = document.get(request.track);
      if (chosen == nullptr || !is_vehicle_track(*chosen) ||
          !in_scene(*chosen, request.scene, nodes.fallback)) {
        return Result::failure(PlatesetAuthoringError::NotAVehicleTrack);
      }
      plan.track = request.track;
    } else if (request.rig != PlatesetRigChoice::NewRig) {
      const std::vector<doc::NodeId> tracks =
          vehicle_tracks(nodes, request.scene);
      if (!tracks.empty()) {
        plan.track = tracks.front();
      }
    }
    if (plan.track == doc::NodeId{}) {
      const std::size_t others = vehicle_tracks(nodes, request.scene).size();
      const std::optional<doc::NodeId> made = make_track(
          plan.document, plan.next_node, request.scene,
          others == 0U ? std::string{"Vehicle"}
                       : "Vehicle " + std::to_string(others + 1U),
          [](doc::Node &lane) {
            put(lane, scene::schema::lane_kind,
                doc::PropertyValue{
                    std::string{scene::schema::lane_kind_vehicle}});
          });
      if (!made.has_value()) {
        return Result::failure(PlatesetAuthoringError::InvalidScene);
      }
      plan.track = *made;
      plan.created_track = true;
    }

    // A screen for every plate the track has none for, wired into the rig
    // its other screens feed - or, for a new track, as the request says.
    const doc::NodeId join =
        plan.created_track
            ? doc::NodeId{}
            : rig_fed_by(nodes, screens_of(nodes, plan.track));
    const std::optional<std::vector<NewScreen>> screens = make_missing_screens(
        plan.document, plan.next_node, request.scene, plan.track, plates);
    if (!screens.has_value()) {
      return Result::failure(PlatesetAuthoringError::InvalidScene);
    }
    Wiring wiring = wire_screens(
        plan.document, plan.next_node, request.scene, *screens,
        plan.created_track ? request.rig : PlatesetRigChoice::Auto, join);
    plan.screens = lanes_of(*screens);
    plan.rig = wiring.rig;
    plan.created_rig = wiring.created;
    plan.skipped = std::move(wiring.skipped);

    // The take: one clip, in front of whatever is already on the track.
    std::int64_t order = 0;
    for (const doc::Node &node : nodes.all) {
      if (node.type == scene::schema::media_type &&
          integer(node, scene::schema::lane_id) ==
              static_cast<std::int64_t>(plan.track.value())) {
        order = std::max(
            order, integer(node, scene::schema::clip_order).value_or(0) + 1);
      }
    }
    doc::Node clip{};
    while (plan.document.contains(doc::NodeId{plan.next_node})) {
      ++plan.next_node;
    }
    clip.id = doc::NodeId{plan.next_node++};
    clip.type = std::string{scene::schema::media_type};
    put(clip, scene::schema::scene_id,
        whole(static_cast<std::int64_t>(request.scene.value())));
    put_take(clip, *set, plates);
    put(clip, scene::schema::clip_start, whole(request.start));
    put(clip, scene::schema::clip_duration, whole(duration));
    // The front plate's own skip: the first frame every plate covers.
    put(clip, scene::schema::source_in, whole(set->slots[front_plate].offset));
    put(clip, scene::schema::lane_id,
        whole(static_cast<std::int64_t>(plan.track.value())));
    put(clip, scene::schema::clip_order, whole(order));
    plan.clip = clip.id;
    plan.document = plan.document.set(std::move(clip));
    return Result::success(std::move(plan));
  } catch (const std::bad_alloc &) {
    return Result::failure(PlatesetAuthoringError::AllocationFailed);
  }
}

core::Result<PlatesetSwapPlan, PlatesetAuthoringError>
plan_plateset_swap(const doc::Root &document, doc::NodeId clip,
                   doc::NodeId plateset, scene::MediaFrameRate show_rate,
                   core::u64 next_node) {
  using Result = core::Result<PlatesetSwapPlan, PlatesetAuthoringError>;
  try {
    const doc::Node *const playing = document.get(clip);
    if (playing == nullptr || !is_take(*playing)) {
      return Result::failure(PlatesetAuthoringError::NotATake);
    }
    const auto lane = integer(*playing, scene::schema::lane_id);
    const doc::Node *const track =
        lane.has_value() && *lane > 0
            ? document.get(doc::NodeId{static_cast<core::u64>(*lane)})
            : nullptr;
    if (track == nullptr || !is_vehicle_track(*track)) {
      return Result::failure(PlatesetAuthoringError::NotATake);
    }
    const assets::AssetRegistry registry = registry_of(document);
    const assets::PlatesetAsset *const incoming =
        plateset_in(registry, plateset);
    if (incoming == nullptr) {
      return Result::failure(PlatesetAuthoringError::NotFound);
    }
    const Plates plates = plates_of(*incoming, registry);
    if (!assets::evaluate_plateset(*incoming, registry).ready() ||
        plates[front_plate] == nullptr) {
      return Result::failure(PlatesetAuthoringError::NotReady);
    }

    PlatesetSwapPlan plan;
    plan.document = document;
    plan.next_node = next_node;
    for (std::size_t slot = 0U; slot < plates.size(); ++slot) {
      if (has_plate(*playing, slot) && plates[slot] == nullptr) {
        plan.unmatched.push_back(static_cast<PlatesetSlot>(slot));
      }
    }
    doc::Node take = *playing;
    retake(take, *incoming, plates, show_rate);
    if (take == *playing) {
      return Result::failure(PlatesetAuthoringError::NothingToSwap);
    }
    plan.document = plan.document.set(std::move(take));

    // A new take may fill slots the track never had a screen for.
    const Nodes nodes = nodes_of(document);
    const scene::SceneId scene{static_cast<core::u64>(std::max<std::int64_t>(
        integer(*track, scene::schema::scene_id)
            .value_or(static_cast<std::int64_t>(nodes.fallback.value())),
        0))};
    const doc::NodeId join = rig_fed_by(nodes, screens_of(nodes, track->id));
    const std::optional<std::vector<NewScreen>> screens = make_missing_screens(
        plan.document, plan.next_node, scene, track->id, plates);
    if (!screens.has_value()) {
      return Result::failure(PlatesetAuthoringError::InvalidScene);
    }
    static_cast<void>(wire_screens(plan.document, plan.next_node, scene,
                                   *screens, PlatesetRigChoice::Auto, join));
    plan.screens = lanes_of(*screens);
    return Result::success(std::move(plan));
  } catch (const std::bad_alloc &) {
    return Result::failure(PlatesetAuthoringError::AllocationFailed);
  }
}

doc::Root follow_plateset(doc::Root document, doc::NodeId plateset,
                          scene::MediaFrameRate show_rate) {
  const assets::AssetRegistry registry = registry_of(document);
  const assets::PlatesetAsset *const set = plateset_in(registry, plateset);
  if (set == nullptr) {
    return document;
  }
  const Plates plates = plates_of(*set, registry);
  if (!assets::evaluate_plateset(*set, registry).ready() ||
      plates[front_plate] == nullptr) {
    return document;
  }
  const doc::Delta contents = doc::diff(doc::Root{}, document);
  for (const doc::NodeChange &change : contents.changes) {
    if (!change.after || !is_take(*change.after) ||
        integer(*change.after, scene::schema::plateset_asset) !=
            static_cast<std::int64_t>(plateset.value())) {
      continue;
    }
    doc::Node take = *change.after;
    retake(take, *set, plates, show_rate);
    if (take != *change.after) {
      document = document.set(std::move(take));
    }
  }
  return document;
}

std::string plateset_authoring_error_text(PlatesetAuthoringError error) {
  switch (error) {
  case PlatesetAuthoringError::NotFound:
    return "That plateset is no longer in the Library";
  case PlatesetAuthoringError::NotReady:
    return "The plateset is not ready: open it in the Asset Viewer to see why";
  case PlatesetAuthoringError::NotATake:
    return "Drop a plateset on a take to play it instead";
  case PlatesetAuthoringError::NotAVehicleTrack:
    return "Drop a plateset on a vehicle track to add a take to it";
  case PlatesetAuthoringError::NothingToSwap:
    return "That take already plays this plateset";
  case PlatesetAuthoringError::InvalidScene:
    return "The scene could not take another track";
  case PlatesetAuthoringError::AllocationFailed:
    return "Out of memory while placing the plateset";
  }
  return {};
}

} // namespace mediaserver::app
