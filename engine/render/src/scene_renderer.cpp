#include <mediaserver/scene/lighting.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/core/idle_report.hpp>
#include <mediaserver/render/effect_execution.hpp>

#include <mediaserver/render/model_loader.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/output_vert_dxil.h>
#include <mediaserver/shaders/scene_coverage_frag_dxil.h>
#include <mediaserver/shaders/sky_frag_dxil.h>
#include <mediaserver/shaders/solid_vert_dxil.h>
#include <mediaserver/shaders/scene_frag_dxil.h>
#include <mediaserver/shaders/scene_linear_frag_dxil.h>
#include <mediaserver/shaders/scene_media_frag_dxil.h>
#include <mediaserver/shaders/scene_media_linear_frag_dxil.h>
#include <mediaserver/shaders/scene_media_normal_frag_dxil.h>
#include <mediaserver/shaders/scene_media_object_id_frag_dxil.h>
#include <mediaserver/shaders/scene_media_velocity_frag_dxil.h>
#include <mediaserver/shaders/scene_normal_frag_dxil.h>
#include <mediaserver/shaders/scene_object_id_frag_dxil.h>
#include <mediaserver/shaders/scene_rsm_flux_frag_dxil.h>
#include <mediaserver/shaders/scene_rsm_normal_frag_dxil.h>
#include <mediaserver/shaders/scene_shadow_frag_dxil.h>
#include <mediaserver/shaders/scene_velocity_frag_dxil.h>
#include <mediaserver/shaders/scene_auxiliary_frag_dxil.h>
#include <mediaserver/shaders/scene_media_auxiliary_frag_dxil.h>
#include <mediaserver/shaders/scene_vert_dxil.h>
#include <mediaserver/shaders/scene_motion_vert_dxil.h>
#endif

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

namespace mediaserver::render {
namespace {

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

[[nodiscard]] ShaderBlobs shader_blobs() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return ShaderBlobs{
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_vert_dxil),
            sizeof(mediaserver_scene_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_motion_vert_dxil),
            sizeof(mediaserver_scene_motion_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_frag_dxil),
            sizeof(mediaserver_scene_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_linear_frag_dxil),
            sizeof(mediaserver_scene_linear_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_media_frag_dxil),
            sizeof(mediaserver_scene_media_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_media_linear_frag_dxil),
            sizeof(mediaserver_scene_media_linear_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_media_object_id_frag_dxil),
            sizeof(mediaserver_scene_media_object_id_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_media_velocity_frag_dxil),
            sizeof(mediaserver_scene_media_velocity_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_media_normal_frag_dxil),
            sizeof(mediaserver_scene_media_normal_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_object_id_frag_dxil),
            sizeof(mediaserver_scene_object_id_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_velocity_frag_dxil),
            sizeof(mediaserver_scene_velocity_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_normal_frag_dxil),
            sizeof(mediaserver_scene_normal_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_auxiliary_frag_dxil),
            sizeof(mediaserver_scene_auxiliary_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_media_auxiliary_frag_dxil),
            sizeof(mediaserver_scene_media_auxiliary_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_shadow_frag_dxil),
            sizeof(mediaserver_scene_shadow_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_rsm_normal_frag_dxil),
            sizeof(mediaserver_scene_rsm_normal_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_rsm_flux_frag_dxil),
            sizeof(mediaserver_scene_rsm_flux_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_output_vert_dxil),
            sizeof(mediaserver_output_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_coverage_frag_dxil),
            sizeof(mediaserver_scene_coverage_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_solid_vert_dxil),
            sizeof(mediaserver_solid_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_sky_frag_dxil),
            sizeof(mediaserver_sky_frag_dxil)},
        rhi::ShaderFormat::Dxil,
        true,
        true,
    };
#else
    // No shader compiler was available when this was built, so there is no 3D
    // path to offer. Saying so plainly is the whole point of the flag.
    return ShaderBlobs{};
#endif
}

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
    rhi::TextureFormat format = rhi::TextureFormat::Rgba8Unorm) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.color_target = true;
    // Sampled as well as drawn into, because presentation blits from it and a
    // readback test reads it.
    desc.sampled = true;
    return desc;
}

[[nodiscard]] rhi::TextureDesc sampled_desc(
    core::u32 width,
    core::u32 height) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    return desc;
}

[[nodiscard]] rhi::TextureDesc depth_desc(core::u32 width, core::u32 height,
                                          bool sampled = false) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Depth32Float;
    desc.depth_target = true;
    // The auxiliary depth is read by the occlusion pass. Reconstructing
    // position from anything else means reconstructing it from a derivative,
    // which spans two surfaces at exactly the discontinuities occlusion cares
    // about. The colour pass's own depth stays write-only.
    desc.sampled = sampled;
    return desc;
}

[[nodiscard]] GpuInstance to_gpu(const scene::MeshInstance& instance,
                                 core::u32 first_effect,
                                 core::u32 effect_count,
                                 bool depth_only = false,
                                 DecodedPixelLayout media_layout =
                                     DecodedPixelLayout::Rgba8,
                                 const scene::Mat4* previous_world = nullptr)
    noexcept {
    GpuInstance record{};
    for (core::u32 index = 0U; index < 16U; ++index) {
        record.world[index] = instance.world.m[index];
        record.previous_world[index] = previous_world == nullptr
            ? instance.world.m[index]
            : previous_world->m[index];
    }
    // The first twelve elements of a column-major 4x4 are exactly its first
    // three columns, and the fourth element of each is zero for a basis, so
    // this is a copy rather than a gather.
    for (core::u32 index = 0U; index < 12U; ++index) {
        record.normal[index] = instance.normal.m[index];
    }
    record.base_color[0U] = instance.material.base_color.x;
    record.base_color[1U] = instance.material.base_color.y;
    record.base_color[2U] = instance.material.base_color.z;
    record.roughness = instance.material.roughness;
    record.emissive[0U] = instance.material.emissive.x
        * instance.material.emissive_strength;
    record.emissive[1U] = instance.material.emissive.y
        * instance.material.emissive_strength;
    record.emissive[2U] = instance.material.emissive.z
        * instance.material.emissive_strength;
    record.metallic = instance.material.metallic;
    record.material_properties[0U] =
        std::isfinite(instance.material.normal_scale)
        ? std::max(instance.material.normal_scale, 0.0F)
        : 1.0F;
    record.material_properties[1U] =
        std::isfinite(instance.material.occlusion_strength)
        ? std::clamp(instance.material.occlusion_strength, 0.0F, 1.0F)
        : 1.0F;
    record.coat[0U] = instance.material.clearcoat;
    record.coat[1U] = instance.material.clearcoat_roughness;
    record.coat[2U] = instance.material.clearcoat_normal_scale;
    record.coat[3U] = instance.material.transmission;
    record.sheen[0U] = instance.material.sheen_color.x;
    record.sheen[1U] = instance.material.sheen_color.y;
    record.sheen[2U] = instance.material.sheen_color.z;
    record.sheen[3U] = instance.material.sheen_roughness;
    record.specular[0U] = instance.material.specular_color.x;
    record.specular[1U] = instance.material.specular_color.y;
    record.specular[2U] = instance.material.specular_color.z;
    record.specular[3U] = instance.material.specular;
    record.anisotropy[0U] = instance.material.anisotropy_strength;
    record.anisotropy[1U] = instance.material.anisotropy_rotation;
    record.anisotropy[2U] = instance.material.ior;
    record.anisotropy[3U] = instance.material.base_alpha;
    record.alpha[0U] = instance.material.alpha_cutoff;
    record.alpha[1U] = static_cast<float>(instance.material.alpha_mode);
    record.alpha[2U] = instance.material.double_sided ? 1.0F : 0.0F;
    record.layer_style[0U] = std::isfinite(instance.style.opacity)
        ? std::clamp(instance.style.opacity, 0.0F, 1.0F)
        : 1.0F;
    record.layer_style[1U] = std::isfinite(instance.style.horizontal_blend)
        ? std::clamp(instance.style.horizontal_blend, 0.0F, 0.5F)
        : 0.0F;
    switch (instance.style.blend) {
    case scene::BlendMode::Normal:
    case scene::BlendMode::Add:
    case scene::BlendMode::Multiply:
    case scene::BlendMode::Screen:
        record.layer_style[2U] = static_cast<float>(
            static_cast<core::u8>(instance.style.blend));
        break;
    default:
        record.layer_style[2U] = 0.0F;
        break;
    }
    // A depth-only instance stands in for geometry another compositor layer
    // owns. The fragment stage returns a fully transparent black for it, so it
    // contributes depth and nothing else; the flag rather than an opacity of
    // zero because zero opacity means "not drawn at all" everywhere else.
    record.layer_style[3U] = depth_only ? 1.0F : 0.0F;
    record.effect_range[0U] = depth_only ? 0U : first_effect;
    record.effect_range[1U] = depth_only ? 0U : effect_count;
    record.effect_range[2U] = static_cast<core::u32>(media_layout);
    const core::u64 source = instance.source.value();
    core::u32 object_id = static_cast<core::u32>(source)
        ^ static_cast<core::u32>(source >> 32U);
    object_id ^= object_id >> 16U;
    object_id *= 0x7FEB352DU;
    object_id ^= object_id >> 15U;
    object_id *= 0x846CA68BU;
    object_id ^= object_id >> 16U;
    record.effect_range[3U] = object_id == 0U ? 1U : object_id;
    // Blur is intentionally absent. It needs an offscreen source and a
    // separable compositor pass; treating a radius as a pointwise colour tweak
    // would claim an effect the renderer did not perform.
    return record;
}

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
    const scene::MeshInstance& instance) noexcept {
    if (!instance.material.fill.has_value()) {
        return std::nullopt;
    }
    const scene::Fill& fill = *instance.material.fill;
    const auto column_length = [&](std::size_t column) noexcept {
        const float x = instance.world.m[column * 4U];
        const float y = instance.world.m[column * 4U + 1U];
        const float z = instance.world.m[column * 4U + 2U];
        const float length = std::sqrt(x * x + y * y + z * z);
        return std::isfinite(length) ? length : 0.0F;
    };
    float width = 1.0F;
    float height = 1.0F;
    if (instance.mesh.primitive == scene::Primitive::Plane) {
        width = column_length(0U);
        height = column_length(2U);
    } else if (instance.mesh.primitive == scene::Primitive::Quad) {
        width = column_length(0U);
        height = column_length(1U);
    }
    SurfaceFillProgram program{};
    auto& ramp = program.parameters[0U].values;
    auto& placement = program.parameters[1U].values;
    ramp[0U] = static_cast<float>(static_cast<core::u8>(fill.mode));
    placement[0U] = fill.center_x * width;
    placement[1U] = fill.center_y * height;
    placement[2U] = width;
    placement[3U] = height;
    if (fill.mode == scene::FillMode::Linear) {
        const float x = scene::deterministic::cos_turns(fill.angle);
        const float y = scene::deterministic::sin_turns(fill.angle);
        // The surface's extent along the ramp, so a size of 1 runs exactly
        // from one edge to the opposite one.
        const float length =
            (std::abs(x) * width + std::abs(y) * height) * fill.size;
        ramp[1U] = length > 0.0F ? 1.0F / length : 0.0F;
        ramp[2U] = x;
        ramp[3U] = y;
    } else if (fill.mode == scene::FillMode::Radial) {
        const float radius =
            fill.size * 0.5F * std::sqrt(width * width + height * height);
        ramp[1U] = radius > 0.0F ? 1.0F / radius : 0.0F;
    }
    const auto color = [](const scene::FillColor& value,
                          GpuInlineEffectParameter& into) noexcept {
        into.values[0U] = value.r;
        into.values[1U] = value.g;
        into.values[2U] = value.b;
        into.values[3U] = value.a;
    };
    color(fill.color_a, program.parameters[2U]);
    color(fill.mode == scene::FillMode::Solid ? fill.color_a : fill.color_b,
          program.parameters[3U]);
    return program;
}

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
    const std::optional<SurfaceFillProgram>& fill = std::nullopt) {
    const std::size_t operation_start = operation_bytes.size();
    const std::size_t parameter_start = parameter_bytes.size();
    const core::u32 curve_lut_start = curve_luts_used;
    range.first = static_cast<core::u32>(
        operation_start / sizeof(GpuInlineEffectOp));
    range.count = 0U;

    const auto rollback = [&]() noexcept {
        operation_bytes.resize(operation_start);
        parameter_bytes.resize(parameter_start);
        curve_luts_used = curve_lut_start;
        range = CompiledInlineRange{};
    };
    const auto room_for = [&](core::u32 parameters) noexcept {
        const std::size_t held_operations =
            operation_bytes.size() / sizeof(GpuInlineEffectOp);
        const std::size_t held_parameters =
            parameter_bytes.size() / sizeof(GpuInlineEffectParameter);
        return held_operations < max_operations
            && held_parameters + parameters <= max_parameters;
    };
    const auto emit_scalar = [&](scene::EffectKind kind, float value) {
        if (!room_for(1U)) {
            return false;
        }
        const core::u32 parameter_offset = static_cast<core::u32>(
            parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
        GpuInlineEffectParameter parameter{};
        parameter.values[0U] = value;
        append(parameter_bytes, parameter);
        append(operation_bytes,
               GpuInlineEffectOp{static_cast<core::u32>(kind),
                                 parameter_offset, 1U, 0U});
        ++range.count;
        return true;
    };
    const auto emit_levels = [&](const scene::LevelsParameters& authored) {
        const auto normalized = scene::normalized_levels(authored);
        if (!normalized.has_value() || !room_for(8U)) {
            return false;
        }
        const core::u32 parameter_offset = static_cast<core::u32>(
            parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
        const scene::LevelsChannelParameters* const channels[]{
            &normalized->master, &normalized->red, &normalized->green,
            &normalized->blue};
        for (const scene::LevelsChannelParameters* const channel : channels) {
            GpuInlineEffectParameter first{};
            first.values[0U] = channel->input_black;
            first.values[1U] = channel->input_white;
            first.values[2U] = channel->gamma;
            first.values[3U] = channel->output_black;
            append(parameter_bytes, first);
            GpuInlineEffectParameter second{};
            second.values[0U] = channel->output_white;
            append(parameter_bytes, second);
        }
        append(operation_bytes,
               GpuInlineEffectOp{
                   static_cast<core::u32>(scene::EffectKind::Levels),
                   parameter_offset, 8U, 0U});
        ++range.count;
        return true;
    };
    const auto emit_lift_gamma_gain =
        [&](const scene::LiftGammaGainParameters& authored) {
            const auto normalized = scene::normalized_lift_gamma_gain(authored);
            if (!normalized.has_value() || !room_for(3U)) {
                return false;
            }
            const core::u32 parameter_offset = static_cast<core::u32>(
                parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
            for (const scene::Vec3 vector : {
                     normalized->lift, normalized->gamma,
                     normalized->gain}) {
                GpuInlineEffectParameter parameter{};
                parameter.values[0U] = vector.x;
                parameter.values[1U] = vector.y;
                parameter.values[2U] = vector.z;
                append(parameter_bytes, parameter);
            }
            append(operation_bytes,
                   GpuInlineEffectOp{
                       static_cast<core::u32>(
                           scene::EffectKind::LiftGammaGain),
                       parameter_offset, 3U, 0U});
            ++range.count;
            return true;
        };
    const auto emit_contrast_pivot =
        [&](const scene::ContrastPivotParameters& authored) {
            const auto normalized = scene::normalized_contrast_pivot(authored);
            if (!normalized.has_value() || !room_for(1U)) {
                return false;
            }
            const core::u32 parameter_offset = static_cast<core::u32>(
                parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
            GpuInlineEffectParameter parameter{};
            parameter.values[0U] = normalized->contrast;
            parameter.values[1U] = normalized->pivot;
            append(parameter_bytes, parameter);
            append(operation_bytes,
                   GpuInlineEffectOp{
                       static_cast<core::u32>(
                           scene::EffectKind::ContrastPivot),
                       parameter_offset, 1U, 0U});
            ++range.count;
            return true;
        };
    const auto emit_color_matrix =
        [&](const scene::ColorMatrixParameters& authored) {
            const auto normalized = scene::normalized_color_matrix(authored);
            if (!normalized.has_value() || !room_for(3U)) {
                return false;
            }
            const core::u32 parameter_offset = static_cast<core::u32>(
                parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
            const float offsets[]{normalized->offset.x, normalized->offset.y,
                                  normalized->offset.z};
            for (std::size_t row = 0U; row < 3U; ++row) {
                GpuInlineEffectParameter parameter{};
                parameter.values[0U] = normalized->matrix[row * 3U];
                parameter.values[1U] = normalized->matrix[row * 3U + 1U];
                parameter.values[2U] = normalized->matrix[row * 3U + 2U];
                parameter.values[3U] = offsets[row];
                append(parameter_bytes, parameter);
            }
            append(operation_bytes,
                   GpuInlineEffectOp{
                       static_cast<core::u32>(
                           scene::EffectKind::ColorMatrix),
                       parameter_offset, 3U, 0U});
            ++range.count;
            return true;
        };
    const auto emit_white_balance =
        [&](const scene::WhiteBalanceParameters& authored) {
            const auto normalized = scene::normalized_white_balance(authored);
            if (!normalized.has_value() || !room_for(3U)) {
                return false;
            }
            const core::u32 parameter_offset = static_cast<core::u32>(
                parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
            for (std::size_t row = 0U; row < 3U; ++row) {
                GpuInlineEffectParameter parameter{};
                parameter.values[0U] = normalized->matrix[row * 3U];
                parameter.values[1U] = normalized->matrix[row * 3U + 1U];
                parameter.values[2U] = normalized->matrix[row * 3U + 2U];
                append(parameter_bytes, parameter);
            }
            append(operation_bytes,
                   GpuInlineEffectOp{
                       static_cast<core::u32>(
                           scene::EffectKind::WhiteBalance),
                       parameter_offset, 3U, 0U});
            ++range.count;
            return true;
        };
    const auto emit_curves = [&](const scene::CurvesParameters& authored) {
        if (!scene::curves_lut_is_canonical(authored)
            || curve_luts_used >= max_curve_luts
            || !room_for(scene::curve_lut_entries)) {
            return false;
        }
        const core::u32 parameter_offset = static_cast<core::u32>(
            parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
        for (const scene::Vec4 sample : authored.lut) {
            GpuInlineEffectParameter parameter{};
            parameter.values[0U] = sample.x;
            parameter.values[1U] = sample.y;
            parameter.values[2U] = sample.z;
            parameter.values[3U] = sample.w;
            append(parameter_bytes, parameter);
        }
        append(operation_bytes,
               GpuInlineEffectOp{
                   static_cast<core::u32>(scene::EffectKind::Curves),
                   parameter_offset, scene::curve_lut_entries, 0U});
        ++range.count;
        ++curve_luts_used;
        return true;
    };
    const auto emit_film_grain =
        [&](const scene::FilmGrainParameters& authored) {
            const auto normalized = scene::normalized_film_grain(authored);
            if (!normalized.has_value() || !room_for(1U)) {
                return false;
            }
            const core::u32 parameter_offset = static_cast<core::u32>(
                parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
            GpuInlineEffectParameter parameter{};
            parameter.values[0U] = normalized->amount;
            parameter.values[1U] = normalized->pixel_scale;
            parameter.values[2U] = normalized->chroma_ratio;
            parameter.values[3U] =
                normalized->mode == scene::FilmGrainMode::RGB ? 1.0F : 0.0F;
            append(parameter_bytes, parameter);
            append(operation_bytes,
                   GpuInlineEffectOp{
                       static_cast<core::u32>(scene::EffectKind::FilmGrain),
                       parameter_offset, 1U, normalized->seed});
            ++range.count;
            return true;
        };

    if (fill.has_value()) {
        if (!room_for(4U)) {
            rollback();
            return false;
        }
        const core::u32 parameter_offset = static_cast<core::u32>(
            parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
        for (const GpuInlineEffectParameter& parameter : fill->parameters) {
            append(parameter_bytes, parameter);
        }
        append(operation_bytes,
               GpuInlineEffectOp{surface_fill_inline_kind, parameter_offset,
                                 4U, 0U});
        ++range.count;
    }

    if (effects.inline_operations.empty()) {
        // Hand-built/legacy Scene values already carry response-shaped scalar
        // values. They form the historical Brightness-then-Saturation program.
        if (effects.brightness_enabled && std::isfinite(effects.brightness)
            && effects.brightness != 1.0F
            && !emit_scalar(scene::EffectKind::Brightness,
                            std::max(effects.brightness, 0.0F))) {
            rollback();
            return false;
        }
        const float saturation =
            scene::normalized_saturation(effects.saturation);
        if (effects.saturation_enabled && saturation != 1.0F
            && !emit_scalar(scene::EffectKind::Saturation, saturation)) {
            rollback();
            return false;
        }
        return true;
    }

    double brightness_product = 1.0;
    double saturation_product = 1.0;
    bool brightness_seen{};
    bool saturation_seen{};
    const auto flush_segment = [&]() {
        if (brightness_seen) {
            const float brightness = scene::brightness_from_product(
                brightness_product, settings.brightness_gamma);
            if (brightness != 1.0F
                && !emit_scalar(scene::EffectKind::Brightness, brightness)) {
                return false;
            }
        }
        if (saturation_seen) {
            const float saturation =
                scene::saturation_from_product(saturation_product);
            if (saturation != 1.0F
                && !emit_scalar(scene::EffectKind::Saturation, saturation)) {
                return false;
            }
        }
        brightness_product = 1.0;
        saturation_product = 1.0;
        brightness_seen = false;
        saturation_seen = false;
        return true;
    };

    for (const scene::InlineEffectOperation& operation :
         effects.inline_operations) {
        switch (operation.kind) {
        case scene::EffectKind::Brightness:
            brightness_seen = true;
            brightness_product = scene::composed_brightness_product(
                brightness_product, static_cast<double>(operation.value));
            break;
        case scene::EffectKind::Saturation:
            saturation_seen = true;
            saturation_product = scene::composed_saturation_product(
                saturation_product, static_cast<double>(operation.value));
            break;
        case scene::EffectKind::Levels:
            if (!flush_segment()) {
                rollback();
                return false;
            }
            if (!scene::levels_is_identity(operation.levels)
                && !emit_levels(operation.levels)) {
                rollback();
                return false;
            }
            break;
        case scene::EffectKind::LiftGammaGain:
            if (!flush_segment()) {
                rollback();
                return false;
            }
            if (!scene::lift_gamma_gain_is_identity(
                    operation.lift_gamma_gain)
                && !emit_lift_gamma_gain(operation.lift_gamma_gain)) {
                rollback();
                return false;
            }
            break;
        case scene::EffectKind::ContrastPivot:
            if (!flush_segment()) {
                rollback();
                return false;
            }
            if (!scene::contrast_pivot_is_identity(
                    operation.contrast_pivot)
                && !emit_contrast_pivot(operation.contrast_pivot)) {
                rollback();
                return false;
            }
            break;
        case scene::EffectKind::ColorMatrix:
            if (!flush_segment()) {
                rollback();
                return false;
            }
            if (!scene::color_matrix_is_identity(operation.color_matrix)
                && !emit_color_matrix(operation.color_matrix)) {
                rollback();
                return false;
            }
            break;
        case scene::EffectKind::WhiteBalance:
            if (!flush_segment()) {
                rollback();
                return false;
            }
            if (!scene::white_balance_is_identity(operation.white_balance)
                && !emit_white_balance(operation.white_balance)) {
                rollback();
                return false;
            }
            break;
        case scene::EffectKind::Curves:
            if (!flush_segment()) {
                rollback();
                return false;
            }
            if (!scene::curves_is_identity(operation.curves)
                && !emit_curves(operation.curves)) {
                rollback();
                return false;
            }
            break;
        case scene::EffectKind::FilmGrain:
            if (!flush_segment()) {
                rollback();
                return false;
            }
            if (!scene::film_grain_is_identity(operation.film_grain)
                && !emit_film_grain(operation.film_grain)) {
                rollback();
                return false;
            }
            break;
        case scene::EffectKind::Blur:
        case scene::EffectKind::BoxBlur:
        case scene::EffectKind::DirectionalBlur:
        case scene::EffectKind::ChromaKey:
        case scene::EffectKind::LumaKey:
        case scene::EffectKind::Count:
            rollback();
            return false;
        }
    }
    if (!flush_segment()) {
        rollback();
        return false;
    }
    return true;
}

[[nodiscard]] scene::BlendMode valid_blend(scene::BlendMode blend) noexcept {
    switch (blend) {
    case scene::BlendMode::Normal:
    case scene::BlendMode::Add:
    case scene::BlendMode::Multiply:
    case scene::BlendMode::Screen:
        return blend;
    }
    return scene::BlendMode::Normal;
}

[[nodiscard]] float opacity(const scene::LayerStyle& style) noexcept {
    return std::isfinite(style.opacity)
        ? std::clamp(style.opacity, 0.0F, 1.0F)
        : 1.0F;
}

[[nodiscard]] bool needs_compositing(const scene::LayerStyle& style) noexcept {
    return opacity(style) < 1.0F
        || (std::isfinite(style.horizontal_blend) &&
            style.horizontal_blend > 0.0F)
        || valid_blend(style.blend) != scene::BlendMode::Normal;
}

[[nodiscard]] std::size_t blend_index(scene::BlendMode blend) noexcept {
    return static_cast<std::size_t>(
        static_cast<core::u8>(valid_blend(blend)));
}

[[nodiscard]] rhi::ColorBlend to_rhi_blend(
    scene::BlendMode blend,
    bool premultiplied = false) noexcept {
    switch (valid_blend(blend)) {
    case scene::BlendMode::Add:
        return premultiplied ? rhi::ColorBlend::PremultipliedAdd
                             : rhi::ColorBlend::Add;
    case scene::BlendMode::Multiply:
        return rhi::ColorBlend::Multiply;
    case scene::BlendMode::Screen:
        return rhi::ColorBlend::Screen;
    case scene::BlendMode::Normal:
        break;
    }
    return premultiplied ? rhi::ColorBlend::PremultipliedNormal
                         : rhi::ColorBlend::Normal;
}

[[nodiscard]] float camera_depth(
    const scene::Mat4& world,
    scene::Vec3 local_center,
    scene::Vec3 eye,
    scene::Vec3 forward) noexcept {
    forward = scene::normalize(forward, scene::Vec3{0.0F, 0.0F, -1.0F});
    const float result = scene::dot(
        scene::transform_point(world, local_center) - eye, forward);
    return std::isfinite(result) ? result : 0.0F;
}

constexpr ModelBounds unit_bounds{
    scene::Vec3{-0.5F, -0.5F, -0.5F}, scene::Vec3{0.5F, 0.5F, 0.5F}};

// Where a procedural primitive lies before its world transform. The quad and
// plane have no thickness, which is what marks them flat for layer ordering.
[[nodiscard]] ModelBounds primitive_bounds(scene::Primitive primitive) noexcept {
    switch (primitive) {
    case scene::Primitive::Quad:
        return ModelBounds{scene::Vec3{-0.5F, -0.5F, 0.0F},
                           scene::Vec3{0.5F, 0.5F, 0.0F}};
    case scene::Primitive::Plane:
        return ModelBounds{scene::Vec3{-0.5F, 0.0F, -0.5F},
                           scene::Vec3{0.5F, 0.0F, 0.5F}};
    case scene::Primitive::Box:
    case scene::Primitive::Sphere:
    case scene::Primitive::Cylinder:
        break;
    }
    return unit_bounds;
}

// How much plane-side layer ordering one recording may do: pairs of layers
// compared, and relations kept, on average per layer. A frame needing more
// keeps the remaining flat layers in centre order and counts them, so this
// stays a fraction of a millisecond however many layers a show composites.
constexpr std::size_t layer_pair_budget = 32'768U;
constexpr std::size_t layer_edges_per_layer = 8U;

[[nodiscard]] float component(scene::Vec3 value, std::size_t axis) noexcept {
    return axis == 0U ? value.x : axis == 1U ? value.y : value.z;
}

[[nodiscard]] bool finite(scene::Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z);
}

[[nodiscard]] bool finite(const scene::Mat4& value) noexcept {
    return std::all_of(value.m.begin(), value.m.end(), [](float element) {
        return std::isfinite(element);
    });
}

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
    scene::Vec3 local_maximum) noexcept {
    scene::Vec3 candidate_minimum{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    scene::Vec3 candidate_maximum{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
    for (core::u32 corner = 0U; corner < 8U; ++corner) {
        const scene::Vec3 local{
            (corner & 1U) != 0U ? local_maximum.x : local_minimum.x,
            (corner & 2U) != 0U ? local_maximum.y : local_minimum.y,
            (corner & 4U) != 0U ? local_maximum.z : local_minimum.z};
        const scene::Vec3 point = scene::transform_point(world, local);
        if (!finite(point)) {
            return;
        }
        candidate_minimum.x = std::min(candidate_minimum.x, point.x);
        candidate_minimum.y = std::min(candidate_minimum.y, point.y);
        candidate_minimum.z = std::min(candidate_minimum.z, point.z);
        candidate_maximum.x = std::max(candidate_maximum.x, point.x);
        candidate_maximum.y = std::max(candidate_maximum.y, point.y);
        candidate_maximum.z = std::max(candidate_maximum.z, point.z);
    }
    bounds.minimum.x = std::min(bounds.minimum.x, candidate_minimum.x);
    bounds.minimum.y = std::min(bounds.minimum.y, candidate_minimum.y);
    bounds.minimum.z = std::min(bounds.minimum.z, candidate_minimum.z);
    bounds.maximum.x = std::max(bounds.maximum.x, candidate_maximum.x);
    bounds.maximum.y = std::max(bounds.maximum.y, candidate_maximum.y);
    bounds.maximum.z = std::max(bounds.maximum.z, candidate_maximum.z);
    ++bounds.casters;
}

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
    float& texel_depth) noexcept {
    if (bounds.casters == 0U || resolution == 0U
        || !finite(bounds.minimum) || !finite(bounds.maximum)) {
        return false;
    }
    const scene::Vec3 center =
        (bounds.minimum + bounds.maximum) * 0.5F;
    const scene::Vec3 half =
        (bounds.maximum - bounds.minimum) * 0.5F;
    const float radius = std::max(scene::length(half), 0.5F);
    const scene::Vec3 direction = scene::normalize(
        light.direction, scene::Vec3{0.0F, -1.0F, 0.0F});
    const scene::Vec3 up = std::abs(scene::dot(
        direction, scene::Vec3{0.0F, 1.0F, 0.0F})) > 0.95F
        ? scene::Vec3{0.0F, 0.0F, 1.0F}
        : scene::Vec3{0.0F, 1.0F, 0.0F};
    const scene::Vec3 eye = center - direction * (radius * 2.0F + 1.0F);
    const scene::Mat4 view = scene::look_at(eye, center, up);

    scene::Vec3 light_min{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    scene::Vec3 light_max{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
    for (core::u32 corner = 0U; corner < 8U; ++corner) {
        const scene::Vec3 world{
            (corner & 1U) != 0U ? bounds.maximum.x : bounds.minimum.x,
            (corner & 2U) != 0U ? bounds.maximum.y : bounds.minimum.y,
            (corner & 4U) != 0U ? bounds.maximum.z : bounds.minimum.z};
        const scene::Vec3 point = scene::transform_point(view, world);
        light_min.x = std::min(light_min.x, point.x);
        light_min.y = std::min(light_min.y, point.y);
        light_min.z = std::min(light_min.z, point.z);
        light_max.x = std::max(light_max.x, point.x);
        light_max.y = std::max(light_max.y, point.y);
        light_max.z = std::max(light_max.z, point.z);
    }

    const float margin = std::max(radius * 0.05F, 0.05F);
    float left = light_min.x - margin;
    float right = light_max.x + margin;
    float bottom = light_min.y - margin;
    float top = light_max.y + margin;
    const float width = right - left;
    const float height = top - bottom;
    if (!(width > 0.0F) || !(height > 0.0F)) {
        return false;
    }

    // Snap the full-scene projection to shadow texels. Camera and output-slice
    // motion therefore cannot make adjacent nodes choose different shadow
    // samples for the same authored world point.
    const float texel_x = width / static_cast<float>(resolution);
    const float texel_y = height / static_cast<float>(resolution);
    const float center_x = (left + right) * 0.5F;
    const float center_y = (bottom + top) * 0.5F;
    const float snapped_x = std::floor(center_x / texel_x + 0.5F) * texel_x;
    const float snapped_y = std::floor(center_y / texel_y + 0.5F) * texel_y;
    left += snapped_x - center_x;
    right += snapped_x - center_x;
    bottom += snapped_y - center_y;
    top += snapped_y - center_y;

    const float near_plane = std::max(0.01F, -light_max.z - margin);
    const float far_plane = std::max(near_plane + 0.01F, -light_min.z + margin);
    const float depth_span = far_plane - near_plane;
    scene::Mat4 projection = scene::identity();
    projection.set(0U, 0U, 2.0F / (right - left));
    projection.set(0U, 3U, -(right + left) / (right - left));
    projection.set(1U, 1U, 2.0F / (top - bottom));
    projection.set(1U, 3U, -(top + bottom) / (top - bottom));
    // Reversed [0,1] depth for a right-handed view whose forward axis is -Z.
    projection.set(2U, 2U, 1.0F / depth_span);
    projection.set(2U, 3U, far_plane / depth_span);
    into = scene::multiply(projection, view);

    scene::Mat4 inverse_projection = scene::identity();
    inverse_projection.set(0U, 0U, (right - left) * 0.5F);
    inverse_projection.set(0U, 3U, (right + left) * 0.5F);
    inverse_projection.set(1U, 1U, (top - bottom) * 0.5F);
    inverse_projection.set(1U, 3U, (top + bottom) * 0.5F);
    inverse_projection.set(2U, 2U, depth_span);
    inverse_projection.set(2U, 3U, -far_plane);
    inverse_into = scene::multiply(
        scene::invert_affine(view), inverse_projection);
    texel_depth = std::max(texel_x, texel_y) / depth_span;
    return finite(into) && finite(inverse_into)
        && std::isfinite(texel_depth);
}

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
    core::u32 count) noexcept {
    if (count == 0U) {
        return far_plane;
    }
    const float ratio =
        static_cast<float>(index) / static_cast<float>(count);
    const float logarithmic =
        near_plane * std::pow(far_plane / near_plane, ratio);
    const float uniform = near_plane + (far_plane - near_plane) * ratio;
    constexpr float lambda = 0.7F;
    return lambda * logarithmic + (1.0F - lambda) * uniform;
}

// How far the camera cascades need to reach. A show's far plane is usually far
// beyond anything that casts, so subdividing to it would spend every near
// cascade on empty space. Bound it by the casters instead, using the same
// replicated scene bounds every node already agrees on.
[[nodiscard]] float cascade_far_distance(
    const scene::Camera& camera,
    const DirectionalShadowBounds& bounds,
    float near_distance,
    float far_plane) noexcept {
    if (bounds.casters == 0U) {
        return std::max(far_plane, near_distance * 2.0F);
    }
    (void)camera;
    const float reach = std::max(near_distance * 2.0F,
        scene::length(bounds.maximum - bounds.minimum) * 2.0F);
    return std::clamp(reach, near_distance * 2.0F,
                      std::max(far_plane, near_distance * 2.0F));
}

// The bounding sphere of the authored full-canvas camera's frustum between two
// distances. `window` is the near-plane rectangle that camera sees over the
// whole canvas, so this deliberately ignores which slice the node renders.
[[nodiscard]] bool camera_cascade_volume(
    const scene::Camera& camera,
    const scene::FrustumWindow& window,
    float near_distance,
    float far_distance,
    CascadeVolume& into) noexcept {
    if (!(window.near_plane > 0.0F) || !(far_distance > near_distance)) {
        return false;
    }
    const scene::Vec3 forward =
        scene::normalize(camera.target - camera.eye);
    const scene::Vec3 right =
        scene::normalize(scene::cross(forward, camera.up));
    const scene::Vec3 up = scene::cross(right, forward);
    if (!finite(forward) || !finite(right) || !finite(up)) {
        return false;
    }
    std::array<scene::Vec3, 8U> corners{};
    core::u32 written{};
    for (const float distance : {near_distance, far_distance}) {
        const float scale = distance / window.near_plane;
        for (const float x : {window.left * scale, window.right * scale}) {
            for (const float y :
                 {window.bottom * scale, window.top * scale}) {
                corners[written++] = camera.eye + forward * distance
                    + right * x + up * y;
            }
        }
    }
    scene::Vec3 center{};
    for (const scene::Vec3& corner : corners) {
        if (!finite(corner)) {
            return false;
        }
        center = center + corner;
    }
    center = center * 0.125F;
    float radius{};
    for (const scene::Vec3& corner : corners) {
        radius = std::max(radius, scene::length(corner - center));
    }
    if (!finite(center) || !std::isfinite(radius) || !(radius > 0.0F)) {
        return false;
    }
    into.center = center;
    into.radius = std::ceil(radius * 16.0F) / 16.0F;
    return true;
}

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
    float& texel_depth) noexcept {
    if (bounds.casters == 0U || resolution == 0U || !finite(volume.center)
        || !std::isfinite(volume.radius) || !(volume.radius > 0.0F)) {
        return false;
    }
    const scene::Vec3 direction = scene::normalize(
        light.direction, scene::Vec3{0.0F, -1.0F, 0.0F});
    const scene::Vec3 up = std::abs(scene::dot(
        direction, scene::Vec3{0.0F, 1.0F, 0.0F})) > 0.95F
        ? scene::Vec3{0.0F, 0.0F, 1.0F}
        : scene::Vec3{0.0F, 1.0F, 0.0F};
    const scene::Vec3 scene_center = (bounds.minimum + bounds.maximum) * 0.5F;
    const float scene_radius = std::max(scene::length(bounds.maximum - bounds.minimum), 1.0F);
    const scene::Vec3 eye = scene_center - direction * (scene_radius * 2.0F + 1.0F);
    const scene::Mat4 view = scene::look_at(eye, scene_center, up);
    const scene::Vec3 light_center =
        scene::transform_point(view, volume.center);
    if (!finite(light_center)) {
        return false;
    }

    const float texel =
        (volume.radius * 2.0F) / static_cast<float>(resolution);
    if (!(texel > 0.0F)) {
        return false;
    }
    const float snapped_x =
        std::floor(light_center.x / texel + 0.5F) * texel;
    const float snapped_y =
        std::floor(light_center.y / texel + 0.5F) * texel;
    const float left = snapped_x - volume.radius;
    const float right = snapped_x + volume.radius;
    const float bottom = snapped_y - volume.radius;
    const float top = snapped_y + volume.radius;

    float scene_min_z = std::numeric_limits<float>::max();
    float scene_max_z = std::numeric_limits<float>::lowest();
    for (core::u32 corner = 0U; corner < 8U; ++corner) {
        const scene::Vec3 world{
            (corner & 1U) != 0U ? bounds.maximum.x : bounds.minimum.x,
            (corner & 2U) != 0U ? bounds.maximum.y : bounds.minimum.y,
            (corner & 4U) != 0U ? bounds.maximum.z : bounds.minimum.z};
        const scene::Vec3 point = scene::transform_point(view, world);
        if (!finite(point)) {
            return false;
        }
        scene_min_z = std::min(scene_min_z, point.z);
        scene_max_z = std::max(scene_max_z, point.z);
    }
    // View space looks down -Z, so the largest z is the closest to the light.
    const float near_z =
        scene_max_z;
    const float far_z = scene_min_z;
    const float margin = std::max(scene_radius * 0.05F, 0.05F);
    const float near_plane = std::max(0.01F, -near_z - margin);
    const float far_plane = std::max(near_plane + 0.01F, -far_z + margin);
    const float depth_span = far_plane - near_plane;

    scene::Mat4 projection = scene::identity();
    projection.set(0U, 0U, 2.0F / (right - left));
    projection.set(0U, 3U, -(right + left) / (right - left));
    projection.set(1U, 1U, 2.0F / (top - bottom));
    projection.set(1U, 3U, -(top + bottom) / (top - bottom));
    projection.set(2U, 2U, 1.0F / depth_span);
    projection.set(2U, 3U, far_plane / depth_span);
    into = scene::multiply(projection, view);
    texel_depth = texel / depth_span;
    return finite(into) && std::isfinite(texel_depth);
}

// A media surface is the unit XY quad generated by Primitive::Quad. Test its
// four corners in homogeneous clip space so quads crossing the eye or a
// frustum plane remain visible. This only returns true after one complete
// separating plane is proven; malformed transforms therefore fail open.
[[nodiscard]] bool media_quad_outside_view(
    const scene::Mat4& world,
    const scene::Mat4& view_projection,
    const scene::EffectStack& effects,
    const scene::EffectSettings& effect_settings) noexcept {
    const scene::EffectBoundsExpansion expansion =
        effect_stack_bounds(effects, effect_settings);
    if (expansion.x > 0.0F || expansion.y > 0.0F) {
        // A blur can contribute pixels beyond the geometric quad. Until its
        // expanded bounds are projected, decoding it is the conservative
        // choice.
        return false;
    }

    constexpr std::array<scene::Vec4, 4U> corners{{
        {-0.5F, -0.5F, 0.0F, 1.0F},
        {0.5F, -0.5F, 0.0F, 1.0F},
        {-0.5F, 0.5F, 0.0F, 1.0F},
        {0.5F, 0.5F, 0.0F, 1.0F},
    }};
    std::array<scene::Vec4, corners.size()> clip{};
    const scene::Mat4 clip_from_local =
        scene::multiply(view_projection, world);
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        clip[index] = scene::transform(clip_from_local, corners[index]);
        const scene::Vec4 point = clip[index];
        if (!std::isfinite(point.x) || !std::isfinite(point.y)
            || !std::isfinite(point.z) || !std::isfinite(point.w)) {
            return false;
        }
    }

    const auto all_outside = [&clip](auto&& predicate) noexcept {
        return std::all_of(clip.begin(), clip.end(), predicate);
    };
    // Direct3D clip space is -w..w in X/Y and 0..w in Z. This renderer uses
    // reversed depth, but near and far still occupy those same two Z planes.
    return all_outside([](scene::Vec4 p) { return p.x < -p.w; })
        || all_outside([](scene::Vec4 p) { return p.x > p.w; })
        || all_outside([](scene::Vec4 p) { return p.y < -p.w; })
        || all_outside([](scene::Vec4 p) { return p.y > p.w; })
        || all_outside([](scene::Vec4 p) { return p.z < 0.0F; })
        || all_outside([](scene::Vec4 p) { return p.z > p.w; });
}

// Whether a model-space box lies wholly beyond one side plane of a shadow
// projection. Every point of the box is then beyond it too, so the caster
// rasterizes nothing into that map and leaving it out changes no texel. Only
// the side planes are tested: a cascade's depth range spans every caster by
// construction, and a backend clamping depth still clips the sides. The slack
// keeps a box grazing an edge in against rounding; non-finite corners keep it.
[[nodiscard]] bool outside_shadow_projection(
    const scene::Mat4& view_projection,
    const scene::Mat4& world,
    const ModelBounds& bounds) noexcept {
    const scene::Mat4 clip_from_local = scene::multiply(view_projection, world);
    bool left = true, right = true, below = true, above = true;
    for (unsigned corner = 0U; corner < 8U; ++corner) {
        const scene::Vec4 p = scene::transform(clip_from_local, scene::Vec4{
            (corner & 1U) != 0U ? bounds.maximum.x : bounds.minimum.x,
            (corner & 2U) != 0U ? bounds.maximum.y : bounds.minimum.y,
            (corner & 4U) != 0U ? bounds.maximum.z : bounds.minimum.z, 1.0F});
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.w)) {
            return false;
        }
        const float slack = 1.0e-3F * std::abs(p.w) + 1.0e-5F;
        left = left && p.x < -p.w - slack;
        right = right && p.x > p.w + slack;
        below = below && p.y < -p.w - slack;
        above = above && p.y > p.w + slack;
    }
    return left || right || below || above;
}

// Folds one value into a running 64-bit digest. Shadow reuse keys only; a
// collision would reuse a stale map, so the mix is a full avalanche rather
// than a cheap combine.
[[nodiscard]] constexpr core::u64 mix_digest(core::u64 digest, core::u64 value) noexcept {
    core::u64 z = digest ^ (value + 0x9E3779B97F4A7C15ULL + (digest << 6U) + (digest >> 2U));
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

[[nodiscard]] core::u64 digest_bytes(core::u64 digest, std::span<const core::u8> bytes) noexcept {
    std::size_t offset = 0U;
    for (; offset + sizeof(core::u64) <= bytes.size(); offset += sizeof(core::u64)) {
        core::u64 word{};
        std::memcpy(&word, bytes.data() + offset, sizeof(word));
        digest = mix_digest(digest, word);
    }
    core::u64 tail{};
    if (offset < bytes.size()) {
        std::memcpy(&tail, bytes.data() + offset, bytes.size() - offset);
    }
    return mix_digest(digest, tail ^ (static_cast<core::u64>(bytes.size()) << 56U));
}

// A digest no other draw shares, for an input that cannot be named across
// recordings or renderers: a key holding one is never matched.
[[nodiscard]] core::u64 unmatched_digest() noexcept {
    static std::atomic<core::u64> next{1U};
    return mix_digest(0x554E4D41544348ULL, next.fetch_add(1U, std::memory_order_relaxed));
}

// The one derivation of a frame's viewpoint.
//
// Shared rather than duplicated because the compositor draws the environment
// behind layers that a renderer draws into: two derivations that agreed today
// would drift the background against the geometry by whatever the apron is the
// first time one of them changed.
[[nodiscard]] bool resolve_render_view(
    const scene::Scene& scene, scene::CanvasRect slice,
    const SceneRenderOptions& options, SceneRenderView& out) noexcept {
    if (!options.view.has_value()) {
        if (!scene::contains(scene.canvas, slice)) {
            return false;
        }
        // The window is rebuilt around the offset rather than the matrix
        // shifted afterwards, because the conversion to projection units then
        // divides by the canvas extent every node agrees on. Two adjacent
        // slices add the same number to the edge they share and it stays
        // bit-identical.
        out.matrices = scene::view_for_slice(scene.camera, scene.canvas, slice,
                                             options.jitter);
        // The whole canvas under the same camera and the same jitter. Every
        // node derives this identically because nothing about the slice enters
        // it, which is exactly why motion is expressed against it.
        out.canvas_view_projection =
            scene::view_for_slice(scene.camera, scene.canvas,
                                  scene::whole(scene.canvas), options.jitter)
                .view_projection;
        out.eye = scene.camera.eye;
        out.forward = scene::normalize(scene.camera.target - scene.camera.eye,
                                       scene::Vec3{0.0F, 0.0F, -1.0F});
        return true;
    }
    out = *options.view;
    if (!finite(out.eye) || !finite(out.forward)
        || !(scene::length(out.forward) > 1.0e-20F)
        || !finite(out.matrices.view) || !finite(out.matrices.projection)
        || !finite(out.matrices.view_projection)) {
        return false;
    }
    // An explicit view has already resolved its window, so the offset is
    // applied to the matrix in that view's own target pixels. The two forms
    // are asserted equivalent in the scene math suite.
    out.matrices = scene::jittered(out.matrices, options.jitter, slice.width,
                                   slice.height);
    // An explicit view has already resolved its own window, so there is no
    // wider canvas to project against and the two are the same matrix.
    if (!options.full_canvas_view) out.canvas_view_projection = out.matrices.view_projection;
    out.forward = scene::normalize(out.forward);
    return true;
}

} // namespace

bool SceneRenderer::shaders_available() noexcept {
    return shader_blobs().present;
}

void SceneRenderer::order_composites_by_planes(
    const SceneRenderView& view) noexcept {
    const auto count = composite_draws_.size();
    if (count < 2U) return;
    const bool orthographic = scene::orthographic(view.matrices.projection);
    const auto forward =
        scene::normalize(view.forward, scene::Vec3{0.0F, 0.0F, -1.0F});

    layer_volumes_.clear();
    for (const auto& draw : composite_draws_) {
        LayerVolume volume{};
        volume.nearest = std::numeric_limits<float>::infinity();
        volume.farthest = -std::numeric_limits<float>::infinity();
        const auto& bounds = draw.bounds;
        for (std::size_t corner = 0U; corner < volume.corners.size(); ++corner) {
            const scene::Vec3 local{
                (corner & 1U) != 0U ? bounds.maximum.x : bounds.minimum.x,
                (corner & 2U) != 0U ? bounds.maximum.y : bounds.minimum.y,
                (corner & 4U) != 0U ? bounds.maximum.z : bounds.minimum.z};
            volume.corners[corner] =
                scene::transform_point(draw.instance.world, local);
            const float depth =
                scene::dot(volume.corners[corner] - view.eye, forward);
            volume.nearest = std::min(volume.nearest, depth);
            volume.farthest = std::max(volume.farthest, depth);
        }
        // No thickness along one local axis: the surface lies in the plane
        // through the corners spanned by the other two.
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            if (component(bounds.maximum, axis) != component(bounds.minimum, axis)) {
                continue;
            }
            const std::size_t u = std::size_t{1} << ((axis + 1U) % 3U);
            const std::size_t v = std::size_t{1} << ((axis + 2U) % 3U);
            const auto origin = volume.corners[0U];
            const auto normal = scene::cross(
                volume.corners[u] - origin, volume.corners[v] - origin);
            const float area = scene::length(normal);
            if (!(area > 1.0e-12F)) break;
            volume.normal = normal * (1.0F / area);
            volume.offset = scene::dot(volume.normal, origin);
            volume.viewer_side = orthographic
                ? -scene::dot(volume.normal, forward)
                : scene::dot(volume.normal, view.eye) - volume.offset;
            // Seen edge on, a surface has no side the viewer is on.
            volume.planar = std::isfinite(volume.viewer_side)
                && std::abs(volume.viewer_side) > 1.0e-6F;
            break;
        }
        layer_volumes_.push_back(volume);
    }

    // Whether every corner of `points` is on one side of `plane`: +1 the
    // viewer's side, -1 the far side. Points within the tolerance count as
    // on the plane, so coplanar layers are on neither side and keep their
    // composite order.
    const auto wholly = [](const LayerVolume& plane,
                           const LayerVolume& points, float side) {
        const float tolerance = 1.0e-4F + 1.0e-5F * std::abs(plane.offset);
        const float facing = plane.viewer_side > 0.0F ? side : -side;
        bool off_plane = false;
        for (const auto& corner : points.corners) {
            const float distance =
                (scene::dot(plane.normal, corner) - plane.offset) * facing;
            if (!(distance >= -tolerance)) return false;
            off_plane = off_plane || distance > tolerance;
        }
        return off_plane;
    };
    // Whether `under` has to be drawn before `over`.
    const auto beneath = [&](const LayerVolume& under, const LayerVolume& over) {
        // Apart in depth, the centre order already draws the farther first.
        if (over.farthest < under.nearest || under.farthest < over.nearest) {
            return false;
        }
        return (under.planar && wholly(under, over, 1.0F))
            || (over.planar && wholly(over, under, -1.0F));
    };

    // Each flat layer against every other layer, a pair of flat ones once,
    // within a fixed budget of pairs and relations per recording.
    layer_edges_.clear();
    std::size_t pairs_left = layer_pair_budget;
    for (std::size_t first = 0U; first < count; ++first) {
        if (!layer_volumes_[first].planar) continue;
        if (pairs_left < count || layer_edges_.size() + count
                > layer_edges_.capacity()) {
            // Out of budget: this flat layer and the ones after it keep the
            // centre order against everything.
            for (std::size_t rest = first; rest < count; ++rest) {
                if (layer_volumes_[rest].planar) ++stats_.unordered_layers;
            }
            break;
        }
        pairs_left -= count;
        const auto& a = layer_volumes_[first];
        for (std::size_t second = 0U; second < count; ++second) {
            const auto& b = layer_volumes_[second];
            if (second == first || (b.planar && second < first)) continue;
            const bool a_under = beneath(a, b);
            // Both at once only for two that cross: leave those to the sort.
            if (a_under == beneath(b, a)) continue;
            const auto from = static_cast<core::u32>(a_under ? first : second);
            const auto to = static_cast<core::u32>(a_under ? second : first);
            layer_edges_.emplace_back(from, to);
        }
    }
    if (layer_edges_.empty()) return;

    // Kahn's algorithm, always taking the earliest ready layer in the sorted
    // order, so layers with no relation keep exactly the order they had.
    std::sort(layer_edges_.begin(), layer_edges_.end());
    layer_edge_offsets_.assign(count + 1U, 0U);
    layer_in_degree_.assign(count, 0U);
    for (const auto& [from, to] : layer_edges_) {
        ++layer_edge_offsets_[from + 1U];
        ++layer_in_degree_[to];
    }
    for (std::size_t index = 0U; index < count; ++index) {
        layer_edge_offsets_[index + 1U] += layer_edge_offsets_[index];
    }
    layer_placed_.assign(count, 0U);
    layer_ready_.clear();
    layer_order_.clear();
    const auto earliest_first = std::greater<core::u32>{};
    const auto make_ready = [&](core::u32 index) {
        layer_ready_.push_back(index);
        std::push_heap(layer_ready_.begin(), layer_ready_.end(), earliest_first);
    };
    for (std::size_t index = 0U; index < count; ++index) {
        if (layer_in_degree_[index] == 0U) {
            make_ready(static_cast<core::u32>(index));
        }
    }
    std::size_t unplaced = 0U;
    while (layer_order_.size() < count) {
        if (layer_ready_.empty()) {
            // A cycle: release the earliest layer still waiting on it.
            while (layer_placed_[unplaced] != 0U) ++unplaced;
            make_ready(static_cast<core::u32>(unplaced));
        }
        std::pop_heap(layer_ready_.begin(), layer_ready_.end(), earliest_first);
        const auto next = layer_ready_.back();
        layer_ready_.pop_back();
        if (layer_placed_[next] != 0U) continue;
        layer_placed_[next] = 1U;
        layer_order_.push_back(next);
        for (auto edge = layer_edge_offsets_[next];
             edge < layer_edge_offsets_[next + 1U]; ++edge) {
            const auto to = layer_edges_[edge].second;
            if (layer_placed_[to] == 0U && layer_in_degree_[to] > 0U
                && --layer_in_degree_[to] == 0U) {
                make_ready(to);
            }
        }
    }

    // Apply the order in place, one permutation cycle at a time.
    for (std::size_t start = 0U; start < count; ++start) {
        if (layer_order_[start] == start) continue;
        CompositeDraw held = std::move(composite_draws_[start]);
        std::size_t at = start;
        for (;;) {
            const std::size_t from = layer_order_[at];
            layer_order_[at] = static_cast<core::u32>(at);
            if (from == start) {
                composite_draws_[at] = std::move(held);
                break;
            }
            composite_draws_[at] = std::move(composite_draws_[from]);
            at = from;
        }
    }
}

void SceneRenderer::bind_material_textures(std::span<rhi::SampledTexture> into,
    const std::array<const rhi::Texture*, 14U>& textures,
    const std::array<ModelSampler, 14U>& samplers, core::u32 anisotropy,
    const rhi::Texture& background) noexcept {
    for (std::size_t i = 0U; i < textures.size(); ++i)
        into[i] = {textures[i], &material_samplers_[material_sampler_index(samplers[i], anisotropy)]};
    if (!scene_linear()) {
        into[14] = {&background, &media_sampler_};
        into[15] = {&ies_atlas_, &media_sampler_};
        return;
    }
    // Fourteen material samplers plus linear-clamp and nearest-clamp fit s0-s15.
    // Other advanced textures share the appropriate clamp sampler in HLSL.
    const EnvironmentMaps maps = environment_ ? environment_->maps() : EnvironmentMaps{};
    into[14] = {maps.specular ? maps.specular : &environment_cube_fallback_, &environment_sampler_};
    bind_shadow_products(into);
    into[21] = {maps.irradiance ? maps.irradiance : &environment_fallback_, &media_sampler_};
    into[22] = {environment_ && environment_->brdf() ? environment_->brdf() : &environment_fallback_, &environment_sampler_};
    into[23] = {occlusion_recorded_ && occlusion_ ? &occlusion_->target() : &material_fallback_, &media_sampler_};
    into[25] = {&background, &media_sampler_};
    into[26] = {&auxiliary_depth_, &shadow_sampler_};
    into[27] = {&ies_atlas_, &shadow_sampler_};
    for (std::size_t i = 0; i < reflection_filters_.size(); ++i)
        into[28 + i] = {active_reflections_[i].enabled ? reflection_filters_[i]->maps().specular : &environment_cube_fallback_, &environment_sampler_};
    into[32] = {planar_texture_ ? planar_texture_ : &material_fallback_, &media_sampler_};

}

void SceneRenderer::bind_shadow_products(std::span<rhi::SampledTexture> into) const noexcept {
    // Lanes past the cascades a frame draws are never read, but are bound.
    for (std::size_t i = 0U; i < scene::max_shadow_cascades; ++i) {
        const auto& product = cascades_[i] ? cascades_[i]
            : cascade_homes_[std::min(i, cascade_homes_.size() - 1U)];
        into[15U + i] = {&product->textures[0U], &shadow_sampler_};
    }
    const auto& rsm = rsm_ ? rsm_ : rsm_home_;
    into[19] = {&rsm->textures[0U], &shadow_sampler_};
    into[20] = {&rsm->textures[1U], &shadow_sampler_};
    into[24] = {&(atlas_ ? atlas_ : atlas_home_)->textures[0U], &shadow_sampler_};
}

SceneRenderer::SceneRenderer(
    rhi::Device& device,
    RendererLimits limits,
    rhi::Texture color,
    rhi::Texture depth,
    rhi::GraphicsPipeline pipeline,
    std::array<rhi::GraphicsPipeline, 4U> blended_pipelines,
    std::array<rhi::GraphicsPipeline, 4U> media_blended_pipelines,
    rhi::GraphicsPipeline media_depth_pipeline,
    rhi::GraphicsPipeline blended_depth_pipeline,
    rhi::Sampler media_sampler,
    rhi::Texture material_fallback,
    rhi::Texture normal_fallback,
    std::shared_ptr<GeometryResidency> geometry,
    rhi::StorageBuffer instances,
    rhi::StorageBuffer lights,
    rhi::StorageBuffer inline_effect_ops,
    rhi::StorageBuffer inline_effect_parameters,
    SceneRendererMode mode,
    rhi::Texture velocity,
    rhi::Texture object_id,
    rhi::Texture normal,
    rhi::Texture coverage,
    rhi::Texture auxiliary_depth,
    rhi::GraphicsPipeline velocity_pipeline,
    rhi::GraphicsPipeline object_id_pipeline,
    rhi::GraphicsPipeline normal_pipeline,
    rhi::GraphicsPipeline media_velocity_pipeline,
    rhi::GraphicsPipeline media_object_id_pipeline,
    rhi::GraphicsPipeline media_normal_pipeline,
    rhi::GraphicsPipeline coverage_pipeline,
    rhi::Sampler coverage_sampler) noexcept
    : device_(&device),
      limits_(limits),
      color_(std::move(color)),
      depth_(std::move(depth)),
      pipeline_(std::move(pipeline)),
      blended_pipelines_(std::move(blended_pipelines)),
      media_blended_pipelines_(std::move(media_blended_pipelines)),
      media_depth_pipeline_(std::move(media_depth_pipeline)),
      blended_depth_pipeline_(std::move(blended_depth_pipeline)),
      media_sampler_(std::move(media_sampler)),
      material_fallback_(std::move(material_fallback)),
      normal_fallback_(std::move(normal_fallback)),
      geometry_(std::move(geometry)),
      instance_buffer_(std::move(instances)),
      light_buffer_(std::move(lights)),
      inline_effect_op_buffer_(std::move(inline_effect_ops)),
      inline_effect_parameter_buffer_(std::move(inline_effect_parameters)),
      mode_(mode), velocity_(std::move(velocity)),
      object_id_(std::move(object_id)), normal_(std::move(normal)),
      coverage_(std::move(coverage)),
      auxiliary_depth_(std::move(auxiliary_depth)),
      velocity_pipeline_(std::move(velocity_pipeline)),
      object_id_pipeline_(std::move(object_id_pipeline)),
      normal_pipeline_(std::move(normal_pipeline)),
      media_normal_pipeline_(std::move(media_normal_pipeline)),
      media_velocity_pipeline_(std::move(media_velocity_pipeline)),
      media_object_id_pipeline_(std::move(media_object_id_pipeline)),
      coverage_pipeline_(std::move(coverage_pipeline)),
      coverage_sampler_(std::move(coverage_sampler)) {}

Result SceneRenderer::create(
    rhi::Device& device,
    core::u32 width,
    core::u32 height,
    const RendererLimits& limits,
    SceneRendererMode mode) {
    const auto blobs = shader_blobs();
    if (!blobs.present) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }
    // Every scene renderer names its device as a candidate for video decode.
    // A scene is the only thing that samples media, so this is the complete
    // set the decoder has to agree with.
    offer_video_decode_device(device);
    if (width == 0U || height == 0U || limits.max_instances == 0U
        || limits.max_history_slots == 0U
        || limits.max_model_refresh_generations == 0U
        || limits.max_model_textures == 0U
        || limits.max_model_texture_bytes == 0U
        || limits.max_vertices == 0U || limits.max_indices == 0U
        || limits.max_skin_vertices == 0U || limits.geometry_upload_bytes == 0U
        || limits.texture_upload_bytes == 0U
        || limits.max_lights == 0U || limits.max_inline_effect_ops == 0U
        || limits.directional_shadow_resolution == 0U
        || limits.directional_shadow_resolution > 4'096U
        || limits.max_directional_shadow_cascades == 0U
        || limits.max_directional_shadow_cascades > scene::max_shadow_cascades
        || limits.max_inline_curve_luts == 0U
        || limits.max_inline_effect_parameter_vectors == 0U) {
        return Result::failure(RenderError::InvalidConfiguration);
    }
    if (!device.supports(blobs.format)) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }
    const bool advanced = mode == SceneRendererMode::AdvancedLinear;
    if (advanced && !device.capabilities().advanced_raster()) {
        return Result::failure(RenderError::CapabilityUnsupported);
    }
    const auto color_format = advanced ? rhi::TextureFormat::Rgba16Float
                                       : rhi::TextureFormat::Rgba8Unorm;

    auto color = device.create_texture(color_desc(width, height, color_format));
    auto transmission_background = device.create_texture(color_desc(width, height, color_format));
    if (!transmission_background) return Result::failure(RenderError::AllocationFailed);
    if (!color) {
        return Result::failure(RenderError::AllocationFailed);
    }
    auto depth = device.create_texture(depth_desc(width, height));
    if (!depth) {
        return Result::failure(RenderError::AllocationFailed);
    }

    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = blobs.format;
    vertex.bytecode = blobs.vertex;
    // Vertices, indices, instances, and the parallel tangent stream; two
    // uniform slots, the view and the per-batch offsets.
    vertex.storage_buffer_count = 6U;
    vertex.uniform_buffer_count = 2U;

    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = blobs.format;
    fragment.bytecode = advanced ? blobs.linear_fragment : blobs.fragment;
    // Five material maps, then the fixed cascade set and the two reflective-
    // shadow-map attributes. The cascade slots are declared whether or not a
    // show authored that many, so one pipeline serves every authored count and
    // the binding shape never depends on document state.
    // ... and then the three image-based-lighting inputs, declared whether or
    // not a show authored an environment for the same reason the cascade slots
    // are: one pipeline serves every authored state, and the binding shape
    // never depends on document content.
    fragment.sampler_count =
        advanced ? 16U : basic_fragment_textures;
    fragment.sampled_texture_count = advanced ? advanced_fragment_textures : basic_fragment_textures;
    fragment.storage_buffer_count = advanced ? 7U : 4U;
    fragment.uniform_buffer_count = 1U;

    rhi::DepthState depth_state{};
    depth_state.enabled = true;
    depth_state.write = true;
    // Reversed depth: the scene layer's projection puts the near plane at one.
    depth_state.greater = true;

    auto pipeline = device.create_pipeline(
        vertex,
        fragment,
        color_format,
        rhi::ColorBlend::Disabled,
        depth_state);
    if (!pipeline) {
        return Result::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }

    // Media uses the same pulled vertex stage and binds two source slots. RGBA
    // clips use the first and ignore the fallback second slot; planar video
    // binds luma and chroma directly and converts in this fragment stage. It always
    // goes through a compositing pipeline because source pixels may have alpha.
    rhi::ShaderDesc media_fragment{};
    media_fragment.stage = rhi::ShaderStage::Fragment;
    media_fragment.format = blobs.format;
    media_fragment.bytecode = advanced ? blobs.media_linear_fragment
                                       : blobs.media_fragment;
    media_fragment.sampler_count = 2U;
    media_fragment.storage_buffer_count = 2U;
    // The media stage reads the same scene constants as the lit stage, for the
    // one field both have to agree about: the project's brightness curve.
    media_fragment.uniform_buffer_count = 1U;

    rhi::DepthState composite_depth = depth_state;
    composite_depth.write = false;
    constexpr std::array<scene::BlendMode, 4U> blend_modes{
        scene::BlendMode::Normal,
        scene::BlendMode::Add,
        scene::BlendMode::Multiply,
        scene::BlendMode::Screen};
    std::array<rhi::GraphicsPipeline, 4U> blended_pipelines{};
    std::array<rhi::GraphicsPipeline, 4U> media_blended_pipelines{};
    for (std::size_t index = 0U; index < blend_modes.size(); ++index) {
        auto blended = device.create_pipeline(
            vertex,
            fragment,
            color_format,
            to_rhi_blend(blend_modes[index], advanced),
            composite_depth);
        if (!blended) {
            return Result::failure(
                blended.error() == rhi::RhiError::ShaderFormatUnsupported
                    ? RenderError::ShaderFormatUnsupported
                    : RenderError::BackendFailure);
        }
        blended_pipelines[index] = std::move(blended).value();

        auto media_blended = device.create_pipeline(
            vertex,
            media_fragment,
            color_format,
            to_rhi_blend(blend_modes[index], advanced),
            composite_depth);
        if (!media_blended) {
            return Result::failure(
                media_blended.error() == rhi::RhiError::ShaderFormatUnsupported
                    ? RenderError::ShaderFormatUnsupported
                    : RenderError::BackendFailure);
        }
        media_blended_pipelines[index] = std::move(media_blended).value();
    }
    // A clip that occludes an isolated layer needs depth without colour. The
    // ordinary source-over factors leave the destination untouched at the zero
    // alpha the depth-only branch of the media stage returns, so this is the
    // blended pipeline with depth writes turned back on.
    auto media_depth_pipeline = device.create_pipeline(
        vertex,
        media_fragment,
        color_format,
        advanced ? rhi::ColorBlend::PremultipliedNormal
                 : rhi::ColorBlend::Normal,
        depth_state);
    if (!media_depth_pipeline) {
        return Result::failure(
            media_depth_pipeline.error() ==
                    rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }
    // The lit counterpart, for the depth pass a blended surface makes of its
    // own opaque pixels before the layers are composited.
    auto blended_depth_pipeline = device.create_pipeline(
        vertex,
        fragment,
        color_format,
        advanced ? rhi::ColorBlend::PremultipliedNormal
                 : rhi::ColorBlend::Normal,
        depth_state);
    if (!blended_depth_pipeline) {
        return Result::failure(
            blended_depth_pipeline.error() ==
                    rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }

    rhi::Texture velocity;
    rhi::Texture object_velocity;
    rhi::Texture object_id;
    rhi::Texture normal;
    rhi::Texture coverage;
    rhi::Texture auxiliary_depth;
    rhi::GraphicsPipeline velocity_pipeline;
    rhi::GraphicsPipeline normal_pipeline;
    rhi::GraphicsPipeline media_normal_pipeline;
    rhi::GraphicsPipeline object_id_pipeline;
    rhi::GraphicsPipeline media_velocity_pipeline;
    rhi::GraphicsPipeline media_object_id_pipeline;
    rhi::GraphicsPipeline coverage_pipeline;
    rhi::Sampler coverage_sampler;
    std::vector<rhi::Texture> directional_shadow;
    rhi::Texture shadow_depth;
    rhi::GraphicsPipeline shadow_pipeline;
    rhi::Sampler shadow_sampler;
    rhi::Texture rsm_normal;
    rhi::Texture rsm_flux;
    rhi::GraphicsPipeline rsm_normal_pipeline;
    rhi::GraphicsPipeline auxiliary_pipeline;
    rhi::GraphicsPipeline media_auxiliary_pipeline;
    rhi::GraphicsPipeline rsm_flux_pipeline;
    if (advanced) {
        auto created_velocity = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::Rgba32Float));
        auto created_object_velocity = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::Rgba32Float));
        if (!created_object_velocity) return Result::failure(RenderError::AllocationFailed);
        object_velocity = std::move(created_object_velocity).value();
        auto created_object_id = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::R32Uint));
        auto created_normal = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::Rg16Float));
        auto created_coverage = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::R32Float));
        auto created_auxiliary_depth =
            device.create_texture(depth_desc(width, height, true));
        // One map per allocated cascade. They are created here, once, so an
        // authored cascade change is a different draw count over resources
        // that already exist rather than an allocation during recording.
        bool cascades_created = true;
        try {
            directional_shadow.reserve(
                limits.max_directional_shadow_cascades);
        } catch (const std::bad_alloc&) {
            return Result::failure(RenderError::AllocationFailed);
        }
        for (core::u32 cascade = 0U;
             cascade < limits.max_directional_shadow_cascades; ++cascade) {
            auto created_cascade = device.create_texture(color_desc(
                limits.directional_shadow_resolution,
                limits.directional_shadow_resolution,
                rhi::TextureFormat::R32Float));
            if (!created_cascade) {
                cascades_created = false;
                break;
            }
            directional_shadow.push_back(std::move(created_cascade).value());
        }
        auto created_shadow_depth = device.create_texture(depth_desc(
            limits.directional_shadow_resolution,
            limits.directional_shadow_resolution));
        auto created_rsm_normal = device.create_texture(color_desc(
            limits.directional_shadow_resolution,
            limits.directional_shadow_resolution,
            rhi::TextureFormat::Rgba16Float));
        auto created_rsm_flux = device.create_texture(color_desc(
            limits.directional_shadow_resolution,
            limits.directional_shadow_resolution,
            rhi::TextureFormat::Rgba16Float));
        if (!created_velocity || !created_object_id || !created_normal
            || !created_coverage
            || !created_auxiliary_depth || !cascades_created
            || !created_shadow_depth || !created_rsm_normal
            || !created_rsm_flux) {
            return Result::failure(RenderError::AllocationFailed);
        }

        rhi::ShaderDesc object_fragment{};
        object_fragment.stage = rhi::ShaderStage::Fragment;
        object_fragment.format = blobs.format;
        object_fragment.sampler_count = 1U;
        object_fragment.uniform_buffer_count = 1U;
        object_fragment.bytecode = blobs.object_id_fragment;
        auto created_object_pipeline = device.create_pipeline(
            vertex, object_fragment, rhi::TextureFormat::R32Uint,
            rhi::ColorBlend::Disabled, depth_state);

        rhi::ShaderDesc velocity_fragment{};
        velocity_fragment.stage = rhi::ShaderStage::Fragment;
        velocity_fragment.format = blobs.format;
        velocity_fragment.sampler_count = 1U;
        velocity_fragment.uniform_buffer_count = 1U;
        velocity_fragment.bytecode = blobs.velocity_fragment;
        auto motion_vertex=vertex;
        motion_vertex.bytecode=blobs.motion_vertex;
        auto created_velocity_pipeline = device.create_pipeline(
            motion_vertex, velocity_fragment, rhi::TextureFormat::Rgba32Float,
            rhi::ColorBlend::Disabled, depth_state);

        rhi::ShaderDesc media_object_fragment{};
        media_object_fragment.stage = rhi::ShaderStage::Fragment;
        media_object_fragment.format = blobs.format;
        media_object_fragment.bytecode = blobs.media_object_id_fragment;
        media_object_fragment.sampler_count = 2U;
        auto created_media_object_pipeline = device.create_pipeline(
            vertex, media_object_fragment, rhi::TextureFormat::R32Uint,
            rhi::ColorBlend::Disabled, depth_state);

        rhi::ShaderDesc media_velocity_fragment{};
        media_velocity_fragment.stage = rhi::ShaderStage::Fragment;
        media_velocity_fragment.format = blobs.format;
        media_velocity_fragment.bytecode = blobs.media_velocity_fragment;
        media_velocity_fragment.sampler_count = 2U;
        auto created_media_velocity_pipeline = device.create_pipeline(
            vertex, media_velocity_fragment, rhi::TextureFormat::Rgba32Float,
            rhi::ColorBlend::Disabled, depth_state);

        rhi::ShaderDesc normal_fragment{};
        normal_fragment.stage = rhi::ShaderStage::Fragment;
        normal_fragment.format = blobs.format;
        normal_fragment.sampler_count = 1U;
        normal_fragment.uniform_buffer_count = 1U;
        normal_fragment.bytecode = blobs.normal_fragment;
        auto created_normal_pipeline = device.create_pipeline(
            vertex, normal_fragment, rhi::TextureFormat::Rg16Float,
            rhi::ColorBlend::Disabled, depth_state);

        rhi::ShaderDesc media_normal_fragment{};
        media_normal_fragment.stage = rhi::ShaderStage::Fragment;
        media_normal_fragment.format = blobs.format;
        media_normal_fragment.bytecode = blobs.media_normal_fragment;
        media_normal_fragment.sampler_count = 2U;
        auto created_media_normal_pipeline = device.create_pipeline(
            vertex, media_normal_fragment, rhi::TextureFormat::Rg16Float,
            rhi::ColorBlend::Disabled, depth_state);

        // The three products above in one pass where the device writes
        // several targets at once. Same vertex stages as the single-target
        // passes: motion for opaque geometry, the plain stage for media.
        if (device.capabilities().multiple_render_targets) {
            constexpr std::array<rhi::TextureFormat, 3U> auxiliary_formats{
                rhi::TextureFormat::R32Uint, rhi::TextureFormat::Rgba32Float,
                rhi::TextureFormat::Rg16Float};
            rhi::ShaderDesc auxiliary_fragment = velocity_fragment;
            auxiliary_fragment.bytecode = blobs.auxiliary_fragment;
            auto created_auxiliary = device.create_pipeline(
                motion_vertex, auxiliary_fragment, auxiliary_formats,
                rhi::ColorBlend::Disabled, depth_state);
            rhi::ShaderDesc media_auxiliary_fragment = media_velocity_fragment;
            media_auxiliary_fragment.bytecode = blobs.media_auxiliary_fragment;
            auto created_media_auxiliary = device.create_pipeline(
                vertex, media_auxiliary_fragment, auxiliary_formats,
                rhi::ColorBlend::Disabled, depth_state);
            if (!created_auxiliary || !created_media_auxiliary) {
                return Result::failure(RenderError::BackendFailure);
            }
            auxiliary_pipeline = std::move(created_auxiliary).value();
            media_auxiliary_pipeline = std::move(created_media_auxiliary).value();
        }

        rhi::ShaderDesc coverage_vertex{};
        coverage_vertex.stage = rhi::ShaderStage::Vertex;
        coverage_vertex.format = blobs.format;
        coverage_vertex.bytecode = blobs.coverage_vertex;
        rhi::ShaderDesc coverage_fragment{};
        coverage_fragment.stage = rhi::ShaderStage::Fragment;
        coverage_fragment.format = blobs.format;
        coverage_fragment.bytecode = blobs.coverage_fragment;
        coverage_fragment.sampler_count = 1U;
        auto created_coverage_pipeline = device.create_pipeline(
            coverage_vertex, coverage_fragment, rhi::TextureFormat::R32Float,
            rhi::ColorBlend::Disabled);
        auto created_coverage_sampler =
            device.create_sampler(rhi::Filter::Nearest);
        rhi::ShaderDesc shadow_fragment{};
        shadow_fragment.stage = rhi::ShaderStage::Fragment;
        shadow_fragment.format = blobs.format;
        shadow_fragment.sampler_count = 1U;
        shadow_fragment.uniform_buffer_count = 1U;
        shadow_fragment.bytecode = blobs.shadow_fragment;
        auto created_shadow_pipeline = device.create_pipeline(
            vertex, shadow_fragment, rhi::TextureFormat::R32Float,
            rhi::ColorBlend::Disabled, depth_state);
        rhi::ShaderDesc rsm_normal_fragment{};
        rsm_normal_fragment.stage = rhi::ShaderStage::Fragment;
        rsm_normal_fragment.format = blobs.format;
        rsm_normal_fragment.sampler_count = 1U;
        rsm_normal_fragment.uniform_buffer_count = 1U;
        rsm_normal_fragment.bytecode = blobs.rsm_normal_fragment;
        auto created_rsm_normal_pipeline = device.create_pipeline(
            vertex, rsm_normal_fragment, rhi::TextureFormat::Rgba16Float,
            rhi::ColorBlend::Disabled, depth_state);
        rhi::ShaderDesc rsm_flux_fragment{};
        rsm_flux_fragment.stage = rhi::ShaderStage::Fragment;
        rsm_flux_fragment.format = blobs.format;
        rsm_flux_fragment.bytecode = blobs.rsm_flux_fragment;
        rsm_flux_fragment.sampler_count = 1U;
        rsm_flux_fragment.uniform_buffer_count = 1U;
        auto created_rsm_flux_pipeline = device.create_pipeline(
            vertex, rsm_flux_fragment, rhi::TextureFormat::Rgba16Float,
            rhi::ColorBlend::Disabled, depth_state);
        auto created_shadow_sampler =
            device.create_sampler(rhi::Filter::Nearest);
        if (!created_object_pipeline || !created_velocity_pipeline
            || !created_media_object_pipeline
            || !created_media_velocity_pipeline
            || !created_normal_pipeline || !created_media_normal_pipeline
            || !created_coverage_pipeline || !created_coverage_sampler
            || !created_shadow_pipeline || !created_shadow_sampler
            || !created_rsm_normal_pipeline
            || !created_rsm_flux_pipeline) {
            return Result::failure(RenderError::BackendFailure);
        }
        velocity = std::move(created_velocity).value();
        object_id = std::move(created_object_id).value();
        normal = std::move(created_normal).value();
        coverage = std::move(created_coverage).value();
        auxiliary_depth = std::move(created_auxiliary_depth).value();
        object_id_pipeline = std::move(created_object_pipeline).value();
        velocity_pipeline = std::move(created_velocity_pipeline).value();
        normal_pipeline = std::move(created_normal_pipeline).value();
        media_normal_pipeline =
            std::move(created_media_normal_pipeline).value();
        media_object_id_pipeline =
            std::move(created_media_object_pipeline).value();
        media_velocity_pipeline =
            std::move(created_media_velocity_pipeline).value();
        coverage_pipeline = std::move(created_coverage_pipeline).value();
        coverage_sampler = std::move(created_coverage_sampler).value();
        shadow_depth = std::move(created_shadow_depth).value();
        shadow_pipeline = std::move(created_shadow_pipeline).value();
        shadow_sampler = std::move(created_shadow_sampler).value();
        rsm_normal = std::move(created_rsm_normal).value();
        rsm_flux = std::move(created_rsm_flux).value();
        rsm_normal_pipeline =
            std::move(created_rsm_normal_pipeline).value();
        rsm_flux_pipeline =
            std::move(created_rsm_flux_pipeline).value();
    }

    // Image-based lighting, Advanced only. A device without compute keeps the
    // hemisphere rather than failing: this is an image-quality addition, not a
    // capability the renderer's contract depends on, and refusing to construct
    // over it would take a working Advanced renderer off a machine that had
    // one.
    std::optional<EnvironmentPrefilter> environment;
    rhi::Texture environment_fallback;
    rhi::Texture environment_cube_fallback;
    rhi::Sampler environment_sampler;
    {
        // Bound whether or not an environment exists, so the descriptor table
        // has one shape for every scene. Never sampled: the shader selects on
        // the intensity constant.
        rhi::TextureDesc cube_desc = sampled_desc(1U, 1U);
        cube_desc.array_layers = 6U;
        cube_desc.cube = true;
        auto created_fallback = device.create_texture(sampled_desc(1U, 1U));
        auto created_cube = device.create_texture(cube_desc);
        auto created_sampler = device.create_sampler(rhi::Filter::Linear);
        if (!created_fallback || !created_cube || !created_sampler) {
            return Result::failure(RenderError::AllocationFailed);
        }
        environment_fallback = std::move(created_fallback).value();
        environment_cube_fallback = std::move(created_cube).value();
        environment_sampler = std::move(created_sampler).value();
        if (advanced && EnvironmentPrefilter::shaders_available()
            && device.capabilities().compute) {
            auto created_environment = EnvironmentPrefilter::create(device);
            if (created_environment) {
                environment.emplace(std::move(created_environment).value());
            }
        }
    }

    // The Advanced sky also draws analytic sun discs with the flat background,
    // so it can operate even on a device without environment compute support.
    rhi::GraphicsPipeline sky_pipeline;
    if (advanced && blobs.sky_present) {
        rhi::ShaderDesc sky_vertex{};
        sky_vertex.stage = rhi::ShaderStage::Vertex;
        sky_vertex.format = blobs.format;
        sky_vertex.bytecode = blobs.fullscreen_vertex;
        rhi::ShaderDesc sky_fragment{};
        sky_fragment.stage = rhi::ShaderStage::Fragment;
        sky_fragment.format = blobs.format;
        sky_fragment.bytecode = blobs.sky_fragment;
        sky_fragment.sampler_count = 1U;
        sky_fragment.uniform_buffer_count = 1U;
        auto created_sky = device.create_pipeline(
            sky_vertex, sky_fragment, color_format, rhi::ColorBlend::Disabled,
            rhi::DepthState{});
        if (created_sky) {
            sky_pipeline = std::move(created_sky).value();
        } else {
            // No sky is a flat background, not a failed renderer.
            environment.reset();
        }
    }
    // Linear, so a clip at one resolution resamples cleanly onto a quad of any
    // size on screen.
    auto media_sampler = device.create_sampler(rhi::Filter::Linear);
    if (!media_sampler) {
        return Result::failure(RenderError::AllocationFailed);
    }
    auto material_fallback = device.create_texture(sampled_desc(1U, 1U));
    auto normal_fallback = device.create_texture(sampled_desc(1U, 1U));
    auto offline_media = device.create_texture(sampled_desc(64U, 64U));
    auto ies_desc = sampled_desc(ies_vertical_samples, ies_horizontal_samples * max_ies_profiles);
    ies_desc.format = rhi::TextureFormat::R32Float;
    auto ies_atlas = device.create_texture(ies_desc);
    if (!ies_atlas) return Result::failure(RenderError::AllocationFailed);
    if (!material_fallback || !normal_fallback || !offline_media) {
        return Result::failure(RenderError::AllocationFailed);
    }

    static_assert(sizeof(scene::MeshVertex) == GeometryArena::vertex_bytes);
    static_assert(sizeof(scene::Vec4) == GeometryArena::tangent_bytes);
    static_assert(sizeof(GpuSkin) == GeometryArena::skin_bytes);
    GeometryArenaLimits geometry_limits{};
    geometry_limits.max_vertices = limits.max_vertices;
    geometry_limits.max_indices = limits.max_indices;
    geometry_limits.max_skins = std::min(limits.max_skin_vertices, limits.max_vertices);
    geometry_limits.initial_vertices = std::min(limits.initial_vertices, limits.max_vertices);
    geometry_limits.initial_indices = std::min(limits.initial_indices, limits.max_indices);
    auto geometry = GeometryResidency::for_device(device, geometry_limits);
    auto instances = device.create_storage_buffer(
        limits.max_instances * static_cast<core::u32>(sizeof(GpuInstance)));
    auto lights = device.create_storage_buffer(
        limits.max_lights * static_cast<core::u32>(sizeof(GpuLight)));
    auto inline_effect_ops = device.create_storage_buffer(
        limits.max_inline_effect_ops
        * static_cast<core::u32>(sizeof(GpuInlineEffectOp)));
    auto inline_effect_parameters = device.create_storage_buffer(
        limits.max_inline_effect_parameter_vectors
        * static_cast<core::u32>(sizeof(GpuInlineEffectParameter)));
    if (!geometry || !instances || !lights
        || !inline_effect_ops || !inline_effect_parameters) {
        return Result::failure(RenderError::AllocationFailed);
    }

    SceneRenderer renderer{
        device,
        limits,
        std::move(color).value(),
        std::move(depth).value(),
        std::move(pipeline).value(),
        std::move(blended_pipelines),
        std::move(media_blended_pipelines),
        std::move(media_depth_pipeline).value(),
        std::move(blended_depth_pipeline).value(),
        std::move(media_sampler).value(),
        std::move(material_fallback).value(),
        std::move(normal_fallback).value(),
        std::move(geometry).value(),
        std::move(instances).value(),
        std::move(lights).value(),
        std::move(inline_effect_ops).value(),
        std::move(inline_effect_parameters).value(),
        mode,
        std::move(velocity),
        std::move(object_id),
        std::move(normal),
        std::move(coverage),
        std::move(auxiliary_depth),
        std::move(velocity_pipeline),
        std::move(object_id_pipeline),
        std::move(normal_pipeline),
        std::move(media_velocity_pipeline),
        std::move(media_object_id_pipeline),
        std::move(media_normal_pipeline),
        std::move(coverage_pipeline),
        std::move(coverage_sampler)};
    auto pose_buffer=device.create_storage_buffer(max_frame_pose_bindings*static_cast<core::u32>(sizeof(GpuPose)));
    if (!pose_buffer) return Result::failure(RenderError::AllocationFailed);
    renderer.pose_buffer_=std::move(pose_buffer).value();
    renderer.pose_scratch_=std::make_unique<ModelPoseScratch>();
    renderer.current_pose_.resize(max_model_pose_bindings);
    renderer.previous_pose_.resize(max_model_pose_bindings);
    renderer.pose_upload_.reserve(max_frame_pose_bindings*sizeof(GpuPose));
    renderer.animated_instances_.reserve(limits.max_instances);
    // Occlusion, where the device can run it. Constructed rather than
    // refused when absent: a scene that authors no radius never dispatches,
    // and a device without compute keeps a working Advanced renderer.
    if (mode == SceneRendererMode::AdvancedLinear
        && AmbientOcclusion::shaders_available()
        && device.capabilities().compute
        && device.capabilities().storage_images) {
        auto created_occlusion =
            AmbientOcclusion::create(device, width, height);
        if (created_occlusion) {
            renderer.occlusion_.emplace(std::move(created_occlusion).value());
            auto temporal_ao = TemporalResolve::create(device, width, height,
                TemporalResolveLimits{limits.max_temporal_history_targets},
                rhi::TextureFormat::R32Float);
            if (temporal_ao) {
                renderer.temporal_occlusion_.emplace(std::move(temporal_ao).value());
            }
        }
    }
    // Particles, where the device can simulate them. Like occlusion, a device
    // that cannot keeps a working renderer; a scene that places an effect is
    // then refused readiness with the reason.
    if (mode == SceneRendererMode::AdvancedLinear) {
        auto particles = ParticlePass::create(device, color_format, limits.particles);
        if (particles) {
            renderer.particle_pass_ = std::make_unique<ParticlePass>(std::move(particles).value());
            renderer.particle_packages_ = std::make_unique<ParticlePackageCache>(ajfx::gpu::ShaderFormat::Dxil);
        } else {
            renderer.particles_unsupported_ = std::move(particles).error().message;
        }
    } else {
        renderer.particles_unsupported_ = "particle effects need the Advanced renderer";
    }
    // The local-light shadow atlas, and the light table that indexes it.
    // Allocated once at construction like the cascades: every node runs the
    // same build and therefore reserves the same bytes whatever a show
    // authors.
    if (mode == SceneRendererMode::AdvancedLinear) {
        renderer.shadow_atlas_limits_ = limits.shadow_atlas;
        auto atlas = device.create_texture(color_desc(
            limits.shadow_atlas.extent, limits.shadow_atlas.extent,
            rhi::TextureFormat::R32Float));
        auto atlas_depth = device.create_texture(depth_desc(
            limits.shadow_atlas.extent, limits.shadow_atlas.extent));
        auto probes = device.create_storage_buffer(scene::max_baked_probes * static_cast<core::u32>(sizeof(BakedProbe)));
        if (!probes) return Result::failure(RenderError::AllocationFailed);
        renderer.probe_buffer_ = std::move(probes).value();
        auto shadows = device.create_storage_buffer(
            static_cast<core::u32>(scene::max_scene_point_lights
                                   * sizeof(GpuLightShadow)));
        if (!atlas || !atlas_depth || !shadows) {
            return Result::failure(RenderError::AllocationFailed);
        }
        {
            const auto held = renderer.geometry_->lock();
            renderer.atlas_home_ = renderer.geometry_->adopt_shadow(
                {std::move(atlas).value(), rhi::Texture{}});
        }
        if (!renderer.atlas_home_) {
            return Result::failure(RenderError::AllocationFailed);
        }
        renderer.shadow_atlas_depth_ = std::move(atlas_depth).value();
        renderer.light_shadow_buffer_ = std::move(shadows).value();
        auto particle_lights = device.create_writable_buffer(max_particle_lights * 128U);
        if (!particle_lights) {
            return Result::failure(RenderError::AllocationFailed);
        }
        renderer.particle_light_buffer_ = std::move(particle_lights).value();
        try {
            renderer.light_shadow_scratch_.reserve(
                static_cast<std::size_t>(scene::max_scene_point_lights)
                * sizeof(GpuLightShadow));
        } catch (const std::bad_alloc&) {
            return Result::failure(RenderError::AllocationFailed);
        }
    }
    if (advanced) {
        // Registered with the shared geometry, so another renderer drawing
        // the same maps samples these instead.
        const auto held = renderer.geometry_->lock();
        try {
            renderer.cascade_homes_.reserve(directional_shadow.size());
        } catch (const std::bad_alloc&) {
            return Result::failure(RenderError::AllocationFailed);
        }
        for (auto& cascade : directional_shadow) {
            auto adopted = renderer.geometry_->adopt_shadow({std::move(cascade), rhi::Texture{}});
            if (!adopted) {
                return Result::failure(RenderError::AllocationFailed);
            }
            renderer.cascade_homes_.push_back(std::move(adopted));
        }
        renderer.rsm_home_ = renderer.geometry_->adopt_shadow({std::move(rsm_normal), std::move(rsm_flux)});
        if (!renderer.rsm_home_) {
            return Result::failure(RenderError::AllocationFailed);
        }
    }
    renderer.shadow_depth_ = std::move(shadow_depth);
    renderer.shadow_pipeline_ = std::move(shadow_pipeline);
    renderer.transmission_background_ = std::move(transmission_background).value();
    renderer.shadow_sampler_ = std::move(shadow_sampler);
    renderer.rsm_normal_pipeline_ = std::move(rsm_normal_pipeline);
    renderer.auxiliary_pipeline_ = std::move(auxiliary_pipeline);
    renderer.media_auxiliary_pipeline_ = std::move(media_auxiliary_pipeline);
    if (limits.view_shading && device.capabilities().wireframe_fill) {
        // The lit and media colour passes, rasterizing edges. Depth still
        // tests and writes, so an edge behind another is drawn after it in
        // the same pass; the colour pass clears its own depth, so the edges
        // are tested only against each other and every one shows.
        const rhi::RasterState edges{rhi::FillMode::Wireframe};
        auto wireframe = device.create_pipeline(
            vertex, fragment, color_format, rhi::ColorBlend::Disabled,
            depth_state, edges);
        auto media_wireframe = device.create_pipeline(
            vertex, media_fragment, color_format,
            advanced ? rhi::ColorBlend::PremultipliedNormal
                     : rhi::ColorBlend::Normal,
            depth_state, edges);
        if (!wireframe || !media_wireframe) {
            return Result::failure(RenderError::BackendFailure);
        }
        renderer.wireframe_pipeline_ = std::move(wireframe).value();
        renderer.media_wireframe_pipeline_ =
            std::move(media_wireframe).value();
    }
    renderer.rsm_flux_pipeline_ = std::move(rsm_flux_pipeline);
    renderer.environment_ = std::move(environment);
    renderer.offline_media_ = std::move(offline_media).value();
    renderer.environment_fallback_ = std::move(environment_fallback);
    renderer.environment_cube_fallback_ =
        std::move(environment_cube_fallback);
    renderer.environment_sampler_ = std::move(environment_sampler);
    if (!renderer.ltc_asset_.create(device)) return Result::failure(RenderError::AllocationFailed);
    renderer.ies_atlas_ = std::move(ies_atlas).value();
    renderer.ies_atlas_scratch_.resize(ies_profile_texels * max_ies_profiles);
    renderer.sky_pipeline_ = std::move(sky_pipeline);
    for (std::size_t index = 0U; index < material_sampler_count; ++index) {
        auto sampler = device.create_sampler(material_sampler_desc(index));
        if (!sampler) return Result::failure(RenderError::AllocationFailed);
        renderer.material_samplers_[index] = std::move(sampler).value();
    }

    // Every CPU arena is reserved to its fixed cap during renderer creation.
    // Asynchronous model publication may happen while the operator is on air;
    // integrating completed geometry must not turn into a large vector growth
    // allocation on that recording's frame path.
    try {
    renderer.instance_scratch_.reserve(
        static_cast<std::size_t>(limits.max_instances) * sizeof(GpuInstance));
    renderer.light_scratch_.reserve(
        static_cast<std::size_t>(limits.max_lights) * sizeof(GpuLight));
    renderer.inline_effect_op_scratch_.reserve(
        static_cast<std::size_t>(limits.max_inline_effect_ops)
        * sizeof(GpuInlineEffectOp));
    renderer.inline_effect_parameter_scratch_.reserve(
        static_cast<std::size_t>(limits.max_inline_effect_parameter_vectors)
        * sizeof(GpuInlineEffectParameter));
    renderer.inline_program_cache_.reserve(limits.max_instances);
    renderer.batch_uniforms_.reserve(
        static_cast<std::size_t>(limits.max_instances) * sizeof(BatchConstants));
    renderer.batches_.reserve(limits.max_instances);
    renderer.opaque_submissions_.reserve(limits.max_instances);
    renderer.reordered_instances_.reserve(static_cast<std::size_t>(limits.max_instances) * sizeof(GpuInstance));
    renderer.geometry_selection_.reserve(limits.max_instances);
    if (advanced) {
        // Model textures are shared by every renderer on the device.
        auto textures = TextureResidency::for_device(device);
        if (!textures) return Result::failure(RenderError::AllocationFailed);
        renderer.texture_streamer_.emplace(limits.max_model_textures, std::move(textures).value());
        renderer.texture_requests_.reserve(limits.max_model_textures);
    }
    renderer.opaque_visibility_.reserve(limits.max_instances);
    renderer.camera_batch_uniforms_.reserve(static_cast<std::size_t>(limits.max_instances) * sizeof(BatchConstants));
    renderer.camera_pulled_batch_uniforms_.reserve(static_cast<std::size_t>(limits.max_instances) * sizeof(BatchConstants));
    renderer.casters_.batches.reserve(limits.max_instances);
    renderer.casters_.uniforms.reserve(static_cast<std::size_t>(limits.max_instances) * sizeof(BatchConstants));
    renderer.casters_.textures.reserve(limits.max_instances);
    renderer.instance_digests_.reserve(limits.max_instances);
    renderer.camera_batches_.reserve(limits.max_instances);
    renderer.camera_batch_textures_.reserve(static_cast<std::size_t>(limits.max_instances) *
        (mode == SceneRendererMode::AdvancedLinear ? advanced_fragment_textures : basic_fragment_textures));
    // Must match fragment_textures_per_batch exactly. A short reservation is
    // not a wrong image, it is a vector growth on the frame path - which is
    // the thing this arena exists to prevent.
    renderer.batch_textures_.reserve(
        static_cast<std::size_t>(limits.max_instances)
        * (advanced ? advanced_fragment_textures
                    : basic_fragment_textures));
    renderer.rsm_flux_textures_.reserve(limits.max_instances);
    renderer.camera_coverage_textures_.reserve(limits.max_instances);
    renderer.composite_draws_.reserve(limits.max_instances);
    renderer.layer_volumes_.reserve(limits.max_instances);
    renderer.layer_edges_.reserve(
        static_cast<std::size_t>(limits.max_instances) * layer_edges_per_layer);
    renderer.layer_edge_offsets_.reserve(
        static_cast<std::size_t>(limits.max_instances) + 1U);
    renderer.layer_in_degree_.reserve(limits.max_instances);
    renderer.layer_ready_.reserve(limits.max_instances);
    renderer.layer_order_.reserve(limits.max_instances);
    renderer.layer_placed_.reserve(limits.max_instances);
    renderer.depth_only_media_.reserve(scene::max_scene_media);
    renderer.retired_models_.reserve(
        limits.max_model_refresh_generations);
    renderer.retired_particles_.reserve(max_retired_particles);
    renderer.pending_model_integrations_.reserve(
        limits.max_model_refresh_generations);
    renderer.object_velocity_ = std::move(object_velocity);
    renderer.previous_transforms_.reserve(limits.max_instances);
    renderer.motion_source_transforms_.reserve(limits.max_instances);
    renderer.next_transforms_.reserve(limits.max_instances);
    renderer.view_histories_.resize(limits.max_history_slots);
    } catch (const std::bad_alloc&) {
        return Result::failure(RenderError::AllocationFailed);
    }
    if (advanced) {
        for (std::size_t i = 0; i < renderer.reflection_filters_.size(); ++i) {
            auto filter = EnvironmentPrefilter::create(device);
            auto source = device.create_texture(color_desc(reflection_capture_width, reflection_capture_height, rhi::TextureFormat::Rgba32Float));
            if (!filter || !source) return Result::failure(RenderError::AllocationFailed);
            renderer.reflection_filters_[i] = std::move(filter).value();
            renderer.reflection_sources_[i] = std::move(source).value();
        }
        // Not built here. The mirror is a second complete renderer at full
        // extent - it very nearly doubles this one - and a scene with no
        // reflective surface never draws a pixel through it. `ensure_planar`
        // builds it when one is authored.
        renderer.planar_buildable_ = limits.planar_reflections;
    }
    return Result::success(std::move(renderer));
}

void SceneRenderer::ensure_planar() noexcept {
    if (planar_renderer_ || !planar_buildable_ || device_ == nullptr) {
        return;
    }
    // One attempt. This is the same bargain the Advanced renderer strikes
    // above it: a renderer that cannot build its mirror now will not manage
    // it on the next frame, and retrying would put the construction of a
    // whole nested renderer on the frame path for the life of the show.
    planar_buildable_ = false;
    auto nested_limits = limits_;
    nested_limits.planar_reflections = false;
    auto planar = SceneRenderer::create(*device_, color_.width(),
                                        color_.height(), nested_limits, mode_);
    if (!planar) {
        return;
    }
    try {
        planar_renderer_ =
            std::make_unique<SceneRenderer>(std::move(planar).value());
    } catch (const std::bad_alloc&) {
        planar_renderer_.reset();
    }
}

bool SceneRenderer::resize(
    rhi::Device& device,
    core::u32 width,
    core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (color_.width() == width && color_.height() == height) {
        return true;
    }
    const bool advanced = mode_ == SceneRendererMode::AdvancedLinear;
    auto color = device.create_texture(color_desc(
        width, height, advanced ? rhi::TextureFormat::Rgba16Float
                                : rhi::TextureFormat::Rgba8Unorm));
    if (!color) {
        return false;
    }
    auto depth = device.create_texture(depth_desc(width, height));
    if (!depth) {
        return false;
    }
    rhi::Texture velocity;
    rhi::Texture object_velocity;
    rhi::Texture object_id;
    rhi::Texture normal;
    rhi::Texture coverage;
    rhi::Texture auxiliary_depth;
    if (advanced) {
        auto created_velocity = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::Rgba32Float));
        auto created_object_velocity = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::Rgba32Float));
        if (!created_object_velocity) return false;
        object_velocity = std::move(created_object_velocity).value();
        auto created_object_id = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::R32Uint));
        auto created_normal = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::Rg16Float));
        auto created_coverage = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::R32Float));
        auto created_auxiliary_depth =
            device.create_texture(depth_desc(width, height, true));
        if (!created_velocity || !created_object_id || !created_normal
            || !created_coverage || !created_auxiliary_depth) {
            return false;
        }
        velocity = std::move(created_velocity).value();
        object_id = std::move(created_object_id).value();
        normal = std::move(created_normal).value();
        coverage = std::move(created_coverage).value();
        auxiliary_depth = std::move(created_auxiliary_depth).value();
    }
    if (planar_renderer_ && !planar_renderer_->resize(device, width, height)) return false;
    // Both replaced together or neither: a colour target that outlived its
    // depth buffer would fail every subsequent draw.
    auto transmission_background = device.create_texture(color_desc(width, height, color_.desc().format));
    if (!transmission_background) return false;
    transmission_background_ = std::move(transmission_background).value();
    color_ = std::move(color).value();
    depth_ = std::move(depth).value();
    if (advanced) {
        velocity_ = std::move(velocity);
        object_velocity_ = std::move(object_velocity);
        object_id_ = std::move(object_id);
        normal_ = std::move(normal);
        coverage_ = std::move(coverage);
        auxiliary_depth_ = std::move(auxiliary_depth);
    }
    history_valid_ = false;
    motion_source_show_frame_ = 0U;
    previous_transforms_.clear();
    motion_source_transforms_.clear();
    next_transforms_.clear();
    std::fill(
        view_histories_.begin(), view_histories_.end(),
        ViewHistoryState{});
    return true;
}

core::u64 SceneRenderer::RecordingBudget::remaining(
    rhi::CommandList& commands, core::u64 limit) {
    const auto current = commands.recording_token();
    if (!current) {
        return 0U;
    }
    if (!recording || !recording.same_recording(current)) {
        recording = current;
        used = 0U;
    }
    return used >= limit ? 0U : limit - used;
}

bool SceneRenderer::stream_geometry(
    GeometryResidency::Entry& entry,
    rhi::CommandList& commands,
    bool complete_now) {
    const auto budget = complete_now
        ? std::numeric_limits<core::u64>::max()
        : geometry_budget_.remaining(commands, limits_.geometry_upload_bytes);
    const auto streamed = geometry_->stream(entry, commands, complete_now, budget);
    if (streamed.written != 0U) {
        stats_.geometry_uploaded = true;
        stats_.upload_bytes += streamed.written;
        if (!complete_now) {
            geometry_budget_.used += streamed.written;
        }
    }
    return streamed.drawable;
}

SceneRenderer::TextureUpload SceneRenderer::stream_model_textures(
    PendingModelIntegration& integration, rhi::CommandList& commands) {
    using rhi::CommandRecordingOutcome;
    // Upload space left for the draw data recorded after these rows.
    constexpr core::u64 upload_reserve = 16ULL << 20U;
    const auto position = [](core::u64 texture, core::u64 mip, core::u64 row) noexcept {
        return texture << 40U | mip << 32U | row;
    };
    const auto current = commands.recording_token();
    auto& writes = integration.texture_writes;
    auto& next = integration.texture_next;
    // A canceled recording never ran: upload again from its first row.
    for (auto write = writes.begin(); write != writes.end(); ++write) {
        const auto outcome = write->recording.outcome();
        if (outcome == CommandRecordingOutcome::Canceled
            || outcome == CommandRecordingOutcome::Unavailable) {
            next = std::min(next, write->begin);
            writes.erase(write, writes.end());
            break;
        }
    }
    while (!writes.empty()
           && writes.front().recording.outcome() == CommandRecordingOutcome::Submitted) {
        writes.erase(writes.begin());
    }

    auto& targets = integration.candidate.textures;
    const auto& sources = integration.decoded_asset->textures;
    const core::u64 end = position(targets.size(), 0U, 0U);
    core::u64 budget = current ? texture_budget_.remaining(commands, limits_.texture_upload_bytes) : 0U;
    while (next < end) {
        const auto index = static_cast<std::size_t>(next >> 40U);
        const auto mip = static_cast<core::u32>((next >> 32U) & 0xFFU);
        const auto row = static_cast<core::u32>(next & 0xFFFF'FFFFULL);
        if (index >= sources.size() || !sources[index].valid() || !targets[index]) {
            next = position(index + 1U, 0U, 0U);
            continue;
        }
        const ModelTexture& source = sources[index];
        const auto width = mip == 0U ? source.width : source.lower_mips[mip - 1U].width;
        const auto height = mip == 0U ? source.height : source.lower_mips[mip - 1U].height;
        const std::span<const core::u8> pixels =
            mip == 0U ? source.rgba : source.lower_mips[mip - 1U].rgba;
        const core::u32 block = source.encoding == ModelTextureEncoding::Rgba8 ? 1U : 4U;
        const core::u64 row_bytes = block == 1U ? static_cast<core::u64>(width) * 4U
            : static_cast<core::u64>((width + 3U) / 4U) * 16U;
        // The list's capacity is measured in rows of the widest level, so
        // it holds at least as many rows of a narrower one.
        const core::u32 capacity =
            commands.texture_upload_row_capacity(targets[index], upload_reserve);
        // A row wider than the whole limit still goes, alone, so any limit
        // makes progress.
        const core::u64 budget_rows = texture_budget_.used == 0U
            ? std::max<core::u64>(budget / row_bytes, 1U) : budget / row_bytes;
        const auto block_rows = static_cast<core::u32>(std::min<core::u64>({
            (height - row + block - 1U) / block,
            (static_cast<core::u64>(capacity) + block - 1U) / block,
            budget_rows}));
        if (!current || block_rows == 0U) {
            break;
        }
        const auto rows = std::min(height - row, block_rows * block);
        const auto bytes = pixels.subspan(
            static_cast<std::size_t>((row / block) * row_bytes),
            static_cast<std::size_t>(block_rows * row_bytes));
        if (!commands.upload_texture_rows(targets[index], mip, 0U, row, rows, bytes)) {
            return TextureUpload::Failed;
        }
        budget -= std::min<core::u64>(budget, bytes.size());
        texture_budget_.used += bytes.size();
        stats_.upload_bytes += bytes.size();
        if (writes.empty() || !writes.back().recording.same_recording(current)) {
            writes.push_back({current, next});
        }
        next = row + rows < height ? position(index, mip, row + rows)
            : mip < source.lower_mips.size() ? position(index, mip + 1U, 0U)
            : position(index + 1U, 0U, 0U);
    }
    const bool landed = next >= end && std::all_of(writes.begin(), writes.end(),
        [&](const PendingModelIntegration::TextureWrite& write) noexcept {
            return current && current.same_recording(write.recording);
        });
    return landed ? TextureUpload::Drawable : TextureUpload::Pending;
}

bool SceneRenderer::streaming_geometry(const std::string& source) const noexcept {
    const auto found = resident_models_.find(source);
    return found != resident_models_.end()
        && std::any_of(pending_model_integrations_.begin(), pending_model_integrations_.end(),
            [&](const PendingModelIntegration& integration) noexcept {
                return integration.active == &found->second && !integration.recording
                    && !integration.blocked;
            });
}

void SceneRenderer::note_use(const ResidentModel& model, rhi::CommandList& commands) {
    const auto current = commands.recording_token();
    if (!current) {
        // Without a token this use cannot be followed; such a model is never
        // reclaimed rather than reclaimed early.
        model.untracked_use = true;
        return;
    }
    std::erase_if(model.uses, [](const rhi::CommandRecordingToken& token) {
        return token.outcome() != rhi::CommandRecordingOutcome::Pending;
    });
    // The geometry may be shared with renderers that outlive this one, so
    // its own use list decides when its slots are reused.
    if (model.geometry) {
        geometry_->note_use(*model.geometry, commands);
    }
    for (const auto& token : model.uses) {
        if (token.same_recording(current)) {
            return;
        }
    }
    model.uses.push_back(current);
}

void SceneRenderer::reclaim_retired_models() noexcept {
    // Bounded per recording: released textures are destroyed here.
    std::size_t released{};
    for (auto model = retired_models_.begin();
         model != retired_models_.end() && released < 8U;) {
        const bool in_use = model->untracked_use
            || std::any_of(model->uses.begin(), model->uses.end(),
                [](const rhi::CommandRecordingToken& token) {
                    return token.outcome() == rhi::CommandRecordingOutcome::Pending;
                });
        const bool textures_busy = std::any_of(
            model->textures.begin(), model->textures.end(),
            [](const rhi::Texture& texture) { return !texture.safe_to_release(); });
        if (in_use || textures_busy) {
            ++model;
            continue;
        }
        // Erasing drops this renderer's hold on the geometry; the residency
        // reuses the slots once no renderer holds them.
        model_texture_bytes_ -= std::min(model_texture_bytes_, model->texture_bytes);
        model = retired_models_.erase(model);
        ++released;
    }
    if (released != 0U) {
        stats_.model_texture_bytes = texture_streamer_
            ? texture_streamer_->stats().allocated_bytes : model_texture_bytes_;
    }
    geometry_->collect();
}

const SceneRenderer::MeshRange* SceneRenderer::ensure_resident(
    scene::MeshKey key, rhi::CommandList& commands) {
    const auto normal_key = scene::normalized(key);
    auto found = resident_.find(normal_key);
    if (found == resident_.end()) {
        bool grew = false;
        auto* const entry = geometry_->procedural(normal_key, commands, grew);
        stats_.geometry_uploaded = stats_.geometry_uploaded || grew;
        if (entry == nullptr) {
            return nullptr;
        }
        try {
            found = resident_.emplace(normal_key, ResidentMesh{
                MeshRange{entry->span.first_index, entry->span.index_count}, entry}).first;
        } catch (...) {
            return nullptr;
        }
    }
    // Procedural meshes are drawn in the recording that needs them, so their
    // (small) upload is all made here or the instance waits.
    if (!stream_geometry(*found->second.entry, commands, true)) {
        return nullptr;
    }
    return &found->second.range;
}

bool SceneRenderer::resolve_recording_transactions(
    rhi::CommandList& commands) {
    reclaim_retired_models();
    rhi::CommandRecordingToken current_recording;
    const auto is_current = [&](const rhi::CommandRecordingToken& pending) {
        if (!current_recording) {
            current_recording = commands.recording_token();
        }
        return current_recording
            && current_recording.same_recording(pending);
    };

    for (auto pending = pending_model_integrations_.begin();
         pending != pending_model_integrations_.end();) {
        if (!pending->recording) {
            ++pending;
            continue;
        }
        const auto outcome = pending->recording.outcome();
        if (outcome == rhi::CommandRecordingOutcome::Pending) {
            if (!is_current(pending->recording)) {
                // A second outer recording cannot sample provisional textures
                // while the first recording still owns their only uploads.
                return false;
            }
            ++pending;
            continue;
        }
        if (outcome == rhi::CommandRecordingOutcome::Submitted) {
            if (pending->active == nullptr) {
                return false;
            }
            if (pending->active->generation != 0U) {
                if (retired_models_.size()
                    >= limits_.max_model_refresh_generations) {
                    return false;
                }
                retired_models_.push_back(std::move(*pending->active));
            }
            *pending->active = std::move(pending->candidate);
            pending = pending_model_integrations_.erase(pending);
            continue;
        }

        // Canceled (or conservatively unavailable) candidate resources remain
        // state-owned and reusable. Their texture copies are re-recorded into
        // the next command list, while their already-appended mesh ranges and
        // driver allocations consume no additional retirement/arena capacity.
        // The authored active generation was never overwritten.
        if (pending->active == nullptr) {
            return false;
        }
        pending->recording = {};
        pending->blocked = false;
        ++pending;
    }

    const auto reconcile_upload = [&](rhi::CommandRecordingToken& token,
                                      bool& state,
                                      bool canceled_state) {
        if (!token) {
            return true;
        }
        switch (token.outcome()) {
        case rhi::CommandRecordingOutcome::Submitted:
            token = {};
            return true;
        case rhi::CommandRecordingOutcome::Canceled:
        case rhi::CommandRecordingOutcome::Unavailable:
            state = canceled_state;
            token = {};
            return true;
        case rhi::CommandRecordingOutcome::Pending:
            return is_current(token);
        }
        return false;
    };
    return reconcile_upload(
               material_fallback_upload_recording_,
               material_fallback_uploaded_, false);
}

const SceneRenderer::ResidentModel* SceneRenderer::ensure_model_resident(
    const std::string& source,
    rhi::CommandList& commands,
    core::u64 publication_fence) {
    // Whatever this returns is drawn in this recording: note it, so a later
    // retirement reuses its slots only after the recording is submitted.
    const ResidentModel* const model = integrate_model(source, commands, publication_fence);
    if (model != nullptr) {
        note_use(*model, commands);
    }
    return model;
}

const SceneRenderer::ResidentModel* SceneRenderer::integrate_model(
    const std::string& source,
    rhi::CommandList& commands,
    core::u64 publication_fence) {
    auto found = resident_models_.find(source);
    const bool has_resident = found != resident_models_.end()
        && found->second.generation != 0U;
    auto pending = found == resident_models_.end()
        ? pending_model_integrations_.end()
        : std::find_if(
            pending_model_integrations_.begin(),
            pending_model_integrations_.end(),
            [&](const PendingModelIntegration& integration) noexcept {
                return integration.active == &found->second;
            });
    if (pending != pending_model_integrations_.end()
        && pending->recording.outcome()
            == rhi::CommandRecordingOutcome::Pending) {
        const auto current = commands.recording_token();
        if (current && current.same_recording(pending->recording)) {
            return &pending->candidate;
        }
        return has_resident ? &found->second : nullptr;
    }

    const ModelCacheObservation observed =
        model_cache_.observe(source, publication_fence, authored_asset_generation_);
    switch (observed.state) {
    case ModelCacheState::Pending:
    case ModelCacheState::Busy:
        ++stats_.models_pending;
        break;
    case ModelCacheState::Failed:
        ++stats_.models_failed;
        break;
    case ModelCacheState::Ready:
    case ModelCacheState::Unseen:
        break;
    }
    if (observed.stale_ready) {
        ++stats_.models_stale;
    }

    // A candidate leaving its transaction may still have writes in flight
    // into its slots and textures: it is retired, and they are reused only
    // after those recordings.
    const auto retire_candidate = [&](PendingModelIntegration& integration) {
        if (integration.candidate.geometry) {
            for (const auto& write : integration.candidate.geometry->writes) {
                integration.candidate.uses.push_back(write.recording);
            }
        }
        for (const auto& write : integration.texture_writes) {
            integration.candidate.uses.push_back(write.recording);
        }
        retired_models_.push_back(std::move(integration.candidate));
    };

    // A canceled-generation retry is superseded only after the command's
    // publication fence can see a different complete asset. A newer live
    // request may still be Pending or Failed while `observed` intentionally
    // returns this candidate's stale generation; discarding it merely because
    // current_generation advanced would make one recording depend on worker
    // timing outside its frozen epoch.
    if (pending != pending_model_integrations_.end()
        && observed.ready()
        && observed.generation != 0U
        && observed.generation != pending->generation) {
        // Explicit refresh supersedes the canceled candidate. Keep its driver
        // resources in the already-reserved retirement slot rather than
        // destroying them on the frame thread.
        retire_candidate(*pending);
        pending = pending_model_integrations_.erase(pending);
    }

    const bool retry = pending != pending_model_integrations_.end()
        && !pending->recording;
    const std::shared_ptr<const ModelAsset> decoded_asset = retry
        ? pending->decoded_asset
        : observed.asset;
    const core::u64 decoded_generation = retry
        ? pending->generation
        : observed.generation;

    // A contended/pending/failed cache never displaces complete GPU geometry.
    // Likewise, a command-list fence may deliberately hide a CPU publication
    // that completed after another mapped view began this recording.
    if (decoded_asset == nullptr || (!retry && !observed.ready())) {
        return has_resident ? &found->second : nullptr;
    }
    if (has_resident && found->second.generation == decoded_generation) {
        return &found->second;
    }
    if (texture_streamer_ && !texture_streamer_->contains(source, decoded_generation, *decoded_asset)) {
        ++stats_.models_pending;
        return has_resident ? &found->second : nullptr;
    }
    const bool replacing = has_resident;
    if (found != resident_models_.end()
        && (found->second.blocked_generation == decoded_generation
            || (retry && pending->blocked))) {
        ++stats_.model_integration_refusals;
        return replacing ? &found->second : nullptr;
    }

    // Reserve the path key before any bounded integration work. A generation-
    // zero entry is a non-drawable sticky-refusal record for a cold source.
    if (found == resident_models_.end()) {
        try {
            found = resident_models_.try_emplace(
                source, ResidentModel{}).first;
        } catch (...) {
            ++stats_.model_integration_refusals;
            return nullptr;
        }
    }
    const auto block_generation = [&]() noexcept
        -> const ResidentModel* {
        found->second.blocked_generation = decoded_generation;
        if (retry) {
            pending->blocked = true;
        }
        ++stats_.model_integration_refusals;
        return replacing ? &found->second : nullptr;
    };
    const auto retry_later = [&]() noexcept -> const ResidentModel* {
        // Candidate/receipt capacity is recording-local pressure, not a defect
        // in this decoded generation. Preserve the current wall answer and let
        // a later command retry after earlier transactions reconcile.
        ++stats_.model_integration_refusals;
        return replacing ? &found->second : nullptr;
    };
    ++stats_.model_integration_attempts;
    const auto active_pending = static_cast<std::size_t>(std::count_if(
        pending_model_integrations_.begin(),
        pending_model_integrations_.end(),
        [](const PendingModelIntegration& integration) noexcept {
            return integration.candidate.generation != 0U;
        }));
    if ((!retry
            && retired_models_.size() + active_pending
                >= limits_.max_model_refresh_generations)
        || (!retry
            && pending_model_integrations_.size()
                >= limits_.max_model_refresh_generations)) {
        return retry_later();
    }

    const auto recording = commands.recording_token();
    if (!recording) {
        return retry_later();
    }

    const ModelAsset& decoded = *decoded_asset;
    const auto model_textures = texture_streamer_ ? std::span<const ModelTexture>{} :
        std::span<const ModelTexture>{decoded.textures};
    // Streams the candidate's geometry and (Basic) textures. Once all of it
    // has landed, or was written earlier in this recording, it is drawable
    // here; until then the wall keeps its current answer.
    const auto advance = [&](PendingModelIntegration& integration)
        -> const ResidentModel* {
        const bool geometry_drawable = integration.candidate.geometry
            && stream_geometry(*integration.candidate.geometry, commands, false);
        const TextureUpload textures = stream_model_textures(integration, commands);
        if (textures == TextureUpload::Failed) {
            integration.blocked = true;
            found->second.blocked_generation = decoded_generation;
            ++stats_.model_integration_refusals;
            return replacing ? &found->second : nullptr;
        }
        if (!geometry_drawable || textures != TextureUpload::Drawable) {
            ++stats_.models_pending;
            return replacing ? &found->second : nullptr;
        }
        integration.recording = recording;
        integration.blocked = false;
        ++stats_.model_integrations;
        return &integration.candidate;
    };
    if (retry && pending->candidate.generation == decoded_generation) {
        return advance(*pending);
    }

    core::u64 texture_bytes{};
    for (const ModelTexture& texture : model_textures) {
        if (!texture.valid()) {
            continue;
        }
        const auto bytes = texture_mip_bytes(texture, 0U);
        if (texture_bytes
                > std::numeric_limits<core::u64>::max() - bytes) {
            return block_generation();
        }
        texture_bytes += bytes;
    }
    if (texture_bytes > limits_.max_model_texture_bytes
        || model_texture_bytes_
            > limits_.max_model_texture_bytes - texture_bytes) {
        return block_generation();
    }

    ResidentModel resident{};
    resident.decoded_asset = decoded_asset;
    resident.bounds = decoded.bounds;
    resident.generation = decoded_generation;
    resident.texture_bytes = texture_bytes;
    try {
        // Both vectors reach their exact decoded sizes before the mesh arenas
        // are mutated or any command captures a GPU texture.
        resident.textures.reserve(model_textures.size());
        resident.parts.reserve(decoded.parts.size());
    } catch (...) {
        return block_generation();
    }
    const auto retain_partial = [&]() -> const ResidentModel* {
        const bool owns_resource = std::any_of(
            resident.textures.begin(), resident.textures.end(),
            [](const rhi::Texture& texture) noexcept {
                return static_cast<bool>(texture);
            });
        if (owns_resource) {
            model_texture_bytes_ += texture_bytes;
            retired_models_.push_back(std::move(resident));
            stats_.model_texture_bytes = texture_streamer_ ? texture_streamer_->stats().allocated_bytes : model_texture_bytes_;
        }
        return block_generation();
    };
    for (const ModelTexture& source_texture : model_textures) {
        rhi::Texture texture{};
        if (source_texture.valid()) {
            auto desc = sampled_desc(source_texture.width, source_texture.height);
            desc.format = model_texture_format(source_texture);
            desc.mip_levels = static_cast<core::u32>(source_texture.lower_mips.size() + 1U);
            auto created = device_->create_texture(desc);
            if (!created) {
                return retain_partial();
            }
            texture = std::move(created).value();
        }
        resident.textures.push_back(std::move(texture));
    }

    // Another renderer on this device may already hold this decoded
    // generation's geometry; then it is shared rather than written again.
    GeometryResidency::Handle geometry = geometry_->find(decoded);
    if (!geometry) {
        bool grew = false;
        geometry = geometry_->allocate(decoded_asset, commands, grew);
        stats_.geometry_uploaded = stats_.geometry_uploaded || grew;
    }
    if (!geometry) {
        // Slots free as retired generations are reclaimed; only a full arena
        // with nothing left to reclaim refuses this generation for good.
        if (!retired_models_.empty()) {
            const bool owns_resource = std::any_of(
                resident.textures.begin(), resident.textures.end(),
                [](const rhi::Texture& texture) noexcept {
                    return static_cast<bool>(texture);
                });
            if (owns_resource) {
                model_texture_bytes_ += texture_bytes;
                retired_models_.push_back(std::move(resident));
            }
            return retry_later();
        }
        return retain_partial();
    }
    const GeometrySpan span = geometry->span;
    resident.geometry = std::move(geometry);
    for (const auto& source_part : decoded.parts) {
        resident.parts.push_back(ResidentModelPart{
            MeshRange{
                span.first_index + source_part.first_index,
            source_part.index_count},
            source_part.material,
            source_part.has_material,
            source_part.base_color_texture,
            source_part.metallic_roughness_texture,
            source_part.normal_texture,
            source_part.occlusion_texture,
            source_part.emissive_texture,
            source_part.clearcoat_texture,
            source_part.clearcoat_roughness_texture,
            source_part.clearcoat_normal_texture,
            source_part.sheen_color_texture,
            source_part.sheen_roughness_texture,
            source_part.transmission_texture,
            source_part.specular_texture,
            source_part.specular_color_texture,
            source_part.anisotropy_texture,
            source_part.samplers});
    }
    if (retry) {
        if (pending->candidate.geometry || !pending->candidate.textures.empty()) {
            retire_candidate(*pending);
        }
        pending->candidate = std::move(resident);
        pending->recording = {};
        pending->blocked = false;
        pending->texture_next = 0U;
        pending->texture_writes.clear();
    } else {
        pending_model_integrations_.push_back(PendingModelIntegration{
            &found->second,
            std::move(resident),
            decoded_asset,
            {},
            decoded_generation,
            false});
        pending = std::prev(pending_model_integrations_.end());
    }
    model_texture_bytes_ += texture_bytes;
    stats_.model_texture_bytes = texture_streamer_ ? texture_streamer_->stats().allocated_bytes : model_texture_bytes_;
    return advance(*pending);
}

bool SceneRenderer::render(
    rhi::CommandList& commands,
    const scene::Scene& scene,
    scene::CanvasRect slice) {
    return render(commands, scene, slice, SceneRenderOptions{});
}

bool SceneRenderer::render(
    rhi::CommandList& commands,
    const scene::Scene& scene,
    scene::CanvasRect slice,
    const SceneRenderOptions& options) {
    media_cache_.begin_frame();
    struct EndMediaFrame final {
        MediaCache* cache{};
        ~EndMediaFrame() { cache->end_frame(); }
    } end_media_frame{&media_cache_};
    return render(commands, scene, slice, options, media_cache_);
}

// One recording of the environment as a background, into whichever target owns
// it. `render` draws it into its own colour target; the compositor hands in the
// accumulator its layers will be composited over.
//
// It owns the clear: the pass clears colour and depth as any first pass does
// and fills every pixel, so the colour handed in is overwritten rather than
// blended against.
bool SceneRenderer::record_sky(
    rhi::CommandList& commands, rhi::Texture& target, rhi::Color clear,
    const SceneRenderView& render_view, const scene::Scene& scene,
    float exposure, scene::CanvasRect slice, bool explicit_view) noexcept {
    if (!background_resident(scene)) {
        return false;
    }
    SkyConstants sky_constants{};
    const scene::Mat4 inverse_view_projection =
        scene::invert(render_view.canvas_view_projection);
    for (core::u32 index = 0U; index < 16U; ++index) {
        sky_constants.inverse_view_projection[index] =
            inverse_view_projection.m[index];
    }
    // The target's extent, not the renderer's: the compositor's accumulator is
    // padded to the same size, and reading it from the wrong texture is how a
    // background ends up scaled against the geometry in front of it.
    sky_constants.parameters[0U] = static_cast<float>(slice.width) / static_cast<float>(target.width());
    sky_constants.parameters[1U] = static_cast<float>(slice.height) / static_cast<float>(target.height());
    sky_constants.parameters[2U] =
        environment_generation_ != 0U ? (physical_environment_ ? 1.0F : std::max(scene.environment.map_intensity, 0.0F)) : 0.0F;
    sky_constants.parameters[3U] = exposure;
    sky_constants.canvas[0] = static_cast<float>(explicit_view ? slice.width : scene.canvas.width);
    sky_constants.canvas[1] = static_cast<float>(explicit_view ? slice.height : scene.canvas.height);
    sky_constants.canvas[2] = explicit_view ? 0.0F : static_cast<float>(slice.x);
    sky_constants.canvas[3] = explicit_view ? 0.0F : static_cast<float>(slice.y);
    sky_constants.fallback[0] = scene.environment.sky_color.x;
    sky_constants.fallback[1] = scene.environment.sky_color.y;
    sky_constants.fallback[2] = scene.environment.sky_color.z;
    sky_constants.fallback[3] = environment_generation_ != 0U ? 1.0F : 0.0F;
    for (std::size_t i = 0; i < std::min<std::size_t>(scene.directional_lights.size(), scene::max_scene_directional_lights); ++i) {
        const auto& light = scene.directional_lights[i];
        const float radius = std::clamp(light.angular_radius, 0.0F, 0.249F);
        if (radius <= 0.0F || light.intensity <= 0.0F) continue;
        const auto direction = -scene::normalize(light.direction, {0, -1, 0});
        const auto emission = scene::light_emission(light, scene.renderer_settings.photometric_scale);
        const float sine = scene::deterministic::sin_turns(radius);
        // A uniform disc integrates to E = pi * L * sin(radius)^2 on a
        // perpendicular receiver. Radius changes area, preserving illuminance.
        const float radiance = emission.intensity / std::max(3.14159265358979323846F * sine * sine, 1e-12F);
        auto& source = sky_constants.sun_direction_radius[i];
        source[0] = direction.x; source[1] = direction.y; source[2] = direction.z;
        source[3] = radius * 6.28318530717958647692F;
        auto& color = sky_constants.sun_radiance[i];
        color[0] = emission.color.x * radiance;
        color[1] = emission.color.y * radiance;
        color[2] = emission.color.z * radiance;
    }

    const std::array<rhi::SampledTexture, 1U> sky_sources{
        rhi::SampledTexture{environment_generation_ != 0U ? (physical_environment_ ? environment_->background() : environment_->maps().specular) : &environment_cube_fallback_,
                            &environment_sampler_}};
    const std::array<rhi::DrawBatch, 1U> sky_batches{rhi::DrawBatch{3U, 1U}};
    rhi::PulledDraw sky{};
    sky.pipeline = &sky_pipeline_;
    sky.fragment_textures = sky_sources;
    sky.fragment_uniform = bytes_of(sky_constants);
    sky.batches = sky_batches;
    // `depth_` rather than a depth target of the compositor's: it is already
    // sized to this renderer's padded extent, and the layer render that
    // follows clears it before drawing anything of its own.
    return graph_.draw_pulled(commands, core::RenderPass::Sky, target, clear, depth_, 0.0F, sky);
}

bool SceneRenderer::background_resident(const scene::Scene& scene) const noexcept {
    if (!sky_pipeline_) return false;
    if (environment_ && environment_->maps().complete() && environment_generation_ != 0U) return true;
    return std::any_of(scene.directional_lights.begin(), scene.directional_lights.end(),
        [](const auto& light) { return light.angular_radius > 0.0F && light.intensity > 0.0F; });
}

// The compositor's background.
//
// Isolated layers are transparent by contract, so none of them may draw a sky;
// this is how the environment still ends up behind them. The view is resolved
// through the same helper the layer renders use, from the same options, so the
// background cannot disagree with the geometry drawn over it.
bool SceneRenderer::record_background(
    rhi::CommandList& commands, rhi::Texture& target,
    const scene::Scene& scene, scene::CanvasRect slice,
    const SceneRenderOptions& options) noexcept {
    graph_.begin();
    if (!prepare_physical_environment(commands, scene)) return false;
    if (!background_resident(scene)) {
        return false;
    }
    SceneRenderView render_view{};
    if (!resolve_render_view(scene, slice, options, render_view)) {
        return false;
    }
    const float exposure = mode_ == SceneRendererMode::AdvancedLinear
        ? scene::camera_exposure_multiplier(scene.camera)
        : 1.0F;
    return record_sky(commands, target,
                      rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, render_view, scene,
                      exposure, slice, options.view.has_value());
}

bool SceneRenderer::prepare_physical_environment(
    rhi::CommandList& commands, const scene::Scene& scene) noexcept {
    if (embedded_environment_) return true;
    if (!scene.environment.physical_sky.enabled) {
        if (physical_environment_) {
            physical_environment_ = false;
            environment_generation_ = 0;
        }
        return true;
    }
    if (!scene_linear() || !environment_ || !sky_pipeline_) {
        stats_.assets.ready = false;
        static_cast<void>(core::report_idle_once("Physical sky", "Advanced compute sky pipeline is unavailable"));
        return false;
    }
    const auto sky = scene::clamped(scene.environment.physical_sky);
    const float scale = scene.renderer_settings.photometric_scale;
    if (!physical_environment_ || sky != physical_environment_settings_
        || scale != physical_environment_scale_ || !environment_->maps().complete()) {
        if (!environment_->prepare(commands) || !environment_->record_physical(commands, sky, scale)) return false;
        ++stats_.physical_sky_updates;
        physical_environment_settings_ = sky;
        physical_environment_scale_ = scale;
        physical_environment_ = true;
        environment_generation_ = 1;
        environment_source_.clear();
    }
    return true;
}

bool SceneRenderer::adopt_environment(
    rhi::CommandList& commands, const DecodedFrame& frame,
    scene::Turns rotation) noexcept {
    if (!environment_.has_value() || !frame.valid()
        || frame.format != rhi::TextureFormat::Rgba16Float) {
        return false;
    }
    if (device_ == nullptr) {
        return false;
    }
    if (!environment_source_texture_
        || environment_source_texture_.width() != frame.width
        || environment_source_texture_.height() != frame.height) {
        rhi::TextureDesc desc{};
        desc.width = frame.width;
        desc.height = frame.height;
        desc.format = rhi::TextureFormat::Rgba16Float;
        desc.sampled = true;
        auto created = device_->create_texture(desc);
        if (!created) {
            return false;
        }
        environment_source_texture_ = std::move(created).value();
    }
    if (!commands.upload(environment_source_texture_, frame.pixels)) {
        return false;
    }
    stats_.upload_bytes += frame.pixels.size();
    return environment_->record(
        commands, environment_source_texture_, rotation);
}

namespace {

// FNV-1a over a placement's parameter tracks, where they start and the show
// rate: the inputs its whole history depends on.
core::u64 parameters_digest(const scene::ParticleEffect& effect, scene::MediaFrameRate rate) noexcept {
    core::u64 hash = 0xcbf29ce484222325ULL;
    const auto mix = [&hash](const void* data, std::size_t bytes) {
        const auto* const at = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0U; i < bytes; ++i) {
            hash = (hash ^ at[i]) * 0x100000001b3ULL;
        }
    };
    for (const scene::ParticleParameter& parameter : effect.parameters) {
        mix(parameter.key.data(), parameter.key.size() + 1U);
        mix(&parameter.base, sizeof(parameter.base));
        const scene::ChannelKeyframes& curve = parameter.curve;
        mix(curve.frames.data(), curve.frames.size() * sizeof(core::i64));
        mix(curve.values.data(), curve.values.size() * sizeof(double));
        mix(curve.interpolations.data(), curve.interpolations.size() * sizeof(scene::KeyInterpolation));
        for (const std::optional<scene::KeyTangent>& tangent : curve.tangents) {
            const double handle[5] = {tangent ? static_cast<double>(tangent->mode) : -1.0,
                                      tangent ? tangent->in_slope : 0.0, tangent ? tangent->out_slope : 0.0,
                                      tangent ? tangent->in_weight.value_or(-1.0) : 0.0,
                                      tangent ? tangent->out_weight.value_or(-1.0) : 0.0};
            mix(handle, sizeof(handle));
        }
    }
    mix(&effect.start_frame, sizeof(effect.start_frame));
    mix(&rate.numerator, sizeof(rate.numerator));
    mix(&rate.denominator, sizeof(rate.denominator));
    return hash;
}

// A placement's parameters as the simulation asks for them: each bound
// component at the step's own show time. Step `tick` of an effect starting at
// show frame `start` is at start + tick * num / (rate * den) show frames,
// worked in integers so every node splits it into the same frame and fraction.
// It owns copies of the tracks it reads, so the rigid physics thread can use
// it while the scene changes under the frame.
class PlacedParameters final : public ajfx::ParameterProvider {
public:
    PlacedParameters(std::vector<ParticleParameterBinding> bound, core::u64 start, core::u32 num,
                     core::u32 den, core::u32 rate) noexcept
        : bound_{std::move(bound)}, start_{start}, num_{num}, scale_{static_cast<core::u64>(den) * rate} {}

    void values_at(std::uint64_t tick, std::span<ajfx::ParameterValue> values) const override {
        const core::u64 elapsed = tick * num_;
        const core::u64 whole = elapsed / scale_;
        const double fraction = static_cast<double>(elapsed % scale_) / static_cast<double>(scale_);
        const auto frame = static_cast<core::i64>(start_ + whole);
        for (const ParticleParameterBinding& one : bound_) {
            if (one.index < values.size()) {
                values[one.index][one.component] =
                    static_cast<float>(scene::particle_parameter_at(one.track, frame, fraction));
            }
        }
    }

private:
    std::vector<ParticleParameterBinding> bound_;
    core::u64 start_;
    core::u64 num_;
    core::u64 scale_;
};

// The placement's tracks bound to the package's parameters. A component the
// package does not publish is kept and ignored.
std::shared_ptr<const ajfx::ParameterProvider> placed_parameters(const scene::ParticleEffect& effect,
                                                                 const ajfx::Effect& package,
                                                                 scene::MediaFrameRate rate) {
    std::vector<ParticleParameterBinding> bound;
    for (const scene::ParticleParameter& track : effect.parameters) {
        const std::optional<scene::ParticleParameterKey> key = scene::parse_particle_parameter_key(track.key);
        if (!key) {
            continue;
        }
        for (std::size_t index = 0U; index < package.parameters.size(); ++index) {
            if (package.parameters[index].name == key->name) {
                const std::size_t components = package.parameters[index].type == ajfx::ParameterType::Float ? 1U
                    : package.parameters[index].type == ajfx::ParameterType::Vec3 ? 3U : 4U;
                if (key->component < components) {
                    bound.push_back({index, key->component, track});
                }
                break;
            }
        }
    }
    return std::make_shared<const PlacedParameters>(std::move(bound), effect.start_frame, rate.numerator,
                                                    rate.denominator, package.simulation_rate);
}

} // namespace

void SceneRenderer::prepare_particles(rhi::CommandList& commands, const scene::Scene& scene,
                                      const SceneRenderOptions& options) {
    particle_draws_.clear();
    particle_lights_used_ = 0U;
    const auto refuse = [this](core::u32& count, std::string_view reason) {
        ++count;
        if (stats_.particle_issue.empty()) {
            stats_.particle_issue = reason;
        }
    };
    // Retired instances go once nothing submitted uses them.
    std::erase_if(retired_particles_, [](const ParticleInstance& instance) { return instance.safe_to_release(); });
    // Moves a slot's instance to retirement, or says there is no room yet.
    const auto retire = [this](ParticleSlot& slot) {
        if (!slot.instance) {
            return true;
        }
        if (retired_particles_.size() == max_retired_particles) {
            return false;
        }
        retired_particles_.push_back(std::move(*slot.instance));
        slot.instance.reset();
        return true;
    };
    std::erase_if(particle_slots_, [&scene, &retire](auto& slot) {
        const bool gone = std::none_of(scene.particle_effects.begin(), scene.particle_effects.end(),
            [&slot](const scene::ParticleEffect& effect) { return effect.source_node == slot.first; });
        return gone && retire(slot.second);
    });
    // A reflection shows no particles; the view that reflects it draws them.
    if (scene.particle_effects.empty() || options.reflection_pass) {
        return;
    }
    // At most one new instance a frame: each allocates its buffers and
    // checkpoint ring, which is bounded but not free.
    bool instanced = false;
    core::u64 drawn = 0xcbf29ce484222325ULL;
    const auto mix = [&drawn](core::u64 value) noexcept {
        for (core::u32 byte = 0U; byte < 8U; ++byte) {
            drawn = (drawn ^ ((value >> (byte * 8U)) & 0xFFU)) * 0x100000001b3ULL;
        }
    };
    for (const scene::ParticleEffect& effect : scene.particle_effects) {
        const std::optional<core::u64> frame = scene::particle_effect_frame(effect, scene.show_frame);
        if (!frame) {
            continue; // not started: nothing to draw, and nothing missing
        }
        if (particle_pass_ == nullptr) {
            refuse(stats_.particles_unsupported, particles_unsupported_);
            continue;
        }
        const ParticlePackageObservation observed =
            particle_packages_->observe(effect.source, authored_asset_generation_);
        if (observed.package == nullptr) {
            if (observed.state == ParticlePackageState::Failed) {
                refuse(stats_.particles_failed, observed.error);
            } else {
                ++stats_.particles_pending;
            }
            continue;
        }
        ParticleSlot& slot = particle_slots_[effect.source_node];
        const core::u32 seed = scene::particle_instance_seed(effect);
        // A new package or seed starts a new instance; the old one keeps
        // drawing until retirement has room for it.
        if ((slot.package != observed.package || slot.seed != seed) && retire(slot)) {
            slot = ParticleSlot{observed.package, seed, std::nullopt, {}};
        }
        if (!slot.instance) {
            if (!slot.error.empty()) {
                refuse(stats_.particles_failed, slot.error);
                continue;
            }
            if (instanced) {
                ++stats_.particles_pending;
                continue;
            }
            instanced = true;
            auto created = particle_pass_->create_instance(slot.package->effect, slot.package->kernels,
                                                           slot.package->textures, slot.package->fields,
                                                           slot.package->meshes, seed);
            if (!created) {
                slot.error = std::move(created).error().message;
                refuse(stats_.particles_failed, slot.error);
                continue;
            }
            slot.instance.emplace(std::move(created).value());
            slot.parameters = parameters_digest(effect, scene.show_rate);
            slot.provider = placed_parameters(effect, slot.package->effect, scene.show_rate);
            particle_pass_->restart(*slot.instance, slot.provider);
            slot.generation = ++particle_generation_;
        }
        // A changed parameter changes the effect's whole past.
        if (const core::u64 digest = parameters_digest(effect, scene.show_rate); digest != slot.parameters) {
            slot.provider = placed_parameters(effect, slot.package->effect, scene.show_rate);
            particle_pass_->restart(*slot.instance, slot.provider);
            slot.parameters = digest;
            slot.generation = ++particle_generation_;
        }
        if (!particle_pass_->advance(commands, *slot.instance, *frame,
                scene.show_rate.numerator, scene.show_rate.denominator, slot.provider.get())) {
            if (const std::optional<std::string> failed = slot.instance->failure()) {
                refuse(stats_.particles_failed, *failed);
            } else {
                ++stats_.particles_behind;
            }
            continue;
        }
        // Its lights, in the slots after the effects before it. One that
        // would overrun the budget is refused rather than lit partly.
        if (const core::u32 lights = particle_pass_->light_count(*slot.instance); lights != 0U) {
            if (!particle_light_buffer_ || particle_lights_used_ + lights > max_particle_lights) {
                refuse(stats_.particles_failed, "particle effects ask for more than " +
                                                    std::to_string(max_particle_lights) + " lights");
                continue;
            }
            if (!particle_pass_->write_lights(commands, *slot.instance, effect.world, particle_light_buffer_,
                                              particle_lights_used_)) {
                continue;
            }
            particle_lights_used_ += lights;
        }
        particle_draws_.emplace_back(&*slot.instance, effect.world);
        mix(effect.source_node.value());
        mix(slot.generation);
        mix(*frame);
        for (const float lane : effect.world.m) {
            mix(std::bit_cast<core::u32>(lane));
        }
    }
    if (particle_pass_ != nullptr) {
        if (std::optional<ParticleError> failed = particle_pass_->take_error()) {
            // The frame's simulations are suspect: draw none of them.
            refuse(stats_.particles_failed, failed->message);
            particle_draws_.clear();
        }
    }
    if (!particle_draws_.empty()) {
        stats_.particle_digest = drawn;
    }
    if (stats_.particles_pending + stats_.particles_failed + stats_.particles_unsupported
            + stats_.particles_behind != 0U) {
        stats_.assets.ready = false;
    }
}

bool SceneRenderer::record_particles(rhi::CommandList& commands, const scene::ViewMatrices& view,
                                     ParticleTargets targets) {
    // Over everything the scene drew, tested against its depth and writing
    // none: sprites blend back to front where they sort.
    targets.color = &color_;
    targets.depth = &depth_;
    // Every effect's colour first, while the velocity target still holds the
    // scene alone for soft particles to fade against; then their motion.
    for (const auto& [instance, world] : particle_draws_) {
        if (!particle_pass_->draw(commands, *instance, view, world, targets)) {
            return false;
        }
        ++stats_.particles_drawn;
    }
    if (targets.velocity != nullptr) {
        for (const auto& [instance, world] : particle_draws_) {
            if (!particle_pass_->draw_motion(commands, *instance, view, world, targets)) {
                return false;
            }
        }
    }
    return true;
}

bool SceneRenderer::render(
    rhi::CommandList& commands,
    const scene::Scene& scene,
    scene::CanvasRect slice,
    const SceneRenderOptions& options,
    MediaCache& media_cache) {
    // The geometry is shared with every renderer on this device.
    const auto geometry_held = geometry_->lock();
    stats_ = RenderStats{};
    graph_.begin();
    struct PublishGraph { FrameGraph& graph; RenderStats& stats;
        ~PublishGraph() { stats.graph = graph.stats(); }
    } publish_graph{graph_, stats_};
    for (const auto& light : scene.directional_lights)
        if (!scene::supported_light_unit(light)) return false;
    for (const auto& light : scene.point_lights)
        if (!scene::supported_light_unit(light)) return false;

    authored_asset_generation_ = scene::asset_generation_key(options.residency_scene ? *options.residency_scene : scene);
    if(!media_cache.adopt_generation(authored_asset_generation_,(options.residency_scene ? *options.residency_scene : scene).media)) return false;
    stats_.model_texture_bytes = texture_streamer_ ? texture_streamer_->stats().allocated_bytes : model_texture_bytes_;
    geometry_selection_.clear();
    const std::size_t requested_instances = scene.instances.size() + scene.models.size() + scene.media.size();
    if (requested_instances > limits_.max_instances) {
        stats_.dropped_instances = static_cast<core::u32>(std::min<std::size_t>(
            requested_instances, std::numeric_limits<core::u32>::max()));
        return false;
    }
    if (!resolve_recording_transactions(commands)) {
        return false;
    }
    // The command list, rather than a show-frame/timer key, is the safe local
    // publication boundary. Every renderer and mapped view recorded into this
    // list reuses this exact epoch even if the worker completes in between.
    const core::u64 model_publication_fence = scene.models.empty() &&
        (!options.residency_scene || options.residency_scene->models.empty())
        ? 0U
        : commands.latch_asset_publication_epoch(
              model_cache_.publication_epoch());

    if (!scene::valid(scene.camera) ||
        options.history_slot >= view_histories_.size() ||
        (!options.view.has_value() && !scene::contains(scene.canvas, slice))) {
        return false;
    }

    SceneRenderView render_view{};
    if (!resolve_render_view(scene, slice, options, render_view)) {
        return false;
    }
    // An editor view's shading aid. Wireframe needs its own pipelines, which
    // only a renderer built for an editor view has: refused, not drawn lit.
    const ViewShading shading = render_view.shading;
    const bool wireframe = shading == ViewShading::Wireframe;
    if (wireframe && (!wireframe_pipeline_ || !media_wireframe_pipeline_)) {
        static_cast<void>(core::report_idle_once(
            "view shading",
            "wireframe needs a renderer built for an editor view"));
        return false;
    }
    if ((shading == ViewShading::UvChecker
         || shading == ViewShading::WorldNormals
         || render_view.model_part.has_value())
        && !limits_.view_shading) {
        static_cast<void>(core::report_idle_once(
            "view shading",
            "UV checker, world normals and part isolation need a renderer "
            "built for an editor view"));
        return false;
    }

    planar_active_ = false;
    planar_texture_ = nullptr;
    // Views that draw no light need no mirror either.
    if (!options.reflection_pass && scene.environment.planar.enabled
        && view_shading_lit(shading)) {
        const auto& p = scene.environment.planar;
        const auto& full = options.residency_scene ? *options.residency_scene : scene;
        const bool receiver = p.receiver != doc::NodeId{} &&
            (std::any_of(full.instances.begin(), full.instances.end(), [&](const auto& v) { return v.source == p.receiver; }) ||
             std::any_of(full.models.begin(), full.models.end(), [&](const auto& v) { return v.source_node == p.receiver; }));
        if (!scene_linear() || (!planar_renderer_ && !options.shared_planar) || !receiver) {
            ++stats_.planar_refusals;
            stats_.reflection_reason = !scene_linear() ? "Planar reflections require Advanced" : !receiver ? "Choose a planar receiver object" : "Only one planar reflection pass is supported";
            static_cast<void>(core::report_idle_once("Planar reflection", stats_.reflection_reason.data()));
        } else if (options.shared_planar) {
            planar_active_ = true;
            planar_texture_ = options.shared_planar;
        } else {
            const auto mirror = scene::reflection_matrix(p);
            auto mirrored = render_view;
            mirrored.matrices.view = scene::multiply(render_view.matrices.view, mirror);
            mirrored.matrices.view_projection = scene::multiply(render_view.matrices.view_projection, mirror);
            mirrored.canvas_view_projection = scene::multiply(render_view.canvas_view_projection, mirror);
            mirrored.eye = scene::transform_point(mirror, render_view.eye);
            const auto direction = scene::transform(mirror, {render_view.forward.x, render_view.forward.y, render_view.forward.z, 0});
            mirrored.forward = {direction.x, direction.y, direction.z};
            SceneRenderOptions reflection_options{};
            reflection_options.reflection_pass = true;
            reflection_options.full_canvas_view = !options.view.has_value();
            reflection_options.residency_scene = &full;
            reflection_options.view = mirrored;
            reflection_options.excluded_source = p.receiver;
            reflection_options.history = SceneHistoryAction::Reset;
            const auto n = p.normal * (scene::dot(p.normal, render_view.eye - p.point) >= 0 ? 1.0F : -1.0F);
            reflection_options.clip_plane = {n.x, n.y, n.z, -scene::dot(n, p.point) - 0.0005F};
            if (!planar_renderer_->render(commands, full, slice, reflection_options, media_cache)) return false;
            planar_active_ = true;
            planar_texture_ = &planar_renderer_->target();
            ++stats_.planar_passes;
        }
    }

    if (texture_streamer_ && (!scene.models.empty() ||
        (options.residency_scene && !options.residency_scene->models.empty()) ||
        texture_streamer_->stats().allocated_bytes != 0U)) {
        const auto& full = options.residency_scene ? *options.residency_scene : scene;
        texture_requests_.clear();
        bool complete = true;
        for (const auto models : {std::span<const scene::ModelInstance>{full.models},
                                  std::span<const scene::ModelInstance>{full.publication_retained_models}})
        for (const auto& model : models) {
            if (model.source.empty()) continue;
            // One observation per textured source in this recording. A later
            // try-lock miss for another instance cannot mix refresh generations.
            const auto prior = std::find_if(texture_requests_.begin(), texture_requests_.end(),
                [&](const auto& request) { return request.source == model.source; });
            const auto observed = prior == texture_requests_.end()
                ? model_cache_.observe(model.source, model_publication_fence, authored_asset_generation_)
                : ModelCacheObservation{};
            auto asset = prior == texture_requests_.end() ? observed.asset : prior->asset;
            auto generation = prior == texture_requests_.end() ? observed.generation : prior->generation;
            if (!asset) {
                const auto held = resident_models_.find(model.source);
                if (held != resident_models_.end()) {
                    asset = held->second.decoded_asset;
                    generation = held->second.generation;
                }
            }
            if (!asset) {
                complete = false;
                if (observed.state == ModelCacheState::Failed) ++stats_.models_failed;
                else ++stats_.models_pending;
                continue;
            }
            const float pixels = projected_model_pixels(full, model.world, asset->bounds);
            for (core::u32 i = 0U; i < asset->textures.size(); ++i) {
                if (!asset->textures[i].valid()) continue;
                auto found = std::find_if(texture_requests_.begin(), texture_requests_.end(),
                    [&](const auto& request) { return request.source == model.source && request.texture == i; });
                if (found != texture_requests_.end()) {
                    found->projected_pixels = std::max(found->projected_pixels, pixels);
                    continue;
                }
                if (texture_requests_.size() == texture_requests_.capacity()) {
                    stats_.texture_streaming.state = TextureStreamState::CapacityRefused;
                    return false;
                }
                texture_requests_.push_back({model.source, asset, generation, i, pixels, full.show_frame});
            }
        }
        if (!complete) {
            stats_.texture_streaming = texture_streamer_->stats();
            stats_.texture_streaming.state = TextureStreamState::Loading;
            return false; // No partial full-scene plan; the output owner holds.
        }
        const auto settings = scene::clamped(full.renderer_settings);
        const auto budget = std::min(limits_.max_model_texture_bytes,
            static_cast<core::u64>(settings.texture_budget_mib) * 1024ULL * 1024ULL);
        const bool prepared = texture_streamer_->prepare(*device_, commands, texture_requests_,
            budget, settings.texture_streaming, settings.texture_mip_bias);
        stats_.texture_streaming = texture_streamer_->stats();
        stats_.model_texture_bytes = stats_.texture_streaming.allocated_bytes;
        stats_.upload_bytes += stats_.texture_streaming.upload_bytes;
        if (!prepared) return false;
    }

    // Decided here, before anything is bound.
    //
    // The per-batch descriptor arrays are assembled during scene traversal,
    // which happens long before the occlusion dispatch, so a flag set by that
    // dispatch would bind last frame's answer. The dispatch runs under exactly
    // this condition and a failure returns, so the binding and the recording
    // cannot disagree.
    occlusion_recorded_ = occlusion_.has_value()
        && mode_ == SceneRendererMode::AdvancedLinear
        && scene.renderer_settings.occlusion_radius > 0.0F;

    if (!material_fallback_uploaded_) {
        constexpr std::array<core::u8, 4U> white{255U, 255U, 255U, 255U};
        constexpr std::array<core::u8, 4U> positive_z{
            128U, 128U, 255U, 255U};
        // Fixed setup texture: opaque grey with a dark X, sampled through the
        // ordinary media pass so transforms, depth and opacity stay identical.
        std::array<core::u8, 64U * 64U * 4U> offline_pixels{};
        for (core::u32 y = 0U; y < 64U; ++y) {
            for (core::u32 x = 0U; x < 64U; ++x) {
                const bool cross = std::abs(static_cast<int>(x) - static_cast<int>(y)) <= 2 ||
                    std::abs(static_cast<int>(x + y) - 63) <= 2;
                const core::u8 shade = cross ? 40U : 128U;
                const auto offset = (y * 64U + x) * 4U;
                offline_pixels[offset] = shade;
                offline_pixels[offset + 1U] = shade;
                offline_pixels[offset + 2U] = shade;
                offline_pixels[offset + 3U] = 255U;
            }
        }
        const auto recording = commands.recording_token();
        if (!recording) {
            return false;
        }
        if (!commands.upload(material_fallback_, white)
            || !commands.upload(normal_fallback_, positive_z)
            || !commands.upload(offline_media_, offline_pixels)) {
            return false;
        }
        material_fallback_uploaded_ = true;
        material_fallback_upload_recording_ = recording;
        stats_.upload_bytes += white.size() + positive_z.size() + offline_pixels.size();
    }

    // The environment, adopted at a generation boundary.
    //
    // Observed rather than loaded: the cache reads and decodes on its own
    // worker, and this never waits for it. A scene that authored no map takes
    // the early exit and costs nothing; a map still decoding leaves the
    // previous environment - or the hemisphere - in place for this frame
    // rather than flashing.
    //
    // Prefiltering is recorded into this same command list, so the maps a
    // frame lights from are the ones its own recording produced.
    const auto& complete_scene = options.residency_scene ? *options.residency_scene : scene;
    stats_.assets = {scene::asset_set_digest(complete_scene), true, 0};
    stats_.ltc_ready = ltc_asset_.prepare(commands, authored_asset_generation_);
    stats_.ltc_generation = ltc_asset_.generation();
    stats_.upload_bytes += ltc_asset_.upload_bytes();
    if (!stats_.ltc_ready) {
        stats_.assets.ready = false;
        static_cast<void>(core::report_idle_once("area lighting", "LTC table upload unavailable"));
        return false;
    }
    for (const auto& light : complete_scene.point_lights) if (scene::area_light(light.kind)) {
        ++stats_.area_lights;
        if (light.casts_shadow) {
            ++stats_.area_shadow_requests;
            stats_.assets.ready = false;
            static_cast<void>(core::report_idle_once("area-light shadows", "area-light shadow maps are not implemented"));
        }
    }
    if (!sky_pipeline_ && std::any_of(complete_scene.directional_lights.begin(), complete_scene.directional_lights.end(),
        [](const auto& light) { return light.angular_radius > 0.0F && light.intensity > 0.0F; })) {
        static_cast<void>(core::report_idle_once("sun disc", "Advanced sky pipeline is unavailable"));
    }
    for (const auto& model : complete_scene.models) {
        const auto observed = model_cache_.observe(model.source, model_publication_fence, authored_asset_generation_);
        stats_.assets.ready = stats_.assets.ready && observed.ready() && !observed.stale_ready;
    }
    // The complete authored scene determines atlas slots, independent of camera,
    // culling, slice, or compositor isolation. Strings are views into this snapshot.
    std::array<std::string_view, max_ies_profiles> ies_sources{};
    std::array<bool, max_ies_profiles> ies_available{};
    core::u32 ies_count = 0;
    bool ies_dirty = !ies_uploaded_;
    if (ies_upload_recording_) {
        const auto outcome = ies_upload_recording_.outcome();
        if (outcome == rhi::CommandRecordingOutcome::Canceled || outcome == rhi::CommandRecordingOutcome::Unavailable)
            ies_dirty = true;
        if (outcome != rhi::CommandRecordingOutcome::Pending) ies_upload_recording_ = {};
    }
    const auto ies_fence = commands.latch_asset_publication_epoch(ies_cache_.publication_epoch(), rhi::AssetPublicationDomain::Ies);
    for (const auto& light : complete_scene.point_lights) {
        if (light.ies_source.empty()) continue;
        if (scene::area_light(light.kind)) {
            ++stats_.ies_refused;
            stats_.assets.ready = false;
            static_cast<void>(core::report_idle_once("IES lighting", "IES profiles require point or spot emission"));
            continue;
        }
        bool duplicate = false;
        for (core::u32 i = 0; i < ies_count; ++i) if (ies_sources[i] == light.ies_source) duplicate = true;
        if (duplicate) continue;
        if (ies_count == max_ies_profiles || scene::area_light(light.kind)) {
            ++stats_.ies_refused;
            stats_.assets.ready = false;
            static_cast<void>(core::report_idle_once("IES lighting", "profile atlas capacity or unsupported area light"));
            continue;
        }
        const auto slot = ies_count++;
        ies_sources[slot] = light.ies_source;
        const auto observed = conformance_ies_
            ? IesObservation{conformance_ies_, IesCacheState::Ready}
            : ies_cache_.observe(light.ies_source, ies_fence, authored_asset_generation_);
        if (observed.profile && observed.state == IesCacheState::Ready) {
            ies_available[slot] = true;
            ++stats_.ies_ready;
            if (ies_profiles_[slot] != observed.profile) {
                ies_profiles_[slot] = observed.profile;
                std::copy(observed.profile->values.begin(), observed.profile->values.end(),
                    ies_atlas_scratch_.begin() + static_cast<std::ptrdiff_t>(slot * ies_profile_texels));
                ies_dirty = true;
            }
        } else {
            stats_.assets.ready = false;
            if (observed.state == IesCacheState::Failed || observed.state == IesCacheState::Capacity) {
                ++stats_.ies_failed;
                stats_.ies_error = observed.error;
                static_cast<void>(core::report_idle_once("IES lighting", ies_error_label(observed.error)));
            } else {
                ++stats_.ies_pending;
                static_cast<void>(core::report_idle_once("IES lighting", "profile loading or publication pending"));
            }
        }
    }
    if (ies_dirty) {
        ies_uploaded_ = false;
        const auto token = commands.recording_token();
        const auto bytes = std::span<const core::u8>{reinterpret_cast<const core::u8*>(ies_atlas_scratch_.data()),
            ies_atlas_scratch_.size() * sizeof(float)};
        if (!token || !commands.upload(ies_atlas_, bytes)) return false;
        ies_uploaded_ = true;
        ies_upload_recording_ = token;
        stats_.upload_bytes += bytes.size();
    }
    const auto ies_slot_for = [&](const scene::PointLight& light) -> int {
        if (light.ies_source.empty()) return -1;
        if (scene::area_light(light.kind)) return -2;
        for (core::u32 i = 0; i < ies_count; ++i)
            if (ies_sources[i] == light.ies_source && ies_available[i]) return static_cast<int>(i);
        return -2;
    };
    const auto& baked = scene.environment.baked;
    const bool reflections_authored = std::any_of(baked.reflections.begin(), baked.reflections.end(), [](const auto& p) { return p.enabled; });
    stats_.baked_ready = !baked.enabled && !reflections_authored;
    active_reflections_ = {};
    if ((baked.enabled || reflections_authored) && mode_ == SceneRendererMode::AdvancedLinear) {
        const auto observed = probe_cache_.observe(baked.source, baked.content,
            commands.latch_asset_publication_epoch(probe_cache_.publication_epoch(), rhi::AssetPublicationDomain::Probes), authored_asset_generation_);
        stats_.baked_state = observed.state;
        if (observed.asset && observed.state == ProbeCacheState::Ready) {
            if (probe_upload_recording_) {
                const auto outcome = probe_upload_recording_.outcome();
                if (outcome == rhi::CommandRecordingOutcome::Canceled || outcome == rhi::CommandRecordingOutcome::Unavailable)
                    probes_uploaded_ = false;
                if (outcome != rhi::CommandRecordingOutcome::Pending) probe_upload_recording_ = {};
            }
            if (probe_asset_ != observed.asset || !probes_uploaded_) {
                const auto data = std::span<const core::u8>{reinterpret_cast<const core::u8*>(observed.asset->probes.data()), observed.asset->probes.size() * sizeof(BakedProbe)};
                if (!commands.upload(probe_buffer_, data)) return false;
                probe_asset_ = observed.asset;
                probes_uploaded_ = true;
                probe_upload_recording_ = commands.recording_token();
                stats_.upload_bytes += data.size();
            }
            stats_.baked_ready = true;
            stats_.baked_probes = static_cast<core::u32>(probe_asset_->probes.size());
            if (reflection_upload_recording_) {
                const auto outcome = reflection_upload_recording_.outcome();
                if (outcome == rhi::CommandRecordingOutcome::Canceled || outcome == rhi::CommandRecordingOutcome::Unavailable) reflection_asset_.reset();
                if (outcome != rhi::CommandRecordingOutcome::Pending) reflection_upload_recording_ = {};
            }
            if (reflections_authored && reflection_asset_ != observed.asset) {
                reflection_asset_.reset();
                for (std::size_t i = 0; i < observed.asset->reflections.size(); ++i) {
                    const auto& pixels = observed.asset->reflections[i];
                    if (pixels.empty()) continue;
                    const auto data = std::span<const core::u8>{reinterpret_cast<const core::u8*>(pixels.data()), pixels.size() * sizeof(scene::Vec4)};
                    if (!commands.upload(reflection_sources_[i], data) || !reflection_filters_[i]->prepare(commands) ||
                        !reflection_filters_[i]->record(commands, reflection_sources_[i], 0)) return false;
                }
                reflection_asset_ = observed.asset;
                reflection_upload_recording_ = commands.recording_token();
            }
            // Asset slots are canonical node order. Look up authored influence
            // by identity; changing slot order cannot change the reduction.
            for (std::size_t i = 0; i < observed.asset->grid.reflections.size(); ++i) {
                const auto& captured = observed.asset->grid.reflections[i];
                if (!captured.enabled) continue;
                for (const auto& p : baked.reflections) {
                    if (!p.enabled || p.source != captured.source || p.position != captured.position) continue;
                    active_reflections_[i] = p; ++stats_.reflection_probes; break;
                }
            }
            const auto requested = static_cast<core::u32>(std::count_if(baked.reflections.begin(), baked.reflections.end(), [](const auto& p) { return p.enabled; }));
            if (requested != stats_.reflection_probes) {
                stats_.reflection_refusals = requested - std::min(requested, stats_.reflection_probes);
                stats_.baked_ready = false;
                stats_.reflection_reason = "Reflection capture missing or moved; rebake lighting";
                static_cast<void>(core::report_idle_once("Reflection probes", stats_.reflection_reason.data()));
            }

        }
    }

    if (!prepare_physical_environment(commands, scene)) return false;
    if (!scene.environment.physical_sky.enabled && environment_.has_value() && !embedded_environment_) {

        const EnvironmentCacheObservation observed =
            environment_cache_.observe(
                scene.environment.map_source,
                commands.latch_asset_publication_epoch(
                    environment_cache_.publication_epoch(), rhi::AssetPublicationDomain::Environments), authored_asset_generation_);
        // Rotation joins the test because it is baked into the cube rather
        // than applied per frame: without it the dial moves the authored value
        // and nothing re-records, so the environment never turns.
        stats_.assets.ready = stats_.assets.ready && (scene.environment.map_source.empty() || (observed.ready() && observed.state == EnvironmentCacheState::Ready));
        const bool changed = observed.generation != environment_generation_
            || scene.environment.map_source != environment_source_
            || scene.environment.map_rotation != environment_rotation_;
        if (scene.environment.map_source.empty()) {
            environment_generation_ = 0U;
            environment_source_.clear();
        } else if (changed && observed.ready()) {
            // A fresh equirect upload and the whole prefilter chain, once.
            // Everything downstream of here is per-frame constant.
            if (!environment_->prepare(commands)) {
                return false;
            }
            if (!adopt_environment(commands, *observed.frame,
                                   scene.environment.map_rotation)) {
                return false;
            }
            environment_generation_ = observed.generation;
            environment_rotation_ = scene.environment.map_rotation;
            try {
                environment_source_ = scene.environment.map_source;
            } catch (const std::bad_alloc&) {
                environment_generation_ = 0U;
                return false;
            }
        }
    }

    if (scene_linear() && (reflections_authored || planar_active_) && environment_ && !environment_->prepare(commands)) return false;
    if (reflections_authored && stats_.reflection_probes == 0 && stats_.reflection_reason.empty()) {
        stats_.reflection_reason = !scene_linear() ? "Reflection probes require Advanced" : "Reflection asset loading, missing or failed";
        static_cast<void>(core::report_idle_once("Reflection probes", stats_.reflection_reason.data()));
    }
    instance_scratch_.clear();
    opaque_submissions_.clear();
    reordered_instances_.clear();
    opaque_visibility_.clear();
    camera_batch_uniforms_.clear();
    camera_batches_.clear();
    camera_batch_textures_.clear();
    light_scratch_.clear();
    inline_effect_op_scratch_.clear();
    inline_effect_parameter_scratch_.clear();
    inline_program_cache_.clear();
    batch_uniforms_.clear();
    batches_.clear();
    batch_textures_.clear();
    rsm_flux_textures_.clear();
    camera_coverage_textures_.clear();
    composite_draws_.clear();
    depth_only_media_.clear();
    next_transforms_.clear();
    DirectionalShadowBounds shadow_bounds{};
    // Transform history is independent of an output's camera or slice. Build
    // it from the complete evaluated scene, not only the items visible in this
    // view, so sequential output maps can safely share the two retained scene
    // generations while keeping their view-projection histories separate.
    const auto remember_transform =
        [&](doc::NodeId source, const scene::Mat4& world) noexcept {
            if (source.value() != 0U
                && next_transforms_.size() < limits_.max_instances) {
                next_transforms_.push_back(PreviousTransform{source, world});
            }
        };
    for (const auto& instance : scene.instances) {
        remember_transform(instance.source, instance.world);
    }
    for (const auto& model : scene.models) {
        if (options.excluded_source != doc::NodeId{} && model.source_node == options.excluded_source) continue;
        remember_transform(model.motion_source.value() ? model.motion_source : model.source_node, model.world);
    }
    for (const auto& media : scene.media) {
        remember_transform(media.source_node, media.world);
    }
    std::sort(next_transforms_.begin(),next_transforms_.end(),
        [](const PreviousTransform& a,const PreviousTransform& b) { return a.source < b.source; });

    ViewHistoryState& view_history =
        view_histories_[options.history_slot];
    const bool can_advance_history = view_history.valid
        && view_history.previous_show_rate == scene.show_rate
        && view_history.previous_scene == scene.id
        && view_history.previous_show_frame
            != std::numeric_limits<core::u64>::max()
        && scene.show_frame == view_history.previous_show_frame + 1U;
    const bool can_hold_history = view_history.valid
        && view_history.previous_show_rate == scene.show_rate
        && view_history.previous_scene == scene.id
        && scene.show_frame == view_history.previous_show_frame;
    const auto transforms_for_show_frame =
        [&](core::u64 show_frame) noexcept
            -> const std::vector<PreviousTransform>* {
            if (!history_valid_ || previous_scene_ != scene.id) {
                return nullptr;
            }
            if (previous_show_frame_ == show_frame) {
                return &previous_transforms_;
            }
            if (motion_source_show_frame_ == show_frame) {
                return &motion_source_transforms_;
            }
            return nullptr;
        };
    const std::vector<PreviousTransform>* motion_transforms{};
    scene::Mat4 motion_view_projection =
        render_view.canvas_view_projection;
    bool advance_history{};
    bool hold_history{};
    switch (options.history) {
    case SceneHistoryAction::Automatic:
        if (can_hold_history) {
            motion_transforms = transforms_for_show_frame(
                view_history.motion_transform_show_frame);
            if (motion_transforms != nullptr) {
                motion_view_projection =
                    view_history.motion_source_view_projection;
                hold_history = true;
            }
        } else if (can_advance_history) {
            motion_transforms = transforms_for_show_frame(
                view_history.previous_show_frame);
            if (motion_transforms != nullptr) {
                motion_view_projection =
                    view_history.previous_view_projection;
                advance_history = true;
            }
        }
        break;
    case SceneHistoryAction::Initialize:
    case SceneHistoryAction::Reset:
        break;
    case SceneHistoryAction::Advance:
        // An explicit Advance is the facade's published contract. Refuse a
        // disagreement rather than recording reset velocity under that label.
        if (!can_advance_history) {
            return false;
        }
        motion_transforms = transforms_for_show_frame(
            view_history.previous_show_frame);
        if (motion_transforms == nullptr) {
            return false;
        }
        motion_view_projection = view_history.previous_view_projection;
        advance_history = true;
        break;
    case SceneHistoryAction::Hold:
        if (!can_hold_history) {
            return false;
        }
        motion_transforms = transforms_for_show_frame(
            view_history.motion_transform_show_frame);
        if (motion_transforms == nullptr) {
            return false;
        }
        motion_view_projection =
            view_history.motion_source_view_projection;
        hold_history = true;
        break;
    }
    const auto instance_record =
        [&](const scene::MeshInstance& instance,
            core::u32 first_effect,
            core::u32 effect_count,
            bool depth_only = false,
            DecodedPixelLayout media_layout = DecodedPixelLayout::Rgba8,
            bool has_normal_map = false,
            bool has_occlusion_map = false,
            bool normal_bc5 = false, bool coat_bc5 = false, doc::NodeId motion_source = {}) {
            const auto previous = motion_transforms == nullptr
                ? static_cast<const PreviousTransform*>(nullptr)
                : [&]() noexcept -> const PreviousTransform* {
                      const auto key = motion_source.value() ? motion_source : instance.source;
                      const auto found = std::lower_bound(
                          motion_transforms->begin(),
                          motion_transforms->end(), key,
                          [](const PreviousTransform& held,doc::NodeId wanted) noexcept {
                              return held.source < wanted;
                          });
                      return found == motion_transforms->end() || found->source != key
                          ? nullptr
                          : &*found;
                  }();
            const scene::Mat4* prior =
                previous == nullptr ? nullptr : &previous->world;
            auto record = to_gpu(
                instance, first_effect, effect_count, depth_only,
                media_layout, prior);
            for (const auto& animated:animated_instances_) if (animated.source==instance.source) {
                record.pose[0]=animated.offset; record.pose[1]=animated.count;
                record.pose[2]=animated.skin_base; record.pose[3]=animated.first_vertex;
                record.wind[0]=animated.wind.x; record.wind[1]=animated.wind.y; record.wind[2]=animated.wind.z;
                record.wind[3]=animated.minimum_y;
                record.previous_wind[0]=animated.previous_wind.x; record.previous_wind[1]=animated.previous_wind.y; record.previous_wind[2]=animated.previous_wind.z;
                record.previous_wind[3]=animated.inverse_height;
                break;
            }
            record.alpha[3] = options.reflection_pass ? -1.0F :
                (planar_active_ && instance.source == scene.environment.planar.receiver ? 1.0F : 0.0F);

            // A bound fallback is a descriptor-valid absence, not authored
            // material data. Zeroing these two strengths makes that absence
            // mathematically exact even though RGBA8 cannot encode 0.5 in the
            // normal map's X/Y channels exactly.
            if (!has_normal_map) {
                record.material_properties[0U] = 0.0F;
            }
            if (!has_occlusion_map) {
                record.material_properties[1U] = 0.0F;
            }
            record.material_properties[2U] = normal_bc5 ? 1.0F : 0.0F;
            record.material_properties[3U] = coat_bc5 ? 1.0F : 0.0F;
            return record;
        };
    core::u32 curve_luts_used{};

    core::u32 next_ordinal = 0U;
    const auto held_instances = [&]() noexcept {
        return static_cast<std::size_t>(stats_.instances)
            + composite_draws_.size() + depth_only_media_.size();
    };
    const std::array<const rhi::Texture*, 14U> fallback_material_textures{
        &material_fallback_, &material_fallback_, &normal_fallback_,
        &material_fallback_, &material_fallback_,
        &material_fallback_, &material_fallback_, &normal_fallback_,
        &material_fallback_, &material_fallback_, &material_fallback_,
        &material_fallback_, &material_fallback_, &normal_fallback_};
    const auto append_material_textures =
        [&](const std::array<const rhi::Texture*, 14U>& textures,
            const std::array<ModelSampler, 14U>& samplers) {
        std::array<rhi::SampledTexture, advanced_fragment_textures> bindings{};
        bind_material_textures(bindings, textures, samplers,
            scene.renderer_settings.texture_anisotropy, material_fallback_);
        const auto count = scene_linear() ? advanced_fragment_textures : basic_fragment_textures;
        batch_textures_.insert(batch_textures_.end(), bindings.begin(), bindings.begin() + count);
        if (scene_linear()) rsm_flux_textures_.push_back(bindings[0]);
    };
    const auto inline_range_for =
        [&](doc::NodeId source,
            const scene::EffectStack& effects,
            InlineProgramRange& range,
            const std::optional<SurfaceFillProgram>& fill = std::nullopt) {
            if (source.value() != 0U) {
                const auto found = std::find_if(
                    inline_program_cache_.begin(),
                    inline_program_cache_.end(),
                    [source](const InlineProgramCacheEntry& entry) noexcept {
                        return entry.source == source;
                    });
                if (found != inline_program_cache_.end()) {
                    range = found->range;
                    return true;
                }
            }

            CompiledInlineRange compiled{};
            if (!compile_inline_program(
                    effects,
                    scene.effect_settings,
                    limits_.max_inline_effect_ops,
                    limits_.max_inline_effect_parameter_vectors,
                    limits_.max_inline_curve_luts,
                    curve_luts_used,
                    inline_effect_op_scratch_,
                    inline_effect_parameter_scratch_,
                    compiled,
                    fill)) {
                return false;
            }
            range = InlineProgramRange{compiled.first, compiled.count};
            if (source.value() != 0U) {
                inline_program_cache_.push_back(
                    InlineProgramCacheEntry{source, range});
            }
            return true;
        };
    struct SiblingOrder final {
        std::optional<doc::NodeId> parent;
        core::i64 order{};
    };
    const auto queue_composite = [&](const MeshRange& range,
                                     const scene::MeshInstance& instance,
                                     ResolvedMediaFrame media,
                                     const std::array<const rhi::Texture*, 14U>&
                                         material_textures,
                                     float depth,
                                     doc::NodeId source,
                                     std::optional<core::u64> timeline_order =
                                         std::nullopt,
                                     std::optional<SiblingOrder> sibling_order =
                                         std::nullopt,
                                     std::array<ModelSampler, 14U> samplers = {},
                                     ModelBounds bounds = unit_bounds) {
        if (held_instances() >= limits_.max_instances) {
            ++stats_.dropped_instances;
            return false;
        }
        CompositeDraw draw{};
        draw.range = range;
        draw.instance = instance;
        draw.material_textures = material_textures;
        draw.samplers = samplers;
        draw.media = media;
        draw.blend = valid_blend(instance.style.blend);
        draw.depth = depth;
        draw.source_order = timeline_order.value_or(source.value());
        if (sibling_order.has_value()) {
            draw.sibling_ordered = true;
            draw.sibling_parent = sibling_order->parent;
            draw.sibling_order = sibling_order->order;
        }
        draw.bounds = bounds;
        draw.ordinal = next_ordinal;
        ++next_ordinal;
        composite_draws_.push_back(draw);
        return true;
    };

    const GeometryFrusta frusta{scene};
    animated_instances_.clear();
    pose_upload_.clear();
    const auto camera_visible = [&](const scene::Mat4& world, const ModelBounds& bounds) {
        // A free Stage or physical-wall projection can see outside the authored
        // canvas. Scene has no replicated wall planes, so explicit views retain
        // all geometry; LOD still uses the authored camera on every surface.
        return !scene.renderer_settings.frustum_culling || options.view.has_value()
            || frusta.visible(world, bounds);
    };
    const auto selected = [&](doc::NodeId source, bool model, bool visible, core::u32 lod) {
        geometry_selection_.push_back({source, model, visible, lod});
        if (visible) ++stats_.visible_geometry; else ++stats_.culled_geometry;
    };

    for (const auto& instance : scene.instances) {
        if (options.excluded_source != doc::NodeId{} && instance.source == options.excluded_source) continue;
        if (opacity(instance.style) <= 0.0F) {
            continue;
        }
        if (held_instances() >= limits_.max_instances) {
            ++stats_.dropped_instances;
            return false;
        }
        const bool visible = camera_visible(instance.world, {{-0.5F,-0.5F,-0.5F},{0.5F,0.5F,0.5F}});
        selected(instance.source, false, visible, 0U);
        const auto key = scene::normalized(instance.mesh);
        const MeshRange* const mesh_range = ensure_resident(key, commands);
        if (mesh_range == nullptr || mesh_range->index_count == 0U) {
            ++stats_.dropped_instances;
            continue;
        }

        auto instance_textures = fallback_material_textures;
        const std::array<std::string_view, 5U> texture_sources{
            instance.material.base_color_texture,
            instance.material.metallic_roughness_texture,
            instance.material.normal_texture,
            instance.material.occlusion_texture,
            instance.material.emissive_texture};
        const bool has_generator_texture = std::any_of(
            texture_sources.begin(), texture_sources.end(),
            [](std::string_view source) { return !source.empty(); });
        for (std::size_t texture_index = 0U;
             texture_index < texture_sources.size(); ++texture_index) {
            if (texture_sources[texture_index].empty()) continue;
            const auto texture = media_cache.resolve(
                *device_, commands,
                std::string{texture_sources[texture_index]}, 0U, {},
                instance.source.value());
            if (texture) instance_textures[texture_index] = texture.primary;
        }

        // Occluders join the batched depth-writing pass whatever their own
        // compositing style says, because the only thing wanted from them is
        // the depth their silhouette would have written.
        const bool instance_depth_only = options.depth_only(instance.source);
        if (!instance_depth_only && (has_generator_texture
            || needs_compositing(instance.style)
            || instance.material.transmission > 0.0F || instance.material.alpha_mode == scene::MaterialAlphaMode::Blend)) {
            if (!visible) continue;
            queue_composite(
                *mesh_range,
                instance,
                ResolvedMediaFrame{},
                instance_textures,
                camera_depth(instance.world, scene::Vec3{}, render_view.eye,
                             render_view.forward),
                instance.source,
                std::nullopt,
                SiblingOrder{instance.parent_group, instance.group_order},
                {},
                primitive_bounds(key.primitive));
            continue;
        }

        InlineProgramRange effect_range{};
        if (!instance_depth_only
            && !inline_range_for(
                instance.source, instance.style.effects, effect_range,
                surface_fill_program(instance))) {
            return false;
        }
        append(
            instance_scratch_,
            instance_record(
                instance,
                effect_range.first,
                effect_range.count,
                instance_depth_only));
        expand_directional_shadow_bounds(
            shadow_bounds, instance.world,
            scene::Vec3{-0.5F, -0.5F, -0.5F},
            scene::Vec3{0.5F, 0.5F, 0.5F});
        opaque_submissions_.push_back({*mesh_range, fallback_material_textures,
            {}, key, instance.source, 0U, stats_.instances, visible, {}, instance.world,
            ModelBounds{{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}}});
        ++stats_.instances;
        if (instance_depth_only) {
            ++stats_.depth_only_instances;
        }
    }
    // Imported models upload once per asset while retaining a batch per source
    // material part. Their transformed asset-bounds centre supplies one stable
    // sort depth for every part of an instance.
    for (const auto& model : scene.models) {
        if (options.excluded_source != doc::NodeId{} && model.source_node == options.excluded_source) continue;
        if (opacity(model.style) <= 0.0F) {
            continue;
        }
        const std::string& model_source = model.source;
        const ResidentModel* const resident = model_source.empty()
            ? nullptr
            : ensure_model_resident(
                model_source, commands, model_publication_fence);
        if (resident == nullptr) {
            ++stats_.dropped_instances;
            // A cold model can be omitted only while pending uploads - its
            // textures, or its geometry still streaming into the arena - keep
            // assets unready, so the presenter submits this recording and
            // holds the picture. Ready textures must never turn a geometry
            // refusal into a successfully presented hole.
            if (texture_streamer_ && stats_.texture_streaming.state !=
                    TextureStreamState::Loading && !streaming_geometry(model_source)) return false;
            continue;
        }
        if (texture_streamer_ &&
            (!texture_streamer_->contains(model_source, resident->generation, *resident->decoded_asset) ||
             stats_.model_integration_refusals != 0U)) return false;
        if (resident->parts.empty()) {
            ++stats_.dropped_instances;
            continue;
        }
        const auto& asset=*resident->decoded_asset;
        const bool deforming=!asset.pose_bindings.empty() || model.animation.wind!=scene::Vec3{};
        ModelBounds animated_bounds=resident->bounds;
        if (deforming) {
            const auto count=static_cast<core::u32>(asset.pose_bindings.size());
            const auto offset=static_cast<core::u32>(pose_upload_.size()/sizeof(GpuPose));
            if (animated_instances_.size()==animated_instances_.capacity() || count>max_frame_pose_bindings-offset) {
                stats_.assets.ready=false; ++stats_.dropped_instances;
                static_cast<void>(core::report_idle_once("Model animation","Frame joint palette capacity exceeded")); return false;
            }
            const auto previous_frame=advance_history ? view_history.previous_show_frame
                : hold_history ? view_history.motion_transform_show_frame : scene.show_frame;
            if (!evaluate_model_pose(asset,model.animation,scene.show_frame,scene.show_rate,*pose_scratch_,current_pose_)
                || !evaluate_model_pose(asset,model.animation,previous_frame,scene.show_rate,*pose_scratch_,previous_pose_)) {
                stats_.assets.ready=false;
                static_cast<void>(core::report_idle_once("Model animation","Invalid animation clip or nonfinite pose")); return false;
            }
            for (core::u32 j=0; j<count; ++j) append(pose_upload_,GpuPose{current_pose_[j],previous_pose_[j]});
            const auto wind=model_wind(model.animation,scene.show_frame);
            const auto prior_wind=model_wind(model.animation,previous_frame);
            const float height=asset.bounds.maximum.y-asset.bounds.minimum.y;
            animated_instances_.push_back({model.source_node,offset,count,wind,prior_wind,asset.bounds.minimum.y,height>1e-8F?1.0F/height:0.0F,
                resident->geometry->span.first_skin,resident->geometry->span.first_vertex});
            // Wind is applied in baked model space before skinning; expand the
            // bind bounds first, then transform every corner by every binding.
            auto expanded=asset.bounds;
            const scene::Vec3 extent{std::abs(wind.x),std::abs(wind.y),std::abs(wind.z)};
            expanded.minimum=expanded.minimum-extent; expanded.maximum=expanded.maximum+extent;
            animated_bounds=model_pose_bounds(asset,std::span{current_pose_}.first(count),wind);
            // Conservative skin motion is independent of the view. Dynamic
            // models keep LOD zero; external animated LODs fail ingest.
            if (count && wind!=scene::Vec3{}) {
                for (core::u32 j=0;j<count;++j) for (unsigned corner=0;corner<8;++corner) {
                    const auto p=scene::transform_point(current_pose_[j],{(corner&1U)?expanded.maximum.x:expanded.minimum.x,
                        (corner&2U)?expanded.maximum.y:expanded.minimum.y,(corner&4U)?expanded.maximum.z:expanded.minimum.z});
                    animated_bounds.minimum={std::min(animated_bounds.minimum.x,p.x),std::min(animated_bounds.minimum.y,p.y),std::min(animated_bounds.minimum.z,p.z)};
                    animated_bounds.maximum={std::max(animated_bounds.maximum.x,p.x),std::max(animated_bounds.maximum.y,p.y),std::max(animated_bounds.maximum.z,p.z)};
                }
            }
        }
        const bool visible = camera_visible(model.world, animated_bounds);
        const auto& levels = resident->decoded_asset->lods;
        // An isolated part is named among the full-detail parts, so an
        // isolating view stays at full detail.
        const core::u32 lod = deforming || render_view.model_part.has_value()
            ? 0U : select_model_lod(scene, model.world, resident->bounds, levels);
        selected(model.source_node, true, visible, lod);
        if (levels.size() > 1U) ++stats_.lod_models;
        if (lod > 0U) ++stats_.lod_reductions;
        const bool model_depth_only = options.depth_only(model.source_node);
        const auto local_center =
            (animated_bounds.minimum + animated_bounds.maximum) * 0.5F;
        const float depth = camera_depth(model.world, local_center,
                                         render_view.eye,
                                         render_view.forward);
        const auto texture_for = [&](std::optional<core::u32> index)
            -> const rhi::Texture* {
            if (texture_streamer_ && index) {
                if (const auto* texture = texture_streamer_->find(model_source, resident->generation, *index))
                    return texture;
                return &material_fallback_;
            }
            if (index && *index < resident->textures.size()) {
                const auto& texture = resident->textures[*index];
                if (texture) {
                    return &texture;
                }
            }
            return &material_fallback_;
        };
        const ModelLod level = levels.empty()
            ? ModelLod{0U, static_cast<core::u32>(resident->parts.size()), 0.0F}
            : levels[lod];
        // Fit/count the base caster set. A LOD with fewer material runs must
        // not move a cascade or change its caster count.
        if (model_depth_only || !needs_compositing(model.style)) {
            const core::u32 base_parts = levels.empty()
                ? static_cast<core::u32>(resident->parts.size()) : levels.front().part_count;
            for (core::u32 i = 0U; i < base_parts; ++i) {
                expand_directional_shadow_bounds(shadow_bounds, model.world,
                    animated_bounds.minimum, animated_bounds.maximum);
            }
        }
        for (core::u32 part_index = level.first_part;
             part_index < level.first_part + level.part_count; ++part_index) {
            if (render_view.model_part.has_value()
                && part_index - level.first_part != *render_view.model_part) {
                continue;
            }
            const auto& part = resident->parts[part_index];
            if (held_instances() >= limits_.max_instances) {
                ++stats_.dropped_instances;
                return false;
            }
            if (part.range.index_count == 0U) {
                ++stats_.dropped_instances;
                continue;
            }

            scene::MeshInstance proxy{};
            proxy.world = model.world;
            proxy.normal = model.normal;
            // Source-assigned material factors are authoritative. The authored
            // model material is used only for an unassigned primitive; an
            // explicit future override therefore needs its own state bit rather
            // than making today's default gray erase imported appearance.
            proxy.material = scene::model_part_material(model,
                part.has_material ? part.material : model.material);
            proxy.source = model.source_node;
            proxy.style = model.style;
            const std::array<const rhi::Texture*, 14U> material_textures{
                texture_for(part.base_color_texture),
                texture_for(part.metallic_roughness_texture),
                part.normal_texture.has_value()
                    ? texture_for(part.normal_texture)
                    : &normal_fallback_,
                texture_for(part.occlusion_texture),
                texture_for(part.emissive_texture),
                texture_for(part.clearcoat_texture),
                texture_for(part.clearcoat_roughness_texture),
                part.clearcoat_normal_texture ? texture_for(part.clearcoat_normal_texture) : &normal_fallback_,
                texture_for(part.sheen_color_texture),
                texture_for(part.sheen_roughness_texture),
                texture_for(part.transmission_texture),
                texture_for(part.specular_texture),
                texture_for(part.specular_color_texture),
                part.anisotropy_texture ? texture_for(part.anisotropy_texture) : &normal_fallback_};
            if (!part.clearcoat_normal_texture) proxy.material.clearcoat_normal_scale = 0.0F;
            const bool has_normal_map =
                part.normal_texture.has_value()
                && material_textures[2U] != &material_fallback_
                && material_textures[2U] != &normal_fallback_;
            const bool has_occlusion_map =
                part.occlusion_texture.has_value()
                && material_textures[3U] != &material_fallback_;

            if (!model_depth_only && (needs_compositing(model.style)
                || proxy.material.transmission > 0.0F || proxy.material.alpha_mode == scene::MaterialAlphaMode::Blend)) {
                if (!visible) continue;
                queue_composite(
                    part.range,
                    proxy,
                    ResolvedMediaFrame{},
                    material_textures,
                    depth,
                    model.source_node,
                    std::nullopt,
                    SiblingOrder{model.parent_group, model.group_order}, part.samplers,
                    animated_bounds);
                continue;
            }

            const auto slot = stats_.instances;
            InlineProgramRange effect_range{};
            if (!model_depth_only
                && !inline_range_for(
                    model.source_node, model.style.effects, effect_range)) {
                return false;
            }
            append(
                instance_scratch_,
                instance_record(
                    proxy,
                    effect_range.first,
                    effect_range.count,
                    model_depth_only,
                    DecodedPixelLayout::Rgba8,
                    has_normal_map,
                    has_occlusion_map,
                    material_textures[2U]->desc().format == rhi::TextureFormat::Bc5Unorm,
                    material_textures[7U]->desc().format == rhi::TextureFormat::Bc5Unorm, model.motion_source));
            opaque_submissions_.push_back({part.range, material_textures,
                model.source, {}, model.source_node, part_index, slot, visible, part.samplers,
                model.world, animated_bounds});
            ++stats_.instances;
            if (model_depth_only) {
                ++stats_.depth_only_instances;
            }
        }
    }

    std::sort(opaque_submissions_.begin(), opaque_submissions_.end(),
        [](const OpaqueSubmission& a, const OpaqueSubmission& b) {
            if (a.asset != b.asset) return a.asset < b.asset;
            if (a.mesh != b.mesh) return a.mesh < b.mesh;
            if (a.part != b.part) return a.part < b.part;
            if (a.source != b.source) return a.source < b.source;
            return a.original_slot < b.original_slot;
        });
    for (const auto& submission : opaque_submissions_) {
        const auto offset = static_cast<std::size_t>(submission.original_slot) * sizeof(GpuInstance);
        reordered_instances_.insert(reordered_instances_.end(),
            instance_scratch_.data() + offset, instance_scratch_.data() + offset + sizeof(GpuInstance));
        opaque_visibility_.push_back(submission.visible ? 1U : 0U);
    }
    instance_scratch_.swap(reordered_instances_);
    // Batched opaque geometry is drawn indexed where the device can, so a
    // vertex shared by several triangles is shaded once rather than once per
    // triangle. Draws of the same triangles produce the same picture either way.
    const bool indexed = device_->capabilities().indexed_geometry;
    const auto draw_batch = [&](const MeshRange& range, core::u32 instances) noexcept {
        return rhi::DrawBatch{range.index_count, instances, indexed ? range.first_index : 0U};
    };
    const auto batch_constants = [&](const MeshRange& range, core::u32 first_instance) noexcept {
        return BatchConstants{range.first_index, first_instance, indexed ? 1U : 0U, 0U};
    };
    for (core::u32 first = 0U; first < opaque_submissions_.size();) {
        const auto& head = opaque_submissions_[first];
        core::u32 end = first + 1U;
        if (scene.renderer_settings.geometry_instancing) {
            while (end < opaque_submissions_.size()) {
                const auto& next = opaque_submissions_[end];
                if (head.asset != next.asset || head.mesh != next.mesh || head.part != next.part
                    || head.range.first_index != next.range.first_index
                    || head.range.index_count != next.range.index_count || head.textures != next.textures
                    || head.samplers != next.samplers) break;
                ++end;
            }
        }
        batches_.push_back(draw_batch(head.range, end - first));
        append(batch_uniforms_, batch_constants(head.range, first));
        const auto texture_begin = batch_textures_.size();
        append_material_textures(head.textures, head.samplers);
        stats_.triangles += (head.range.index_count / 3U) * (end - first);
        for (core::u32 cursor = first; cursor < end;) {
            if (opaque_visibility_[cursor] == 0U) { ++cursor; continue; }
            const core::u32 begin = cursor++;
            while (cursor < end && opaque_visibility_[cursor] != 0U) ++cursor;
            camera_batches_.push_back(draw_batch(head.range, cursor - begin));
            append(camera_batch_uniforms_, batch_constants(head.range, begin));
            camera_batch_textures_.insert(camera_batch_textures_.end(),
                batch_textures_.begin() + static_cast<std::ptrdiff_t>(texture_begin), batch_textures_.end());
            if (scene_linear()) camera_coverage_textures_.push_back(batch_textures_[texture_begin]);
            stats_.camera_instances += cursor - begin;
            stats_.camera_triangles += (head.range.index_count / 3U) * (cursor - begin);
            if (cursor - begin > 1U) ++stats_.instanced_batches;
        }
        first = end;
    }
    std::sort(geometry_selection_.begin(), geometry_selection_.end(),
        [](const GeometrySelection& a, const GeometrySelection& b) {
            if (a.model != b.model) return a.model < b.model;
            return a.source < b.source;
        });

    // A media frame can contain alpha even when its layer opacity is one, so it
    // always joins the compositing queue. That is the only honest treatment
    // without asset metadata guaranteeing every texel is opaque.
    for (const auto& clip : scene.media) {
        if (opacity(clip.style) <= 0.0F) {
            continue;
        }
        if (clip.mesh.primitive == scene::Primitive::Quad
            && media_quad_outside_view(
                clip.world, render_view.matrices.view_projection,
                clip.style.effects, scene.effect_settings)) {
            ++stats_.culled_media;
            continue;
        }
        if (held_instances() >= limits_.max_instances) {
            ++stats_.dropped_instances;
            return false;
        }
        const auto key = scene::normalized(clip.mesh);
        const MeshRange* const mesh_range = ensure_resident(key, commands);
        if (mesh_range == nullptr || mesh_range->index_count == 0U) {
            continue;
        }
        // A half-resolution clip decodes as a source of its own; a full one
        // keeps its path and allocates nothing here.
        const std::string half_source =
            clip.resolution == scene::MediaResolution::Half
            ? media_decode_source(clip) : std::string{};
        const std::string& media_source =
            half_source.empty() ? clip.source : half_source;
        ResolvedMediaFrame media_frame = clip.offline || media_source.empty()
            ? ResolvedMediaFrame{} : media_cache.resolve(
            *device_, commands, media_source, clip.frame_index,
            MediaLoopWindow{clip.loop_first, clip.loop_count},
            clip.source_node.value(), clip.asset.value());
        if (!media_frame) {
            media_frame = ResolvedMediaFrame{&offline_media_};
        }

        scene::MeshInstance proxy{};
        proxy.mesh = clip.mesh;
        proxy.world = clip.world;
        proxy.normal = clip.normal;
        proxy.source = clip.source_node;
        proxy.style = clip.style;
        if (options.depth_only(clip.source_node)) {
            CompositeDraw draw{};
            draw.range = *mesh_range;
            draw.instance = proxy;
            draw.media = media_frame;
            draw.ordinal = next_ordinal;
            ++next_ordinal;
            depth_only_media_.push_back(draw);
            continue;
        }
        queue_composite(
            *mesh_range,
            proxy,
            media_frame,
            fallback_material_textures,
            camera_depth(clip.world, scene::Vec3{}, render_view.eye,
                         render_view.forward),
            clip.source_node,
            clip.compositing_order,
            clip.timeline_lane == doc::NodeId{}
                ? std::optional<SiblingOrder>{SiblingOrder{
                      clip.parent_group, clip.group_order}}
                : std::nullopt,
            {},
            primitive_bounds(key.primitive));
    }
    // Clips that are about to cut in. Named after the drawing pass, so a frame
    // the show needs now is always queued ahead of a frame it needs shortly.
    for (const auto& upcoming : scene.media_prewarm) {
        if (upcoming.source.empty()) {
            continue;
        }
        media_cache.prewarm(
            *device_, commands,
            media_decode_source(upcoming.source, upcoming.resolution),
            upcoming.frame_index,
            MediaLoopWindow{upcoming.loop_first, upcoming.loop_count},
            upcoming.source_node.value());
    }
    // Back-to-front for colour correctness, then the layer's own order and
    // the collection ordinal for a total order. The latter two make equal-depth
    // layers deterministic even though procedural instances arrived grouped
    // by mesh rather than by document order. Media clips carry their lane and
    // clip order in place of a node id, so clips sharing one surface - equal
    // depth to the bit - stack the way the timeline stacks them, while clips on
    // surfaces at different depths occlude by where they actually are. Depth
    // has to stay the primary key: these draws do not write depth, so drawing
    // order is the only thing deciding which one survives.
    std::sort(
        composite_draws_.begin(),
        composite_draws_.end(),
        [](const CompositeDraw& left, const CompositeDraw& right) noexcept {
            if (left.depth != right.depth) {
                return left.depth > right.depth;
            }
            if (left.sibling_ordered && right.sibling_ordered
                && left.sibling_parent == right.sibling_parent
                && left.sibling_order != right.sibling_order) {
                return left.sibling_order < right.sibling_order;
            }
            if (left.source_order != right.source_order) {
                return left.source_order < right.source_order;
            }
            return left.ordinal < right.ordinal;
        });
    // Centres alone misorder a layer standing in front of a large surface
    // whose centre is nearer the camera than its own.
    order_composites_by_planes(render_view);
    // Depth-only clips are recorded before every destination-reading draw, so
    // their instance slots are claimed first and in document order.
    for (auto& draw : depth_only_media_) {
        draw.first_instance = stats_.instances;
        append(
            instance_scratch_,
            instance_record(
                draw.instance, 0U, 0U, true, draw.media.layout));
        stats_.triangles += draw.range.index_count / 3U;
        ++stats_.instances;
        ++stats_.depth_only_instances;
    }
    for (auto& draw : composite_draws_) {
        draw.first_instance = stats_.instances;
        InlineProgramRange effect_range{};
        if (!inline_range_for(
                draw.instance.source,
                draw.instance.style.effects,
                effect_range,
                surface_fill_program(draw.instance))) {
            return false;
        }
        draw.first_effect = effect_range.first;
        draw.effect_count = effect_range.count;
        append(
            instance_scratch_,
            instance_record(
                draw.instance, draw.first_effect, draw.effect_count, false,
                draw.media.layout,
                !draw.media
                    && draw.material_textures[2U] != &normal_fallback_,
                !draw.media
                    && draw.material_textures[3U] != &material_fallback_,
                !draw.media && draw.material_textures[2U]->desc().format == rhi::TextureFormat::Bc5Unorm,
                !draw.media && draw.material_textures[7U]->desc().format == rhi::TextureFormat::Bc5Unorm));
        stats_.triangles += draw.range.index_count / 3U;
        ++stats_.instances;
        if (draw.media) {
            if (draw.media.primary == &offline_media_) ++stats_.offline_media;
            else ++stats_.media;
        }
    }

    stats_.batches = static_cast<core::u32>(
        camera_batches_.size() + depth_only_media_.size() + composite_draws_.size());
    const auto ready_models = static_cast<std::size_t>(std::count_if(
        resident_models_.begin(), resident_models_.end(),
        [](const auto& model) noexcept {
            return model.second.generation != 0U;
        })) + static_cast<std::size_t>(std::count_if(
            pending_model_integrations_.begin(),
            pending_model_integrations_.end(),
            [](const PendingModelIntegration& pending) noexcept {
                return pending.active != nullptr
                    && pending.active->generation == 0U
                    && pending.candidate.generation != 0U;
            }));
    stats_.meshes_resident = static_cast<core::u32>(
        resident_.size() + ready_models);
    stats_.geometry_capacity_bytes = geometry_->arena().capacity_bytes();
    stats_.geometry_used_bytes = geometry_->arena().used_bytes();

    const auto push_light = [&](const float vector[3U],
                                GpuLightKind kind,
                                scene::Vec3 color,
                                float intensity,
                                float range,
                                const scene::PointLight* spot = nullptr) {
        const auto held = static_cast<core::u32>(light_scratch_.size() / sizeof(GpuLight));
        if (held >= limits_.max_lights) {
            ++stats_.dropped_lights;
            return;
        }
        GpuLight record{};
        record.vector[0U] = vector[0U];
        record.vector[1U] = vector[1U];
        record.vector[2U] = vector[2U];
        record.kind = kind;
        record.color_intensity[0U] = color.x;
        record.color_intensity[1U] = color.y;
        record.color_intensity[2U] = color.z;
        record.color_intensity[3U] = intensity;
        record.params[0U] = range;
        if (spot != nullptr) {
            const auto slot = ies_slot_for(*spot);
            record.ies[3] = slot < 0 ? static_cast<float>(slot) : static_cast<float>(slot) * static_cast<float>(ies_horizontal_samples);
            if (slot >= 0) {
                const auto tangent = ies_tangent(*spot);
                const auto axis = scene::normalize(spot->direction, {0, -1, 0});
                record.ies[0] = tangent.x; record.ies[1] = tangent.y; record.ies[2] = tangent.z;
                record.ies_axis[0] = axis.x; record.ies_axis[1] = axis.y; record.ies_axis[2] = axis.z;
            }
        }
        if (spot != nullptr && scene::area_light(spot->kind)) {
            const auto axis = scene::normalize(spot->direction, {0, -1, 0});
            const auto u = scene::area_light_tangent(*spot);
            const auto v = scene::cross(axis, u);
            record.spot[0] = axis.x; record.spot[1] = axis.y; record.spot[2] = axis.z;
            record.spot[3] = spot->two_sided ? 1.0F : 0.0F;
            record.area_u[0] = u.x; record.area_u[1] = u.y; record.area_u[2] = u.z;
            record.area_u[3] = spot->kind == scene::LocalLightKind::Sphere ? spot->radius : spot->width * 0.5F;
            record.area_v[0] = v.x; record.area_v[1] = v.y; record.area_v[2] = v.z;
            record.area_v[3] = spot->height * 0.5F;
        }
        if (spot != nullptr && spot->kind == scene::LocalLightKind::Spot) {
            // Deterministic trig, because two nodes comparing a dot product
            // against differently-rounded cosines would put the edge of the
            // cone in different places on the same wall.
            record.params[1U] =
                scene::deterministic::cos_turns(spot->outer_cone);
            record.params[2U] =
                scene::deterministic::cos_turns(spot->inner_cone);
            const scene::Vec3 axis = scene::normalize(
                spot->direction, scene::Vec3{0.0F, -1.0F, 0.0F});
            record.spot[0U] = axis.x;
            record.spot[1U] = axis.y;
            record.spot[2U] = axis.z;
            record.spot[3U] = spot->unit == scene::LightUnit::Unitless ? 0.0F : 1.0F;
            // Which row of the shadow table describes this light, or -1. An
            // index rather than a flag, because the table only holds the
            // lights that got a tile and is far shorter than the light list.
            record.params[3U] = -1.0F;
            for (std::size_t tile = 0U;
                 tile < shadow_atlas_plan_.tiles.size(); ++tile) {
                if (shadow_atlas_plan_.tiles[tile].light == spot->source) {
                    record.params[3U] = static_cast<float>(tile);
                    break;
                }
            }
        } else {
            record.params[3U] = -1.0F;
        }
        append(light_scratch_, record);
    };

    // The shadow atlas partition for this frame, decided before any light is
    // packed so each one can carry its own tile index.
    //
    // Derived from the authored lights alone - no camera enters it - so every
    // node reaches the same assignment for the same scene. See shadow_atlas.hpp
    // for why a distance-ranked policy would be a seam.
    shadow_atlas_plan_ =
        plan_shadow_atlas(scene.point_lights, shadow_atlas_limits_);
    stats_.shadow_tiles =
        static_cast<core::u32>(shadow_atlas_plan_.tiles.size());
    stats_.shadow_lights_refused = shadow_atlas_plan_.refused;

    // One row per tile, built here rather than while rasterizing, so the
    // matrix a tile is drawn with and the matrix the lit pass reads back are
    // the same value and cannot drift.
    light_shadow_scratch_.clear();
    for (const ShadowTile& tile : shadow_atlas_plan_.tiles) {
        const scene::PointLight* source = nullptr;
        for (const auto& candidate : scene.point_lights) {
            if (candidate.source == tile.light) {
                source = &candidate;
                break;
            }
        }
        GpuLightShadow record{};
        if (source != nullptr) {
            // Near plane from the light's own range rather than a constant: a
            // practical two metres from a wall and one lighting a whole set
            // need very different depth precision, and the range is the only
            // authored quantity that says which this is.
            const float far_plane = std::max(source->range, 0.01F);
            const float near_plane = std::max(far_plane * 0.005F, 0.01F);
            const scene::ViewMatrices light_view = scene::spot_light_view(
                source->position, source->direction, source->outer_cone,
                near_plane, far_plane);
            for (core::u32 index = 0U; index < 16U; ++index) {
                record.view_projection[index] =
                    light_view.view_projection.m[index];
            }
        }
        record.tile[0U] = static_cast<float>(tile.x);
        record.tile[1U] = static_cast<float>(tile.y);
        record.tile[2U] = static_cast<float>(tile.extent);
        record.tile[3U] = 1.0F / static_cast<float>(tile.extent);
        append(light_shadow_scratch_, record);
    }
    if (light_shadow_scratch_.empty()) {
        // A scene with no casting spot still has to leave the buffer with an
        // address, because the lit pipeline binds it whatever the scene holds.
        // An empty upload leaves it with none and the lit draw is refused -
        // which is a completely black frame for a scene that never asked for a
        // shadow in the first place.
        append(light_shadow_scratch_, GpuLightShadow{});
    }

    for (const auto& light : scene.directional_lights) {
        const float vector[3U]{light.direction.x, light.direction.y, light.direction.z};
        const auto emission = scene::light_emission(light, scene.renderer_settings.photometric_scale);
        // The finite solar disc is already in the physical sky's specular IBL.
        // Reserve the unused directional range lane to avoid adding a second,
        // point-source specular sun on top of it. Diffuse keeps cascade shadows.
        const bool physical_sun = physical_environment_ && &light == &scene.directional_lights.front();
        push_light(vector, GpuLightKind::Directional, emission.color, emission.intensity, physical_sun ? -1.0F : 0.0F);
    }
    for (const auto& light : scene.point_lights) {
        const float vector[3U]{light.position.x, light.position.y, light.position.z};
        const auto slot = ies_slot_for(light);
        const auto emission = ies_light_emission(light, scene.renderer_settings.photometric_scale,
            slot >= 0 ? ies_profiles_[static_cast<std::size_t>(slot)].get() : nullptr);
        const auto kind = light.kind == scene::LocalLightKind::Spot ? GpuLightKind::Spot
            : light.kind == scene::LocalLightKind::Rectangle ? GpuLightKind::Rectangle
            : light.kind == scene::LocalLightKind::Sphere ? GpuLightKind::Sphere : GpuLightKind::Point;
        push_light(vector, kind,
                   emission.color, emission.intensity, light.range, &light);
    }


    // The shadow table only exists on the Advanced path, which is the only
    // one that binds it. Basic has no atlas, no table and no buffer, and
    // uploading into a resource it never allocated fails the whole frame.
    if (light_shadow_buffer_
        && !commands.upload(light_shadow_buffer_, light_shadow_scratch_)) {
        return false;
    }
    if (!commands.upload(pose_buffer_,pose_upload_)
        || !commands.upload(instance_buffer_, instance_scratch_)
        || !commands.upload(light_buffer_, light_scratch_)
        || !commands.upload(
            inline_effect_op_buffer_, inline_effect_op_scratch_)
        || !commands.upload(
            inline_effect_parameter_buffer_,
            inline_effect_parameter_scratch_)) {
        return false;
    }
    stats_.inline_effect_ops = static_cast<core::u32>(
        inline_effect_op_scratch_.size() / sizeof(GpuInlineEffectOp));
    stats_.upload_bytes += pose_upload_.size() + instance_scratch_.size() + light_scratch_.size()
        + inline_effect_op_scratch_.size()
        + inline_effect_parameter_scratch_.size();

    // Rasterize the same full-canvas triangles on every node. Only the
    // viewport origin changes for a slice; clipping and interpolation retain
    // their common projection, including with sharp anisotropic highlights.
    rhi::RasterViewport raster_viewport{};
    if (!options.view.has_value() || options.full_canvas_view) {
        const float scale_x = static_cast<float>(color_.width()) / static_cast<float>(slice.width);
        const float scale_y = static_cast<float>(color_.height()) / static_cast<float>(slice.height);
        raster_viewport = {-static_cast<float>(slice.x) * scale_x,
            -static_cast<float>(slice.y) * scale_y,
            static_cast<float>(scene.canvas.width) * scale_x,
            static_cast<float>(scene.canvas.height) * scale_y};
        // The authored canvas may be 65,535 pixels wide, beyond the native
        // viewport bound. Keep rendering its small node targets with their
        // slice projection; lighting still reconstructs in canvas coordinates.
        if (!raster_viewport.valid()) raster_viewport = {};
    }
    ViewConstants view_constants{};
    view_constants.motion_viewport[0]=raster_viewport.empty()?0.0F:raster_viewport.x;
    view_constants.motion_viewport[1]=raster_viewport.empty()?0.0F:raster_viewport.y;
    view_constants.motion_viewport[2]=1.0F/(raster_viewport.empty()?static_cast<float>(color_.width()):raster_viewport.width);
    view_constants.motion_viewport[3]=1.0F/(raster_viewport.empty()?static_cast<float>(color_.height()):raster_viewport.height);
    view_constants.temporal_parameters[0U] = advance_history
        || (hold_history && view_history.motion_transform_show_frame != scene.show_frame)
        ? 1.0F : 0.0F;
    for (core::u32 index = 0U; index < 16U; ++index) {
        view_constants.view_projection[index] = raster_viewport.empty()
            ? render_view.matrices.view_projection.m[index]
            : render_view.canvas_view_projection.m[index];
        view_constants.previous_view_projection[index] =
            motion_view_projection.m[index];
        view_constants.canvas_view_projection[index] =
            render_view.canvas_view_projection.m[index];
    }

    // Directional shadow cascades. The authored count is replicated policy;
    // the last cascade is always the full-scene fit, so one cascade is exactly
    // the single whole-scene map this renderer had before cascades existed and
    // the reflective-shadow-map pass keeps its unchanged projection.
    const scene::RendererSettings renderer_settings =
        scene::clamped(scene.renderer_settings);
    std::array<scene::Mat4, scene::max_shadow_cascades>
        cascade_view_projection{};
    for (scene::Mat4& matrix : cascade_view_projection) {
        matrix = scene::identity();
    }
    std::array<float, scene::max_shadow_cascades> cascade_texel_depth{};
    scene::Mat4 shadow_inverse_view_projection = scene::identity();
    core::u32 cascade_count = 0U;
    if (mode_ == SceneRendererMode::AdvancedLinear
        && !scene.directional_lights.empty()
        && renderer_settings.shadow_cascades != 0U
        && !cascade_homes_.empty()) {
        const core::u32 requested = std::min(
            renderer_settings.shadow_cascades,
            static_cast<core::u32>(cascade_homes_.size()));
        // The far cascade is fitted to every caster, exactly as the single map
        // was, so it always covers the scene however the camera is pointed.
        if (directional_shadow_matrix(
                shadow_bounds, scene.directional_lights.front(),
                limits_.directional_shadow_resolution,
                cascade_view_projection[requested - 1U],
                shadow_inverse_view_projection,
                cascade_texel_depth[requested - 1U])) {
            cascade_count = requested;
            // Nearer cascades subdivide the authored full-canvas camera, never
            // this node's slice, so two nodes place the same world point in the
            // same cascade and read the same texels for it.
            const scene::FrustumWindow window = scene::window_for_slice(
                scene.camera, scene.canvas, scene::whole(scene.canvas));
            const float near_distance =
                std::max(window.near_plane, 0.0001F);
            const float far_distance = cascade_far_distance(
                scene.camera, shadow_bounds, near_distance,
                window.far_plane);
            for (core::u32 cascade = 0U; cascade + 1U < requested;
                 ++cascade) {
                CascadeVolume volume{};
                if (!camera_cascade_volume(
                        scene.camera, window,
                        cascade_split(near_distance, far_distance, cascade,
                                      requested),
                        cascade_split(near_distance, far_distance,
                                      cascade + 1U, requested),
                        volume)
                    || !cascade_shadow_matrix(
                        shadow_bounds, volume,
                        scene.directional_lights.front(),
                        limits_.directional_shadow_resolution,
                        cascade_view_projection[cascade],
                        cascade_texel_depth[cascade])) {
                    // A cascade that cannot be fitted is not silently skipped:
                    // dropping it would leave a hole a neighbouring node might
                    // not have. Fall back to the full-scene map alone, which
                    // every node derives identically.
                    cascade_view_projection[0U] =
                        cascade_view_projection[requested - 1U];
                    cascade_texel_depth[0U] =
                        cascade_texel_depth[requested - 1U];
                    cascade_count = 1U;
                    break;
                }
            }
        }
    }
    const bool directional_shadow_enabled = cascade_count != 0U;
    const bool directional_gi_enabled = directional_shadow_enabled
        && renderer_settings.gi_strength > 0.0F
        && !scene.environment.baked.enabled;
    prepare_particles(commands, scene, options);
    stats_.assets.ready = stats_.assets.ready && stats_.planar_refusals == 0 && (options.shared_planar || !planar_active_ || planar_renderer_->stats().assets.ready) && stats_.baked_ready && stats_.models_pending == 0 && stats_.models_failed == 0 && stats_.models_stale == 0 && stats_.model_integration_refusals == 0 && stats_.texture_streaming.state == TextureStreamState::Ready;
    stats_.directional_shadow_rendered = directional_shadow_enabled;
    stats_.directional_shadow_cascades = cascade_count;
    stats_.directional_shadow_fit = cascade_view_projection;
    stats_.directional_shadow_casters = directional_shadow_enabled
        ? shadow_bounds.casters
        : 0U;
    stats_.directional_gi_rendered = directional_gi_enabled;

    SceneConstants scene_constants{};
    view_constants.clip_plane[0] = options.clip_plane.x; view_constants.clip_plane[1] = options.clip_plane.y;
    view_constants.clip_plane[2] = options.clip_plane.z; view_constants.clip_plane[3] = options.clip_plane.w;
    for (std::size_t i = 0; i < active_reflections_.size(); ++i) {
        const auto& p = active_reflections_[i]; if (!p.enabled) continue;
        scene_constants.reflection_position[i][0] = p.position.x; scene_constants.reflection_position[i][1] = p.position.y; scene_constants.reflection_position[i][2] = p.position.z;
        scene_constants.reflection_position[i][3] = p.sphere ? 1.0F : 0.0F;
        scene_constants.reflection_extent[i][0] = p.extent.x; scene_constants.reflection_extent[i][1] = p.extent.y; scene_constants.reflection_extent[i][2] = p.extent.z;
        scene_constants.reflection_parameters[i][0] = p.blend;
        scene_constants.reflection_parameters[i][1] = p.intensity;
        scene_constants.reflection_parameters[i][2] = static_cast<float>(reflection_filters_[i]->limits().specular_mips);
        scene_constants.reflection_parameters[i][3] = 1;
    }
    scene_constants.planar_parameters[0] = planar_active_ ? scene.environment.planar.strength : 0;
    scene_constants.planar_parameters[1] = static_cast<float>(stats_.reflection_probes);

    scene_constants.texture_sampling[0] = scene.renderer_settings.texture_lod_bias;
    scene_constants.shadow_quality[0] = renderer_settings.shadow_blend_band;
    scene_constants.shadow_quality[1] = renderer_settings.shadow_filter_radius;
    const float sun_radius = scene.directional_lights.empty() ? 0.0F
        : std::clamp(scene.directional_lights.front().angular_radius, 0.0F, 0.249F);
    scene_constants.shadow_quality[2] = scene::deterministic::sin_turns(sun_radius)
        / scene::deterministic::cos_turns(sun_radius);
    scene_constants.contact_shadow[0] = renderer_settings.contact_shadow_length;
    scene_constants.contact_shadow[1] = renderer_settings.contact_shadow_thickness;
    scene_constants.contact_shadow[2] = 32.0F;
    // An orthographic view has no eye point to look from: every pixel sees
    // along the view. w = 0 carries that direction, toward the viewer, in
    // place of a position, and the shading reads it as the view vector.
    if (scene::orthographic(render_view.matrices.projection)) {
        scene_constants.eye_position[0U] = -render_view.forward.x;
        scene_constants.eye_position[1U] = -render_view.forward.y;
        scene_constants.eye_position[2U] = -render_view.forward.z;
        scene_constants.eye_position[3U] = 0.0F;
    } else {
        scene_constants.eye_position[0U] = render_view.eye.x;
        scene_constants.eye_position[1U] = render_view.eye.y;
        scene_constants.eye_position[2U] = render_view.eye.z;
        scene_constants.eye_position[3U] = 1.0F;
    }
    scene_constants.view_mode[0U] = static_cast<float>(shading);
    scene_constants.sky_ambient[0U] = scene.environment.sky_color.x;
    scene_constants.sky_ambient[1U] = scene.environment.sky_color.y;
    scene_constants.sky_ambient[2U] = scene.environment.sky_color.z;
    scene_constants.sky_ambient[3U] = scene.environment.ambient;
    scene_constants.ground_exposure[0U] = scene.environment.ground_color.x;
    scene_constants.ground_exposure[1U] = scene.environment.ground_color.y;
    scene_constants.ground_exposure[2U] = scene.environment.ground_color.z;
    // Basic remains the regression/reference renderer. Advanced consumes the
    // replicated physical camera controls, normalized so the existing default
    // camera is neutral while each authored stop retains its exact ratio.
    scene_constants.ground_exposure[3U] =
        mode_ == SceneRendererMode::AdvancedLinear
        ? scene::camera_exposure_multiplier(scene.camera)
        : 1.0F;
    scene_constants.counts[0U] =
        static_cast<core::u32>(light_scratch_.size() / sizeof(GpuLight));
    // Published for passes outside the lit draw. The buffer is sized to the
    // authored maximum and only partly written, so a consumer needs telling
    // where the lights stop.
    uploaded_light_count_ = scene_constants.counts[0U];
    scene_constants.particle_lights[0U] = particle_lights_used_;
    if (scene_linear() && renderer_settings.contact_shadow_length > 0.0F
        && renderer_settings.contact_shadow_thickness > 0.0F) {
        for (core::u32 i = 0U; i < uploaded_light_count_; ++i) {
            GpuLight light{};
            std::memcpy(&light, light_scratch_.data() + static_cast<std::size_t>(i) * sizeof(light), sizeof(light));
            if ((i == 0U && light.kind == GpuLightKind::Directional)
                || (light.kind == GpuLightKind::Spot && light.params[3U] >= 0.0F))
                ++stats_.contact_shadow_lights;
        }
        if (stats_.contact_shadow_lights == 0U)
            static_cast<void>(core::report_idle_once("contact shadows", "no directional light or allocated shadow-casting spot"));
    } else if (renderer_settings.contact_shadow_length > 0.0F) {
        static_cast<void>(core::report_idle_once("contact shadows", scene_linear()
            ? "contact thickness is zero" : "Advanced renderer is not selected"));
    } else if (scene_linear()) {
        static_cast<void>(core::report_idle_once("contact shadows", "contact length is zero"));
    }
    scene_constants.counts[1U] =
        scene.effect_settings.brightness_curve == scene::BrightnessCurve::Gain
            ? 1U
            : 0U;
    scene_constants.grain_context[0U] = slice.x;
    scene_constants.grain_context[1U] = slice.y;
    scene_constants.grain_context[2U] =
        static_cast<core::u32>(scene.show_frame & 0xFFFF'FFFFULL);
    scene_constants.grain_context[3U] =
        static_cast<core::u32>(scene.show_frame >> 32U);
    scene_constants.grain_extent[0U] = slice.width;
    scene_constants.grain_extent[1U] = slice.height;
    scene_constants.grain_extent[2U] = width();
    scene_constants.grain_extent[3U] = height();
    const auto lighting_inverse = scene::invert(render_view.canvas_view_projection);
    for (core::u32 index = 0U; index < 16U; ++index) {
        scene_constants.lighting_inverse_view_projection[index] = lighting_inverse.m[index];
        scene_constants.lighting_view_projection[index] = render_view.canvas_view_projection.m[index];
    }
    scene_constants.lighting_canvas[0U] = static_cast<float>(options.view && !options.full_canvas_view ? slice.width : scene.canvas.width);
    scene_constants.lighting_canvas[1U] = static_cast<float>(options.view && !options.full_canvas_view ? slice.height : scene.canvas.height);
    scene_constants.lighting_canvas[2U] = options.view && !options.full_canvas_view ? 0.0F : static_cast<float>(slice.x);
    scene_constants.lighting_canvas[3U] = options.view && !options.full_canvas_view ? 0.0F : static_cast<float>(slice.y);
    for (core::u32 cascade = 0U; cascade < scene::max_shadow_cascades;
         ++cascade) {
        for (core::u32 index = 0U; index < 16U; ++index) {
            scene_constants.shadow_view_projection[cascade][index] =
                cascade_view_projection[cascade].m[index];
        }
        scene_constants.shadow_cascade_texel_depth[cascade] =
            cascade_texel_depth[cascade];
    }
    for (core::u32 index = 0U; index < 16U; ++index) {
        scene_constants.shadow_inverse_view_projection[index] =
            shadow_inverse_view_projection.m[index];
    }
    const float reciprocal_shadow = 1.0F
        / static_cast<float>(limits_.directional_shadow_resolution);
    scene_constants.shadow_parameters[0U] = reciprocal_shadow;
    scene_constants.shadow_parameters[1U] = reciprocal_shadow;
    // In shadow texels, so each cascade scales them by its own texel depth.
    scene_constants.shadow_parameters[2U] = 1.5F;
    scene_constants.shadow_parameters[3U] = 6.0F;
    if (baked.enabled && stats_.baked_ready && probe_asset_) {
        const auto& grid = probe_asset_->grid;
        scene_constants.baked_minimum[0] = grid.minimum.x;
        scene_constants.baked_minimum[1] = grid.minimum.y;
        scene_constants.baked_minimum[2] = grid.minimum.z;
        scene_constants.baked_maximum[0] = grid.maximum.x;
        scene_constants.baked_maximum[1] = grid.maximum.y;
        scene_constants.baked_maximum[2] = grid.maximum.z;
        scene_constants.baked_dimensions[0] = static_cast<float>(grid.count_x);
        scene_constants.baked_dimensions[1] = static_cast<float>(grid.count_y);
        scene_constants.baked_dimensions[2] = static_cast<float>(grid.count_z);
        scene_constants.baked_dimensions[3] = baked.strength;
    }
    scene_constants.gi_parameters[0U] = 0.15F;
    scene_constants.gi_parameters[1U] = directional_gi_enabled
        ? renderer_settings.gi_strength
        : 0.0F;
    scene_constants.gi_parameters[2U] = 0.0025F;
    // The environment, or nothing. Zero intensity is what selects the
    // hemisphere fallback in the shader, so a scene with no authored map takes
    // exactly the path it took before any of this existed.
    if (environment_.has_value() && environment_->maps().complete()
        && environment_generation_ != 0U) {
        scene_constants.environment_parameters[0U] =
            physical_environment_ ? 1.0F : std::max(scene.environment.map_intensity, 0.0F);
        scene_constants.environment_parameters[1U] =
            static_cast<float>(environment_->limits().specular_mips);
    }
    scene_constants.counts[2U] = cascade_count;
    // Directional lights are packed before local lights; the first authored
    // directional light therefore has the stable packed index zero.
    scene_constants.counts[3U] = 0U;

    const rhi::StorageBuffer* const vertex_bindings[]{
        &geometry_->buffer(GeometryStream::Vertices), &geometry_->buffer(GeometryStream::Indices), &instance_buffer_,
        &geometry_->buffer(GeometryStream::Tangents), &geometry_->buffer(GeometryStream::Skins), &pose_buffer_};
    // The batched passes read their indices through the input assembler.
    // Single composited draws stay pulled; their batch constants say so.
    const rhi::StorageBuffer* const batch_indices =
        indexed ? &geometry_->buffer(GeometryStream::Indices) : nullptr;
    // The shadow table is the fourth, and only the Advanced pipeline declares
    // it. Bound as a span rather than a fixed array so the count follows the
    // pipeline: Basic binding four against a signature that declares three is
    // a refused draw, and the refusal says nothing about which count was
    // wrong.
    const rhi::StorageBuffer* const lit_fragment_buffers[]{
        &light_buffer_,
        &inline_effect_op_buffer_,
        &inline_effect_parameter_buffer_,
        &ltc_asset_.buffer(), &light_shadow_buffer_, &probe_buffer_, &particle_light_buffer_};
    const std::span<const rhi::StorageBuffer* const> lit_fragment_bindings{
        lit_fragment_buffers,
        mode_ == SceneRendererMode::AdvancedLinear ? 7U : 4U};
    const rhi::StorageBuffer* const media_fragment_bindings[]{
        &inline_effect_op_buffer_, &inline_effect_parameter_buffer_};

    const std::array<float, 4U> coverage_sampling{renderer_settings.texture_lod_bias, 0.0F, 0.0F, 0.0F};

    // Shadow maps: each drawn from the casters its projection can reach, and
    // kept from an earlier frame while nothing its draw reads has changed -
    // the projection, the caster instance records and joint palettes, the
    // geometry arena, the alpha textures. That is the usual state of a set
    // under a fixed sun: the full-scene cascade and the reflective shadow map
    // depend on no camera, so they stay put while the camera moves, and a
    // held camera keeps every cascade.
    //
    // The same digest finds a map another renderer on this device drew. Every
    // view of one scene - Program, its transition target, the stage view, a
    // mapped output - fits its cascades to the authored camera rather than to
    // its own view, so the first of them recorded draws the maps and the rest
    // sample them.
    //
    // Neither choice can make two nodes disagree. A caster is left out of a
    // map only when it lies wholly beyond one of the map's side planes, where
    // it would have drawn nothing; and a reused map holds exactly the texels
    // the same draw would write again.
    const bool atlas_wanted = !shadow_atlas_plan_.tiles.empty() && atlas_home_;
    const rhi::CommandRecordingToken shadow_recording =
        directional_shadow_enabled || atlas_wanted ? commands.recording_token() : rhi::CommandRecordingToken{};
    const TextureResidency* const texture_residency =
        texture_streamer_ ? texture_streamer_->residency().get() : nullptr;
    std::unique_lock<std::recursive_mutex> textures_held;
    if (directional_shadow_enabled || atlas_wanted) {
        if (texture_streamer_ && texture_streamer_->residency()) {
            textures_held = texture_streamer_->residency()->lock();
        }
        instance_digests_.clear();
        for (std::size_t slot = 0U; slot < opaque_submissions_.size(); ++slot) {
            GpuInstance instance{};
            std::memcpy(&instance, instance_scratch_.data() + slot * sizeof(GpuInstance),
                        sizeof(instance));
            // Digested as a caster draw reads it. The previous transform, the
            // reflection/receiver flag and the inline program range feed only
            // camera products, and differ between views of one scene.
            std::fill(std::begin(instance.previous_world), std::end(instance.previous_world), 0.0F);
            instance.alpha[3U] = 0.0F;
            std::fill(std::begin(instance.effect_range), std::end(instance.effect_range), 0U);
            core::u64 digest = digest_bytes(0U, bytes_of(instance));
            if (instance.pose[1] != 0U) {
                // A skinned caster moves with its joint palette, which its
                // record only points at. A caster draw reads the current poses.
                const std::size_t poses = pose_upload_.size() / sizeof(GpuPose);
                const std::size_t first = instance.pose[0];
                const std::size_t count = instance.pose[1];
                if (first <= poses && count <= poses - first) {
                    for (std::size_t pose = first; pose < first + count; ++pose) {
                        digest = digest_bytes(digest, std::span<const core::u8>{
                            pose_upload_.data() + pose * sizeof(GpuPose) + offsetof(GpuPose, current),
                            sizeof(scene::Mat4)});
                    }
                } else {
                    digest = mix_digest(digest, unmatched_digest());
                }
            }
            instance_digests_.push_back(digest);
        }
    }
    // A coverage texture by what it holds rather than by its object: this
    // renderer's white fallback by a constant, a model texture by the serial
    // its residency gave it, which no other texture has had. Anything else
    // matches nothing.
    const auto coverage_identity = [&](const rhi::SampledTexture& bound) noexcept {
        core::u64 texture = 0U;
        if (bound.texture == &material_fallback_) {
            texture = 0x5748'4954'45ULL;
        } else if (texture_residency != nullptr) {
            texture = texture_residency->serial(bound.texture);
        }
        if (texture == 0U) {
            texture = unmatched_digest();
        }
        const auto* const samplers = material_samplers_.data();
        const core::u64 sampler = bound.sampler >= samplers
                && bound.sampler < samplers + material_samplers_.size()
            ? static_cast<core::u64>(bound.sampler - samplers)
            : unmatched_digest();
        return mix_digest(texture, sampler);
    };
    // Fills casters_ with the opaque instances `view_projection` can reach,
    // in the batch order every map uses, and returns a digest of what drawing
    // them reads.
    const core::u64 caster_scope = mix_digest(geometry_->epoch(),
        std::bit_cast<core::u32>(coverage_sampling[0U]));
    const auto gather_casters = [&](const scene::Mat4& view_projection) -> core::u64 {
        casters_.batches.clear();
        casters_.uniforms.clear();
        casters_.textures.clear();
        casters_.instances = 0U;
        core::u64 key = digest_bytes(caster_scope, std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(view_projection.m.data()), sizeof(view_projection.m)});
        for (std::size_t index = 0U; index < batches_.size(); ++index) {
            BatchConstants head{};
            std::memcpy(&head, batch_uniforms_.data() + index * sizeof(BatchConstants), sizeof(head));
            const rhi::DrawBatch& batch = batches_[index];
            const rhi::SampledTexture& coverage_texture = rsm_flux_textures_[index];
            const core::u32 end = head.first_instance + batch.instance_count;
            core::u32 run_begin = 0U;
            bool in_run = false;
            for (core::u32 slot = head.first_instance; slot <= end; ++slot) {
                const bool reached = slot < end && !outside_shadow_projection(view_projection,
                    opaque_submissions_[slot].world, opaque_submissions_[slot].bounds);
                if (reached && !in_run) {
                    run_begin = slot;
                    in_run = true;
                } else if (!reached && in_run) {
                    in_run = false;
                    casters_.batches.push_back({batch.vertex_count, slot - run_begin, batch.first_index});
                    BatchConstants constants = head;
                    constants.first_instance = run_begin;
                    append(casters_.uniforms, constants);
                    casters_.textures.push_back(coverage_texture);
                    casters_.instances += slot - run_begin;
                    key = mix_digest(key, (static_cast<core::u64>(head.first_index) << 32U) | batch.vertex_count);
                    key = mix_digest(key, (static_cast<core::u64>(run_begin) << 32U) | (slot - run_begin));
                    key = mix_digest(key, coverage_identity(coverage_texture));
                    for (core::u32 drawn = run_begin; drawn < slot; ++drawn) {
                        key = mix_digest(key, instance_digests_[drawn]);
                    }
                }
            }
        }
        return key;
    };
    // A product's key also names its kind and size, so a cascade never
    // stands in for an atlas or a map of another resolution.
    const auto product_key = [](core::u64 key, core::u64 kind, core::u32 extent) noexcept {
        return mix_digest(mix_digest(key, kind), extent);
    };
    // Chooses what this recording samples for `key`: what it sampled last
    // time, or what another renderer drew, while either is current. Otherwise
    // binds this renderer's own product, cleared of its key until the caller
    // has drawn it, and returns false. Its own other products are never taken
    // for a slot: one of them may yet be redrawn in this recording.
    const auto sample = [&](GeometryResidency::ShadowHandle& bound,
                            const GeometryResidency::ShadowHandle& home, core::u64 key) {
        const auto owned = [&](const GeometryResidency::ShadowHandle& product) noexcept {
            return product == rsm_home_ || product == atlas_home_
                || std::find(cascade_homes_.begin(), cascade_homes_.end(), product)
                    != cascade_homes_.end();
        };
        if (!bound || !GeometryResidency::shadow_current(*bound, key, shadow_recording)) {
            bound = geometry_->find_shadow(key, shadow_recording);
        }
        if (bound && bound != home && owned(bound)) {
            bound.reset();
        }
        if (!bound) {
            bound = home;
            home->key = 0U;
            home->recording = {};
            home->submitted = false;
            return false;
        }
        ++stats_.shadow_maps_reused;
        if (bound != home) {
            ++stats_.shadow_maps_shared;
        }
        return true;
    };
    const auto drawn = [&](GeometryResidency::ShadowProduct& product, core::u64 key) noexcept {
        product.key = key;
        product.recording = shadow_recording;
        product.submitted = false;
    };
    const auto caster_draw = [&](const rhi::GraphicsPipeline& pipeline) noexcept {
        rhi::PulledDraw caster{};
        caster.pipeline = &pipeline;
        caster.fragment_uniform = bytes_of(coverage_sampling);
        caster.fragment_textures = casters_.textures;
        caster.fragment_textures_per_batch = 1U;
        caster.vertex_buffers = vertex_bindings;
        caster.batch_uniforms = casters_.uniforms;
        caster.batch_uniform_stride = static_cast<core::u32>(sizeof(BatchConstants));
        caster.batches = casters_.batches;
        caster.index_buffer = batch_indices;
        return caster;
    };
    const core::u32 shadow_extent = limits_.directional_shadow_resolution;
    for (core::u32 cascade = cascade_count; cascade < scene::max_shadow_cascades; ++cascade) {
        cascades_[cascade].reset();
    }
    if (directional_shadow_enabled) {
        ViewConstants shadow_view{};
        const auto view_from = [&](const scene::Mat4& matrix) noexcept {
            for (core::u32 index = 0U; index < 16U; ++index) {
                shadow_view.view_projection[index] = matrix.m[index];
                shadow_view.previous_view_projection[index] = matrix.m[index];
                shadow_view.canvas_view_projection[index] = matrix.m[index];
            }
        };
        // The full-scene fit comes last, so after this loop casters_ holds
        // its casters for the reflective-shadow-map passes.
        core::u64 full_scene_key{};
        for (core::u32 cascade = 0U; cascade < cascade_count; ++cascade) {
            full_scene_key = gather_casters(cascade_view_projection[cascade]);
            const core::u64 key = product_key(full_scene_key, 0x435343ULL, shadow_extent);
            if (sample(cascades_[cascade], cascade_homes_[cascade], key)) {
                continue;
            }
            view_from(cascade_view_projection[cascade]);
            rhi::PulledDraw shadow_draw = caster_draw(shadow_pipeline_);
            shadow_draw.vertex_uniform = bytes_of(shadow_view);
            if (!graph_.draw_pulled(commands, core::RenderPass::Shadows,
                    cascade_homes_[cascade]->textures[0U],
                    rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, shadow_depth_, 0.0F,
                    shadow_draw)) {
                return false;
            }
            stats_.shadow_caster_instances += casters_.instances;
            drawn(*cascade_homes_[cascade], key);
        }
        if (directional_gi_enabled) {
            const auto& light = scene.directional_lights.front();
            const auto emission = scene::light_emission(light, scene.renderer_settings.photometric_scale);
            RsmFluxConstants flux_constants{};
            flux_constants.direction_intensity[0U] = light.direction.x;
            flux_constants.direction_intensity[1U] = light.direction.y;
            flux_constants.direction_intensity[2U] = light.direction.z;
            flux_constants.direction_intensity[3U] = emission.intensity;
            flux_constants.light_color[0U] = emission.color.x;
            flux_constants.light_color[1U] = emission.color.y;
            flux_constants.light_color[2U] = emission.color.z;
            flux_constants.light_color[3U] = scene.renderer_settings.texture_lod_bias;
            const core::u64 rsm_key = product_key(digest_bytes(
                mix_digest(full_scene_key, 0x52534DULL), bytes_of(flux_constants)),
                0x52534DULL, shadow_extent);
            if (!sample(rsm_, rsm_home_, rsm_key)) {
                // The reflective-shadow-map attributes belong to the
                // full-scene fit, which is the last cascade.
                view_from(cascade_view_projection[cascade_count - 1U]);
                rhi::PulledDraw rsm_draw = caster_draw(rsm_normal_pipeline_);
                rsm_draw.vertex_uniform = bytes_of(shadow_view);
                if (!graph_.draw_pulled(commands, core::RenderPass::ShadowAttributes,
                        rsm_home_->textures[0U], rhi::Color{}, shadow_depth_, 0.0F,
                        rsm_draw)) {
                    return false;
                }
                rsm_draw.pipeline = &rsm_flux_pipeline_;
                rsm_draw.fragment_uniform = bytes_of(flux_constants);
                if (!graph_.draw_pulled(commands, core::RenderPass::ShadowAttributes,
                        rsm_home_->textures[1U], rhi::Color{}, shadow_depth_, 0.0F,
                        rsm_draw)) {
                    return false;
                }
                drawn(*rsm_home_, rsm_key);
            }
        } else {
            rsm_.reset();
        }
    } else {
        rsm_.reset();
    }

    // The local-light shadow atlas: one tile per shadow-casting spot.
    //
    // Outside the directional block on purpose. A set lit only by practicals
    // has no directional light and no cascades at all, and that is exactly the
    // scene this feature exists for - nesting it under the sun would mean the
    // interior sets that need local shadows most were the ones that never got
    // them.
    //
    // The atlas is cleared once and every tile then loads, because a clear is
    // not viewport-scoped: clearing per tile would wipe the tiles already
    // drawn. Each draw is confined to its own rectangle by the viewport, so
    // two lights cannot write each other's map. For the same reason the atlas
    // is reused or redrawn whole.
    //
    // Each tile draws the casters its spot's projection reaches, under the
    // same rule as the cascades.
    if (atlas_wanted) {
        const auto tile_projection = [&](std::size_t index) noexcept {
            GpuLightShadow record{};
            std::memcpy(&record,
                        light_shadow_scratch_.data()
                            + index * sizeof(GpuLightShadow),
                        sizeof(record));
            scene::Mat4 matrix{};
            for (core::u32 lane = 0U; lane < 16U; ++lane) {
                matrix.m[lane] = record.view_projection[lane];
            }
            return matrix;
        };
        core::u64 atlas_key = mix_digest(0x41544CULL, shadow_atlas_plan_.tiles.size());
        for (std::size_t index = 0U; index < shadow_atlas_plan_.tiles.size(); ++index) {
            const ShadowTile& tile = shadow_atlas_plan_.tiles[index];
            atlas_key = mix_digest(atlas_key, gather_casters(tile_projection(index)));
            atlas_key = mix_digest(atlas_key, (static_cast<core::u64>(tile.x) << 32U) | tile.y);
            atlas_key = mix_digest(atlas_key, tile.extent);
        }
        atlas_key = product_key(atlas_key, 0x41544CULL, shadow_atlas_limits_.extent);
        if (!sample(atlas_, atlas_home_, atlas_key)) {
            rhi::Texture& atlas = atlas_home_->textures[0U];
            if (!graph_.clear(commands, core::RenderPass::Shadows, atlas,
                                rhi::Color{0.0F, 0.0F, 0.0F, 1.0F})) {
                return false;
            }
            ViewConstants tile_view{};
            for (std::size_t index = 0U;
                 index < shadow_atlas_plan_.tiles.size(); ++index) {
                const ShadowTile& tile = shadow_atlas_plan_.tiles[index];
                const scene::Mat4 projection = tile_projection(index);
                static_cast<void>(gather_casters(projection));
                for (core::u32 lane = 0U; lane < 16U; ++lane) {
                    tile_view.view_projection[lane] = projection.m[lane];
                    tile_view.previous_view_projection[lane] = projection.m[lane];
                    tile_view.canvas_view_projection[lane] = projection.m[lane];
                }
                rhi::PulledDraw tile_draw = caster_draw(shadow_pipeline_);
                tile_draw.vertex_uniform = bytes_of(tile_view);
                tile_draw.load = true;
                tile_draw.viewport =
                    rhi::ViewportRect{tile.x, tile.y, tile.extent, tile.extent};
                if (!graph_.draw_pulled(commands, core::RenderPass::Shadows,
                        atlas, rhi::Color{0.0F, 0.0F, 0.0F, 1.0F},
                        shadow_atlas_depth_, 0.0F, tile_draw)) {
                    return false;
                }
                stats_.shadow_caster_instances += casters_.instances;
            }
            drawn(*atlas_home_, atlas_key);
        }
    } else {
        atlas_.reset();
    }
    textures_held = {};
    // The batches were assembled before it was known whose maps this
    // recording samples.
    if (scene_linear()) {
        const std::span<rhi::SampledTexture> batch_textures{camera_batch_textures_};
        for (std::size_t first = 0U; first + advanced_fragment_textures <= batch_textures.size();
             first += advanced_fragment_textures) {
            bind_shadow_products(batch_textures.subspan(first, advanced_fragment_textures));
        }
    }

    if (options.reflection_pass) scene_constants.ground_exposure[3] = 1.0F;

    rhi::PulledDraw draw{};
    draw.pipeline = wireframe ? &wireframe_pipeline_ : &pipeline_;
    draw.vertex_buffers = vertex_bindings;
    draw.fragment_buffers = lit_fragment_bindings;
    draw.vertex_uniform = bytes_of(view_constants);
        draw.raster_viewport = raster_viewport;
    draw.fragment_uniform = bytes_of(scene_constants);
    draw.batch_uniforms = camera_batch_uniforms_;
    draw.batch_uniform_stride = static_cast<core::u32>(sizeof(BatchConstants));
    draw.batches = camera_batches_;
    draw.index_buffer = batch_indices;
    draw.fragment_textures = camera_batch_textures_;
    draw.fragment_textures_per_batch =
        mode_ == SceneRendererMode::AdvancedLinear
        ? advanced_fragment_textures
        : basic_fragment_textures;
    draw.store_depth =
        !composite_draws_.empty() || !depth_only_media_.empty() || !particle_draws_.empty();

    if (mode_ == SceneRendererMode::AdvancedLinear && !options.reflection_pass) {
        // Motion of a deforming triangle is solved from all three of its
        // vertices, which the vertex stage finds by its position in the index
        // stream: such a frame draws the motion products pulled.
        const bool pulled_motion = indexed && !animated_instances_.empty();
        if (pulled_motion) {
            camera_pulled_batch_uniforms_.assign(
                camera_batch_uniforms_.begin(), camera_batch_uniforms_.end());
            for (std::size_t offset = 0U; offset < camera_pulled_batch_uniforms_.size();
                 offset += sizeof(BatchConstants)) {
                BatchConstants constants{};
                std::memcpy(&constants, camera_pulled_batch_uniforms_.data() + offset, sizeof(constants));
                constants.indexed = 0U;
                std::memcpy(camera_pulled_batch_uniforms_.data() + offset, &constants, sizeof(constants));
            }
        }
        const auto render_auxiliary =
            [&](rhi::Texture& target,
                const rhi::GraphicsPipeline& auxiliary_pipeline,
                const rhi::GraphicsPipeline& media_auxiliary_pipeline,
                bool object_only = false, bool motion = false,
                std::span<rhi::Texture* const> additional_targets = {}) {
                // A merged pass is timed as Identity, its first product.
                const auto category = &target == &object_id_ ? core::RenderPass::Identity
                    : &target == &object_velocity_ ? core::RenderPass::ObjectVelocity
                    : &target == &normal_ ? core::RenderPass::Normals : core::RenderPass::Velocity;
                ViewConstants auxiliary_view = view_constants;
                if (object_only) {
                    // Both endpoints use this frame's camera and jitter. Only
                    // the retained previous object transform can create motion.
                    std::copy(std::begin(view_constants.canvas_view_projection),
                        std::end(view_constants.canvas_view_projection),
                        std::begin(auxiliary_view.previous_view_projection));
                }
                rhi::PulledDraw auxiliary{};
                auxiliary.pipeline = &auxiliary_pipeline;
                auxiliary.vertex_buffers = vertex_bindings;
                auxiliary.vertex_uniform = bytes_of(auxiliary_view);
        auxiliary.raster_viewport = raster_viewport;
                const bool pulled = motion && pulled_motion;
                auxiliary.batch_uniforms = pulled
                    ? std::span<const core::u8>{camera_pulled_batch_uniforms_}
                    : std::span<const core::u8>{camera_batch_uniforms_};
                auxiliary.batch_uniform_stride =
                    static_cast<core::u32>(sizeof(BatchConstants));
                auxiliary.batches = camera_batches_;
                auxiliary.index_buffer = pulled ? nullptr : batch_indices;
                auxiliary.fragment_uniform = bytes_of(coverage_sampling);
                auxiliary.fragment_textures = camera_coverage_textures_;
                auxiliary.fragment_textures_per_batch = 1U;
                auxiliary.additional_targets = additional_targets;
                // This is the public Advanced depth product after the velocity
                // replay. Preserve it even for a scene with no composites.
                auxiliary.store_depth = true;
                if (!graph_.draw_pulled(commands, category,
                        target, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}, auxiliary_depth_, 0.0F,
                        auxiliary)) {
                    return false;
                }

                // Depth-only media is part of the colour pass's tested
                // visibility but owns no categorical product in this isolated
                // layer. Its media auxiliary shader alpha-tests the sampled
                // coverage, returns zero, and still writes depth where it
                // genuinely occludes later layers.
                for (const auto& occluder : depth_only_media_) {
                    const rhi::DrawBatch batch{
                        occluder.range.index_count, 1U};
                    const BatchConstants offsets{
                        occluder.range.first_index,
                        occluder.first_instance,
                        0U,
                        0U};
                    const std::array<rhi::SampledTexture, 2U> sources{{
                        rhi::SampledTexture{
                            occluder.media.primary, &media_sampler_},
                        rhi::SampledTexture{
                            occluder.media.chroma != nullptr
                                ? occluder.media.chroma
                                : &material_fallback_,
                            &media_sampler_},
                    }};
                    rhi::PulledDraw layer{};
                    layer.pipeline = &media_auxiliary_pipeline;
                    layer.vertex_buffers = vertex_bindings;
                    layer.vertex_uniform = bytes_of(auxiliary_view);
        layer.raster_viewport = raster_viewport;
                    layer.additional_targets = additional_targets;
                    layer.fragment_textures = sources;
                    layer.batch_uniforms = bytes_of(offsets);
                    layer.batch_uniform_stride =
                        static_cast<core::u32>(sizeof(BatchConstants));
                    layer.batches =
                        std::span<const rhi::DrawBatch>{&batch, 1U};
                    layer.load = true;
                    layer.store_depth = true;
                    if (!graph_.draw_pulled(commands, category,
                            target, rhi::Color{}, auxiliary_depth_, 0.0F,
                            layer)) {
                        return false;
                    }
                }
                for (std::size_t index = 0U;
                     index < composite_draws_.size(); ++index) {
                    const auto& composite = composite_draws_[index];
                    const rhi::DrawBatch batch{
                        composite.range.index_count, 1U};
                    const BatchConstants offsets{
                        composite.range.first_index,
                        composite.first_instance,
                        0U,
                        0U};
                    rhi::PulledDraw layer{};
                    layer.pipeline = composite.media
                        ? &media_auxiliary_pipeline
                        : &auxiliary_pipeline;
                    layer.additional_targets = additional_targets;
                    layer.vertex_buffers = vertex_bindings;
                    layer.vertex_uniform = bytes_of(auxiliary_view);
        layer.raster_viewport = raster_viewport;
                    std::array<rhi::SampledTexture, 2U> media_sources{};
                    const rhi::SampledTexture coverage_source{
                        composite.material_textures[0],
                        &material_samplers_[material_sampler_index(composite.samplers[0], renderer_settings.texture_anisotropy)]};
                    if (!composite.media) {
                        layer.fragment_textures = {&coverage_source, 1U};
                        layer.fragment_uniform = bytes_of(coverage_sampling);
                    }
                    if (composite.media) {
                        media_sources = std::array<rhi::SampledTexture, 2U>{{
                            rhi::SampledTexture{
                                composite.media.primary, &media_sampler_},
                            rhi::SampledTexture{
                                composite.media.chroma != nullptr
                                    ? composite.media.chroma
                                    : &material_fallback_,
                                &media_sampler_},
                        }};
                        layer.fragment_textures = media_sources;
                    }
                    layer.batch_uniforms = bytes_of(offsets);
                    layer.batch_uniform_stride =
                        static_cast<core::u32>(sizeof(BatchConstants));
                    layer.batches =
                        std::span<const rhi::DrawBatch>{&batch, 1U};
                    layer.load = true;
                    layer.store_depth = true;
                    if (!graph_.draw_pulled(commands, category,
                            target, rhi::Color{}, auxiliary_depth_, 0.0F,
                            layer)) {
                        return false;
                    }
                }
                return true;
            };
        // Depth, identity, motion and normals, all before colour.
        //
        // They used to run after it, which was fine while nothing in the
        // colour pass read them. Occlusion does: it needs this frame's depth
        // and normals, and the lit pass needs this frame's occlusion. Each
        // call already clears and rewrites the same auxiliary depth, so which
        // one runs first does not change what the depth ends up holding.
        //
        // A device with multiple render targets writes identity, motion and
        // normals in one pass: the same rasterization, coverage and
        // equations, one geometry replay instead of three.
        const bool merged_auxiliary = auxiliary_pipeline_ && media_auxiliary_pipeline_;
        rhi::Texture* const merged_targets[]{&velocity_, &normal_};
        if ((renderer_settings.motion_blur && renderer_settings.motion_blur_radius > 0U
                && !render_auxiliary(object_velocity_, velocity_pipeline_, media_velocity_pipeline_, true, true))
            || (merged_auxiliary
                ? !render_auxiliary(object_id_, auxiliary_pipeline_, media_auxiliary_pipeline_,
                      false, true, merged_targets)
                : (!render_auxiliary(
                        object_id_, object_id_pipeline_, media_object_id_pipeline_)
                    || !render_auxiliary(
                        velocity_, velocity_pipeline_, media_velocity_pipeline_, false, true)
                    || !render_auxiliary(
                        normal_, normal_pipeline_, media_normal_pipeline_)))) {
            return false;
        }
        // Occlusion, from the depth and normals the replay above just wrote.
        //
        // The slice origin travels with it so the sample kernel is rotated by
        // the absolute canvas pixel rather than by a node-local index: two
        // nodes asked about the same wall pixel must rotate identically, or a
        // crease that crosses their seam is sampled two different ways and the
        // seam appears in the occlusion.
        if (occlusion_recorded_) {
            if (!occlusion_->resize(*device_, color_.width(),
                                    color_.height())) {
                return false;
            }
            if (!occlusion_->record(
                    commands, auxiliary_depth_, normal_,
                    scene::invert(render_view.matrices.view_projection), slice,
                    scene.show_frame,
                    scene.renderer_settings.occlusion_radius,
                    scene.renderer_settings.occlusion_strength)) {
                return false;
            }
            if (scene::antialiasing_for(scene.renderer_settings)
                == scene::AntialiasingMode::Temporal) {
                if (!temporal_occlusion_ || !temporal_occlusion_->resize(
                        *device_, color_.width(), color_.height())) return false;
                HistoryDisposition disposition = HistoryDisposition::Initialize;
                switch (options.history) {
                case SceneHistoryAction::Advance: disposition = HistoryDisposition::Advance; break;
                case SceneHistoryAction::Hold: disposition = HistoryDisposition::Hold; break;
                case SceneHistoryAction::Reset: disposition = HistoryDisposition::Reset; break;
                default: break;
                }
                const TemporalResolve::Sources sources{
                    &occlusion_->target(), &velocity_, &object_id_,
                    options.view ? color_.width() : scene.canvas.width,
                    options.view ? color_.height() : scene.canvas.height,
                    options.view
                        ? orthographic_history_depth_tolerance(
                              options.view->matrices.projection,
                              color_.height())
                        : 0.0F};
                auto resolved = temporal_occlusion_->apply(*device_, commands,
                    options.history_slot, disposition, sources);
                if (!resolved) return false;
                // Descriptor arrays already point at this stable AO texture.
                if (resolved.value() != &occlusion_->target()
                    && !graph_.copy(commands, core::RenderPass::Lighting, *resolved.value(),
                        {0U, 0U, color_.width(), color_.height()},
                        occlusion_->target())) return false;
            }
        }
    }

    // The ordinary clear is the sky, so an empty scene still reads as a frame.
    // Isolated compositor layers instead retain transparent uncovered pixels.
    // Depth clears to zero because the projection is reversed.
    const rhi::Color clear = options.transparent_background
        ? rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}
        : rhi::Color{
              scene.environment.sky_color.x
                  * scene_constants.ground_exposure[3U],
              scene.environment.sky_color.y
                  * scene_constants.ground_exposure[3U],
              scene.environment.sky_color.z
                  * scene_constants.ground_exposure[3U],
              1.0F};
    // The environment as the background, before anything is drawn over it.
    //
    // It owns the clear when it runs: the pass clears colour and depth as any
    // first pass does, fills every pixel with the environment, and the lit pass
    // then loads instead of clearing. A scene with no map skips all of this and
    // the lit pass clears exactly as it always did, which is what keeps the
    // no-environment output bit for bit unchanged.
    // An isolated compositor layer must stay transparent where it drew
    // nothing, so it gets no sky: filling one with a background makes every
    // layer opaque and the composite shows only whichever happened to be last.
    // The environment still lights that layer - only the background is
    // withheld, and the compositor owns its own.
    const bool draw_sky =
        !options.transparent_background && background_resident(scene);
    if (draw_sky
        && !record_sky(commands, color_, clear, render_view, scene,
                       scene_constants.ground_exposure[3U], slice, options.view.has_value() && !options.full_canvas_view)) {
        return false;
    }
    draw.load = draw_sky;
    if (!graph_.draw_pulled(commands, core::RenderPass::Lighting, color_, clear, depth_, 0.0F, draw)) {
        return false;
    }

    // Clips that occlude this pass, one depth-writing alpha-tested pass each.
    // They run before any composited layer so every later draw is tested
    // against the complete depth of the frame rather than part of it.
    for (std::size_t index = 0U; index < depth_only_media_.size(); ++index) {
        const auto& occluder = depth_only_media_[index];
        const rhi::DrawBatch occluder_batch{occluder.range.index_count, 1U};
        const BatchConstants occluder_offsets{
            occluder.range.first_index, occluder.first_instance, 0U, 0U};
        rhi::PulledDraw occluder_draw{};
        occluder_draw.pipeline =
            wireframe ? &media_wireframe_pipeline_ : &media_depth_pipeline_;
        occluder_draw.vertex_buffers = vertex_bindings;
        occluder_draw.vertex_uniform = bytes_of(view_constants);
        occluder_draw.raster_viewport = raster_viewport;
        const std::array<rhi::SampledTexture, 2U> occluder_sources{{
            rhi::SampledTexture{occluder.media.primary, &media_sampler_},
            rhi::SampledTexture{
                occluder.media.chroma != nullptr
                    ? occluder.media.chroma
                    : &material_fallback_,
                &media_sampler_},
        }};
        occluder_draw.fragment_textures = occluder_sources;
        occluder_draw.fragment_buffers = media_fragment_bindings;
        occluder_draw.fragment_uniform = bytes_of(scene_constants);
        occluder_draw.batch_uniforms = bytes_of(occluder_offsets);
        occluder_draw.batch_uniform_stride =
            static_cast<core::u32>(sizeof(BatchConstants));
        occluder_draw.batches =
            std::span<const rhi::DrawBatch>{&occluder_batch, 1U};
        occluder_draw.load = true;
        occluder_draw.store_depth = index + 1U < depth_only_media_.size() ||
            !composite_draws_.empty() || !particle_draws_.empty();
        if (!graph_.draw_pulled(commands, core::RenderPass::Media,
                color_, clear, depth_, 0.0F, occluder_draw)) {
            return false;
        }
    }

    const bool needs_transmission = std::any_of(composite_draws_.begin(), composite_draws_.end(),
        [](const auto& item) { return !item.media && item.instance.material.transmission > 0.0F; });
    if (needs_transmission && !graph_.copy(commands, core::RenderPass::Lighting, color_,
        {0U, 0U, color_.width(), color_.height()}, transmission_background_)) return false;

    // Destination-reading layers are intentionally one batch per pass. That
    // preserves their sorted order while allowing every layer to choose its
    // own fixed-function blend equation and, for media, its own texture.
    // `depth_pass` records the layer instead into depth alone, for its fully
    // opaque pixels.
    const auto record_composite = [&](const CompositeDraw& composite,
                                      bool depth_pass, bool store_depth) {
        const rhi::DrawBatch composite_batch{
            composite.range.index_count, 1U};
        const BatchConstants composite_offsets{
            composite.range.first_index,
            composite.first_instance,
            0U,
            depth_pass ? 1U : 0U};
        rhi::PulledDraw composite_draw{};
        const auto pipeline_index = blend_index(composite.blend);
        composite_draw.pipeline = depth_pass
            ? (composite.media ? &media_depth_pipeline_
                               : &blended_depth_pipeline_)
            : wireframe
            ? (composite.media ? &media_wireframe_pipeline_
                               : &wireframe_pipeline_)
            : composite.media
            ? &media_blended_pipelines_[pipeline_index]
            : &blended_pipelines_[pipeline_index];
        composite_draw.vertex_buffers = vertex_bindings;
        composite_draw.vertex_uniform = bytes_of(view_constants);
        composite_draw.raster_viewport = raster_viewport;
        // The same set the lit pass binds, because this uses the same
        // pipeline. A composited layer that bound fewer would be a different
        // count than its own root signature declares - which is a refused draw
        // with no explanation, and is exactly what happened the last time a
        // binding was added and this array was missed.
        constexpr std::size_t material_binding_count =
            advanced_fragment_textures;
        std::array<rhi::SampledTexture, material_binding_count>
            material_bindings{};
        std::array<rhi::SampledTexture, 2U> media_bindings{};
        if (!composite.media) {
            bind_material_textures(material_bindings, composite.material_textures,
                composite.samplers, scene.renderer_settings.texture_anisotropy,
                composite.instance.material.transmission > 0.0F
                    ? transmission_background_ : material_fallback_);
            composite_draw.fragment_textures = scene_linear()
                ? std::span<const rhi::SampledTexture>{material_bindings}
                : std::span<const rhi::SampledTexture>{
                      material_bindings.data(), basic_fragment_textures};
            composite_draw.fragment_buffers = lit_fragment_bindings;
            composite_draw.fragment_uniform = bytes_of(scene_constants);
        } else {
            media_bindings = std::array<rhi::SampledTexture, 2U>{{
                rhi::SampledTexture{
                    composite.media.primary, &media_sampler_},
                rhi::SampledTexture{
                    composite.media.chroma != nullptr
                        ? composite.media.chroma
                        : &material_fallback_,
                    &media_sampler_},
            }};
            composite_draw.fragment_textures = media_bindings;
            composite_draw.fragment_buffers = media_fragment_bindings;
            composite_draw.fragment_uniform = bytes_of(scene_constants);
        }
        composite_draw.batch_uniforms = bytes_of(composite_offsets);
        composite_draw.batch_uniform_stride =
            static_cast<core::u32>(sizeof(BatchConstants));
        composite_draw.batches =
            std::span<const rhi::DrawBatch>{&composite_batch, 1U};
        composite_draw.load = true;
        composite_draw.store_depth = store_depth;
        return graph_.draw_pulled(commands, core::RenderPass::Media,
            color_, clear, depth_, 0.0F, composite_draw);
    };
    // Layers write no depth when composited, so two that cross would each be
    // drawn whole over the other in sorted order. First, every layer that a
    // later one could pass behind writes the depth of its fully opaque pixels,
    // so the later one is hidden there. Only Normal layers hide what is
    // behind them; the last layer has nothing after it to hide; a layer
    // below full opacity has no opaque pixels; and wireframe draws edges.
    for (std::size_t index = 0U;
         !wireframe && index + 1U < composite_draws_.size(); ++index) {
        const auto& composite = composite_draws_[index];
        if (composite.blend != scene::BlendMode::Normal
            || opacity(composite.instance.style) < 1.0F) {
            continue;
        }
        if (!record_composite(composite, true, true)) return false;
    }
    for (std::size_t index = 0U; index < composite_draws_.size(); ++index) {
        if (!record_composite(composite_draws_[index], false,
                index + 1U < composite_draws_.size() || !particle_draws_.empty())) {
            return false;
        }
    }
    ParticleTargets particle_targets{};
    particle_targets.raster_viewport = raster_viewport;
    // The velocity target holds this frame's motion only where the auxiliary
    // pass ran: Advanced, and not a reflection capture.
    const bool scene_motion = mode_ == SceneRendererMode::AdvancedLinear && !options.reflection_pass;
    if (scene_motion) {
        particle_targets.scene_depth = &velocity_;
    }
    if (scene_motion && scene::antialiasing_for(scene.renderer_settings) == scene::AntialiasingMode::Temporal) {
        // Sprite motion for the temporal resolve, between the same two
        // endpoints as the scene's own motion: the previous frame's canvas
        // matrix, and where particles were then by their velocity.
        particle_targets.velocity = &velocity_;
        particle_targets.canvas_view_projection = render_view.canvas_view_projection;
        particle_targets.previous_view_projection = motion_view_projection;
        particle_targets.motion_valid = view_constants.temporal_parameters[0U] > 0.5F;
        const core::u64 previous_frame = advance_history ? view_history.previous_show_frame
                                                         : view_history.motion_transform_show_frame;
        if (particle_targets.motion_valid && scene.show_frame > previous_frame && scene.show_rate.numerator != 0U) {
            particle_targets.motion_seconds =
                static_cast<float>(static_cast<double>(scene.show_frame - previous_frame) *
                                   scene.show_rate.denominator / scene.show_rate.numerator);
        }
    }
    if (!record_particles(commands,
            raster_viewport.empty()
                ? render_view.matrices
                : scene::ViewMatrices{render_view.matrices.view, render_view.matrices.projection,
                                      render_view.canvas_view_projection},
            particle_targets)) {
        return false;
    }

    if (mode_ == SceneRendererMode::AdvancedLinear) {
        // Coverage samples the colour target, so this one
        // genuinely has to follow the colour pass.
        const std::array<rhi::SampledTexture, 1U> coverage_sources{{
            rhi::SampledTexture{&color_, &coverage_sampler_},
        }};
        if (!graph_.fullscreen(commands, core::RenderPass::Coverage,
                coverage_, rhi::Color{}, coverage_pipeline_, coverage_sources,
                {}, false)) {
            return false;
        }
    }

    if (!hold_history) {
        const bool same_global_frame = history_valid_
            && previous_scene_ == scene.id
            && previous_show_frame_ == scene.show_frame;
        const bool same_global_transforms = same_global_frame
            && previous_transforms_ == next_transforms_;
        if (!same_global_frame) {
            const bool sequential_global_frame = history_valid_
                && previous_scene_ == scene.id
                && previous_show_frame_
                    != std::numeric_limits<core::u64>::max()
                && scene.show_frame == previous_show_frame_ + 1U;
            if (sequential_global_frame) {
                // Preserve N while committing N+1. A later mapped view for
                // N+1 still needs N even though another view recorded first.
                motion_source_transforms_.swap(previous_transforms_);
                motion_source_show_frame_ = previous_show_frame_;
                previous_transforms_.swap(next_transforms_);
            } else {
                // A discontinuity has no valid motion source. Current==prior
                // keeps Initialize/Reset velocity exactly zero and retryable.
                previous_transforms_.swap(next_transforms_);
                motion_source_transforms_.assign(
                    previous_transforms_.begin(), previous_transforms_.end());
                motion_source_show_frame_ = scene.show_frame;
            }
            previous_scene_ = scene.id;
            previous_show_frame_ = scene.show_frame;
            history_valid_ = true;
        } else if (!same_global_transforms) {
            // A same-frame visual edit is a reset. Replace both retained
            // generations so no view can accidentally reuse pre-edit motion.
            previous_transforms_.swap(next_transforms_);
            motion_source_transforms_.assign(
                previous_transforms_.begin(), previous_transforms_.end());
            motion_source_show_frame_ = scene.show_frame;
        }

        if (advance_history) {
            view_history.motion_source_view_projection =
                view_history.previous_view_projection;
            view_history.motion_transform_show_frame =
                view_history.previous_show_frame;
        } else {
            view_history.motion_source_view_projection =
                render_view.canvas_view_projection;
            view_history.motion_transform_show_frame = scene.show_frame;
        }
        // The canvas-wide matrix, because this is what the *next* frame reads
        // as its previous endpoint and both endpoints have to live in the same
        // node-independent space.
        view_history.previous_view_projection =
            render_view.canvas_view_projection;
        view_history.previous_scene = scene.id;
        view_history.previous_show_frame = scene.show_frame;
        view_history.previous_show_rate = scene.show_rate;
        view_history.valid = true;
    }
    return true;
}

} // namespace mediaserver::render
