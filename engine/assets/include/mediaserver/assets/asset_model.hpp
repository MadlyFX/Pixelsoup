#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/doc/document.hpp>

#include <map>

namespace mediaserver::assets {

struct AssetCounters final {
    // Cumulative parse failures since construction or the last reset.
    core::u64 rejected{};
    // Currently valid document nodes, before source deduplication/capacity.
    core::u32 media{};
    core::u32 models{};
    core::u32 projection_meshes{};
    core::u32 platesets{};
    // Currently valid nodes omitted because a lower NodeId owns their exact
    // source string within the same asset kind. The same source may coexist as
    // Media, Model, and ProjectionMesh because those are distinct manifest
    // roles.
    core::u32 duplicate_sources{};

    bool operator==(const AssetCounters&) const = default;

    [[nodiscard]] core::u32 over_capacity() const noexcept;
};

class AssetModel final {
  public:
    AssetModel() = default;

    void reset(const doc::Root& root);
    void apply(const doc::Delta& delta);

    [[nodiscard]] AssetRegistry evaluate() const;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] const AssetCounters& counters() const noexcept {
        return counters_;
    }

  private:
    // NodeId order makes both capacity truncation and lowest-id
    // (kind, source) wins deterministic regardless of checkpoint/delta
    // arrival order.
    std::map<doc::NodeId, Asset> entries_;
    std::map<doc::NodeId, AssetFolder> folders_;
    AssetCounters counters_{};

    void put(const doc::Node& node);
    void forget(doc::NodeId id) noexcept;
    void recount() noexcept;
};

} // namespace mediaserver::assets
