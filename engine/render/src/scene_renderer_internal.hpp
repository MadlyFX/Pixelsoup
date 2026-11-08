#pragma once

// What SceneRenderer's sources share: the GPU-side records and their HLSL
// counterparts, the shader set, descriptor helpers, inline-effect programs,
// layer ordering, shadow projections and view resolution. Internal to the
// render library.

#include <mediaserver/scene/lighting.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/core/idle_report.hpp>
#include <mediaserver/render/effect_execution.hpp>
#include <mediaserver/render/model_loader.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <new>
#include <span>
#include <utility>

namespace mediaserver::render::scene_detail {

using Result = core::Result<SceneRenderer, RenderError>;

// ---------------------------------------------------------------------------
// The GPU-side records. Every one of these is described twice - here and in
// the HLSL - and a mismatch would be a silently wrong picture rather than a
// compile error, so each carries a size assertion and each field comment names
// its counterpart.

// scene.vert.hlsl: struct Vertex.
static_assert(sizeof(scene::MeshVertex) == 32U);

// scene.vert.hlsl: struct Instance. Matrices are stored as explicit columns on
// both sides, so nothing here transposes anything.
struct GpuInstance final {
    float world[16U]{};  // four columns of the object-to-world transform
    float previous_world[16U]{};
    float normal[12U]{}; // three columns of the inverse-transpose basis
    float base_color[3U]{};
    float roughness{};
    float emissive[3U]{};
    float metallic{};
    // normal scale, occlusion strength, BC5 base normal, BC5 coat normal
    float material_properties[4U]{};
    float coat[4U]{};
    float sheen[4U]{};
    float specular[4U]{};
    float anisotropy[4U]{};
    float alpha[4U]{};

    // opacity, horizontal edge blend, BlendMode, depth-only flag
    float layer_style[4U]{};
    // first inline operation, operation count, DecodedPixelLayout, reserved
    core::u32 effect_range[4U]{};
    core::u32 pose[4U]{}; // offset, binding count, skin base, first vertex
    float wind[4U]{}; // displacement, minimum bind y
    float previous_wind[4U]{}; // displacement, inverse bind height
};

static_assert(sizeof(GpuInstance) == 384U);
// The skin record itself is written by GeometryResidency.
struct GpuSkin final { std::array<core::u32,4U> joints{}; scene::Vec4 weights{1,0,0,0}; };
struct GpuPose final { scene::Mat4 current; scene::Mat4 previous; };
static_assert(sizeof(GpuSkin)==32U && sizeof(GpuPose)==128U);
static_assert(alignof(GpuInstance) == 4U);
static_assert(static_cast<core::u8>(scene::BlendMode::Normal) == 0U);
static_assert(static_cast<core::u8>(scene::BlendMode::Add) == 1U);
static_assert(static_cast<core::u8>(scene::BlendMode::Multiply) == 2U);
static_assert(static_cast<core::u8>(scene::BlendMode::Screen) == 3U);

// scene.frag.hlsl / scene_media.frag.hlsl: an operation selects a typed slice
// of the separate float4 parameter buffer. Keeping parameters out of the
// instance record is what lets Levels carry all twenty values without fixed
// per-instance lanes.
struct GpuInlineEffectOp final {
    core::u32 kind{};
    core::u32 parameter_offset{};
    core::u32 parameter_count{};
    core::u32 reserved{};
};

struct GpuInlineEffectParameter final {
    float values[4U]{};
};

static_assert(sizeof(GpuInlineEffectOp) == 16U);
static_assert(sizeof(GpuInlineEffectParameter) == 16U);
static_assert(static_cast<core::u8>(scene::EffectKind::Brightness) == 0U);
static_assert(static_cast<core::u8>(scene::EffectKind::Saturation) == 1U);
static_assert(static_cast<core::u8>(scene::EffectKind::Levels) == 3U);
static_assert(
    static_cast<core::u8>(scene::EffectKind::LiftGammaGain) == 4U);
static_assert(
    static_cast<core::u8>(scene::EffectKind::ContrastPivot) == 5U);
static_assert(
    static_cast<core::u8>(scene::EffectKind::ColorMatrix) == 6U);
static_assert(
    static_cast<core::u8>(scene::EffectKind::WhiteBalance) == 7U);
static_assert(static_cast<core::u8>(scene::EffectKind::Curves) == 8U);
static_assert(static_cast<core::u8>(scene::EffectKind::FilmGrain) == 9U);

enum class GpuLightKind : core::u32 { Directional, Point, Spot, Rectangle, Sphere };

// light.hlsli: struct Light.
struct GpuLight final {
    float vector[3U]{}; // direction for a sun, position for a local light
    GpuLightKind kind{GpuLightKind::Directional};
    float color_intensity[4U]{};
    // x: range. y: cosine of the outer cone half-angle. z: cosine of the inner
    // one. Cosines rather than angles because the shader compares against a
    // dot product, and converting once here is one deterministic evaluation
    // instead of one per pixel per light.
    float params[4U]{};
    // xyz: cone axis. w: one for a photometric smoothstep cone, zero for
    // the legacy unitless squared cone. Light kind is explicit above.
    float spot[4U]{};
    float ies[4U]{0.0F, 0.0F, 0.0F, -1.0F};
    float ies_axis[4U]{};
    float area_u[4U]{}; // tangent, rectangle half width or sphere radius
    float area_v[4U]{}; // bitangent, rectangle half height
};

static_assert(sizeof(GpuLight) == 128U);

// scene.frag.hlsl: StructuredBuffer<LightShadow>.
//
// Separate from `GpuLight` because only a handful of lights cast and a matrix
// on every light would be sixty four matrices uploaded to describe four.
struct GpuLightShadow final {
    float view_projection[16U]{};
    // xy: the tile's origin in atlas texels. z: its extent. w: the depth one
    // shadow texel spans, for a bias expressed in texels rather than in world
    // units - the same mechanism the cascades use, per tile instead of per
    // cascade.
    float tile[4U]{};
};

static_assert(sizeof(GpuLightShadow) == 80U);

// scene.vert.hlsl: cbuffer ViewConstants.
struct ViewConstants final {
    float view_projection[16U]{};
    float previous_view_projection[16U]{};
    float canvas_view_projection[16U]{};
    float temporal_parameters[4U]{};
    float clip_plane[4U]{};
    float motion_viewport[4U]{};
};

static_assert(sizeof(ViewConstants) == 240U);

// scene.vert.hlsl: cbuffer BatchConstants.
struct BatchConstants final {
    core::u32 first_index{};
    core::u32 first_instance{};
    // Nonzero when the draw is indexed and SV_VertexID is the vertex itself.
    core::u32 indexed{};
    core::u32 reserved1{};
};

static_assert(sizeof(BatchConstants) == 16U);

// scene.frag.hlsl: cbuffer SceneConstants.
struct SceneConstants final {
    float eye_position[4U]{};
    float sky_ambient[4U]{};
    float ground_exposure[4U]{};
    core::u32 counts[4U]{};
    // slice origin x/y, exact show frame low/high u32
    core::u32 grain_context[4U]{};
    // slice width/height, render-target width/height
    core::u32 grain_extent[4U]{};
    // x: the view's ViewShading. scene_media.frag.hlsl declares the block up
    // to here, so this stays ahead of everything it does not read.
    float view_mode[4U]{};
    // One light projection per cascade. Only counts[2] of them are valid; the
    // last valid one is the full-scene fit the RSM pass also used.
    float shadow_view_projection[scene::max_shadow_cascades][16U]{};
    // reciprocal extent x/y, constant bias, slope bias. Both biases are in
    // shadow texels; the per-cascade depth of one texel is below.
    float shadow_parameters[4U]{};
    // Reversed-depth offset of one texel in each cascade.
    float shadow_cascade_texel_depth[4U]{};
    float shadow_inverse_view_projection[16U]{};
    // RSM radius, GI strength, minimum squared distance, reserved
    float gi_parameters[4U]{};
    // x: environment intensity, zero when no prefiltered environment is
    // resident. y: mips in the prefiltered specular chain.
    float environment_parameters[4U]{};
    // Reconstruct lighting positions from the shared canvas pixel and depth.
    float lighting_inverse_view_projection[16U]{};
    // xy: projection canvas extent. zw: this slice's origin in that canvas.
    float lighting_canvas[4U]{};
    float baked_minimum[4U]{};
    float baked_maximum[4U]{};
    float baked_dimensions[4U]{};
    float texture_sampling[4U]{};
    // Blend fraction, filter radius (texels), tangent of sun angular radius.
    float shadow_quality[4U]{};
    // Length, thickness, fixed screen reach, reserved.
    float contact_shadow[4U]{};
    float lighting_view_projection[16U]{};
    float reflection_position[scene::max_reflection_probes][4U]{};
    float reflection_extent[scene::max_reflection_probes][4U]{};
    float reflection_parameters[scene::max_reflection_probes][4U]{};
    float planar_parameters[4U]{};
    // x: particle lights in the particle light buffer.
    core::u32 particle_lights[4U]{};
};

// sky.frag.hlsl: cbuffer SkyConstants.
struct SkyConstants final {
    float inverse_view_projection[16U]{};
    float parameters[4U]{};
    float canvas[4U]{};
    float fallback[4U]{};
    float sun_direction_radius[scene::max_scene_directional_lights][4U]{};
    float sun_radiance[scene::max_scene_directional_lights][4U]{};
};
static_assert(sizeof(SkyConstants) == 240U);

// The exact constant-buffer size, asserted so a field added here without one
// added to the shader is a build error rather than a frame of misread
// constants.
static_assert(sizeof(SceneConstants) == 960U);

// scene_rsm_flux.frag.hlsl: cbuffer RsmFluxConstants.
struct RsmFluxConstants final {
    float direction_intensity[4U]{};
    float light_color[4U]{};
};

static_assert(sizeof(RsmFluxConstants) == 32U);

// ---------------------------------------------------------------------------

struct ShaderBlobs final {
    std::span<const core::u8> vertex;
    std::span<const core::u8> motion_vertex;
    std::span<const core::u8> fragment;
    std::span<const core::u8> linear_fragment;
    std::span<const core::u8> media_fragment;
    std::span<const core::u8> media_linear_fragment;
    std::span<const core::u8> media_object_id_fragment;
    std::span<const core::u8> media_velocity_fragment;
    std::span<const core::u8> media_normal_fragment;
    std::span<const core::u8> object_id_fragment;
    std::span<const core::u8> velocity_fragment;
    std::span<const core::u8> normal_fragment;
    std::span<const core::u8> auxiliary_fragment;
    std::span<const core::u8> media_auxiliary_fragment;
    std::span<const core::u8> shadow_fragment;
    std::span<const core::u8> rsm_normal_fragment;
    std::span<const core::u8> rsm_flux_fragment;
    std::span<const core::u8> coverage_vertex;
    std::span<const core::u8> coverage_fragment;
    // The fullscreen triangle and the sky that rides on it. Separate presence
    // flag: a build without them keeps a working renderer and a flat
    // background rather than failing to construct.
    std::span<const core::u8> fullscreen_vertex;
    std::span<const core::u8> sky_fragment;
    rhi::ShaderFormat format{rhi::ShaderFormat::Dxil};
    bool present{};
    bool sky_present{};
};

[[nodiscard]] ShaderBlobs shader_blobs() noexcept;

template <typename T>
void append(std::vector<core::u8>& into, const T& value) {
    const auto offset = into.size();
    into.resize(offset + sizeof(T));
    std::memcpy(into.data() + offset, &value, sizeof(T));
}

template <typename T>
[[nodiscard]] std::span<const core::u8> bytes_of(const T& value) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&value), sizeof(T)};
}

// The lit pipeline's fragment texture count, in one place.
//
// Five material textures, one slot per shadow cascade, two reflective-shadow
// map products, three image-based-lighting products, and screen-space
// occlusion. Every slot is declared whether or not a show authored the thing
// behind it, because one pipeline serves every authored state and the binding
// shape must never depend on document content.
//
// Named because it was previously written out at four separate call sites -
// the pipeline's sampler count, the per-batch arena reservation, the draw's
// per-batch stride, and the composite layer's own binding array - and adding a
// binding meant finding all four. Three of four is a refused draw with no
// explanation.
inline constexpr core::u32 advanced_fragment_textures =
    33U;
inline constexpr core::u32 basic_fragment_textures = 16U;

[[nodiscard]] rhi::TextureDesc color_desc(
    core::u32 width,
    core::u32 height,
    rhi::TextureFormat format = rhi::TextureFormat::Rgba8Unorm) noexcept;

[[nodiscard]] rhi::TextureDesc sampled_desc(
    core::u32 width,
    core::u32 height) noexcept;

[[nodiscard]] rhi::TextureDesc depth_desc(core::u32 width, core::u32 height,
                                          bool sampled = false) noexcept;

[[nodiscard]] GpuInstance to_gpu(const scene::MeshInstance& instance,
                                 core::u32 first_effect,
                                 core::u32 effect_count,
                                 bool depth_only = false,
                                 DecodedPixelLayout media_layout =
                                     DecodedPixelLayout::Rgba8,
                                 const scene::Mat4* previous_world = nullptr)
    noexcept;

struct CompiledInlineRange final {
    core::u32 first{};
    core::u32 count{};
};

// scene.frag.hlsl: surface_fill. A Background surface's fill rides its inline
// program as an operation of its own kind, outside scene::EffectKind, which
// every colour-effect evaluator skips. Its four parameter vectors are the
// mode and reciprocal ramp extent with the direction, the centre and the
// surface's world width and height, then colours A and B.
inline constexpr core::u32 surface_fill_inline_kind = 32U;
static_assert(surface_fill_inline_kind
    > static_cast<core::u32>(scene::EffectKind::FilmGrain));

struct SurfaceFillProgram final {
    std::array<GpuInlineEffectParameter, 4U> parameters{};
};

// The fill measured in the surface's own world size, with the same
// deterministic trigonometry and extents as the compositor's Background, so
// the shader only places the pixel and interpolates. UV u runs along local X;
// v along local Z on a plane and down local Y on a quad (scene.cpp
// mask_source_plane). Any other mesh is filled in its UV square.
[[nodiscard]] std::optional<SurfaceFillProgram> surface_fill_program(
    const scene::MeshInstance& instance) noexcept;

[[nodiscard]] bool compile_inline_program(
    const scene::EffectStack& effects,
    const scene::EffectSettings& settings,
    core::u32 max_operations,
    core::u32 max_parameters,
    core::u32 max_curve_luts,
    core::u32& curve_luts_used,
    std::vector<core::u8>& operation_bytes,
    std::vector<core::u8>& parameter_bytes,
    CompiledInlineRange& range,
    const std::optional<SurfaceFillProgram>& fill = std::nullopt);

[[nodiscard]] scene::BlendMode valid_blend(scene::BlendMode blend) noexcept;

[[nodiscard]] float opacity(const scene::LayerStyle& style) noexcept;

[[nodiscard]] bool needs_compositing(const scene::LayerStyle& style) noexcept;

[[nodiscard]] std::size_t blend_index(scene::BlendMode blend) noexcept;

[[nodiscard]] rhi::ColorBlend to_rhi_blend(
    scene::BlendMode blend,
    bool premultiplied = false) noexcept;

[[nodiscard]] float camera_depth(
    const scene::Mat4& world,
    scene::Vec3 local_center,
    scene::Vec3 eye,
    scene::Vec3 forward) noexcept;

inline constexpr ModelBounds unit_bounds{
    scene::Vec3{-0.5F, -0.5F, -0.5F}, scene::Vec3{0.5F, 0.5F, 0.5F}};

// Where a procedural primitive lies before its world transform. The quad and
// plane have no thickness, which is what marks them flat for layer ordering.
[[nodiscard]] ModelBounds primitive_bounds(scene::Primitive primitive) noexcept;

// How much plane-side layer ordering one recording may do: pairs of layers
// compared, and relations kept, on average per layer. A frame needing more
// keeps the remaining flat layers in centre order and counts them, so this
// stays a fraction of a millisecond however many layers a show composites.
inline constexpr std::size_t layer_pair_budget = 32'768U;
inline constexpr std::size_t layer_edges_per_layer = 8U;

[[nodiscard]] float component(scene::Vec3 value, std::size_t axis) noexcept;

[[nodiscard]] bool finite(scene::Vec3 value) noexcept;

[[nodiscard]] bool finite(const scene::Mat4& value) noexcept;

struct DirectionalShadowBounds final {
    scene::Vec3 minimum{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    scene::Vec3 maximum{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
    core::u32 casters{};
};

void expand_directional_shadow_bounds(
    DirectionalShadowBounds& bounds,
    const scene::Mat4& world,
    scene::Vec3 local_minimum,
    scene::Vec3 local_maximum) noexcept;

// `texel_depth` receives the reversed-depth offset equal to one shadow texel's
// world size. Bias is expressed in those units so a tight near cascade is not
// pushed off its receiver by an offset sized for the whole-scene map, which is
// what would leave a contact shadow visibly detached from the thing casting it.
[[nodiscard]] bool directional_shadow_matrix(
    const DirectionalShadowBounds& bounds,
    const scene::DirectionalLight& light,
    core::u32 resolution,
    scene::Mat4& into,
    scene::Mat4& inverse_into,
    float& texel_depth) noexcept;

// A volume of world space one cascade must cover, expressed as a sphere so the
// fit does not change shape as the camera rotates. Sphere fitting is what
// keeps a cascade's texel grid stable; an axis-aligned fit of the same frustum
// swims when the camera turns, and a swimming shadow on an LED wall reads as a
// flickering light rather than as a moving camera.
struct CascadeVolume final {
    scene::Vec3 center{};
    float radius{};
};

// Cascade slice boundaries over [near, far], blending the logarithmic and
// uniform schemes. Derived only from replicated camera and scene values, never
// from this node's slice, so every node computes the same distances.
[[nodiscard]] float cascade_split(
    float near_plane, float far_plane, core::u32 index,
    core::u32 count) noexcept;

// How far the camera cascades need to reach. A show's far plane is usually far
// beyond anything that casts, so subdividing to it would spend every near
// cascade on empty space. Bound it by the casters instead, using the same
// replicated scene bounds every node already agrees on.
[[nodiscard]] float cascade_far_distance(
    const scene::Camera& camera,
    const DirectionalShadowBounds& bounds,
    float near_distance,
    float far_plane) noexcept;

// The bounding sphere of the authored full-canvas camera's frustum between two
// distances. `window` is the near-plane rectangle that camera sees over the
// whole canvas, so this deliberately ignores which slice the node renders.
[[nodiscard]] bool camera_cascade_volume(
    const scene::Camera& camera,
    const scene::FrustumWindow& window,
    float near_distance,
    float far_distance,
    CascadeVolume& into) noexcept;

// The light projection covering one cascade volume. X and Y come from the
// volume, snapped to that cascade's texel grid; the depth range comes from the
// complete caster bounds so a caster between the light and this volume is
// still rasterized into the map even though it sits outside the camera slab.
[[nodiscard]] bool cascade_shadow_matrix(
    const DirectionalShadowBounds& bounds,
    const CascadeVolume& volume,
    const scene::DirectionalLight& light,
    core::u32 resolution,
    scene::Mat4& into,
    float& texel_depth) noexcept;

// A media surface is the unit XY quad generated by Primitive::Quad. Test its
// four corners in homogeneous clip space so quads crossing the eye or a
// frustum plane remain visible. This only returns true after one complete
// separating plane is proven; malformed transforms therefore fail open.
[[nodiscard]] bool media_quad_outside_view(
    const scene::Mat4& world,
    const scene::Mat4& view_projection,
    const scene::EffectStack& effects,
    const scene::EffectSettings& effect_settings) noexcept;

// Whether a model-space box lies wholly beyond one side plane of a shadow
// projection. Every point of the box is then beyond it too, so the caster
// rasterizes nothing into that map and leaving it out changes no texel. Only
// the side planes are tested: a cascade's depth range spans every caster by
// construction, and a backend clamping depth still clips the sides. The slack
// keeps a box grazing an edge in against rounding; non-finite corners keep it.
[[nodiscard]] bool outside_shadow_projection(
    const scene::Mat4& view_projection,
    const scene::Mat4& world,
    const ModelBounds& bounds) noexcept;

// Folds one value into a running 64-bit digest. Shadow reuse keys only; a
// collision would reuse a stale map, so the mix is a full avalanche rather
// than a cheap combine.
[[nodiscard]] constexpr core::u64 mix_digest(core::u64 digest, core::u64 value) noexcept {
    core::u64 z = digest ^ (value + 0x9E3779B97F4A7C15ULL + (digest << 6U) + (digest >> 2U));
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

[[nodiscard]] core::u64 digest_bytes(core::u64 digest, std::span<const core::u8> bytes) noexcept;

// A digest no other draw shares, for an input that cannot be named across
// recordings or renderers: a key holding one is never matched.
[[nodiscard]] core::u64 unmatched_digest() noexcept;

// The one derivation of a frame's viewpoint.
//
// Shared rather than duplicated because the compositor draws the environment
// behind layers that a renderer draws into: two derivations that agreed today
// would drift the background against the geometry by whatever the apron is the
// first time one of them changed.
[[nodiscard]] bool resolve_render_view(
    const scene::Scene& scene, scene::CanvasRect slice,
    const SceneRenderOptions& options, SceneRenderView& out) noexcept;


} // namespace mediaserver::render::scene_detail
