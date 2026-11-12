#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <array>
#include <string>

// What the scene renderer says to and about its particle effects, whether or
// not this build can play them. Playing an effect needs the ajfx runtime
// (ParticlePass, ParticlePackageCache), which a build has only when ajfx is
// configured into it; without it a placed effect is counted as unsupported
// and refused readiness with that reason.
namespace mediaserver::render {

// Whether this build includes the particle runtime.
#if defined(MEDIASERVER_HAS_AJFX)
inline constexpr bool particle_runtime_built = true;
#else
inline constexpr bool particle_runtime_built = false;
#endif

enum class ParticleErrorCode : core::u8 {
    // The device cannot simulate particles (no writable buffers or indirect
    // dispatch, or no shaders for it): the Basic renderer, for one.
    Unsupported,
    InvalidEffect,
    DeviceFailure,
};

struct ParticleError final {
    ParticleErrorCode code{};
    std::string message;
};

// Particle light slots one scene render holds (see ParticlePass::write_lights).
inline constexpr core::u32 max_particle_lights = 64U;

struct ParticleSettings final {
    // Steps advance may take in one frame. Normal playback needs
    // ceil(simulation rate / show rate): three at 24 fps and 60 steps a second.
    core::u32 step_budget{16U};
    // A checkpoint every this many steps, the most recent `checkpoint_count`
    // kept. A seek back lands on the nearest one at or before its target.
    core::u32 checkpoint_interval{120U};
    core::u32 checkpoint_count{4U};
    // Threads stepping rigid physics, its own thread included. Made when the
    // first effect with rigid physics is instanced.
    core::u32 rigid_threads{2U};
    // Steps the rigid physics thread works ahead of the show frame, so
    // playback finds each step ready.
    core::u32 rigid_lead{30U};
    // The lead instead while a host acts on the instance (revise with
    // actions): short, so an action stamped just past it (acting_tick) lands
    // on a tick the thread has not stepped and invalidates nothing, yet long
    // enough that playback still finds each step ready: more than a show
    // frame's steps (one at 60 fps and 60 steps a second, two or three at 24).
    core::u32 acting_rigid_lead{4U};
};

// The first tick an action may be stamped with so that it changes nothing
// an instance already holds, when the last advance asked for `target` and
// its rigid physics runs `lead` steps ahead (ParticleSettings): one past
// what the rigid physics thread may have stepped.
[[nodiscard]] constexpr core::u64 acting_tick(core::u64 target, core::u32 lead) noexcept {
    return target + lead + 1U;
}

// The scene's light for lit emitters (ajfx::Lighting), gathered by the scene
// renderer each frame: its light records, its ambient hemisphere and the
// directional shadow cascades it drew. Lit particles sum the lights per
// particle and sample the cascades per pixel; unlit emitters never read it.
// Without one (`ParticleTargets::lighting` null) a lit emitter is drawn under
// one white light at the camera, as a reference viewer without a scene would.
struct ParticleLighting final {
    // light.hlsli records, `light_count` of them, as the scene's lit pass
    // reads them.
    const rhi::StorageBuffer* lights{};
    core::u32 light_count{};
    // The particles' own lights this frame (light.hlsli records, written by
    // write_lights), `particle_light_count` of them: grid volumes take them
    // with the scene's local lights, so a fire lights its smoke.
    const rhi::StorageBuffer* particle_lights{};
    core::u32 particle_light_count{};
    // rgb: the sky half of the ambient hemisphere; a: overall ambient strength.
    std::array<float, 4U> sky_ambient{};
    // rgb: the ground half; a: the exposure the scene applies to what it
    // lights, so a lit particle sits with the surfaces around it.
    std::array<float, 4U> ground_exposure{1.0F, 1.0F, 1.0F, 1.0F};
    // Directional shadow cascades drawn this frame, nearest first; zero when
    // none were. Each is a reversed-depth map with its light projection and
    // the depth of one of its texels.
    core::u32 cascade_count{};
    std::array<const rhi::Texture*, scene::max_shadow_cascades> cascades{};
    std::array<scene::Mat4, scene::max_shadow_cascades> cascade_view_projection{};
    std::array<float, scene::max_shadow_cascades> cascade_texel_depth{};
    // Shadow bias in texels. A sprite has no receiver plane to lean on, so
    // it takes a constant few.
    float shadow_bias_texels{3.0F};
};

// Where a pass draws. Colour and depth must match in size. With `load` the
// targets keep what the scene drew; otherwise they are cleared first.
struct ParticleTargets final {
    rhi::Texture* color{};
    rhi::Texture* depth{};
    bool load{true};
    rhi::Color clear{};
    // Reversed depth: 0 is the far plane.
    float depth_clear{0.0F};
    rhi::ViewportRect viewport{};
    rhi::RasterViewport raster_viewport{};
    // The scene's velocity target (RGBA32F, the colour's size), which
    // draw_motion writes each sprite's motion into where it covers at least
    // half a pixel. The two canvas-wide matrices are the motion's endpoints,
    // without jitter; `motion_seconds` back is where particles were (by their
    // velocity) at the previous frame, and with `motion_valid` false there is
    // no previous frame to move from.
    rhi::Texture* velocity{};
    // The scene's depth for soft particles: a texture of the velocity
    // product, whose z is reversed device depth. None draws hard edges. It
    // must hold the scene alone, so draw every effect before any motion.
    const rhi::Texture* scene_depth{};
    scene::Mat4 canvas_view_projection{};
    scene::Mat4 previous_view_projection{};
    float motion_seconds{};
    bool motion_valid{};
    // The scene's light for lit emitters; null lights them from the camera.
    const ParticleLighting* lighting{};
};

} // namespace mediaserver::render
