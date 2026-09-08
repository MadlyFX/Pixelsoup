#include <mediaserver/render/volumetric_fog.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/fog_apply_comp_dxil.h>
#include <mediaserver/shaders/fog_integrate_comp_dxil.h>
#include <mediaserver/shaders/fog_scatter_comp_dxil.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using CreateResult = core::Result<VolumetricFog, RenderError>;

[[nodiscard]] bool shaders_present() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return true;
#else
    return false;
#endif
}

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#define MEDIASERVER_FOG_BYTECODE(symbol)                                      \
    std::span<const core::u8> {                                               \
        reinterpret_cast<const core::u8*>(symbol), sizeof(symbol)             \
    }
#else
#define MEDIASERVER_FOG_BYTECODE(symbol) std::span<const core::u8>{}
#endif

[[nodiscard]] std::span<const core::u8> scatter_bytecode() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return MEDIASERVER_FOG_BYTECODE(mediaserver_fog_scatter_comp_dxil);
#else
    return {};
#endif
}

[[nodiscard]] std::span<const core::u8> integrate_bytecode() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return MEDIASERVER_FOG_BYTECODE(mediaserver_fog_integrate_comp_dxil);
#else
    return {};
#endif
}

[[nodiscard]] std::span<const core::u8> apply_bytecode() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return MEDIASERVER_FOG_BYTECODE(mediaserver_fog_apply_comp_dxil);
#else
    return {};
#endif
}

// Scene-linear, like every intermediate on this path. The volume carries
// radiance and a transmittance that must survive being multiplied into it.
[[nodiscard]] rhi::TextureDesc volume_desc(
    core::u32 width, core::u32 height,
    rhi::TextureFormat format = rhi::TextureFormat::Rgba16Float) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.sampled = true;
    desc.storage_read = true;
    desc.storage_write = true;
    // Also a colour target, because a compositor that ends its frame on this
    // texture copies out of it - and a copy source that was never declared one
    // is refused by the backend, which surfaces as a backend failure at the
    // crop rather than anywhere near the pass that produced it.
    desc.color_target = true;
    return desc;
}

constexpr core::u32 fog_group_size = 8U;

[[nodiscard]] core::u32 groups_for(core::u32 extent) noexcept {
    return (extent + fog_group_size - 1U) / fog_group_size;
}

// fog_common.hlsli: cbuffer FogConstants.
struct FogConstants final {
    float inverse_view_projection[16U]{};
    float grid[4U]{};
    float planes[4U]{};
    float albedo[4U]{};
    float canvas[4U]{};
    float shadow[4U]{};
    float environment[4U]{};
};

static_assert(sizeof(FogConstants) == 160U);

[[nodiscard]] std::span<const core::u8> bytes_of(
    const FogConstants& value) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&value), sizeof(value)};
}

// The constant part of the slope-scaled bias the lit draw applies, in tile
// texels. A froxel has no normal, so the slope term has nothing to scale.
constexpr float shadow_bias_texels = 1.5F;

[[nodiscard]] core::Result<rhi::ComputePipeline, RenderError> make_pipeline(
    rhi::Device& device, std::span<const core::u8> bytecode,
    core::u32 sampled, core::u32 buffers = 0U) noexcept {
    using Result = core::Result<rhi::ComputePipeline, RenderError>;
    rhi::ShaderDesc compute{};
    compute.stage = rhi::ShaderStage::Compute;
    compute.format = rhi::ShaderFormat::Dxil;
    compute.bytecode = bytecode;
    compute.sampler_count = sampled;
    compute.storage_buffer_count = buffers;
    compute.storage_texture_count = 1U;
    compute.uniform_buffer_count = 1U;
    auto pipeline = device.create_compute_pipeline(compute);
    if (!pipeline) {
        return Result::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }
    return Result::success(std::move(pipeline).value());
}

} // namespace

bool VolumetricFog::shaders_available() noexcept { return shaders_present(); }

FogFrame fog_frame_for(const scene::Scene& scene, const SceneRenderView* view,
                       const PaddedSlice& padded, core::u32 target_width,
                       core::u32 target_height) noexcept {
    FogFrame frame{};
    frame.clip_planes = {scene.camera.near_plane, scene.camera.far_plane};
    if (view != nullptr) {
        // The view's own frustum and its own raster. A view-driven frame fills
        // its whole target, so that target is what the froxel columns divide;
        // keying it to the authored canvas would treat a plate as though it
        // were the wall's top-left corner, and a plate is rendered at its own
        // density.
        frame.view_projection = view->matrices.view_projection;
        frame.clip_planes = view->clip_planes.value_or(frame.clip_planes);
        frame.grid_canvas = scene::Canvas{target_width, target_height};
        frame.render =
            scene::CanvasRect{0U, 0U, target_width, target_height};
        return frame;
    }
    // The slice path, and deliberately **unjittered**: the grid partitions the
    // world, not a sample pattern, and jittering it would move every cell
    // boundary a fraction of a pixel each frame until the atmosphere crawled.
    // The temporal resolve still antialiases the fogged frame afterwards,
    // which is where the jitter belongs.
    frame.view_projection =
        scene::view_for_slice(scene.camera, scene.canvas,
                              scene::whole(scene.canvas))
            .view_projection;
    frame.grid_canvas = scene.canvas;
    frame.render = padded.render;
    return frame;
}

VolumetricFog::VolumetricFog(FroxelGridLimits limits, rhi::Texture scatter,
                             rhi::Texture integrated, rhi::Texture target,
                             rhi::ComputePipeline scatter_pipeline,
                             rhi::ComputePipeline integrate_pipeline,
                             rhi::ComputePipeline apply_pipeline,
                             rhi::Sampler sampler,
                             rhi::TextureFormat target_format) noexcept
    : limits_(limits), target_format_(target_format),
      scatter_(std::move(scatter)),
      integrated_(std::move(integrated)), target_(std::move(target)),
      scatter_pipeline_(std::move(scatter_pipeline)),
      integrate_pipeline_(std::move(integrate_pipeline)),
      apply_pipeline_(std::move(apply_pipeline)), sampler_(std::move(sampler)) {
}

CreateResult VolumetricFog::create(rhi::Device& device, core::u32 width,
                                   core::u32 height,
                                   const FroxelGridLimits& limits,
                                   rhi::TextureFormat target_format) noexcept {
    if (width == 0U || height == 0U || limits.width == 0U
        || limits.height == 0U || limits.depth == 0U) {
        return CreateResult::failure(RenderError::InvalidConfiguration);
    }
    if (!shaders_present()) {
        return CreateResult::failure(RenderError::ShaderFormatUnsupported);
    }
    if (!device.capabilities().compute
        || !device.capabilities().storage_images) {
        return CreateResult::failure(RenderError::CapabilityUnsupported);
    }

    // Slices laid left to right, exactly as `pack_display_lut_atlas` lays out
    // the display table - one addressing convention for volumes rather than
    // two.
    const core::u32 atlas_width = limits.width * limits.depth;
    auto scatter = device.create_texture(volume_desc(atlas_width,
                                                     limits.height));
    if (!scatter) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }
    auto integrated = device.create_texture(volume_desc(atlas_width,
                                                        limits.height));
    if (!integrated) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }
    auto target =
        device.create_texture(volume_desc(width, height, target_format));
    if (!target) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }

    // The scatter stage reads the shadow atlas plus the light and shadow-table
    // buffers; the integration reads the scatter volume; the apply reads
    // colour, depth and the integrated volume.
    auto scatter_pipeline = make_pipeline(device, scatter_bytecode(), 2U, 2U);
    if (!scatter_pipeline) {
        return CreateResult::failure(scatter_pipeline.error());
    }
    auto integrate_pipeline = make_pipeline(device, integrate_bytecode(), 1U);
    if (!integrate_pipeline) {
        return CreateResult::failure(integrate_pipeline.error());
    }
    auto apply_pipeline = make_pipeline(device, apply_bytecode(), 3U);
    if (!apply_pipeline) {
        return CreateResult::failure(apply_pipeline.error());
    }

    auto sampler = device.create_sampler(rhi::Filter::Nearest);
    if (!sampler) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }

    return CreateResult::success(VolumetricFog{
        limits, std::move(scatter).value(), std::move(integrated).value(),
        std::move(target).value(), std::move(scatter_pipeline).value(),
        std::move(integrate_pipeline).value(),
        std::move(apply_pipeline).value(), std::move(sampler).value(),
        target_format});
}

bool VolumetricFog::resize(rhi::Device& device, core::u32 width,
                           core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (target_.width() == width && target_.height() == height) {
        return true;
    }
    auto target =
        device.create_texture(volume_desc(width, height, target_format_));
    if (!target) {
        return false;
    }
    target_ = std::move(target).value();
    return true;
}

bool VolumetricFog::record(rhi::CommandList& commands,
                           const rhi::Texture& color, const rhi::Texture& depth,
                           const rhi::Texture& shadow_atlas,
                           const rhi::StorageBuffer& lights,
                           const rhi::StorageBuffer& light_shadows,
                           core::u32 light_count, const scene::Scene& scene,
                           const scene::Mat4& view_projection,
                           scene::Canvas grid_canvas,
                           scene::CanvasRect render,
                           std::optional<SceneClipPlanes> clip_planes,
                           const rhi::Texture* environment) noexcept {
    if (color.width() != target_.width() || color.height() != target_.height()
        || depth.width() != target_.width()
        || depth.height() != target_.height()) {
        ++stats_.refusals;
        return false;
    }

    FogConstants constants{};
    const scene::Mat4 inverse = scene::invert(view_projection);
    for (core::u32 index = 0U; index < 16U; ++index) {
        constants.inverse_view_projection[index] = inverse.m[index];
    }
    constants.grid[0U] = static_cast<float>(limits_.width);
    constants.grid[1U] = static_cast<float>(limits_.height);
    constants.grid[2U] = static_cast<float>(limits_.depth);
    // Height falloff: the base height the air is densest at and below, and
    // the rate it thins above it. Zero falloff is the uniform air every scene
    // had before this existed.
    constants.grid[3U] = scene.environment.fog_height;
    constants.albedo[3U] = scene.environment.fog_height_falloff;
    // The range must belong to the projection that wrote depth. Stage's free
    // camera can differ from the authored camera retained for geometry LOD.
    const SceneClipPlanes planes = clip_planes.value_or(
        SceneClipPlanes{scene.camera.near_plane, scene.camera.far_plane});
    if (!std::isfinite(planes.near_plane) || !std::isfinite(planes.far_plane)
        || !(planes.near_plane > 0.0F) || !(planes.far_plane > planes.near_plane)) {
        ++stats_.refusals;
        return false;
    }
    constants.planes[0U] = planes.near_plane;
    constants.planes[1U] = planes.far_plane;
    // Authored density is optical depth over ten world units. This gives the
    // low end of the operator control useful range at ordinary stage scales.
    constants.planes[2U] = scene.environment.fog_density * 0.1F;
    constants.planes[3U] = scene.environment.fog_anisotropy;
    constants.albedo[0U] = scene.environment.fog_color.x;
    constants.albedo[1U] = scene.environment.fog_color.y;
    constants.albedo[2U] = scene.environment.fog_color.z;

    // The lights are read from the buffer the lit draw uploaded, not rebuilt
    // here. Directional and local alike: the shader tells them apart the same
    // way the fragment stage does, so the air is lit by exactly the lights that
    // lit the surfaces, with exactly the same falloff.
    constants.shadow[0U] = shadow_bias_texels;
    constants.shadow[2U] = static_cast<float>(light_count);
    constants.environment[0U] = environment
        ? std::max(scene.environment.map_intensity, 0.0F) : 0.0F;
    const float background_distance = scene.environment.fog_background_distance;
    constants.environment[1U] = std::isfinite(background_distance)
        ? std::clamp(background_distance, 0.0F, scene::max_fog_background_distance)
        : 0.0F;

    // The raster the frustum above fills, which is the authored canvas on a
    // wall slice and the view's own target on a view-driven frame. Taking it
    // from `scene.canvas` unconditionally would key an ICVFX plate - whose
    // raster is the surface scaled by plate density - as though it were the
    // wall's top-left corner.
    constants.canvas[0U] = static_cast<float>(grid_canvas.width);
    constants.canvas[1U] = static_cast<float>(grid_canvas.height);
    // Absolute canvas origin, so a pixel's froxel is a property of the wall
    // rather than of which node owns that part of it.
    constants.canvas[2U] = static_cast<float>(render.x);
    constants.canvas[3U] = static_cast<float>(render.y);

    const core::u32 columns = groups_for(limits_.width);
    const core::u32 rows = groups_for(limits_.height);

    {
        const std::array<rhi::SampledTexture, 2U> sources{
            rhi::SampledTexture{&shadow_atlas, &sampler_},
            rhi::SampledTexture{environment ? environment : &shadow_atlas, &sampler_}};
        const std::array<const rhi::StorageBuffer*, 2U> buffers{
            &lights, &light_shadows};
        const std::array<rhi::StorageTexture, 1U> targets{
            rhi::StorageTexture{&scatter_, 0U, 0U, true}};
        rhi::ComputeDispatch work{};
        work.pipeline = &scatter_pipeline_;
        work.sampled_textures = sources;
        work.storage_buffers = buffers;
        work.storage_textures = targets;
        work.uniform = bytes_of(constants);
        work.groups_x = columns;
        work.groups_y = rows;
        if (!commands.dispatch(work)) {
            return false;
        }
        ++stats_.dispatches;
    }
    {
        const std::array<rhi::SampledTexture, 1U> sources{
            rhi::SampledTexture{&scatter_, &sampler_}};
        const std::array<rhi::StorageTexture, 1U> targets{
            rhi::StorageTexture{&integrated_, 0U, 0U, true}};
        rhi::ComputeDispatch work{};
        work.pipeline = &integrate_pipeline_;
        work.sampled_textures = sources;
        work.storage_textures = targets;
        work.uniform = bytes_of(constants);
        work.groups_x = columns;
        work.groups_y = rows;
        if (!commands.dispatch(work)) {
            return false;
        }
        ++stats_.dispatches;
    }
    {
        const std::array<rhi::SampledTexture, 3U> sources{{
            rhi::SampledTexture{&color, &sampler_},
            rhi::SampledTexture{&depth, &sampler_},
            rhi::SampledTexture{&integrated_, &sampler_},
        }};
        const std::array<rhi::StorageTexture, 1U> targets{
            rhi::StorageTexture{&target_, 0U, 0U, true}};
        rhi::ComputeDispatch work{};
        work.pipeline = &apply_pipeline_;
        work.sampled_textures = sources;
        work.storage_textures = targets;
        work.uniform = bytes_of(constants);
        work.groups_x = groups_for(target_.width());
        work.groups_y = groups_for(target_.height());
        if (!commands.dispatch(work)) {
            return false;
        }
        ++stats_.dispatches;
    }

    ++stats_.volumes;
    return true;
}

} // namespace mediaserver::render
