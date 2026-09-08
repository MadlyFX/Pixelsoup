#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/scene/scene.hpp>

#include <span>
#include <vector>

namespace mediaserver::render {

enum class SceneBridgeError : core::u8 {
    InvalidConfiguration,
    CapacityExceeded,
    AllocationFailed,
};

enum class GpuRecordKind : core::u8 {
    Mesh,
    Media,
    Model,
    DirectionalLight,
    PointLight,
};

// Deterministically ordered CPU mirror of one GPU scene record. Donor-engine
// entity/world types never cross this boundary.
struct GpuSceneRecord final {
    doc::NodeId source{};
    GpuRecordKind kind{GpuRecordKind::Mesh};
    core::u64 asset_identity{};
    core::u64 visual_identity{};
    scene::Mat4 world{};
    scene::Mat4 previous_world{};

    bool operator==(const GpuSceneRecord&) const = default;
};

struct SceneBridgeUpdate final {
    core::u32 records{};
    core::u32 added{};
    core::u32 updated{};
    core::u32 removed{};
    core::u32 unchanged{};
    core::u64 topology_digest{};
    core::u64 visual_digest{};

    bool operator==(const SceneBridgeUpdate&) const = default;
};

class SceneBridge final {
public:
    [[nodiscard]] static core::Result<SceneBridge, SceneBridgeError> create(
        core::u32 max_records);

    SceneBridge(const SceneBridge&) = delete;
    SceneBridge& operator=(const SceneBridge&) = delete;
    SceneBridge(SceneBridge&&) = default;
    SceneBridge& operator=(SceneBridge&&) = default;
    ~SceneBridge() = default;

    // No filesystem access, hashing of file bytes, or capacity growth. Asset
    // identity comes from the evaluated scene's stable document/path carrier.
    [[nodiscard]] core::Result<SceneBridgeUpdate, SceneBridgeError> update(
        const scene::Scene& scene) noexcept;

    // Builds the next mirror into preallocated scratch without changing the
    // committed records. HybridSceneRenderer uses this transaction so a
    // command-recording failure cannot become the next retry's previous frame.
    [[nodiscard]] core::Result<SceneBridgeUpdate, SceneBridgeError> prepare(
        const scene::Scene& scene) noexcept;
    void commit() noexcept;
    void discard() noexcept;

    [[nodiscard]] std::span<const GpuSceneRecord> records() const noexcept {
        return records_;
    }
    [[nodiscard]] core::u32 capacity() const noexcept { return capacity_; }

private:
    explicit SceneBridge(core::u32 capacity) : capacity_(capacity) {}

    core::u32 capacity_{};
    std::vector<GpuSceneRecord> records_;
    std::vector<GpuSceneRecord> scratch_;
    bool prepared_{};
};

} // namespace mediaserver::render
