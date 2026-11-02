#include "preview.hpp"

#include <mediaserver/scene/math.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace ajfx::studio {
namespace {

namespace render = ms::render;
namespace rhi = ms::rhi;
namespace scene = ms::scene;

constexpr float two_pi = 6.28318530718F;
constexpr float field_of_view = preview_field_of_view;
// History keys for the stage props, apart from any effect.
constexpr std::uint64_t prop_identity = 0x414A'4658'5354'0000ULL;

std::string_view describe(render::RenderError error) {
    switch (error) {
    case render::RenderError::Unavailable: return "the GPU could not be used";
    case render::RenderError::ShaderFormatUnsupported: return "this build has no shaders for the GPU";
    case render::RenderError::CapabilityUnsupported: return "the GPU lacks a feature the renderer needs";
    case render::RenderError::InvalidConfiguration: return "the renderer was set up wrongly";
    case render::RenderError::AllocationFailed: return "out of GPU memory";
    case render::RenderError::BackendFailure: return "the GPU reported an error";
    }
    return "the renderer failed";
}

render::RendererLimits preview_limits() {
    render::RendererLimits limits{};
    // An edit restarts the effect; catching up with a playhead up to twenty
    // seconds in takes one frame rather than seconds of holding, and
    // scrubbing back lands on a checkpoint at most two seconds away.
    limits.particles.step_budget = 1200U;
    limits.particles.checkpoint_interval = 120U;
    limits.particles.checkpoint_count = 12U;
    return limits;
}

scene::Mat4 box_world(scene::Vec3 centre, scene::Vec3 size) {
    return scene::multiply(scene::translation(centre), scene::scaling(size));
}

void add_prop(scene::Scene& into, scene::Primitive primitive, const scene::Mat4& world, scene::Vec3 colour,
              float roughness, std::uint64_t index) {
    scene::MeshInstance prop{};
    prop.mesh = scene::MeshKey{primitive, primitive == scene::Primitive::Sphere || primitive == scene::Primitive::Cylinder
                                              ? 32U
                                              : 1U};
    prop.world = world;
    prop.normal = scene::normal_basis(world);
    prop.material.base_color = colour;
    prop.material.roughness = roughness;
    prop.source = ms::doc::NodeId{prop_identity + index};
    prop.bake_static = false;
    into.instances.push_back(std::move(prop));
}

scene::DirectionalLight light_from(scene::Vec3 towards, scene::Vec3 colour, float intensity) {
    scene::DirectionalLight light{};
    light.direction = -scene::normalize(towards);
    light.color = colour;
    light.intensity = intensity;
    light.bake_indirect = false;
    return light;
}

// The renderer's temporal history sees the props and particles it draws, but
// not the camera's pose, the sky or the lights: a host says those changed
// through the document digest. While playing, the camera's motion reprojects
// the history instead, so the pose joins the digest only when paused.
std::uint64_t stage_digest(const PreviewFrame& frame) {
    std::uint64_t state = 0xcbf29ce484222325ULL;
    const auto mix = [&state](std::uint64_t value) {
        for (std::uint32_t byte = 0U; byte < 8U; ++byte) {
            state = (state ^ ((value >> (byte * 8U)) & 0xFFU)) * 0x100000001b3ULL;
        }
    };
    const auto mix_float = [&mix](float value) { mix(std::bit_cast<std::uint32_t>(value)); };
    mix(frame.revision);
    mix(static_cast<std::uint64_t>(frame.backdrop));
    mix_float(frame.glow);
    mix(frame.figure ? 1U : 0U);
    mix(frame.staging_floor ? 1U : 0U);
    mix(frame.paused ? 1U : 0U);
    if (frame.paused) {
        mix_float(frame.orbit.yaw);
        mix_float(frame.orbit.pitch);
        mix_float(frame.orbit.distance);
        mix_float(frame.orbit.target.x);
        mix_float(frame.orbit.target.y);
        mix_float(frame.orbit.target.z);
    }
    return state;
}

// The preview's one placement of the effect.
scene::ParticleEffect preview_placement(const PreviewFrame& frame) {
    scene::ParticleEffect effect{};
    effect.world = scene::identity();
    effect.source = frame.package;
    effect.source_node = ms::doc::NodeId{prop_identity + 1000U};
    effect.seed = 1U;
    effect.parameters = frame.parameters;
    return effect;
}

} // namespace

std::uint32_t preview_instance_seed() {
    return scene::particle_instance_seed(preview_placement(PreviewFrame{}));
}

Orbit frame_bounds(const Orbit& from, const Bounds& bounds, float aspect) {
    Orbit orbit = from;
    scene::Vec3 low{-0.6F, 0.0F, -0.6F};
    scene::Vec3 high{0.6F, 1.8F, 0.6F};
    if (bounds.valid) {
        low = scene::Vec3{bounds.minimum.x, bounds.minimum.y, bounds.minimum.z};
        high = scene::Vec3{bounds.maximum.x, bounds.maximum.y, bounds.maximum.z};
    }
    // The floor is part of the picture: frame from it up.
    low.y = std::min(low.y, 0.0F);
    orbit.target = (low + high) * 0.5F;
    const scene::Vec3 half = (high - low) * 0.5F;
    // Fit the box's height to the vertical field of view and its breadth (as
    // seen turning round it) to the horizontal one, then stand off by its
    // depth so the near side fits too.
    const float breadth = std::max(std::sqrt(half.x * half.x + half.z * half.z), 0.15F);
    const float tall = std::max(half.y, 0.15F);
    const float vertical = field_of_view * two_pi * 0.5F;
    const float horizontal = std::atan(std::tan(vertical) * std::max(aspect, 0.1F));
    const float fit = std::max(tall / std::tan(vertical), breadth / std::tan(horizontal)) + breadth * 0.5F;
    orbit.distance = std::clamp(fit * 1.12F, 0.3F, 400.0F);
    return orbit;
}

std::unique_ptr<Preview> Preview::create(rhi::Device& device, std::string& error) {
    if (!render::CompositorSceneExecutor::shaders_available() || !render::OutputTransform::shaders_available()) {
        error = "This build has no renderer shaders";
        return nullptr;
    }
    if (device.capabilities().backend != rhi::BackendKind::D3d12 || !device.capabilities().advanced_raster()) {
        error = "The preview needs the Advanced renderer, which needs a Direct3D 12 GPU";
        return nullptr;
    }
    auto executor = render::CompositorSceneExecutor::create(device, 64U, 64U, preview_limits(),
                                                            render::MediaCacheConfig{}, rhi::TextureFormat::Rgba16Float);
    if (!executor) {
        error = "The renderer could not start: " + std::string{describe(executor.error())};
        return nullptr;
    }
    auto display = render::OutputTransform::create(device, 64U, 64U);
    if (!display) {
        error = "The display transform could not start: " + std::string{describe(display.error())};
        return nullptr;
    }
    return std::unique_ptr<Preview>{new Preview{device, std::move(executor).value(), std::move(display).value()}};
}

Preview::Preview(rhi::Device& device, render::CompositorSceneExecutor executor, render::OutputTransform display)
    : device_{&device}, executor_{std::move(executor)}, display_{std::move(display)} {}

scene::Scene Preview::scene_for(const PreviewFrame& frame) const {
    scene::Scene stage{};
    stage.canvas = scene::Canvas{frame.width, frame.height};
    stage.show_rate = scene::MediaFrameRate{60U, 1U};
    stage.show_frame = frame.frame;
    stage.temporal_document_digest = stage_digest(frame);
    stage.asset_refresh_generation = frame.package_generation;

    scene::RendererSettings& settings = stage.renderer_settings;
    settings.backend = scene::RendererBackend::Advanced;
    // As on the wall. A paused preview still takes in an edit: a held frame
    // resets its history when the particles it draws change.
    settings.taau = true;
    settings.bloom_strength = std::clamp(frame.glow, 0.0F, scene::max_bloom_strength);
    // Bloom's apron (256 pixels once the shorter side reaches 512) plus the
    // antialiasing and occlusion aprons passes the default limit, which is
    // meant for walls split across machines. The preview is the whole canvas
    // on one machine, where the apron is clipped to the canvas and costs
    // nothing, so it may take the largest.
    settings.slice_apron_limit = scene::max_slice_apron;

    // The camera, orbiting its target.
    const float yaw = frame.orbit.yaw * two_pi;
    const float pitch = frame.orbit.pitch * two_pi;
    const scene::Vec3 offset{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    stage.camera.target = frame.orbit.target;
    stage.camera.eye = frame.orbit.target + offset * frame.orbit.distance;
    stage.camera.field_of_view = field_of_view;
    stage.camera.near_plane = std::max(frame.orbit.distance * 0.01F, 0.01F);
    stage.camera.far_plane = std::max(frame.orbit.distance * 40.0F, 100.0F);

    scene::Environment& environment = stage.environment;
    environment.baked.enabled = false;
    environment.planar.enabled = false;
    environment.fog_density = 0.0F;
    scene::Vec3 floor_colour{0.24F, 0.24F, 0.24F};
    scene::Vec3 line_colour{0.13F, 0.13F, 0.13F};
    switch (frame.backdrop) {
    case Backdrop::Studio:
        environment.sky_color = scene::Vec3{0.20F, 0.21F, 0.23F};
        environment.ground_color = scene::Vec3{0.08F, 0.08F, 0.08F};
        environment.ambient = 1.0F;
        stage.directional_lights.push_back(
            light_from(scene::Vec3{-0.55F, 0.75F, 0.60F}, scene::Vec3{1.0F, 0.96F, 0.90F}, 1.6F));
        stage.directional_lights.push_back(
            light_from(scene::Vec3{0.80F, 0.30F, 0.55F}, scene::Vec3{0.86F, 0.91F, 1.0F}, 0.45F));
        stage.directional_lights.push_back(
            light_from(scene::Vec3{0.20F, 0.65F, -0.90F}, scene::Vec3{1.0F, 1.0F, 1.0F}, 1.0F));
        break;
    case Backdrop::Night:
        environment.sky_color = scene::Vec3{0.012F, 0.016F, 0.03F};
        environment.ground_color = scene::Vec3{0.006F, 0.006F, 0.008F};
        environment.ambient = 1.0F;
        stage.directional_lights.push_back(
            light_from(scene::Vec3{0.45F, 0.8F, -0.4F}, scene::Vec3{0.55F, 0.65F, 0.9F}, 0.16F));
        floor_colour = scene::Vec3{0.2F, 0.2F, 0.21F};
        line_colour = scene::Vec3{0.09F, 0.09F, 0.1F};
        break;
    case Backdrop::Void:
        environment.sky_color = scene::Vec3{};
        environment.ground_color = scene::Vec3{};
        environment.ambient = 0.0F;
        break;
    }

    if (frame.backdrop != Backdrop::Void) {
        // A floor ruled every metre, the lines through the origin a shade
        // lighter, and a 1.8 m figure to judge size by.
        constexpr int lines = 8;
        constexpr float reach = static_cast<float>(lines);
        if (frame.staging_floor) {
            add_prop(stage, scene::Primitive::Plane, box_world(scene::Vec3{}, scene::Vec3{reach * 2.0F, 1.0F, reach * 2.0F}),
                     floor_colour, 0.85F, 0U);
        }
        std::uint64_t index = 1U;
        for (int line = -lines; line <= lines; ++line) {
            const float at = static_cast<float>(line);
            const scene::Vec3 colour = line == 0 ? line_colour * 1.8F : line_colour;
            add_prop(stage, scene::Primitive::Box, box_world(scene::Vec3{0.0F, 0.001F, at}, scene::Vec3{reach * 2.0F, 0.002F, 0.012F}),
                     colour, 0.9F, index++);
            add_prop(stage, scene::Primitive::Box, box_world(scene::Vec3{at, 0.001F, 0.0F}, scene::Vec3{0.012F, 0.002F, reach * 2.0F}),
                     colour, 0.9F, index++);
        }
        if (frame.figure) {
            constexpr float radius = 0.2F;
            constexpr float x = -1.4F;
            const scene::Vec3 colour{0.5F, 0.5F, 0.48F};
            add_prop(stage, scene::Primitive::Cylinder,
                     box_world(scene::Vec3{x, 0.9F, 0.0F}, scene::Vec3{radius * 2.0F, 1.8F - radius * 2.0F, radius * 2.0F}),
                     colour, 0.6F, 200U);
            add_prop(stage, scene::Primitive::Sphere,
                     box_world(scene::Vec3{x, radius, 0.0F}, scene::Vec3{radius * 2.0F, radius * 2.0F, radius * 2.0F}),
                     colour, 0.6F, 201U);
            add_prop(stage, scene::Primitive::Sphere,
                     box_world(scene::Vec3{x, 1.8F - radius, 0.0F}, scene::Vec3{radius * 2.0F, radius * 2.0F, radius * 2.0F}),
                     colour, 0.6F, 202U);
        }
    }

    if (!frame.package.empty()) {
        stage.particle_effects.push_back(preview_placement(frame));
    }
    return stage;
}

const rhi::Texture* Preview::render(rhi::CommandList& commands, const PreviewFrame& frame) {
    status_ = {};
    if (frame.width == 0U || frame.height == 0U) {
        return nullptr;
    }
    if (executor_.width() != frame.width || executor_.height() != frame.height) {
        // Targets change size only between frames the GPU has finished.
        device_->wait_idle();
        if (!executor_.resize(*device_, frame.width, frame.height) ||
            !display_.resize(*device_, frame.width, frame.height)) {
            status_.issue = "The preview could not be resized: out of GPU memory";
            return nullptr;
        }
    }
    const scene::Scene stage = scene_for(frame);
    const render::CompositorSceneResult rendered = executor_.execute(commands, stage, scene::whole(stage.canvas));
    if (!rendered) {
        // The renderer's own reason, where it gave one.
        const render::RendererPassStatus& passes = executor_.renderer_passes();
        std::string_view reason = describe(rendered.error());
        if (!executor_.renderer_readiness().satisfied) {
            reason = executor_.renderer_readiness().headline();
        } else if (!passes.bloom_refusal.empty()) {
            reason = passes.bloom_refusal;
        } else if (!passes.fog_refusal.empty()) {
            reason = passes.fog_refusal;
        } else if (!passes.motion_blur_reason.empty()) {
            reason = passes.motion_blur_reason;
        }
        status_.issue = "The preview could not draw: " + std::string{reason};
        return nullptr;
    }
    // The display transform has no file of its own here, so nothing it reads
    // changes with the effect's package. Keyed to the package's generation,
    // it would rebuild its lookup table on every edit.
    display_.adopt_color(stage.renderer_settings.color, 1U);
    if (!display_.render(commands, *rendered.value(), scene::CanvasRect{0U, 0U, frame.width, frame.height},
                         executor_.scene_linear_output())) {
        status_.issue = "The preview could not reach the screen";
        return nullptr;
    }
    const render::RenderStats& stats = executor_.renderer_passes().geometry;
    status_.drawing = stats.particles_drawn != 0U;
    status_.waiting = stats.particles_pending != 0U || stats.particles_behind != 0U;
    if (stats.particles_failed != 0U || stats.particles_unsupported != 0U) {
        status_.issue = stats.particle_issue.empty() ? std::string{"The renderer refused the effect"} : stats.particle_issue;
    }
    return &display_.target();
}

} // namespace ajfx::studio
