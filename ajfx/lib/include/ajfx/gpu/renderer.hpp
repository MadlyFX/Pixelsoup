#pragma once

#include <ajfx/gpu/device.hpp>
#include <ajfx/gpu/kernels.hpp>
#include <ajfx/gpu/simulation.hpp>
#include <ajfx/gpu/sort.hpp>
#include <ajfx/math.hpp>
#include <ajfx/mesh.hpp>
#include <ajfx/result.hpp>

#include <array>
#include <cstdint>
#include <memory>

// The reference sprite renderer: draws every emitter of a GpuSimulation as
// camera-facing (or velocity-aligned) soft discs, one indirect draw per
// emitter with the instance count the simulation wrote, in emitter order.
// Emitters that sort (see sort.hpp) draw back to front along the camera's
// forward axis; the rest in birth order. Hosts with their own renderer read
// EmitterBuffers and sort with a ParticleSorter themselves.
//
// The effect's grid volumes (Volume) are drawn first, each raymarched in a
// fixed number of steps under the scene's one directional light, absorbing
// by the density and scattering towards the camera; the particles draw over
// them. Hosts read VolumeBuffers and march themselves.
namespace ajfx::gpu {

struct Camera final {
    // clip = view_projection * (position, 1), rows in order. Depth maps to
    // [0, 1] as D3D expects.
    std::array<float, 16> view_projection{};
    // World-space axes of the view, for facing quads towards it.
    Vec3 right{1.0F, 0.0F, 0.0F};
    Vec3 up{0.0F, 1.0F, 0.0F};
    Vec3 forward{0.0F, 0.0F, -1.0F};

    // A perspective camera at `eye` looking at `target`; `vertical_fov` in
    // radians, `aspect` width over height.
    [[nodiscard]] static Camera look_at(Vec3 eye, Vec3 target, Vec3 world_up, float vertical_fov, float aspect,
                                        float near_plane, float far_plane);
    // An orthographic camera looking down -Z, showing `half_width` and
    // `half_height` either side of `center`, from z = 1000 to z = -1000.
    [[nodiscard]] static Camera orthographic(Vec3 center, float half_width, float half_height);
};

// The light the reference renderer draws lit emitters under
// (SpriteRenderer::lighting): one directional light and a sky/ground
// hemisphere. A host lights from its own scene instead.
struct SceneLight final {
    // The way the light travels.
    Vec3 direction{0.0F, -1.0F, 0.0F};
    Vec3 color{1.0F, 1.0F, 1.0F};
    // The hemisphere: what a direction facing straight up and straight down
    // receives, blended between for the rest.
    Vec3 sky{0.2F, 0.2F, 0.2F};
    Vec3 ground{0.05F, 0.05F, 0.05F};
};

class ParticleRenderer final {
public:
    // Pipelines for one render target format, from a kernel set's sprite
    // shaders, and the textures and meshes `effect`'s emitters name, uploaded
    // from `textures` and `meshes` (see read_package_textures and
    // read_package_meshes); fails with MissingTexture or MissingMesh when one
    // is absent. The device must outlive the renderer.
    //
    // This renderer has no depth buffer, so a concave mesh may draw its far
    // side over its near side; hosts with depth (the media server) do not.
    [[nodiscard]] static Result<ParticleRenderer> create(Device& device, const Effect& effect,
                                                         const KernelSet& kernels, const TextureSet& textures,
                                                         TextureFormat format, const MeshSet& meshes = {});

    ParticleRenderer(ParticleRenderer&&) noexcept;
    ParticleRenderer& operator=(ParticleRenderer&&) noexcept;
    ~ParticleRenderer();

    // Records drawing the simulation's current particles into `target`,
    // clearing it first when `clear_color` is given, lit emitters under
    // `light`. The simulation runs the effect the renderer was made for, and
    // `sorter` was made for it too. Call after stepping; nothing runs until
    // the device is flushed.
    void render(const GpuSimulation& simulation, ParticleSorter& sorter, const Camera& camera, Texture target,
                const std::array<float, 4>* clear_color = nullptr, const SceneLight& light = SceneLight{});

private:
    struct State;
    explicit ParticleRenderer(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace ajfx::gpu
