#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/particle_device.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/math.hpp>

#include <ajfx/effect.hpp>
#include <ajfx/gpu/kernels.hpp>
#include <ajfx/gpu/simulation.hpp>
#include <ajfx/gpu/sort.hpp>
#include <ajfx/field.hpp>
#include <ajfx/mesh.hpp>
#include <ajfx/texture.hpp>
#include <ajfx/rigid_runner.hpp>
#include <ajfx/simulation.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// ajfx particle effects in a rendered view: each placed effect (an instance)
// is simulated on the GPU to the step its show frame calls for, then drawn as
// sprites over the scene's colour, depth-tested against its depth.
//
// Determinism: the step comes from the exact show frame (ajfx::tick_at), the
// instance seed from replicated state, and the sprite geometry from the
// camera's own axes, so every node draws the same particles and only the
// slice projection differs.
//
// Bounded frame work: an instance's buffers, sort scratch, textures and
// checkpoint ring are made when it is created, never while recording.
// Advancing takes at most a step budget per frame, and stages the instance's
// textures within each recording's upload space; until both are done the
// instance is behind, reported by `advance` and refused by `draw`, so a late
// node holds rather than drawing particles from the wrong moment or with a
// texture half uploaded.
//
// Alpha-blended emitters draw back to front: each draw sorts them along the
// camera's forward axis (ajfx::gpu::ParticleSorter), which every node shares,
// so slices agree on the order as they do on the particles.
//
// An effect's grid volumes (ajfx::Volume, smoke the particles feed) simulate
// through the same device and draw before the emitters: each grid's box is
// rasterised in world space and every pixel of it marches the view ray
// through the cells in a fixed number of steps, lit by the scene's first
// directional light, its shadow cascades and the hemisphere, stopping at the
// scene's depth. Rays come from the camera's eye and forward axis, never from
// the slice's projection, so slices agree. Volumes write no motion.
//
// Rigid physics emitters (debris that piles, a heap that collapses) run in a
// physics engine on the CPU, which costs too much to step or checkpoint while
// a frame records. The pass steps them on a thread of its own instead
// (ajfx::RigidRunner), ahead of the show frame, and each advance only writes
// the target step's particles once that thread has them. Until then the
// instance is behind, and a seek holds until its physics has caught up.
//
// Those emitters' props (a bucket to spill out of, a wall to pile against)
// come from the same rigid frame: each advance writes the poses of the tick it
// adopted, and a draw puts each prop's model through the mesh-particle path,
// before the particles, so a prop and the debris around it are lit alike. A
// prop draws the model it names whatever it collides as; one that names no
// model draws nothing.
namespace mediaserver::render {

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

class ParticlePass;

// One placed effect: its simulation and checkpoints. Made by
// ParticlePass::create_instance and used with that pass only, which must
// outlive it: its buffers belong to the pass's device.
class ParticleInstance final {
public:
    ParticleInstance(ParticleInstance&&) noexcept;
    ParticleInstance& operator=(ParticleInstance&&) noexcept;
    ~ParticleInstance();

    [[nodiscard]] const ajfx::Effect& effect() const noexcept;
    // The step the simulation holds, and the one the last advance asked for.
    [[nodiscard]] core::u64 step() const noexcept;
    [[nodiscard]] core::u64 target_step() const noexcept;
    // At its target step with every texture uploaded and its rigid physics
    // written: ready to draw.
    [[nodiscard]] bool caught_up() const noexcept;
    // Why it will never catch up: its rigid physics could not be built (a
    // mesh that wraps no hull). Other instances are unaffected.
    [[nodiscard]] std::optional<std::string> failure() const;
    // No submitted work still uses the textures it owns, so it may be
    // destroyed. Its simulation's buffers and kernels need no wait: the
    // particle device retires those itself.
    [[nodiscard]] bool safe_to_release() const noexcept;
    // The simulation itself, for tools and tests. Reading it back submits
    // work of its own, so never on a frame path.
    [[nodiscard]] ajfx::gpu::GpuSimulation& simulation() noexcept;

private:
    friend class ParticlePass;
    struct State;
    explicit ParticleInstance(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

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

class ParticlePass final {
public:
    // Fails with Unsupported where the device cannot run ajfx (see
    // ParticleDevice) or this build has no sprite shaders.
    [[nodiscard]] static core::Result<ParticlePass, ParticleError> create(rhi::Device& device,
                                                                          rhi::TextureFormat color_format,
                                                                          ParticleSettings settings = {});

    ParticlePass(ParticlePass&&) noexcept;
    ParticlePass& operator=(ParticlePass&&) noexcept;
    ~ParticlePass();

    // An instance of `effect` running `kernels`, drawing with `textures` and
    // `meshes` and colliding with `fields` (all from its package), with the
    // seed that
    // makes it differ from other placements of the same effect. Allocates its
    // buffers, sort scratch, textures and checkpoints: call it when an effect
    // is adopted, not while recording a frame (the first advance uploads the
    // fields and meshes). Fails with InvalidEffect, naming the emitter, when a
    // texture, field or mesh the effect uses is missing. Blended meshes write
    // depth, so they hide their own far side as solid geometry does. Meshes
    // are uploaded once per name, so emitters and props that draw the same
    // model share one pair of buffers.
    [[nodiscard]] core::Result<ParticleInstance, ParticleError> create_instance(const ajfx::Effect& effect,
                                                                               const ajfx::gpu::KernelSet& kernels,
                                                                               const ajfx::TextureSet& textures,
                                                                               const ajfx::FieldSet& fields,
                                                                               const ajfx::MeshSet& meshes,
                                                                               core::u32 seed);

    // Records simulating `instance` to the step for `show_frame` at
    // rate_num / rate_den frames a second, within the step budget, and returns
    // whether it got there. Parameter values come from `parameters` (a pure
    // function of the step) or the effect's defaults; the rigid physics
    // thread uses the ones `restart` gave it, which must be the same. Actions
    // are the ones `revise` last gave it.
    [[nodiscard]] bool advance(rhi::CommandList& commands, ParticleInstance& instance, core::u64 show_frame,
                               core::u32 rate_num, core::u32 rate_den,
                               const ajfx::ParameterProvider* parameters = nullptr);

    // Records drawing `instance` placed by `object_to_world`, seen through
    // `view` (its view matrix gives the camera axes and the sort axis; its
    // view-projection rasterises; its projection maps depth back to distance
    // for soft particles). Refused when the instance has not caught up, props
    // included: nothing is drawn from a tick whose rigid frame has not landed.
    // Sorting reuses the instance's scratch, so draws of one instance record
    // one after another.
    [[nodiscard]] bool draw(rhi::CommandList& commands, ParticleInstance& instance,
                            const scene::ViewMatrices& view, const scene::Mat4& object_to_world,
                            const ParticleTargets& targets);
    // Records the sprites' and props' motion into `targets.velocity`, in the
    // order the last `draw` of this instance used, so where they overlap the
    // one drawn on top gives the motion. A prop moves by the velocity its
    // pose reports, as a particle moves by its own.
    [[nodiscard]] bool draw_motion(rhi::CommandList& commands, ParticleInstance& instance,
                                   const scene::ViewMatrices& view, const scene::Mat4& object_to_world,
                                   const ParticleTargets& targets);

    // How many particle lights `instance` writes: the sum of its emitters'
    // light counts.
    [[nodiscard]] core::u32 light_count(const ParticleInstance& instance) const noexcept;

    // Records writing `instance`'s particle lights, placed by
    // `object_to_world`, into slots `first_slot` onwards of `lights` (a
    // writable buffer of light.hlsli Light records, 128 bytes each), as
    // light_count says. Call after advance has caught the instance up.
    [[nodiscard]] bool write_lights(rhi::CommandList& commands, const ParticleInstance& instance,
                                    const scene::Mat4& object_to_world, rhi::StorageBuffer& lights,
                                    core::u32 first_slot);

    // Forgets `instance`'s history: its checkpoints are dropped, and the next
    // advance simulates again from the start. For when what the simulation
    // depends on (its parameters) changed, so the past it holds is no longer
    // the past every node would compute. Its rigid physics follows
    // `parameters` from now on (none for the defaults), called from the
    // rigid physics thread: pass what advance will be given.
    void restart(ParticleInstance& instance,
                 std::shared_ptr<const ajfx::ParameterProvider> parameters = nullptr) noexcept;

    // Has `instance` follow `actions` (none for none) from now on, its GPU
    // simulation and its rigid physics thread alike, where they differ from
    // the old ones only at `from` and later. What it holds up to `from`
    // stands: checkpoints after it are dropped, and a simulation past it
    // comes back to the latest one at or before it on the next advance. An
    // action stamped at acting_tick of the last target therefore costs
    // nothing; one in the past replays from a checkpoint. With actions, its
    // rigid physics runs the acting lead.
    void revise(ParticleInstance& instance, std::shared_ptr<const ajfx::ActionProvider> actions,
                core::u64 from) noexcept;
    // The rigid physics lead `instance` runs now (ParticleSettings).
    [[nodiscard]] core::u32 rigid_lead(const ParticleInstance& instance) const noexcept;

    // The first device failure since the last call, for frame diagnostics.
    [[nodiscard]] std::optional<ParticleError> take_error() noexcept;

private:
    struct State;
    explicit ParticlePass(std::unique_ptr<State> state);
    [[nodiscard]] bool record_sprites(rhi::CommandList& commands, ParticleInstance& instance,
                                      const scene::ViewMatrices& view, const scene::Mat4& object_to_world,
                                      const ParticleTargets& targets, bool motion);
    std::unique_ptr<State> state_;
};

} // namespace mediaserver::render
