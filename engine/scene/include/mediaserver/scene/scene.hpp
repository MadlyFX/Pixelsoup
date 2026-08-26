#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/math.hpp>

#include <optional>
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

// A scene is identified by the document node that defines its stage. Zero
// denotes the implicit compatibility scene when a document has no stage nodes
// (and remains an ordinary authored identity if a stage node itself is id 0).
using SceneId = doc::NodeId;
inline constexpr SceneId legacy_scene_id{};

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

// A media source's native frame cadence. Video placements persist this exact
// rational so every render node maps replicated show time to the same source
// ordinal without consulting local decoder metadata. Image sequences omit it
// and retain their authored one-file-per-show-frame behaviour.
struct MediaFrameRate final {
    core::u32 numerator{};
    core::u32 denominator{};

    bool operator==(const MediaFrameRate&) const = default;
};

// Lightweight project metadata for scene choosers. The full environment is
// carried by an evaluated Scene; callers listing scenes only need identity,
// display name, and canvas geometry.
struct SceneInfo final {
    SceneId id{};
    std::string name;
    Canvas canvas{};

    bool operator==(const SceneInfo&) const = default;
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

// How a drawable is composited over the layers behind it. Kept in the
// evaluated scene rather than in the renderer so every render node makes the
// same choice from replicated state.
enum class BlendMode : core::u8 {
    Normal,
    Add,
    Multiply,
    Screen,
};

// A folded rendering summary of the ordered, first-class effect nodes targeting
// one layer. Effect identity/order remains in Scene::effects; this compact form
// is only the pointwise data a draw needs.
struct EffectStack final {
    bool brightness_enabled{};
    float brightness{1.0F};
    bool blur_enabled{};
    float blur_radius{}; // canvas pixels

    bool operator==(const EffectStack&) const = default;
};

enum class EffectKind : core::u8 {
    Brightness,
    Blur,
};

// One evaluated first-class effect. `source` is its document node and `target`
// is the drawable it processes. Keeping this list preserves identity, order,
// and duplicate effects for the Inspector/compositor even though a drawable
// also carries a compact folded summary for rendering.
struct EffectInstance final {
    doc::NodeId source{};
    doc::NodeId target{};
    EffectKind kind{EffectKind::Brightness};
    core::i64 order{};
    bool enabled{true};
    float value{1.0F};

    bool operator==(const EffectInstance&) const = default;
};

struct LayerStyle final {
    float opacity{1.0F};
    BlendMode blend{BlendMode::Normal};
    EffectStack effects{};

    bool operator==(const LayerStyle&) const = default;
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
    LayerStyle style{};

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
    LayerStyle style{};
    // Runtime-only content-store resolution. nullopt preserves the authored
    // path for legacy/external assets; an empty value deliberately suppresses
    // rendering when a managed asset is missing or fails integrity checks.
    // Scene digesting continues to use `source`, so machine-local cache paths
    // can never make replicated scene state appear divergent.
    std::optional<std::string> resolved_source;

    [[nodiscard]] const std::string& render_source() const noexcept {
        return resolved_source.has_value() ? *resolved_source : source;
    }

    bool operator==(const MediaInstance&) const = default;
};

// An imported 3D model placed in the scene. The scene layer is headless, so
// this carries only a reference (the file path) and the transform; the renderer
// loads the geometry and material behind its own cache, exactly as media carries
// a path the renderer resolves to a texture. A node without the file draws
// nothing rather than failing.
struct ModelInstance final {
    Mat4 world{};
    Mat4 normal{};
    std::string source;
    doc::NodeId source_node{};
    // Imported material factors are used by the renderer when available;
    // these authored values provide a deterministic fallback and an Inspector
    // override path.
    Material material{Vec3{0.72F, 0.72F, 0.74F}, 0.6F, 0.0F,
                      Vec3{0.0F, 0.0F, 0.0F}};
    LayerStyle style{};
    // See MediaInstance::resolved_source. Imported geometry keeps the authored
    // reference for deterministic state while the renderer consumes a verified
    // machine-local content-store path when one has been resolved.
    std::optional<std::string> resolved_source;

    [[nodiscard]] const std::string& render_source() const noexcept {
        return resolved_source.has_value() ? *resolved_source : source;
    }

    bool operator==(const ModelInstance&) const = default;
};

struct DirectionalLight final {
    // The direction the light travels, not the direction towards it.
    Vec3 direction{0.0F, -1.0F, 0.0F};
    Vec3 color{1.0F, 1.0F, 1.0F};
    float intensity{1.0F};
    // Which document node produced this, for the editor's selection path; the
    // renderer ignores it.
    doc::NodeId source{};

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
    // Which document node produced this, for the editor's selection path.
    doc::NodeId source{};

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
inline constexpr core::u32 max_scene_models = 256U;
inline constexpr core::u32 max_scene_effects = 512U;
// Keyframe channels the model applies per frame, and keys one channel holds.
// Channels are not drawn, so these bound parse and per-frame work rather than a
// GPU buffer; past the cap the model truncates deterministically in node-id
// order, exactly as the drawable caps above do.
inline constexpr core::u32 max_scene_channels = 256U;
inline constexpr core::u32 max_channel_keys = 64U;

// A transform component a keyframe channel can drive. The order is the array
// index the model uses when applying overrides, and the value a channel node's
// component names; the editor uses it to place a channel on the right timeline
// row. Position/Rotation/Scale, X/Y/Z (rotation as yaw/pitch/roll).
enum class PropertyChannel : core::u8 {
    PositionX,
    PositionY,
    PositionZ,
    RotationYaw,
    RotationPitch,
    RotationRoll,
    ScaleX,
    ScaleY,
    ScaleZ,
    Opacity,
    Brightness,
    BlurRadius,
    Count,
};

// Source compatibility for the first editor implementation. New code should
// use PropertyChannel: channels now animate compositing and effect properties
// as well as transforms.
using TransformChannel = PropertyChannel;

// How a timeline key reaches the next one. Kept public alongside the keyframe
// view so an editor can draw the same curve the evaluator actually samples.
enum class KeyInterpolation : core::u8 {
    Hold,
    Linear,
    Cubic,
};

// The frames one node/component keyframe channel holds, for the editor's
// timeline. Not part of the drawable scene - the channels already show in the
// transforms - so this is queried from the model on demand rather than carried
// on every Scene. One entry per (target, component) that has keys.
struct ChannelKeyframes final {
    doc::NodeId target{};
    PropertyChannel component{};
    std::vector<core::i64> frames; // deduped, ascending
    // Parallel to frames. Older/headless callers that only need diamonds may
    // leave these empty; SceneModel always fills both.
    std::vector<double> values;
    std::vector<KeyInterpolation> interpolations;

    bool operator==(const ChannelKeyframes&) const = default;
};

struct Scene final {
    // The stage node this value was evaluated from. The implicit legacy scene
    // uses legacy_scene_id when the document contains no stage nodes.
    SceneId id{};
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
    // Imported models, drawn in the lit pass with geometry the renderer loads.
    std::vector<ModelInstance> models;
    // Ordered by target, authored order, then effect node id.
    std::vector<EffectInstance> effects;
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
