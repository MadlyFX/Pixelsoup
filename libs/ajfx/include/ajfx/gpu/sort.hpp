#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/gpu/device.hpp>
#include <ajfx/gpu/kernels.hpp>
#include <ajfx/gpu/simulation.hpp>
#include <ajfx/math.hpp>
#include <ajfx/result.hpp>

#include <cstddef>
#include <memory>

// Depth sorting for alpha-blended emitters. Blending "over" depends on order,
// so those emitters draw back to front; additive emitters, whose result does
// not, and emitters that opt out with `SpriteRenderer::sort = false`, keep
// birth order and cost nothing here.
//
// The sort is a stable integer radix sort on the GPU (shaders/ajfx_sort.hlsl):
// the order depends only on the particles' positions and the axis, so every
// node of a cluster and every seek that holds the same particles draws them in
// the same order. It reads the simulation's live count and dispatch sizes on
// the GPU; nothing is read back. Sorting is per view, not simulation state:
// checkpoints neither hold nor need it.
namespace ajfx::gpu {

// Whether an emitter's particles are depth-sorted before drawing.
[[nodiscard]] bool sorts(const Emitter& emitter) noexcept;

class ParticleSorter final {
public:
    // Scratch for every emitter of `effect` that sorts, sized to its
    // capacity and allocated here, never while recording. The device must
    // outlive the sorter.
    [[nodiscard]] static Result<ParticleSorter> create(Device& device, const Effect& effect, const KernelSet& kernels);

    ParticleSorter(ParticleSorter&&) noexcept;
    ParticleSorter& operator=(ParticleSorter&&) noexcept;
    ~ParticleSorter();

    // Records sorting `emitter`'s live particles farthest first along `axis`:
    // depth is dot(position, axis), in the particles' own space. For a
    // camera's view of an emitter placed by `world`, that is the transpose of
    // world's 3x3 part times the camera's forward axis. Returns a buffer of
    // uint32 record indices, one per live particle in drawing order, valid
    // until the next sort of this emitter; an invalid buffer for an emitter
    // that does not sort. Call after the simulation's step is recorded.
    [[nodiscard]] Buffer sort(const GpuSimulation& simulation, std::size_t emitter, Vec3 axis);

private:
    struct State;
    explicit ParticleSorter(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace ajfx::gpu
