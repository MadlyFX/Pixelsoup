#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/gpu/device.hpp>
#include <ajfx/gpu/kernels.hpp>
#include <ajfx/result.hpp>
#include <ajfx/simulation.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// The GPU simulation. It follows the CPU reference step for step: births are
// scheduled by the same code, so particles appear at the same steps with the
// same ages and spawn indices, and the per-particle arithmetic matches it
// within floating-point tolerance. Repeated runs on one device and driver are
// bit-identical: compaction is a stable prefix sum, never an atomic append.
//
// Nothing is read back while it runs. Each step records, per emitter: the
// fused update kernel over the live particles (an indirect dispatch sized by
// the previous step), a two-level prefix sum and scatter that keeps the
// survivors in birth order, the fused spawn kernel for this step's births, and
// a one-thread kernel that writes the next step's dispatch sizes.
namespace ajfx::gpu {

// Where a host finds an emitter's particles: `stride`-byte records laid out
// by `layout`, in `particles`, with the live count as the first uint32 of
// `counters`. `args` holds draw arguments at `draw_args_offset` (six
// vertices, one instance per live particle) for an indirect draw, and at
// `block_args_offset` the group counts for one group per 4096 live particles.
struct EmitterBuffers final {
    Buffer particles;
    Buffer counters;
    Buffer args;
    std::uint64_t draw_args_offset{0U};
    std::uint64_t block_args_offset{0U};
    ParticleLayout layout;
    std::uint32_t capacity{0U};
};

// A copy of a GpuSimulation's whole state, held on the device. Restoring it
// puts the simulation back exactly where it was.
class GpuCheckpoint final {
public:
    GpuCheckpoint(GpuCheckpoint&&) noexcept;
    GpuCheckpoint& operator=(GpuCheckpoint&&) noexcept;
    ~GpuCheckpoint();

    [[nodiscard]] std::uint64_t tick() const noexcept;

private:
    friend class GpuSimulation;
    struct Data;
    explicit GpuCheckpoint(std::unique_ptr<Data> data);
    std::unique_ptr<Data> data_;
};

class GpuSimulation final {
public:
    // Fails if the effect does not validate, `fields` lacks a distance field
    // it collides with (MissingField), or a kernel or buffer cannot be
    // created. Creating records nothing: the fields are written by the first
    // step. The device must outlive the simulation.
    [[nodiscard]] static Result<GpuSimulation> create(Device& device, Effect effect, const KernelSet& kernels,
                                                      std::uint32_t instance_seed = 0U, const FieldSet& fields = {});
    // As above, with the meshes rigid physics hulls wrap and the solver that
    // runs rigid physics emitters (failing as Simulation::create does). Those
    // emitters are simulated on the CPU by that solver, exactly as the CPU
    // reference simulates them, and each step writes their particles into
    // their buffers; everything that draws reads them as any emitter's.
    [[nodiscard]] static Result<GpuSimulation> create(Device& device, Effect effect, const KernelSet& kernels,
                                                      std::uint32_t instance_seed, SimulationOptions options);

    GpuSimulation(GpuSimulation&&) noexcept;
    GpuSimulation& operator=(GpuSimulation&&) noexcept;
    ~GpuSimulation();

    [[nodiscard]] const Effect& effect() const noexcept;
    [[nodiscard]] std::uint64_t tick() const noexcept;

    // Records one step; nothing runs until the device is flushed.
    void step(const ParameterProvider* parameters = nullptr);
    void advance_to(std::uint64_t target, const ParameterProvider* parameters = nullptr);
    // Back to step 0 with no particles.
    void reset();

    // Buffers for drawing. They swap every step; ask again after stepping.
    [[nodiscard]] EmitterBuffers emitter_buffers(std::size_t emitter) const;

    // Flushes the device and reads one emitter back in the CPU reference's
    // form, for tests and tools. Not for use on a frame path.
    [[nodiscard]] Result<EmitterState> read_emitter(std::size_t emitter);
    // The raw particle records, for exact comparisons between runs.
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_particle_bytes(std::size_t emitter);

    // Records copies of every buffer; valid once the device is flushed. A
    // rigid physics emitter's solver state is saved on the CPU at once.
    [[nodiscard]] Result<GpuCheckpoint> save();
    // A checkpoint's buffers with nothing copied into them yet: allocation
    // only, no recorded work. Fill it with save_into.
    [[nodiscard]] Result<GpuCheckpoint> allocate_checkpoint();
    // Records the copies into a checkpoint this simulation made earlier,
    // reusing its buffers: no allocation, so a host can keep a ring of
    // checkpoints and save into it while recording a frame.
    void save_into(GpuCheckpoint& checkpoint);
    // Records the copies back. The checkpoint must come from this simulation.
    void restore(const GpuCheckpoint& checkpoint);

private:
    struct State;
    explicit GpuSimulation(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace ajfx::gpu
