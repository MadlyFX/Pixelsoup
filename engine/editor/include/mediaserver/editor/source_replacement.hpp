#pragma once

#include <mediaserver/editor/element.hpp>
#include <mediaserver/assets/asset_registry.hpp>

namespace mediaserver::editor {

// Exactly one source is present: a Library asset replaces a placement's source;
// a Timeline layer replaces a compositor input's binding.
struct SourceReplacementIntent final {
  doc::NodeId target{};
  doc::NodeId asset{};
  doc::NodeId layer{};
  scene::SceneId scene{};
  core::u64 gesture{};
  bool operator==(const SourceReplacementIntent &) const = default;
};

[[nodiscard]] inline bool can_replace_source(const ElementInfo &target,
                                             const assets::Asset &asset) noexcept {
  if (target.locked || target.linked) return false;
  return (target.kind == ElementKind::Media &&
          assets::asset_kind(asset) == assets::AssetKind::Media) ||
         (target.kind == ElementKind::Model &&
          assets::asset_kind(asset) == assets::AssetKind::Model);
}

[[nodiscard]] inline bool can_replace_layer(const ElementInfo &target,
                                            const ElementInfo &source) noexcept {
  return !target.locked && !source.locked && target.node != source.node &&
         target.kind == source.kind &&
         (target.kind == ElementKind::Media || target.kind == ElementKind::Model ||
          target.kind == ElementKind::Object || target.kind == ElementKind::Group ||
          target.kind == ElementKind::Track);
}

} // namespace mediaserver::editor
