#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/particle_device.hpp>
#include <mediaserver/render/particle_types.hpp>
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
    //
    // With `cache` (opened for `effect`, as its package holds it), the
    // emitters it caches are read from it on the pass's rigid runner rather
    // than simulated, so a seek is a read; the live emitters step beside
    // them with the cache's performance. `seed` must then be the cache's
    // (Cache::identity), which it was opened with; revise is ignored, since
    // the performance is the cache's.
    [[nodiscard]] core::Result<ParticleInstance, ParticleError> create_instance(
        const ajfx::Effect& effect, const ajfx::gpu::KernelSet& kernels, const ajfx::TextureSet& textures,
        const ajfx::FieldSet& fields, const ajfx::MeshSet& meshes, core::u32 seed,
        std::shared_ptr<const ajfx::Cache> cache = nullptr);

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
