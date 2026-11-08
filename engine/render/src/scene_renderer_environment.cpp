// SceneRenderer backgrounds: the sky, the environment as a background, and
// adopting a physical or authored environment.

#include "scene_renderer_internal.hpp"

namespace mediaserver::render {

using namespace scene_detail;

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

} // namespace mediaserver::render
