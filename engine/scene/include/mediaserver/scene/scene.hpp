#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/math.hpp>

#include <string>
#include <vector>

// The evaluated scene: what the renderer draws, and the last place the answer
// is still a number rather than a pixel.
//
// A Scene is a value, not a graph. It has no parents, no children, no
// invalidation, and nothing lazy in it. Every transform is already composed to
// world space and every animated quantity is already resolved at a show frame,
// because the thing this type exists to make possible is a single comparison:
// two nodes hold equal Scenes, or the show is broken.
//
// That comparison is `digest()`. The frame digest the test pattern uses cannot
// survive this phase - two nodes rendering different slices of one canvas
// produce deliberately different pixels, so equal output is no longer the
// signal. The scene digest replaces it and is strictly better: it compares the
// replicated state itself rather than a rendering of it, so it catches
// divergence one frame after it happens instead of whenever it becomes
// visible, and it is meaningful between nodes with different slices, different
// output resolutions, and different GPUs.
namespace mediaserver::scene {

// Show position, expressed without depending on the transport.
//
// The frame ordinal and the rate are carried rather than a time in seconds
// because that is what is exactly replicated: every node in the cohort agrees
// on the integer frame, and any two of them derive the same instant from it by
// exact arithmetic. A seconds value computed upstream and distributed would
// have been rounded once already.
struct SceneTime final {
    core::u64 frame{};
    core::u32 rate_numerator{60U};
    core::u32 rate_denominator{1U};

    bool operator==(const SceneTime&) const = default;

    // Seconds since frame zero. Exact for any frame count a show can reach:
    // the integer converts to binary64 without loss and the two operations
    // that follow are correctly rounded, so every node computes the same bits.
    [[nodiscard]] double seconds() const noexcept;
};

// Surface response, in the vocabulary a physically based shader wants. The
// current shader uses base colour, roughness, and emission; metallic is
// carried because the material a document author writes should not have to
// change when the shading model grows into it.
struct Material final {
    Vec3 base_color{0.8F, 0.8F, 0.8F};
    float roughness{0.5F};
    float metallic{0.0F};
    Vec3 emissive{0.0F, 0.0F, 0.0F};

    bool operator==(const Material&) const = default;
};

// One drawable. The mesh is a key rather than geometry: the renderer resolves
// it through its cache, and two instances of the same key share one upload.
struct MeshInstance final {
    MeshKey mesh{};
    Mat4 world{};
    // The inverse transpose of the world basis. Precomputed here rather than
    // in the shader because it is per instance, not per vertex, and because
    // the CPU is where the singular case can be handled honestly.
    Mat4 normal{};
    Material material{};
    // Which document node produced this. Carried for diagnostics and for the
    // selection path that will need it; the renderer ignores it.
    doc::NodeId source{};

    bool operator==(const MeshInstance&) const = default;
};

// A clip textured onto a screen-facing quad, resolved at a show frame.
//
// The scene layer is headless, so this carries a *reference* - the sequence's
// path template and the file number to show at this frame - not pixels. The
// renderer resolves that reference to a texture through its own media cache.
// Two nodes evaluating the same show frame compute the same `frame_index`, so
// they display the same file, frame-exact, without any pixel replication.
struct MediaInstance final {
    // The geometry the clip is drawn on. A screen-facing quad by default.
    MeshKey mesh{Primitive::Quad, 1U};
    Mat4 world{};
    Mat4 normal{};
    // The printf-style sequence template and the file number to load now.
    std::string source;
    core::u32 frame_index{};
    // Which document node produced this, for the selection/manipulation path.
    doc::NodeId source_node{};

    bool operator==(const MediaInstance&) const = default;
};

struct DirectionalLight final {
    // The direction the light travels, not the direction towards it.
    Vec3 direction{0.0F, -1.0F, 0.0F};
    Vec3 color{1.0F, 1.0F, 1.0F};
    float intensity{1.0F};

    bool operator==(const DirectionalLight&) const = default;
};

struct PointLight final {
    Vec3 position{};
    Vec3 color{1.0F, 1.0F, 1.0F};
    float intensity{1.0F};
    // Distance at which the light contributes nothing. A hard cutoff rather
    // than a physical inverse square that never quite reaches zero, because a
    // bounded radius is what lets the renderer cull and what an operator can
    // reason about.
    float range{10.0F};

    bool operator==(const PointLight&) const = default;
};

// Ambient surround, standing in for the image-based lighting that replaces it.
// Two colours and a strength is enough to keep unlit sides from going black
// and to give the frame a horizon.
struct Environment final {
    Vec3 sky_color{0.05F, 0.07F, 0.10F};
    Vec3 ground_color{0.02F, 0.02F, 0.02F};
    float ambient{1.0F};

    bool operator==(const Environment&) const = default;
};

// Which node renders which rectangle of the canvas.
//
// This lives in the scene, and therefore in the document, because it has to be
// replicated: a node that disagreed with the Director about its own slice
// would render a correct image of the wrong region, which is the hardest kind
// of fault to see. A node selects its binding by its own index and never
// invents one.
struct SliceBinding final {
    core::u32 node_index{};
    CanvasRect rect{};

    bool operator==(const SliceBinding&) const = default;
};

// Caps, so the renderer sizes its buffers once and a runaway document is
// refused rather than absorbed. Generous against what a single node can draw
// at rate today, and far below what would make the upload interesting.
inline constexpr core::u32 max_scene_instances = 4'096U;
inline constexpr core::u32 max_scene_directional_lights = 4U;
inline constexpr core::u32 max_scene_point_lights = 64U;
inline constexpr core::u32 max_scene_slices = 256U;
inline constexpr core::u32 max_scene_media = 64U;
// Keyframe channels the model applies per frame, and keys one channel holds.
// Channels are not drawn, so these bound parse and per-frame work rather than a
// GPU buffer; past the cap the model truncates deterministically in node-id
// order, exactly as the drawable caps above do.
inline constexpr core::u32 max_scene_channels = 256U;
inline constexpr core::u32 max_channel_keys = 64U;

struct Scene final {
    Canvas canvas{1'920U, 1'080U};
    Camera camera{};
    Environment environment{};
    // Sorted by mesh key and then by source node id. Sorted rather than merely
    // grouped because the renderer draws one batch per distinct mesh and the
    // order of those batches is part of what two nodes have to agree on.
    std::vector<MeshInstance> instances;
    // Clips, kept separate from procedural instances because the renderer draws
    // them in their own textured pass.
    std::vector<MediaInstance> media;
    std::vector<DirectionalLight> directional_lights;
    std::vector<PointLight> point_lights;
    std::vector<SliceBinding> slices;

    // The rectangle bound to this node, or null when the document binds none.
    // A node with no binding renders nothing rather than guessing at the whole
    // canvas: an unmapped output should be black and obviously unmapped.
    [[nodiscard]] const CanvasRect* slice_for(core::u32 node_index) const noexcept;
};

// FNV-1a over every value in the scene, in declaration order, reading floats
// as their bit patterns rather than their decimal forms.
//
// Bit patterns because that is the question being asked. Two nodes that agree
// to seven decimal places and differ in the last bit will render a visible
// seam, so a digest that called them equal would be worse than no digest.
//
// Padding is never hashed - each field is fed in by name - so the value is
// stable across compilers and does not depend on struct layout.
[[nodiscard]] core::u64 digest(const Scene& scene) noexcept;

} // namespace mediaserver::scene
