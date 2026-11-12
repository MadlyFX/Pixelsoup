#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/scene/math.hpp>

#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

// A build without the particle runtime: a placed effect is never drawn, and
// once it has started the renderer refuses readiness and says the build has
// no particle effects - on either renderer, since neither can play one.
namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;

static_assert(!render::particle_runtime_built);

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) + ": check failed: " + expression};
    }
}

#define SCENE_CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

std::unique_ptr<rhi::Device> g_device;

scene::Scene scene_with_effects(scene::RendererBackend backend) {
    scene::Scene frame;
    frame.canvas = {64U, 64U};
    frame.camera.eye = {0, 0, 4};
    frame.camera.target = {0, 0, 0};
    frame.camera.field_of_view = scene::Turns{0.25F};
    frame.renderer_settings.backend = backend;
    frame.show_rate = {24U, 1U};
    frame.show_frame = 10U;
    // Two placements, so a count of started effects is told from a flag.
    for (core::u64 node = 7U; node < 9U; ++node) {
        scene::ParticleEffect effect;
        effect.world = scene::identity();
        effect.source = "effects/sparks.ajfx";
        effect.source_node = doc::NodeId{node};
        effect.seed = 5U;
        frame.particle_effects.push_back(effect);
    }
    return frame;
}

render::RendererLimits limits() {
    render::RendererLimits limits;
    limits.max_instances = 8U;
    limits.max_vertices = 128U;
    limits.max_indices = 128U;
    limits.directional_shadow_resolution = 16U;
    limits.shadow_atlas.extent = 64U;
    return limits;
}

const render::RenderStats& render_once(render::SceneRenderer& renderer, const scene::Scene& frame) {
    auto commands = g_device->acquire_commands();
    SCENE_CHECK(commands);
    SCENE_CHECK(renderer.render(commands.value(), frame, {0U, 0U, 64U, 64U}));
    auto submitted = g_device->submit(std::move(commands).value());
    SCENE_CHECK(submitted);
    SCENE_CHECK(std::move(submitted).value().wait());
    return renderer.stats();
}

void started_effects_refuse_readiness(render::SceneRendererMode mode, scene::RendererBackend backend) {
    auto renderer = render::SceneRenderer::create(*g_device, 64U, 64U, limits(), mode);
    SCENE_CHECK(renderer);
    scene::Scene frame = scene_with_effects(backend);

    // Neither has started: nothing is asked of the build, and it is ready.
    frame.particle_effects[0].start_frame = 100U;
    frame.particle_effects[1].start_frame = 100U;
    const render::RenderStats& waiting = render_once(renderer.value(), frame);
    SCENE_CHECK(waiting.particles_unsupported == 0U && waiting.particle_issue.empty());
    SCENE_CHECK(waiting.assets.ready);

    // One has: it alone is counted, with the build named as the reason.
    frame.particle_effects[0].start_frame = 0U;
    const render::RenderStats& one = render_once(renderer.value(), frame);
    SCENE_CHECK(one.particles_unsupported == 1U);
    SCENE_CHECK(one.particle_issue == "this build does not include particle effects");
    SCENE_CHECK(!one.assets.ready);

    frame.particle_effects[1].start_frame = 0U;
    const render::RenderStats& both = render_once(renderer.value(), frame);
    SCENE_CHECK(both.particles_unsupported == 2U);
    SCENE_CHECK(both.particles_drawn == 0U && both.particles_pending == 0U && both.particles_failed == 0U);
    SCENE_CHECK(both.particle_digest == 0U && !both.assets.ready);

    // Removing them restores readiness.
    frame.particle_effects.clear();
    const render::RenderStats& none = render_once(renderer.value(), frame);
    SCENE_CHECK(none.particles_unsupported == 0U && none.particle_issue.empty() && none.assets.ready);
    g_device->wait_idle();
}

void the_advanced_renderer_refuses_started_effects() {
    started_effects_refuse_readiness(render::SceneRendererMode::AdvancedLinear, scene::RendererBackend::Advanced);
}

void the_basic_renderer_refuses_started_effects() {
    started_effects_refuse_readiness(render::SceneRendererMode::BasicDisplay, scene::RendererBackend::Basic);
}

} // namespace

int main() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        std::cout << "skipped: no D3D12 device\n";
        return 77;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto device = rhi::Device::create(config);
    if (!device || !device.value().capabilities().advanced_raster()) {
        std::cout << "skipped: no Advanced D3D12 device\n";
        return 77;
    }
    g_device = std::make_unique<rhi::Device>(std::move(device).value());

    struct Case final {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"the_advanced_renderer_refuses_started_effects", the_advanced_renderer_refuses_started_effects},
        {"the_basic_renderer_refuses_started_effects", the_basic_renderer_refuses_started_effects},
    };
    int failures = 0;
    for (const Case& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    g_device.reset();
    return failures == 0 ? 0 : 1;
}
