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
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <string_view>

namespace mediaserver::app {
namespace {

using assets::PlatesetSlot;

// Wall plates this wide, on the rig's default 4 m radius, meet at their
// edges: the side of the octagon the eight sockets stand on.
constexpr double plate_width = 3.32;

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

[[nodiscard]] std::optional<PlatesetSlot> slot_from_key(std::string_view key) {
  for (const PlatesetSlot slot : assets::all_plateset_slots) {
    if (assets::plateset_slot_key(slot) == key) {
      return slot;
    }
  }
  return std::nullopt;
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
    if (!readiness.ready() || !readiness.rate.has_value() ||
        readiness.common_frames <= 0) {
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
    const auto allocate = [&plan]() {
      while (plan.document.contains(doc::NodeId{plan.next_node})) {
        ++plan.next_node;
      }
      return doc::NodeId{plan.next_node++};
    };

    // One track and one clip per plate, in socket order.
    std::vector<PlatesetSlot> placed;
    doc::NodeId take{};
    for (const PlatesetSlot slot : assets::all_plateset_slots) {
      const assets::PlatesetSlotAssignment &assignment = set->slot(slot);
      const assets::MediaAsset *const media =
          media_in(registry, assignment.media);
      if (media == nullptr) {
        continue;
      }
      LaneAuthorRequest lane_request{};
      lane_request.action = LaneAuthorAction::Create;
      lane_request.scene = request.scene;
      // The slot first: a long take name is shortened in the middle, and
      // the slot is what tells one track from the next.
      lane_request.name = std::string{assets::plateset_slot_label(slot)} +
                          " / " + set->display_name;
      if (lane_request.name.size() > 128U) {
        lane_request.name = std::string{assets::plateset_slot_label(slot)};
      }
      while (plan.document.contains(doc::NodeId{plan.next_node})) {
        ++plan.next_node;
      }
      auto lane_plan =
          plan_lane_authoring(plan.document, lane_request, plan.next_node);
      if (!lane_plan) {
        return Result::failure(PlatesetAuthoringError::InvalidScene);
      }
      plan.document = std::move(lane_plan.value().document);
      plan.next_node = lane_plan.value().next_node;
      const doc::NodeId lane_id = lane_plan.value().created;
      // The screen takes the plate's own raster, and a width that closes the
      // octagon on the rig's default radius.
      if (const doc::Node *const created = plan.document.get(lane_id)) {
        doc::Node lane = *created;
        if (media->dimensions.has_value()) {
          put(lane, scene::schema::lane_raster_width,
              whole(media->dimensions->width));
          put(lane, scene::schema::lane_raster_height,
              whole(media->dimensions->height));
        }
        put(lane, scene::schema::scale_x, doc::PropertyValue{plate_width});
        plan.document = plan.document.set(std::move(lane));
      }

      doc::Node clip{};
      clip.id = allocate();
      if (take == doc::NodeId{}) {
        take = clip.id;
      }
      clip.type = std::string{scene::schema::media_type};
      put(clip, scene::schema::scene_id,
          whole(static_cast<std::int64_t>(request.scene.value())));
      put(clip, scene::schema::source, doc::PropertyValue{media->source});
      put(clip, scene::schema::asset_id,
          whole(static_cast<std::int64_t>(media->id.value())));
      put(clip, scene::schema::sequence_start, whole(0));
      put(clip, scene::schema::sequence_count,
          whole(static_cast<std::int64_t>(*media->frame_count)));
      put(clip, scene::schema::source_rate_numerator,
          whole(media->frame_rate->numerator));
      put(clip, scene::schema::source_rate_denominator,
          whole(media->frame_rate->denominator));
      put(clip, scene::schema::clip_start, whole(request.start));
      put(clip, scene::schema::clip_duration, whole(duration));
      put(clip, scene::schema::source_in, whole(assignment.offset));
      put(clip, scene::schema::lane_id,
          whole(static_cast<std::int64_t>(lane_id.value())));
      put(clip, scene::schema::plateset_asset,
          whole(static_cast<std::int64_t>(set->id.value())));
      put(clip, scene::schema::plateset_slot,
          doc::PropertyValue{std::string{assets::plateset_slot_key(slot)}});
      put(clip, scene::schema::plateset_take,
          whole(static_cast<std::int64_t>(take.value())));
      if (set->half_resolution) {
        put(clip, scene::schema::media_resolution,
            doc::PropertyValue{
                std::string{scene::schema::media_resolution_half}});
      }
      plan.lanes.push_back(lane_id);
      plan.clips.push_back(clip.id);
      placed.push_back(slot);
      plan.document = plan.document.set(std::move(clip));
    }
    if (placed.empty()) {
      return Result::failure(PlatesetAuthoringError::NotReady);
    }

    // The rig: the scene's first LED Car Process unless asked for a new one.
    const Nodes nodes = nodes_of(plan.document);
    std::vector<Rig> rigs = rigs_in(nodes, request.scene);
    float right_edge = 0.0F;
    for (const doc::Node &node : nodes.all) {
      if (compositor::schema::is_node_type(node.type) &&
          in_scene(node, request.scene, nodes.fallback)) {
        const auto found = node.properties.find(compositor::schema::editor_x);
        if (found != node.properties.end()) {
          if (const auto *const x = std::get_if<double>(&found->second)) {
            right_edge = std::max(right_edge, static_cast<float>(*x));
          }
        }
      }
    }
    Rig rig{};
    const bool reuse =
        !rigs.empty() && request.rig != PlatesetRigChoice::NewRig;
    if (reuse) {
      rig = rigs.front();
    } else {
      rig.node = allocate();
      plan.created_rig = true;
    }
    plan.rig = rig.node;
    // A new rig stands left of the Output it feeds, its Scene Layers left
    // of it, so the picture reads left to right; with no Output yet, to the
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
          in_scene(node, request.scene, nodes.fallback) &&
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
        rig_y - layer_spacing * 0.5F * static_cast<float>(placed.size() - 1U);
    if (!reuse) {
      plan.document = plan.document.set(graph_node(
          rig.node, compositor::NodeKind::LedCarProcess, rig_x, rig_y,
          request.scene));
    }
    const bool replace = request.rig == PlatesetRigChoice::Replace;
    for (std::size_t index = 0U; index < placed.size(); ++index) {
      const PlatesetSlot slot = placed[index];
      const auto port = static_cast<core::u8>(slot);
      const doc::NodeId occupied = rig.wired[port];
      if (occupied != doc::NodeId{}) {
        if (!replace) {
          plan.skipped.push_back(slot);
          continue;
        }
        if (auto erased = plan.document.erase(occupied)) {
          plan.document = std::move(*erased);
        }
      }
      const doc::NodeId layer = allocate();
      plan.document = plan.document.set(graph_node(
          layer, compositor::NodeKind::SceneLayer, layers_x,
          layers_top + layer_spacing * static_cast<float>(index), request.scene,
          plan.lanes[index]));
      plan.document = plan.document.set(make_compositor_edge(
          allocate(), compositor::EdgeEndpoint{layer, 0U},
          compositor::EdgeEndpoint{rig.node, port}));
    }

    // A new rig reaches the picture: straight into Output when it is free,
    // otherwise composited over what Output already shows.
    if (!reuse) {
      const doc::Node *output = nullptr;
      for (const doc::Node &node : nodes.all) {
        if (node.type == compositor::schema::output_type &&
            in_scene(node, request.scene, nodes.fallback) &&
            (output == nullptr || node.id < output->id)) {
          output = std::addressof(node);
        }
      }
      doc::NodeId output_id{};
      if (output == nullptr) {
        output_id = allocate();
        plan.document = plan.document.set(
            graph_node(output_id, compositor::NodeKind::Output,
                       rig_x + 340.0F, rig_y, request.scene));
      } else {
        output_id = output->id;
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
        plan.document = plan.document.set(make_compositor_edge(
            allocate(), rig_out, compositor::EdgeEndpoint{output_id, 0U}));
      } else {
        const auto from = integer(*feeding, compositor::schema::from_node);
        const doc::Node *const source =
            from.has_value() ? plan.document.get(doc::NodeId{
                                   static_cast<core::u64>(*from)})
                             : nullptr;
        if (source != nullptr &&
            source->type == compositor::schema::composite_type) {
          doc::Node edge = make_compositor_edge(
              allocate(), rig_out, compositor::EdgeEndpoint{source->id, 0U});
          put(edge, compositor::schema::order, whole(1'000'000));
          plan.document = plan.document.set(std::move(edge));
        } else if (from.has_value()) {
          const doc::NodeId composite = allocate();
          plan.document = plan.document.set(
              graph_node(composite, compositor::NodeKind::Composite,
                         rig_x + 170.0F, rig_y + 140.0F, request.scene));
          if (auto erased = plan.document.erase(feeding->id)) {
            plan.document = std::move(*erased);
          }
          const auto from_port =
              integer(*feeding, compositor::schema::from_port).value_or(0);
          plan.document = plan.document.set(make_compositor_edge(
              allocate(),
              compositor::EdgeEndpoint{
                  doc::NodeId{static_cast<core::u64>(*from)},
                  static_cast<core::u8>(from_port)},
              compositor::EdgeEndpoint{composite, 0U}));
          doc::Node over = make_compositor_edge(
              allocate(), rig_out, compositor::EdgeEndpoint{composite, 0U});
          put(over, compositor::schema::order, whole(1));
          plan.document = plan.document.set(std::move(over));
          plan.document = plan.document.set(make_compositor_edge(
              allocate(), compositor::EdgeEndpoint{composite, 0U},
              compositor::EdgeEndpoint{output_id, 0U}));
        }
      }
    }
    return Result::success(std::move(plan));
  } catch (const std::bad_alloc &) {
    return Result::failure(PlatesetAuthoringError::AllocationFailed);
  }
}

std::vector<doc::NodeId> plateset_take_clips(const doc::Root &document,
                                             doc::NodeId clip) {
  std::vector<doc::NodeId> result;
  const doc::Node *const lead = document.get(clip);
  if (lead == nullptr || lead->type != scene::schema::media_type) {
    return result;
  }
  const auto take = integer(*lead, scene::schema::plateset_take);
  if (!take.has_value() || *take <= 0) {
    return result;
  }
  const auto start = integer(*lead, scene::schema::clip_start);
  const auto duration = integer(*lead, scene::schema::clip_duration);
  const auto scene_of = integer(*lead, scene::schema::scene_id);
  const doc::Delta contents = doc::diff(doc::Root{}, document);
  for (const doc::NodeChange &change : contents.changes) {
    if (!change.after || change.after->type != scene::schema::media_type) {
      continue;
    }
    const doc::Node &node = *change.after;
    if (integer(node, scene::schema::plateset_take) == take &&
        integer(node, scene::schema::clip_start) == start &&
        integer(node, scene::schema::clip_duration) == duration &&
        integer(node, scene::schema::scene_id) == scene_of) {
      result.push_back(node.id);
    }
  }
  return result;
}

core::Result<PlatesetSwapPlan, PlatesetAuthoringError>
plan_plateset_swap(const doc::Root &document, doc::NodeId clip,
                   doc::NodeId plateset) {
  using Result = core::Result<PlatesetSwapPlan, PlatesetAuthoringError>;
  try {
    const std::vector<doc::NodeId> take = plateset_take_clips(document, clip);
    if (take.empty()) {
      return Result::failure(PlatesetAuthoringError::NotATake);
    }
    const assets::AssetRegistry registry = registry_of(document);
    const assets::PlatesetAsset *const incoming =
        plateset_in(registry, plateset);
    if (incoming == nullptr) {
      return Result::failure(PlatesetAuthoringError::NotFound);
    }
    PlatesetSwapPlan plan;
    plan.document = document;
    bool changed = false;
    std::string old_name;
    for (const doc::NodeId id : take) {
      doc::Node node = *document.get(id);
      const std::optional<PlatesetSlot> slot =
          slot_from_key(text(node, scene::schema::plateset_slot));
      if (!slot.has_value()) {
        continue;
      }
      const assets::PlatesetSlotAssignment &next = incoming->slot(*slot);
      const assets::MediaAsset *const media = media_in(registry, next.media);
      if (media == nullptr || !media->frame_rate.has_value() ||
          !media->frame_count.has_value()) {
        plan.unmatched.push_back(*slot);
        continue;
      }
      // The take position: how far into the take this plate was playing,
      // which the new plate starts at past its own offset.
      core::i64 old_offset = 0;
      const auto previous =
          integer(node, scene::schema::plateset_asset).value_or(0);
      if (const assets::PlatesetAsset *const outgoing = plateset_in(
              registry, doc::NodeId{static_cast<core::u64>(previous)})) {
        old_offset = outgoing->slot(*slot).offset;
        old_name = outgoing->display_name;
      }
      const core::i64 source_in =
          integer(node, scene::schema::source_in).value_or(0);
      put(node, scene::schema::source_in,
          whole(std::max<core::i64>(0, source_in - old_offset + next.offset)));
      put(node, scene::schema::source, doc::PropertyValue{media->source});
      put(node, scene::schema::asset_id,
          whole(static_cast<std::int64_t>(media->id.value())));
      put(node, scene::schema::sequence_count,
          whole(static_cast<std::int64_t>(*media->frame_count)));
      put(node, scene::schema::source_rate_numerator,
          whole(media->frame_rate->numerator));
      put(node, scene::schema::source_rate_denominator,
          whole(media->frame_rate->denominator));
      put(node, scene::schema::plateset_asset,
          whole(static_cast<std::int64_t>(incoming->id.value())));
      // A track named after the old take is renamed after the new one.
      if (const auto lane = integer(node, scene::schema::lane_id);
          lane.has_value() && !old_name.empty()) {
        if (const doc::Node *const track =
                plan.document.get(doc::NodeId{static_cast<core::u64>(*lane)})) {
          const std::string_view name = text(*track, scene::schema::lane_name);
          if (name.ends_with(old_name) &&
              name.size() - old_name.size() + incoming->display_name.size() <=
                  128U) {
            doc::Node renamed = *track;
            put(renamed, scene::schema::lane_name,
                doc::PropertyValue{
                    std::string{name.substr(0U, name.size() - old_name.size())} +
                    incoming->display_name});
            plan.document = plan.document.set(std::move(renamed));
          }
        }
      }
      if (node != *document.get(id)) {
        changed = true;
        plan.document = plan.document.set(std::move(node));
      }
    }
    if (!changed) {
      return Result::failure(PlatesetAuthoringError::NothingToSwap);
    }
    return Result::success(std::move(plan));
  } catch (const std::bad_alloc &) {
    return Result::failure(PlatesetAuthoringError::AllocationFailed);
  }
}

doc::Root follow_plateset_offsets(const doc::Root &before, doc::Root after,
                                  doc::NodeId plateset,
                                  scene::MediaFrameRate show_rate) {
  const assets::AssetRegistry old_registry = registry_of(before);
  const assets::AssetRegistry new_registry = registry_of(after);
  const assets::PlatesetAsset *const old_set =
      plateset_in(old_registry, plateset);
  const assets::PlatesetAsset *const new_set =
      plateset_in(new_registry, plateset);
  if (old_set == nullptr || new_set == nullptr) {
    return after;
  }
  const doc::Delta contents = doc::diff(doc::Root{}, after);
  for (const doc::NodeChange &change : contents.changes) {
    if (!change.after || change.after->type != scene::schema::media_type ||
        integer(*change.after, scene::schema::plateset_asset) !=
            static_cast<std::int64_t>(plateset.value())) {
      continue;
    }
    const std::optional<PlatesetSlot> slot =
        slot_from_key(text(*change.after, scene::schema::plateset_slot));
    if (!slot.has_value()) {
      continue;
    }
    const core::i64 shift =
        new_set->slot(*slot).offset - old_set->slot(*slot).offset;
    if (shift == 0) {
      continue;
    }
    doc::Node node = *change.after;
    const core::i64 source_in =
        integer(node, scene::schema::source_in).value_or(0);
    put(node, scene::schema::source_in,
        whole(std::max<core::i64>(0, source_in + shift)));
    after = after.set(std::move(node));
  }
  // A take now reaching past the end of one of its plates is shortened, as
  // one: every plate of it keeps the same length, the span all still cover.
  const doc::Delta shifted = doc::diff(doc::Root{}, after);
  std::set<doc::NodeId> seen;
  for (const doc::NodeChange &change : shifted.changes) {
    if (!change.after || seen.contains(change.id) ||
        change.after->type != scene::schema::media_type ||
        integer(*change.after, scene::schema::plateset_asset) !=
            static_cast<std::int64_t>(plateset.value())) {
      continue;
    }
    const std::vector<doc::NodeId> take = plateset_take_clips(after, change.id);
    core::i64 longest = std::numeric_limits<core::i64>::max();
    core::i64 duration = 0;
    for (const doc::NodeId id : take) {
      seen.insert(id);
      const doc::Node &plate = *after.get(id);
      duration = integer(plate, scene::schema::clip_duration).value_or(0);
      const core::i64 frames =
          integer(plate, scene::schema::sequence_count).value_or(0);
      const core::i64 numerator =
          integer(plate, scene::schema::source_rate_numerator).value_or(0);
      const core::i64 denominator =
          integer(plate, scene::schema::source_rate_denominator).value_or(0);
      if (frames <= 0 || numerator <= 0 || denominator <= 0) {
        continue;
      }
      // Show frames this plate can still play from its source in.
      const core::i64 remaining =
          frames - integer(plate, scene::schema::source_in).value_or(0);
      const core::i64 playable =
          remaining <= 0
              ? 0
              : remaining * static_cast<core::i64>(show_rate.numerator) *
                    denominator /
                    (static_cast<core::i64>(show_rate.denominator) * numerator);
      longest = std::min(longest, playable);
    }
    if (take.empty() || longest == std::numeric_limits<core::i64>::max() ||
        longest >= duration) {
      continue;
    }
    // Never to nothing: a take that no longer has a frame in common keeps
    // one, and readiness says why.
    const core::i64 allowed = std::max<core::i64>(longest, 1);
    for (const doc::NodeId id : take) {
      doc::Node plate = *after.get(id);
      put(plate, scene::schema::clip_duration, whole(allowed));
      after = after.set(std::move(plate));
    }
  }
  return after;
}

doc::Root follow_plateset_resolution(doc::Root document,
                                     doc::NodeId plateset) {
  const assets::AssetRegistry registry = registry_of(document);
  const assets::PlatesetAsset *const set = plateset_in(registry, plateset);
  if (set == nullptr) {
    return document;
  }
  const doc::Delta contents = doc::diff(doc::Root{}, document);
  for (const doc::NodeChange &change : contents.changes) {
    if (!change.after || change.after->type != scene::schema::media_type ||
        integer(*change.after, scene::schema::plateset_asset) !=
            static_cast<std::int64_t>(plateset.value())) {
      continue;
    }
    doc::Node node = *change.after;
    if (set->half_resolution) {
      put(node, scene::schema::media_resolution,
          doc::PropertyValue{
              std::string{scene::schema::media_resolution_half}});
    } else {
      node.properties.erase(std::string{scene::schema::media_resolution});
    }
    if (node != *change.after) {
      document = document.set(std::move(node));
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
    return "Drop a plateset on a plate of a placed plateset to swap the take";
  case PlatesetAuthoringError::NothingToSwap:
    return "That plateset has no plates for these slots";
  case PlatesetAuthoringError::InvalidScene:
    return "The scene could not take new tracks";
  case PlatesetAuthoringError::AllocationFailed:
    return "Out of memory while placing the plateset";
  }
  return {};
}

} // namespace mediaserver::app
