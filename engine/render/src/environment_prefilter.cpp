#include <mediaserver/render/environment_prefilter.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/ibl_brdf_lut_comp_dxil.h>
#include <mediaserver/shaders/physical_sky_comp_dxil.h>
#include <mediaserver/shaders/ibl_equirect_to_cube_comp_dxil.h>
#include <mediaserver/shaders/ibl_irradiance_sh_comp_dxil.h>
#include <mediaserver/shaders/ibl_specular_prefilter_comp_dxil.h>
#endif

#include <algorithm>
#include <utility>

namespace mediaserver::render {
namespace {

constexpr core::u32 cube_faces = 6U;
// Nine coefficients, one texel each.
constexpr core::u32 sh_coefficients = 9U;
// Matches numthreads in the equirect, prefilter and BRDF shaders.
constexpr core::u32 tile = 8U;
// Matches numthreads in the irradiance shader, which is one group by design.
constexpr core::u32 irradiance_threads = 64U;

[[nodiscard]] core::u32 groups_for(core::u32 extent) noexcept {
    return (extent + tile - 1U) / tile;
}

struct FaceParameters final {
    core::u32 width{};
    core::u32 height{};
    core::u32 face{};
    float rotation{};
};

struct IrradianceParameters final {
    core::u32 face_size{};
    core::u32 padding0{};
    core::u32 padding1{};
    core::u32 padding2{};
};

struct PrefilterParameters final {
    core::u32 width{};
    core::u32 height{};
    core::u32 face{};
    core::u32 samples{};
    float roughness{};
    float source_face_size{};
    core::u32 padding0{};
    core::u32 padding1{};
    float sun_direction_radius[4]{};
    float sun_irradiance[4]{};
};

struct LutParameters final {
    core::u32 width{};
    core::u32 height{};
    core::u32 samples{};
    core::u32 padding{};
};

template <typename T>
[[nodiscard]] std::span<const core::u8> bytes_of(const T& value) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&value), sizeof(T)};
}

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
[[nodiscard]] rhi::ShaderDesc compute_shader(
    std::span<const core::u8> bytecode,
    core::u32 samplers,
    core::u32 storage_textures,
    core::u32 uniforms) noexcept {
    rhi::ShaderDesc desc{};
    desc.stage = rhi::ShaderStage::Compute;
    desc.format = rhi::ShaderFormat::Dxil;
    desc.bytecode = bytecode;
    desc.sampler_count = samplers;
    desc.storage_texture_count = storage_textures;
    desc.uniform_buffer_count = uniforms;
    return desc;
}

template <std::size_t N>
[[nodiscard]] std::span<const core::u8> blob(
    const unsigned char (&value)[N]) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(value), N};
}
#endif

} // namespace

bool EnvironmentPrefilter::shaders_available() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return true;
#else
    return false;
#endif
}

float EnvironmentPrefilter::mip_roughness(
    core::u32 mip, core::u32 mip_count) noexcept {
    if (mip_count <= 1U) {
        return 0.0F;
    }
    // Linear from a mirror at the top to fully rough at the bottom. Linear
    // rather than a perceptual curve because the lit shader inverts this to
    // pick a mip, and the two only agree if the mapping is stated once and is
    // simple enough to invert exactly.
    return static_cast<float>(mip) / static_cast<float>(mip_count - 1U);
}

EnvironmentPrefilter::EnvironmentPrefilter(
    EnvironmentPrefilterLimits limits,
    rhi::Texture cube,
    rhi::Texture irradiance,
    rhi::Texture specular,
    rhi::Texture brdf,
    rhi::Sampler sampler,
    rhi::ComputePipeline equirect,
    rhi::ComputePipeline diffuse,
    rhi::ComputePipeline prefilter,
    rhi::ComputePipeline lut) noexcept
    : limits_(limits),
      cube_(std::move(cube)),
      irradiance_(std::move(irradiance)),
      specular_(std::move(specular)),
      brdf_(std::move(brdf)),
      sampler_(std::move(sampler)),
      equirect_pipeline_(std::move(equirect)),
      irradiance_pipeline_(std::move(diffuse)),
      prefilter_pipeline_(std::move(prefilter)),
      brdf_pipeline_(std::move(lut)) {}

core::Result<EnvironmentPrefilter, RenderError> EnvironmentPrefilter::create(
    rhi::Device& device, EnvironmentPrefilterLimits limits) noexcept {
    using Result = core::Result<EnvironmentPrefilter, RenderError>;
#if !defined(MEDIASERVER_HAS_DXIL_SHADERS)
    static_cast<void>(device);
    static_cast<void>(limits);
    return Result::failure(RenderError::ShaderFormatUnsupported);
#else
    if (!device.supports(rhi::ShaderFormat::Dxil)) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }
    // Compute and storage images are what this whole path is built on. A
    // device without them fails here rather than at the first dispatch.
    if (!device.capabilities().compute
        || !device.capabilities().storage_images) {
        return Result::failure(RenderError::CapabilityUnsupported);
    }
    if (limits.face_size == 0U || limits.specular_mips == 0U
        || limits.irradiance_face_size == 0U || limits.brdf_extent == 0U
        || limits.specular_samples == 0U || limits.brdf_samples == 0U) {
        return Result::failure(RenderError::InvalidConfiguration);
    }
    // Every specular mip must have at least one texel, or the chain names a
    // level nothing can be written into.
    if ((limits.face_size >> (limits.specular_mips - 1U)) == 0U) {
        return Result::failure(RenderError::InvalidConfiguration);
    }

    rhi::TextureDesc cube_desc{};
    cube_desc.width = limits.face_size;
    cube_desc.height = limits.face_size;
    cube_desc.format = rhi::TextureFormat::Rgba16Float;
    cube_desc.sampled = true;
    cube_desc.storage_write = true;
    cube_desc.array_layers = cube_faces;
    cube_desc.cube = true;
    // A mip chain on the base cube as well as on the specular result: the
    // prefilter samples a lower level for a wide lobe, which is what keeps a
    // rough mip from turning a bright texel into a flickering firefly.
    core::u32 base_mips = 1U;
    while ((limits.face_size >> base_mips) > 0U) {
        ++base_mips;
    }
    cube_desc.mip_levels = base_mips;
    auto cube = device.create_texture(cube_desc);

    rhi::TextureDesc specular_desc = cube_desc;
    specular_desc.mip_levels = limits.specular_mips;
    auto specular = device.create_texture(specular_desc);

    rhi::TextureDesc irradiance_desc{};
    irradiance_desc.width = sh_coefficients;
    irradiance_desc.height = 1U;
    // Full float: nine coefficients carry the entire diffuse environment, and
    // a half-float rounding here is a tint on every unlit surface in the shot.
    irradiance_desc.format = rhi::TextureFormat::Rgba32Float;
    irradiance_desc.sampled = true;
    irradiance_desc.storage_write = true;
    auto irradiance = device.create_texture(irradiance_desc);

    rhi::TextureDesc brdf_desc{};
    brdf_desc.width = limits.brdf_extent;
    brdf_desc.height = limits.brdf_extent;
    brdf_desc.format = rhi::TextureFormat::Rgba16Float;
    brdf_desc.sampled = true;
    brdf_desc.storage_write = true;
    auto brdf = device.create_texture(brdf_desc);

    auto sampler = device.create_sampler(rhi::Filter::Linear);
    if (!cube || !specular || !irradiance || !brdf || !sampler) {
        return Result::failure(RenderError::AllocationFailed);
    }

    auto equirect = device.create_compute_pipeline(
        compute_shader(blob(mediaserver_ibl_equirect_to_cube_comp_dxil),
                       1U, 1U, 1U));
    auto diffuse = device.create_compute_pipeline(
        compute_shader(blob(mediaserver_ibl_irradiance_sh_comp_dxil),
                       1U, 1U, 1U));
    auto prefilter = device.create_compute_pipeline(
        compute_shader(blob(mediaserver_ibl_specular_prefilter_comp_dxil),
                       1U, 1U, 1U));
    auto lut = device.create_compute_pipeline(
        compute_shader(blob(mediaserver_ibl_brdf_lut_comp_dxil), 0U, 1U, 1U));
    if (!equirect || !diffuse || !prefilter || !lut) {
        return Result::failure(RenderError::BackendFailure);
    }

    EnvironmentPrefilter result{
        limits, std::move(cube).value(), std::move(irradiance).value(),
        std::move(specular).value(), std::move(brdf).value(),
        std::move(sampler).value(), std::move(equirect).value(),
        std::move(diffuse).value(), std::move(prefilter).value(),
        std::move(lut).value()};
    rhi::TextureDesc table_desc{};
    table_desc.width = 256; table_desc.height = 128;
    table_desc.format = rhi::TextureFormat::Rgba16Float;
    table_desc.sampled = true; table_desc.storage_write = true;
    auto table = device.create_texture(table_desc);
    auto physical = device.create_compute_pipeline(compute_shader(blob(mediaserver_physical_sky_comp_dxil),0U,1U,1U));
    if (!table || !physical) return Result::failure(RenderError::AllocationFailed);
    result.physical_table_ = std::move(table).value();
    result.physical_pipeline_ = std::move(physical).value();
    return Result::success(std::move(result));
#endif
}

bool EnvironmentPrefilter::prepare(rhi::CommandList& commands) noexcept {
    graph_.begin();
    if (brdf_recording_) {
        const auto outcome = brdf_recording_.outcome();
        if (outcome == rhi::CommandRecordingOutcome::Canceled || outcome == rhi::CommandRecordingOutcome::Unavailable) brdf_ready_ = false;
        if (outcome != rhi::CommandRecordingOutcome::Pending) brdf_recording_ = {};
    }

    if (brdf_ready_) {
        // Nothing about this table changes, so recording it again would be
        // work that produces the bytes already there.
        return true;
    }
    const LutParameters parameters{
        limits_.brdf_extent, limits_.brdf_extent, limits_.brdf_samples, 0U};
    const std::array<rhi::StorageTexture, 1U> bindings{
        rhi::StorageTexture{&brdf_, 0U, 0U, true}};
    rhi::ComputeDispatch work{};
    work.pipeline = &brdf_pipeline_;
    work.storage_textures = bindings;
    work.uniform = bytes_of(parameters);
    work.groups_x = groups_for(limits_.brdf_extent);
    work.groups_y = groups_for(limits_.brdf_extent);
    if (!graph_.dispatch(commands, core::RenderPass::Environment, work)) {
        return false;
    }
    brdf_recording_ = commands.recording_token();
    brdf_ready_ = true;
    return true;
}

bool EnvironmentPrefilter::record(
    rhi::CommandList& commands,
    const rhi::Texture& equirect,
    scene::Turns rotation) noexcept {
    return record_source(commands, equirect, rotation, nullptr, 1.0F);
}

bool EnvironmentPrefilter::record_physical(rhi::CommandList& commands,
    const scene::PhysicalSky& value, float scale) noexcept {
    const auto sky = scene::clamped(value);
    const auto direction = scene::physical_sun_direction(sky);
    const auto direct = scene::physical_sun_illuminance(sky)*scale;
    struct Parameters { float direction[4]; float solar_ground[4]; float direct[4]; };
    const Parameters parameters{{direction.x,direction.y,direction.z,sky.haze},
        {sky.solar_illuminance*scale,sky.ground_albedo,256,128},{direct.x,direct.y,direct.z,0}};
    graph_.begin();
    environment_ready_ = false;
    const std::array<rhi::StorageTexture,1> targets{rhi::StorageTexture{&physical_table_,0,0,true}};
    rhi::ComputeDispatch work{};
    work.pipeline = &physical_pipeline_; work.storage_textures = targets;
    work.uniform = bytes_of(parameters); work.groups_x = 32; work.groups_y = 16;
    if (!graph_.dispatch(commands,core::RenderPass::Environment,work)) return false;
    return record_source(commands,physical_table_,0,&sky,scale);
}

bool EnvironmentPrefilter::record_source(rhi::CommandList& commands,
    const rhi::Texture& equirect, scene::Turns rotation,
    const scene::PhysicalSky* sky, float scale) noexcept {
    graph_.begin();
    if (!equirect) {
        return false;
    }
    environment_ready_ = false;

    const std::array<rhi::SampledTexture, 1U> equirect_source{
        rhi::SampledTexture{&equirect, &sampler_}};

    // Face by face into mip zero of the base cube.
    for (core::u32 face = 0U; face < cube_faces; ++face) {
        const FaceParameters parameters{
            limits_.face_size, limits_.face_size, face, rotation};
        const std::array<rhi::StorageTexture, 1U> bindings{
            rhi::StorageTexture{&cube_, 0U, face, true}};
        rhi::ComputeDispatch work{};
        work.pipeline = &equirect_pipeline_;
        work.sampled_textures = equirect_source;
        work.storage_textures = bindings;
        work.uniform = bytes_of(parameters);
        work.groups_x = groups_for(limits_.face_size);
        work.groups_y = groups_for(limits_.face_size);
        if (!graph_.dispatch(commands, core::RenderPass::Environment, work)) {
            return false;
        }
    }

    // The base cube's own mip chain, produced by running the same equirect
    // projection at each smaller size rather than by downsampling.
    //
    // Resampling the source is exact where a box filter of the level above
    // would accumulate error down the chain, and it costs nothing here because
    // this runs once per environment rather than per frame.
    core::u32 level = 1U;
    for (core::u32 size = limits_.face_size >> 1U; size > 0U; size >>= 1U) {
        for (core::u32 face = 0U; face < cube_faces; ++face) {
            const FaceParameters parameters{size, size, face, rotation};
            const std::array<rhi::StorageTexture, 1U> bindings{
                rhi::StorageTexture{&cube_, level, face, true}};
            rhi::ComputeDispatch work{};
            work.pipeline = &equirect_pipeline_;
            work.sampled_textures = equirect_source;
            work.storage_textures = bindings;
            work.uniform = bytes_of(parameters);
            work.groups_x = groups_for(size);
            work.groups_y = groups_for(size);
            if (!graph_.dispatch(commands, core::RenderPass::Environment, work)) {
                return false;
            }
        }
        ++level;
    }

    const std::array<rhi::SampledTexture, 1U> cube_source{
        rhi::SampledTexture{&cube_, &sampler_}};

    // Diffuse. One group, one dispatch, a fixed reduction order.
    {
        const IrradianceParameters parameters{
            std::min(limits_.irradiance_face_size, limits_.face_size),
            0U, 0U, 0U};
        const std::array<rhi::StorageTexture, 1U> bindings{
            rhi::StorageTexture{&irradiance_, 0U, 0U, true}};
        rhi::ComputeDispatch work{};
        work.pipeline = &irradiance_pipeline_;
        work.sampled_textures = cube_source;
        work.storage_textures = bindings;
        work.uniform = bytes_of(parameters);
        work.groups_x = 1U;
        work.groups_y = 1U;
        static_assert(irradiance_threads == 64U);
        if (!graph_.dispatch(commands, core::RenderPass::Environment, work)) {
            return false;
        }
    }

    // Specular, one dispatch per face per mip.
    for (core::u32 mip = 0U; mip < limits_.specular_mips; ++mip) {
        const core::u32 size = std::max(limits_.face_size >> mip, 1U);
        const float roughness = mip_roughness(mip, limits_.specular_mips);
        for (core::u32 face = 0U; face < cube_faces; ++face) {
            PrefilterParameters parameters{
                size,
                size,
                face,
                limits_.specular_samples,
                roughness,
                static_cast<float>(limits_.face_size),
                0U,
                0U};
            if (sky) {
                const auto axis = scene::physical_sun_direction(*sky);
                const auto direct = scene::physical_sun_illuminance(*sky)*scale;
                parameters.sun_direction_radius[0]=axis.x; parameters.sun_direction_radius[1]=axis.y;
                parameters.sun_direction_radius[2]=axis.z; parameters.sun_direction_radius[3]=sky->angular_radius*6.28318530718F;
                parameters.sun_irradiance[0]=direct.x; parameters.sun_irradiance[1]=direct.y; parameters.sun_irradiance[2]=direct.z;
            }
            const std::array<rhi::StorageTexture, 1U> bindings{
                rhi::StorageTexture{&specular_, mip, face, true}};
            rhi::ComputeDispatch work{};
            work.pipeline = &prefilter_pipeline_;
            work.sampled_textures = cube_source;
            work.storage_textures = bindings;
            work.uniform = bytes_of(parameters);
            work.groups_x = groups_for(size);
            work.groups_y = groups_for(size);
            if (!graph_.dispatch(commands, core::RenderPass::Environment, work)) {
                return false;
            }
        }
    }

    environment_recording_ = commands.recording_token();
    environment_ready_ = true;
    return true;
}

EnvironmentMaps EnvironmentPrefilter::maps() const noexcept {
    if (!environment_ready_ || !brdf_ready_
        || (environment_recording_ && (environment_recording_.outcome() == rhi::CommandRecordingOutcome::Canceled
            || environment_recording_.outcome() == rhi::CommandRecordingOutcome::Unavailable))) {
        // Absent rather than partial. A lit shader given half a prefiltered
        // environment produces a plausible wrong image, which is worse than
        // falling back to the hemisphere it had before.
        return EnvironmentMaps{};
    }
    return EnvironmentMaps{&irradiance_, &specular_, &brdf_};
}

} // namespace mediaserver::render
