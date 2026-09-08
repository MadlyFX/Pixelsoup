#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

#include <array>
#include <span>
#include <string_view>
#include <variant>

namespace mediaserver::render {

inline constexpr std::size_t max_render_graph_resources = 128U;
inline constexpr std::size_t max_render_graph_passes = 128U;
inline constexpr std::size_t max_render_graph_accesses = 768U;
inline constexpr std::size_t max_render_graph_transitions = 768U;

struct GraphResource final {
    core::u16 value{};

    [[nodiscard]] explicit operator bool() const noexcept { return value != 0U; }
    bool operator==(const GraphResource&) const = default;
};

enum class GraphResourceKind : core::u8 {
    Texture,
    Buffer,
    AccelerationStructure,
};

enum class GraphPassKind : core::u8 {
    Raster,
    Compute,
    Copy,
    AccelerationStructure,
    Composite,
    Warp,
    Output,
};

enum class GraphQueue : core::u8 {
    Graphics,
    Compute,
    Copy,
};

enum class GraphState : core::u8 {
    Undefined,
    ColorTarget,
    DepthTarget,
    ShaderRead,
    StorageRead,
    StorageWrite,
    CopySource,
    CopyDestination,
    AccelerationStructureRead,
    AccelerationStructureWrite,
    Present,
};

enum class GraphAccessMode : core::u8 {
    Read,
    Write,
    ReadWrite,
};

struct GraphResourceDesc final {
    std::string_view name;
    GraphResourceKind kind{GraphResourceKind::Texture};
    // Imported resources already contain valid data; transient resources must
    // be written before their first read. Exported resources must be written.
    bool imported{};
    bool exported{};
};

struct GraphAccess final {
    GraphResource resource{};
    GraphState state{GraphState::Undefined};
    GraphAccessMode mode{GraphAccessMode::Read};

    bool operator==(const GraphAccess&) const = default;
};

struct GraphPassDesc final {
    std::string_view name;
    GraphPassKind kind{GraphPassKind::Raster};
    GraphQueue queue{GraphQueue::Graphics};
    std::span<const GraphAccess> accesses;
};

enum class RenderGraphError : core::u8 {
    CapacityExceeded,
    InvalidResource,
    InvalidPass,
    DuplicateAccess,
    ReadBeforeWrite,
    ExportNeverWritten,
};

struct GraphTransition final {
    GraphResource resource{};
    core::u16 before_pass{};
    GraphState before{GraphState::Undefined};
    GraphState after{GraphState::Undefined};
    GraphQueue source_queue{GraphQueue::Graphics};
    GraphQueue destination_queue{GraphQueue::Graphics};
    bool queue_sync{};

    bool operator==(const GraphTransition&) const = default;
};

struct CompiledPass final {
    std::string_view name;
    GraphPassKind kind{GraphPassKind::Raster};
    GraphQueue queue{GraphQueue::Graphics};
    core::u16 first_transition{};
    core::u16 transition_count{};

    bool operator==(const CompiledPass&) const = default;
};

class RenderGraphPlan final {
public:
    [[nodiscard]] std::span<const CompiledPass> passes() const noexcept {
        return {passes_.data(), pass_count_};
    }
    [[nodiscard]] std::span<const GraphTransition> transitions() const noexcept {
        return {transitions_.data(), transition_count_};
    }

private:
    friend class RenderGraph;
    std::array<CompiledPass, max_render_graph_passes> passes_{};
    std::array<GraphTransition, max_render_graph_transitions> transitions_{};
    std::size_t pass_count_{};
    std::size_t transition_count_{};
};

// Fixed-capacity declaration/compiler. It performs no allocation and touches no
// graphics API, filesystem, or clock, so a rebuilt show graph cannot stall on
// driver compilation or disk access. Native backends consume the compiled
// transition plan separately.
class RenderGraph final {
public:
    [[nodiscard]] core::Result<GraphResource, RenderGraphError> add_resource(
        const GraphResourceDesc& desc) noexcept;
    [[nodiscard]] core::Result<std::monostate, RenderGraphError> add_pass(
        const GraphPassDesc& desc) noexcept;
    [[nodiscard]] core::Result<RenderGraphPlan, RenderGraphError> compile()
        const noexcept;

    void clear() noexcept;

    [[nodiscard]] std::size_t resource_count() const noexcept {
        return resource_count_;
    }
    [[nodiscard]] std::size_t pass_count() const noexcept {
        return pass_count_;
    }

private:
    struct StoredPass final {
        std::string_view name;
        GraphPassKind kind{GraphPassKind::Raster};
        GraphQueue queue{GraphQueue::Graphics};
        core::u16 first_access{};
        core::u16 access_count{};
    };

    std::array<GraphResourceDesc, max_render_graph_resources> resources_{};
    std::array<StoredPass, max_render_graph_passes> passes_{};
    std::array<GraphAccess, max_render_graph_accesses> accesses_{};
    std::size_t resource_count_{};
    std::size_t pass_count_{};
    std::size_t access_count_{};
};

} // namespace mediaserver::render
