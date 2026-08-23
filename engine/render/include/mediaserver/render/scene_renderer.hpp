#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/scene.hpp>

#include <map>
#include <vector>

// The renderer: a `scene::Scene` and a rectangle of its canvas in, one
// rendered colour target out.
//
// This is the second half of the seam the implementation plan's §6.4 calls the
// critical one. The scene layer never learns that a GPU exists; this layer
// never reads a document. Everything crossing between them is a `Scene`, which
// is a value with no pointers into anything that could change underneath it.
//
// What it does per frame, in order:
//
//  1. Resolves every mesh key the scene references through a cache, generating
//     any that are missing. Geometry is procedural, so a cache miss costs a
//     generator call rather than a file read or a network fetch, and a miss
//     resolves identically on every node.
//  2. Writes one instance record per drawable and one batch per run of
//     instances sharing a mesh. The scene arrives already grouped by mesh, so
//     finding the runs is a linear scan rather than a sort.
//  3. Narrows the camera to this node's rectangle - `scene::view_for_slice` -
//     and uploads the resulting matrix.
//  4. One depth-tested pass, one draw per batch.
//
// The camera is never adjusted for the node. That is the whole design: a node
// renders a sub-frustum of the one camera the cohort shares, so two nodes'
// outputs abut exactly rather than approximately, and neither node holds any
// state the other does not.
//
// What is deliberately absent, so that its absence is a decision rather than
// an oversight: frustum culling (the scenes are small and a wrong cull is
// worse than a slow frame), instancing beyond per-mesh batching, shadows, and
// any global illumination. Each of those has a place in the plan, and each of
// them lands behind this same interface.
namespace mediaserver::render {

enum class RenderError : core::u8 {
    // No GPU, or a device that could not be used.
    Unavailable,
    // This build carries no shader bytecode the device accepts. Distinct from
    // Unavailable because the fix is a build change, not a machine change.
    ShaderFormatUnsupported,
    InvalidConfiguration,
    AllocationFailed,
    BackendFailure,
};

// Fixed capacities. The renderer allocates once at construction and never
// again, so a scene past one of these is refused for the frame and counted
// rather than absorbed into an allocation on the frame path.
struct RendererLimits final {
    core::u32 max_instances{scene::max_scene_instances};
    // Enough for a few hundred primitives at a reasonable tessellation, or a
    // handful at a high one.
    core::u32 max_vertices{1U << 20U};
    core::u32 max_indices{1U << 21U};
    core::u32 max_lights{
        scene::max_scene_directional_lights + scene::max_scene_point_lights};

    bool operator==(const RendererLimits&) const = default;
};

// What one frame cost and what it left out. Reported because a renderer that
// quietly drops geometry looks exactly like a scene that is missing it.
struct RenderStats final {
    core::u32 batches{};
    core::u32 instances{};
    core::u32 triangles{};
    core::u32 meshes_resident{};
    // Instances not drawn because the mesh arena had no room for their
    // geometry, or because the instance array was full.
    core::u32 dropped_instances{};
    core::u32 dropped_lights{};
    // True on a frame that re-uploaded the mesh arena, which happens when a
    // mesh the scene references was not yet resident. Zero on a settled show.
    bool geometry_uploaded{};
    core::u64 upload_bytes{};

    bool operator==(const RenderStats&) const = default;
};

class SceneRenderer final {
public:
    // Fails with ShaderFormatUnsupported on a build with no scene bytecode,
    // which is a normal outcome on a machine without a shader compiler.
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<SceneRenderer, RenderError> create(
        rhi::Device& device,
        core::u32 width,
        core::u32 height,
        const RendererLimits& limits = RendererLimits{});

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;
    SceneRenderer(SceneRenderer&&) = default;
    SceneRenderer& operator=(SceneRenderer&&) = default;
    ~SceneRenderer() = default;

    // The colour target this renderer draws into. Sampled and presentable; the
    // caller blits it to a swapchain or reads it back.
    [[nodiscard]] rhi::Texture& target() noexcept { return color_; }
    [[nodiscard]] core::u32 width() const noexcept { return color_.width(); }
    [[nodiscard]] core::u32 height() const noexcept { return color_.height(); }

    // Reallocates the colour and depth targets. A no-op when the size is
    // unchanged, so it is safe to call every frame from a resizable window.
    [[nodiscard]] bool resize(
        rhi::Device& device,
        core::u32 width,
        core::u32 height) noexcept;

    // Records one frame. `slice` is this node's rectangle of `scene.canvas`;
    // pass `scene::whole(scene.canvas)` to render the entire image on one
    // machine.
    //
    // A slice the canvas does not contain is refused rather than clamped: an
    // output mapped outside its canvas is a feed-map error, and rendering
    // something plausible would hide it.
    [[nodiscard]] bool render(
        rhi::CommandList& commands,
        const scene::Scene& scene,
        scene::CanvasRect slice);

    [[nodiscard]] const RenderStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const RendererLimits& limits() const noexcept { return limits_; }

private:
    // Where one mesh lives in the shared index arena. Vertices are addressed
    // absolutely, so a batch needs no base-vertex fixup.
    struct MeshRange final {
        core::u32 first_index{};
        core::u32 index_count{};
    };

    SceneRenderer(
        RendererLimits limits,
        rhi::Texture color,
        rhi::Texture depth,
        rhi::GraphicsPipeline pipeline,
        rhi::StorageBuffer vertices,
        rhi::StorageBuffer indices,
        rhi::StorageBuffer instances,
        rhi::StorageBuffer lights) noexcept;

    // Generates and appends a mesh if it is not already resident. False when
    // the arena has no room, which the caller turns into dropped instances
    // rather than a failed frame.
    [[nodiscard]] bool ensure_resident(scene::MeshKey key);

    RendererLimits limits_{};
    rhi::Texture color_;
    rhi::Texture depth_;
    rhi::GraphicsPipeline pipeline_;
    rhi::StorageBuffer vertex_buffer_;
    rhi::StorageBuffer index_buffer_;
    rhi::StorageBuffer instance_buffer_;
    rhi::StorageBuffer light_buffer_;

    // CPU-side arenas. The mesh arenas are the authoritative copy and are
    // re-uploaded whole whenever they grow; the per-frame arenas are scratch
    // that is refilled and uploaded every frame.
    std::map<scene::MeshKey, MeshRange> resident_;
    std::vector<core::u8> vertex_arena_;
    std::vector<core::u8> index_arena_;
    std::vector<core::u8> instance_scratch_;
    std::vector<core::u8> light_scratch_;
    std::vector<core::u8> batch_uniforms_;
    std::vector<rhi::DrawBatch> batches_;
    scene::MeshData mesh_scratch_;
    bool geometry_dirty_{};

    RenderStats stats_{};
};

} // namespace mediaserver::render
