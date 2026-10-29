#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/particle_device.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/math.hpp>

#include <ajfx/effect.hpp>
#include <ajfx/gpu/kernels.hpp>
#include <ajfx/gpu/simulation.hpp>
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
// Bounded frame work: an instance's buffers and its checkpoint ring are made
// when it is created, never while recording. Advancing takes at most a step
// budget per frame; a seek further than that from the nearest checkpoint
// leaves the instance behind, reported by `advance` and refused by `draw`, so
// a late node holds rather than drawing particles from the wrong moment.
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

struct ParticleSettings final {
    // Steps advance may take in one frame. Normal playback needs
    // ceil(simulation rate / show rate): three at 24 fps and 60 steps a second.
    core::u32 step_budget{16U};
    // A checkpoint every this many steps, the most recent `checkpoint_count`
    // kept. A seek back lands on the nearest one at or before its target.
    core::u32 checkpoint_interval{120U};
    core::u32 checkpoint_count{4U};
};

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
    [[nodiscard]] bool caught_up() const noexcept { return step() == target_step(); }
    // The simulation itself, for tools and tests. Reading it back submits
    // work of its own, so never on a frame path.
    [[nodiscard]] ajfx::gpu::GpuSimulation& simulation() noexcept;

private:
    friend class ParticlePass;
    struct State;
    explicit ParticleInstance(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
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

    // An instance of `effect` running `kernels` (from its package), with the
    // seed that makes it differ from other placements of the same effect.
    // Allocates its buffers and checkpoints: call it when an effect is
    // adopted, not while recording a frame.
    [[nodiscard]] core::Result<ParticleInstance, ParticleError> create_instance(const ajfx::Effect& effect,
                                                                               const ajfx::gpu::KernelSet& kernels,
                                                                               core::u32 seed);

    // Records simulating `instance` to the step for `show_frame` at
    // rate_num / rate_den frames a second, within the step budget, and returns
    // whether it got there. Parameter values come from `parameters` (a pure
    // function of the step) or the effect's defaults.
    [[nodiscard]] bool advance(rhi::CommandList& commands, ParticleInstance& instance, core::u64 show_frame,
                               core::u32 rate_num, core::u32 rate_den,
                               const ajfx::ParameterProvider* parameters = nullptr);

    // Records drawing `instance` placed by `object_to_world`, seen through
    // `view` (its view matrix gives the camera axes; its view-projection
    // rasterises). Refused when the instance has not caught up with its
    // target step.
    [[nodiscard]] bool draw(rhi::CommandList& commands, const ParticleInstance& instance,
                            const scene::ViewMatrices& view, const scene::Mat4& object_to_world,
                            const ParticleTargets& targets);

    // The first device failure since the last call, for frame diagnostics.
    [[nodiscard]] std::optional<ParticleError> take_error() noexcept;

private:
    struct State;
    explicit ParticlePass(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace mediaserver::render
