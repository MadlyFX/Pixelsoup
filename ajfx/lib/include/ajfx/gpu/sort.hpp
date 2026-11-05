#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/gpu/device.hpp>
#include <ajfx/gpu/kernels.hpp>
#include <ajfx/gpu/simulation.hpp>
#include <ajfx/math.hpp>
#include <ajfx/result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

// Depth sorting for alpha-blended emitters. Blending "over" depends on order,
// so those emitters draw back to front, sprites and meshes alike (a renderer
// without a depth buffer relies on the order to hide far meshes behind near
// ones); additive emitters, whose result does not depend on order, ribbons,
// whose strip needs birth order, and emitters that opt out with
// `SpriteRenderer::sort = false`, keep birth order and cost nothing here.
//
// The sort is a stable integer radix sort on the GPU (shaders/ajfx_sort.hlsl)
// of depth to one part in 32768 of its distance; nearer than that, particles
// keep birth order. The order depends only on the particles' positions and
// the axis, so every node of a cluster and every seek that holds the same
// particles draws them in the same order. It reads the simulation's live
// count and dispatch sizes on the GPU; nothing is read back. Sorting is per
// view, not simulation state: checkpoints neither hold nor need it.
//
// Each emitter sorts on its own, so drawing emitters one after another would
// leave the last one over the others wherever they overlap: smoke drawn after
// sparks hides the sparks in front of it. Sprite emitters that share depth
// with a sorted one are drawn interleaved instead: their common depth range
// is cut into slices, and the slices draw farthest first, each emitter's part
// of a slice in emitter order. An additive sprite emitter is sorted for this
// alone. Particles of different emitters within one slice still draw in
// emitter order.
namespace ajfx::gpu {

// Whether an emitter's particles are depth-sorted before drawing, on its own.
[[nodiscard]] bool sorts(const Emitter& emitter) noexcept;
// Whether emitter `index` of `effect` draws interleaved, slice by slice, with
// the effect's other sprite emitters: a sprite emitter (not a ribbon or mesh)
// that sorts, or is additive, in an effect with two or more such emitters of
// which at least one sorts.
[[nodiscard]] bool interleaved(const Effect& effect, std::size_t index) noexcept;

// The depth slices interleaved emitters draw in.
inline constexpr std::uint32_t depth_slices = 32U;

// Where one depth slice of an interleaved emitter is drawn from.
struct DepthSlice final {
    // Four uint32 draw arguments: six vertices, the slice's particle count.
    Buffer args;
    std::uint64_t args_offset{0U};
    // The word of the emitter's order buffer that holds where in the order
    // the slice starts.
    std::uint32_t start_word{0U};
};

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

    // Records cutting the interleaved emitters into depth slices, after
    // every one of them was sorted for this view. Does nothing for an effect
    // that interleaves none.
    void slice(const GpuSimulation& simulation);
    // Slice `slice` (farthest first) of an interleaved emitter, as the last
    // slice() cut it.
    [[nodiscard]] DepthSlice depth_slice(std::size_t emitter, std::uint32_t slice) const noexcept;

private:
    struct State;
    explicit ParticleSorter(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace ajfx::gpu
