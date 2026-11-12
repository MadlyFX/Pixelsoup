// SceneRenderer particle effects in a build without the particle runtime
// (ajfx): nothing is simulated or drawn, and a scene that places a started
// effect is refused readiness with that reason, as it is on a renderer or
// device that cannot run one. scene_renderer_particles.cpp is the build that
// plays them.

#include "scene_renderer_internal.hpp"

namespace mediaserver::render {

void SceneRenderer::adopt_particle_runtime(rhi::Device&, rhi::TextureFormat, SceneRendererMode,
                                           const ParticleSettings&) {
    particles_unsupported_ = "this build does not include particle effects";
}

bool SceneRenderer::draws_particles() const noexcept {
    return false;
}

void SceneRenderer::prepare_particles(rhi::CommandList&, const scene::Scene& scene,
                                      const SceneRenderOptions& options) {
    particle_lights_used_ = 0U;
    // A reflection shows no particles; the view that reflects it counts them.
    if (options.reflection_pass) {
        return;
    }
    for (const scene::ParticleEffect& effect : scene.particle_effects) {
        if (!scene::particle_effect_frame(effect, scene.show_frame)) {
            continue; // not started: nothing to draw, and nothing missing
        }
        ++stats_.particles_unsupported;
        if (stats_.particle_issue.empty()) {
            stats_.particle_issue = particles_unsupported_;
        }
        stats_.assets.ready = false;
    }
}

bool SceneRenderer::record_particles(rhi::CommandList&, const scene::ViewMatrices&, ParticleTargets) {
    return true;
}

} // namespace mediaserver::render
