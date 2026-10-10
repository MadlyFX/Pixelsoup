#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/rhi/device.hpp>

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace mediaserver::render {

// First-fit allocator over element slots. Its hole list is reserved up front,
// so allocating and releasing make no heap calls on the frame path. A release
// that would need one more hole than reserved is kept as unusable capacity
// rather than refused: correctness never depends on the bookkeeping fitting.
class RangeAllocator final {
public:
    explicit RangeAllocator(core::u32 capacity = 0U, std::size_t max_holes = 4096U);

    [[nodiscard]] std::optional<core::u32> allocate(core::u32 count) noexcept;
    void release(core::u32 offset, core::u32 count) noexcept;
    // Adds slots at the end; a hole touching the old end extends into them.
    void grow(core::u32 capacity) noexcept;

    [[nodiscard]] core::u32 capacity() const noexcept { return capacity_; }
    [[nodiscard]] core::u32 used() const noexcept { return used_; }
    // One past the highest slot that may hold data: what growth must copy.
    [[nodiscard]] core::u32 high_water() const noexcept;
    [[nodiscard]] core::u32 largest_hole() const noexcept;

private:
    struct Hole final {
        core::u32 offset{};
        core::u32 count{};
    };
    std::vector<Hole> holes_;
    std::size_t max_holes_{};
    core::u32 capacity_{};
    core::u32 used_{};
};

// Where one mesh lives in the geometry arena. Vertices and tangents share one
// slot range; skin records exist only for skinned meshes.
struct GeometrySpan final {
    core::u32 first_vertex{};
    core::u32 vertex_count{};
    core::u32 first_index{};
    core::u32 index_count{};
    core::u32 first_skin{};
    core::u32 skin_count{};

    [[nodiscard]] bool empty() const noexcept {
        return vertex_count == 0U && index_count == 0U && skin_count == 0U;
    }
    bool operator==(const GeometrySpan&) const = default;
};

enum class GeometryStream : core::u8 { Vertices, Tangents, Skins, Indices };

// Starting and largest sizes, in elements. A renderer starts small and grows
// only when content needs it, so the many renderers in one process do not
// each hold the largest arena.
struct GeometryArenaLimits final {
    core::u32 initial_vertices{1U << 18U};
    core::u32 max_vertices{48U << 20U};
    core::u32 initial_indices{3U << 18U};
    core::u32 max_indices{144U << 20U};
    core::u32 initial_skins{1U << 16U};
    core::u32 max_skins{4U << 20U};
};

// Mesh geometry resident in device memory: vertices (32 bytes), tangents (16),
// skin records (32) and absolute u32 indices. Content is written by range and
// never re-uploaded whole. Growth allocates larger buffers, copies the used
// contents on the GPU inside the recording that needed the room, and keeps
// the replaced buffers until the GPU has finished with them.
class GeometryArena final {
public:
    static constexpr core::u32 vertex_bytes = 32U;
    static constexpr core::u32 tangent_bytes = 16U;
    static constexpr core::u32 skin_bytes = 32U;
    static constexpr core::u32 index_bytes = 4U;

    [[nodiscard]] static core::Result<GeometryArena, rhi::RhiError> create(
        rhi::Device& device, GeometryArenaLimits limits) noexcept;

    GeometryArena() noexcept = default;
    GeometryArena(GeometryArena&&) noexcept = default;
    GeometryArena& operator=(GeometryArena&&) noexcept = default;

    // Reserves slots for one mesh. When there is no room the buffers grow,
    // with the copy of their contents recorded in `commands` ahead of any
    // later write; generation() then advances. Empty when the largest arena
    // cannot hold the mesh or device memory is exhausted.
    [[nodiscard]] std::optional<GeometrySpan> allocate(
        rhi::CommandList& commands,
        core::u32 vertices, core::u32 indices, core::u32 skins) noexcept;
    // Returns slots that no outstanding draw will read again.
    void release(const GeometrySpan& span) noexcept;
    // Writes whole elements of one stream starting at element `first`.
    [[nodiscard]] bool write(
        rhi::CommandList& commands, GeometryStream stream,
        core::u32 first, std::span<const core::u8> bytes) noexcept;
    // Frees buffers replaced by growth once the GPU no longer uses them.
    void collect() noexcept;

    [[nodiscard]] rhi::StorageBuffer& buffer(GeometryStream stream) noexcept {
        return streams_[static_cast<std::size_t>(stream)];
    }
    [[nodiscard]] core::u64 generation() const noexcept { return generation_; }
    [[nodiscard]] core::u64 used_bytes() const noexcept;
    [[nodiscard]] core::u64 capacity_bytes() const noexcept;
    [[nodiscard]] const GeometryArenaLimits& limits() const noexcept { return limits_; }

    [[nodiscard]] static core::u32 element_bytes(GeometryStream stream) noexcept;

private:
    [[nodiscard]] bool grow(
        rhi::CommandList& commands, RangeAllocator& allocator,
        std::span<const GeometryStream> streams, core::u32 needed,
        core::u32 largest) noexcept;

    rhi::Device* device_{};
    GeometryArenaLimits limits_{};
    std::array<rhi::StorageBuffer, 4U> streams_{};
    RangeAllocator vertices_{};
    RangeAllocator indices_{};
    RangeAllocator skins_{};
    std::vector<rhi::StorageBuffer> retired_;
    core::u64 generation_{};
};

} // namespace mediaserver::render
