#pragma once

#include <mediaserver/editor/source_replacement.hpp>
#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/compositor/compositor_schema.hpp>
#include <mediaserver/scene/linked_scene.hpp>

namespace mediaserver::app {

namespace replacement_detail {
inline core::i64 integer(const doc::Node &node, std::string_view key, core::i64 fallback = 0) {
  const auto it = node.properties.find(key);
  const auto *value = it != node.properties.end() ? std::get_if<core::i64>(&it->second) : nullptr;
  return value != nullptr ? *value : fallback;
}
inline bool locked(const doc::Node &node) {
  const auto it = node.properties.find(scene::schema::locked);
  const auto *value = it != node.properties.end() ? std::get_if<bool>(&it->second) : nullptr;
  return value != nullptr && *value;
}
inline bool belongs(const doc::Node &node, scene::SceneId scene) {
  // Legacy unscoped placements are supplied by the current scene projection.
  return integer(node, scene::schema::scene_id, static_cast<core::i64>(scene.value())) ==
      static_cast<core::i64>(scene.value());
}
} // namespace replacement_detail

// Resolves source identities against the projected document at commit time.
// No file IO, probe, or import runs on the control path. A complete PutNode is
// one undoable change and preserves every unrelated placement/graph property.
[[nodiscard]] inline bool schedule_source_replacement(
    cmd::DirectorScheduler &scheduler, const editor::SourceReplacementIntent &request,
    cmd::FrameNumber apply_at) {
  using namespace replacement_detail;
  const auto &root = scheduler.projected_document();
  const auto *target = root.get(request.target);
  if (target == nullptr || request.scene == scene::SceneId{} ||
      !belongs(*target, request.scene) || locked(*target) ||
      (request.asset == doc::NodeId{}) == (request.layer == doc::NodeId{})) return true;
  auto changed = *target;
  cmd::UndoContextId context{2U};
  if (request.asset != doc::NodeId{}) {
    if (scene::is_linked_occurrence(*target)) return true;
    const auto *asset_node = root.get(request.asset);
    if (asset_node == nullptr) return true;
    assets::AssetModel model;
    model.reset(doc::Root{}.set(*asset_node));
    const auto registry = model.evaluate();
    const auto *asset = registry.asset(request.asset);
    if (asset == nullptr) return true;
    const bool media = target->type == scene::schema::media_type &&
        assets::asset_kind(*asset) == assets::AssetKind::Media;
    const bool model_source = target->type == scene::schema::model_type &&
        assets::asset_kind(*asset) == assets::AssetKind::Model;
    if (!media && !model_source) return true;
    changed.properties.insert_or_assign(std::string{scene::schema::source},
                                        std::string{assets::asset_source(*asset)});
    changed.properties.insert_or_assign(std::string{scene::schema::asset_id},
                                        static_cast<core::i64>(request.asset.value()));
    if (media) {
      const auto &source = std::get<assets::MediaAsset>(*asset);
      changed.properties.insert_or_assign(std::string{scene::schema::sequence_start}, core::i64{0});
      changed.properties.insert_or_assign(std::string{scene::schema::sequence_count},
                                          static_cast<core::i64>(source.frame_count.value_or(1U)));
      changed.properties.erase(std::string{scene::schema::source_rate_numerator});
      changed.properties.erase(std::string{scene::schema::source_rate_denominator});
      if (source.frame_rate.has_value()) {
        changed.properties.insert_or_assign(std::string{scene::schema::source_rate_numerator},
                                            static_cast<core::i64>(source.frame_rate->numerator));
        changed.properties.insert_or_assign(std::string{scene::schema::source_rate_denominator},
                                            static_cast<core::i64>(source.frame_rate->denominator));
      }
    }
  } else {
    if (target->type != compositor::schema::scene_layer_type) return true;
    const auto old_id = integer(*target, compositor::schema::scene_node);
    const auto *old_layer = old_id > 0 ? root.get(doc::NodeId{static_cast<core::u64>(old_id)}) : nullptr;
    const auto *source = root.get(request.layer);
    if (old_layer == nullptr || source == nullptr || locked(*old_layer) || locked(*source) ||
        !belongs(*old_layer, request.scene) || !belongs(*source, request.scene) ||
        old_layer->type != source->type) return true;
    const auto &type = source->type;
    if (type != scene::schema::media_type && type != scene::schema::model_type &&
        type != scene::schema::object_type && type != scene::schema::group_type &&
        type != scene::schema::timeline_lane_type) return true;
    changed.properties.insert_or_assign(std::string{compositor::schema::scene_node},
                                        static_cast<core::i64>(request.layer.value()));
    context = cmd::UndoContextId{6U};
  }
  if (changed == *target) return true;
  return static_cast<bool>(scheduler.schedule_edit(cmd::PutNode{std::move(changed)},
      cmd::PutNode{*target}, context, cmd::TransactionId{request.gesture}, apply_at));
}

} // namespace mediaserver::app
