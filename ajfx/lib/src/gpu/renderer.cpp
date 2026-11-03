#include <ajfx/gpu/renderer.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace ajfx::gpu {
namespace {

constexpr std::uint32_t absent = 0xffffffffU;

Vec3 unit(Vec3 v) {
    const float length = std::sqrt(dot(v, v));
    return length > 0.0F ? v * (1.0F / length) : v;
}

std::uint32_t offset_or_absent(std::int32_t offset) {
    return offset >= 0 ? static_cast<std::uint32_t>(offset) : absent;
}

// Steps the volume raymarch takes along a view ray, and from each of them
// towards the light. Fixed, so a frame is the same frame everywhere.
constexpr std::uint32_t volume_ray_steps = 64U;
constexpr std::uint32_t volume_light_steps = 8U;

// The inverse of a 4x4 (rows in order), by cofactors; the identity when the
// matrix has no inverse.
std::array<float, 16> inverse(const std::array<float, 16>& m) {
    std::array<float, 16> inv{};
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float determinant = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (determinant == 0.0F || !std::isfinite(determinant)) {
        return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    }
    const float scale = 1.0F / determinant;
    for (float& value : inv) {
        value *= scale;
    }
    return inv;
}

// Floats in the camera buffer: the view projection, the three axes, the
// four light rows and the inverse view projection, each row four floats.
constexpr std::size_t camera_floats = 60U;

} // namespace

Camera Camera::look_at(Vec3 eye, Vec3 target, Vec3 world_up, float vertical_fov, float aspect, float near_plane,
                       float far_plane) {
    Camera camera;
    camera.forward = unit(target - eye);
    camera.right = unit(cross(camera.forward, world_up));
    camera.up = cross(camera.right, camera.forward);

    // View space: x right, y up, z towards the viewer; then a right-handed
    // perspective projection with depth in [0, 1].
    const float y_scale = 1.0F / std::tan(vertical_fov * 0.5F);
    const float x_scale = y_scale / aspect;
    const float depth_scale = far_plane / (near_plane - far_plane);
    const float depth_offset = near_plane * far_plane / (near_plane - far_plane);
    const Vec3 back = camera.forward * -1.0F;
    const float view[3][4] = {
        {camera.right.x, camera.right.y, camera.right.z, -dot(camera.right, eye)},
        {camera.up.x, camera.up.y, camera.up.z, -dot(camera.up, eye)},
        {back.x, back.y, back.z, -dot(back, eye)},
    };
    for (int c = 0; c < 4; ++c) {
        camera.view_projection[0 + c] = x_scale * view[0][c];
        camera.view_projection[4 + c] = y_scale * view[1][c];
        camera.view_projection[8 + c] = depth_scale * view[2][c] + (c == 3 ? depth_offset : 0.0F);
        camera.view_projection[12 + c] = -view[2][c];
    }
    return camera;
}

Camera Camera::orthographic(Vec3 center, float half_width, float half_height) {
    Camera camera;
    // Depth runs from z = 1000 (0) to z = -1000 (1), so the projection has
    // an inverse for the volume raymarch; nothing between is clipped.
    camera.view_projection = {
        1.0F / half_width, 0.0F, 0.0F, -center.x / half_width,
        0.0F, 1.0F / half_height, 0.0F, -center.y / half_height,
        0.0F, 0.0F, -1.0F / 2000.0F, 0.5F,
        0.0F, 0.0F, 0.0F, 1.0F,
    };
    return camera;
}

struct ParticleRenderer::State final {
    Device* device{nullptr};
    Pipeline blended;  // alpha and premultiplied emitters
    Pipeline additive;
    // The same for meshes, culling the faces turned away: with no depth
    // buffer here, a convex mesh would otherwise draw its far side over its
    // near side.
    Pipeline blended_mesh;
    Pipeline additive_mesh;
    // The grid volumes' raymarch; invalid for an effect without volumes.
    Pipeline volume;
    Buffer camera;
    // Each emitter's look (curvature, wrap, ambient), four floats apiece.
    Buffer looks;
    // Each emitter's texture and six-way lightmap, invalid where it has
    // none; one upload per name.
    std::vector<Texture> emitter_textures;
    std::vector<Texture> emitter_lightmaps;
    std::vector<Texture> uploaded;
    // Each mesh emitter's models and its draw arguments (the largest model's
    // index count, then the live count copied from the simulation). An
    // emitter that draws a model per body kind (models_by_kind) holds every
    // model it draws in one pair of buffers, and `models` says which range
    // of the indices each kind is drawn from; one draw then covers them all,
    // so the order between models is still the sort's.
    struct MeshBuffers final {
        Buffer vertices;
        Buffer indices;
        Buffer args;
        Buffer models;
        std::uint32_t index_count{0U};
    };
    std::vector<std::optional<MeshBuffers>> meshes;

    ~State() {
        if (device != nullptr) {
            device->destroy_pipeline(blended);
            device->destroy_pipeline(additive);
            device->destroy_pipeline(blended_mesh);
            device->destroy_pipeline(additive_mesh);
            if (volume.valid()) {
                device->destroy_pipeline(volume);
            }
            device->destroy_buffer(camera);
            device->destroy_buffer(looks);
            for (const Texture texture : uploaded) {
                device->destroy_texture(texture);
            }
            for (const std::optional<MeshBuffers>& mesh : meshes) {
                if (mesh) {
                    device->destroy_buffer(mesh->vertices);
                    device->destroy_buffer(mesh->indices);
                    device->destroy_buffer(mesh->args);
                    device->destroy_buffer(mesh->models);
                }
            }
        }
    }
};

ParticleRenderer::ParticleRenderer(std::unique_ptr<State> state) : state_{std::move(state)} {}
ParticleRenderer::ParticleRenderer(ParticleRenderer&&) noexcept = default;
ParticleRenderer& ParticleRenderer::operator=(ParticleRenderer&&) noexcept = default;
ParticleRenderer::~ParticleRenderer() = default;

Result<ParticleRenderer> ParticleRenderer::create(Device& device, const Effect& effect, const KernelSet& kernels,
                                                  const TextureSet& textures, TextureFormat format,
                                                  const MeshSet& meshes) {
    if (kernels.format != device.shader_format() || kernels.sprite_vertex.empty() || kernels.sprite_pixel.empty()) {
        return Error{ErrorCode::GpuUnavailable, "the kernel set has no sprite shaders for this device"};
    }
    if (Result<bool> checked = check_textures(effect, textures); !checked) {
        return checked.error();
    }
    if (Result<bool> checked = check_meshes(effect, meshes); !checked) {
        return checked.error();
    }
    auto state = std::make_unique<State>();
    state->device = &device;
    for (const Emitter& emitter : effect.emitters) {
        std::optional<State::MeshBuffers>& out = state->meshes.emplace_back();
        if (emitter.renderer.geometry != Geometry::Mesh) {
            continue;
        }
        // The model each body kind is drawn as: the emitter's own is kind 0,
        // and a kind that names none is drawn with it. Without kinds there is
        // one model and one entry.
        const bool by_kind = models_by_kind(emitter);
        std::vector<std::string> names{emitter.renderer.mesh};
        // Each kind's own size beside its model, so what is drawn is the
        // body the engine built. Kind 0 is the particle's size exactly.
        std::vector<float> scales{1.0F};
        std::vector<Color> tints{Color{}};
        if (by_kind) {
            for (const RigidBodyKind& kind : rigid_physics(emitter)->bodies) {
                names.push_back(kind.draw.empty() ? emitter.renderer.mesh : kind.draw);
                scales.push_back(kind.scale);
                tints.push_back(kind.draw_tint);
            }
        }
        // One pair of buffers holds every model the emitter draws, each
        // model's indices shifted to where its vertices landed. A kind drawn
        // as a model another kind already placed shares its range.
        std::vector<float> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<std::uint32_t> table{by_kind ? offset_or_absent(particle_layout(emitter).body_kind) : absent,
                                         static_cast<std::uint32_t>(names.size())};
        std::map<std::string, std::pair<std::uint32_t, std::uint32_t>, std::less<>> placed;
        State::MeshBuffers buffers;
        for (std::size_t n = 0U; n < names.size(); ++n) {
            const std::string& name = names[n];
            if (!placed.contains(name)) {
                const MeshData& mesh = meshes.find(name)->second;
                const auto base = static_cast<std::uint32_t>(vertices.size() / 8U);
                const auto first = static_cast<std::uint32_t>(indices.size());
                for (const MeshVertex& v : mesh.vertices) {
                    vertices.insert(vertices.end(), {v.position.x, v.position.y, v.position.z, v.normal.x,
                                                     v.normal.y, v.normal.z, v.u, v.v});
                }
                for (const std::uint32_t index : mesh.indices) {
                    indices.push_back(base + index);
                }
                const auto count = static_cast<std::uint32_t>(mesh.indices.size());
                placed.emplace(name, std::pair{first, count});
                buffers.index_count = std::max(buffers.index_count, count);
            }
            const auto [first, count] = placed.find(name)->second;
            table.push_back(first);
            table.push_back(count);
            // The scale travels as its bits, since the table is one buffer
            // of words and the shader reads it back with asfloat.
            std::uint32_t bits = 0U;
            const float scale = scales[n];
            std::memcpy(&bits, &scale, sizeof(bits));
            table.push_back(bits);
            table.push_back(std::bit_cast<std::uint32_t>(tints[n].r));
            table.push_back(std::bit_cast<std::uint32_t>(tints[n].g));
            table.push_back(std::bit_cast<std::uint32_t>(tints[n].b));
            table.push_back(std::bit_cast<std::uint32_t>(tints[n].a));
        }
        for (const auto& [bytes, label, target] :
             {std::tuple{vertices.size() * 4U, " mesh vertices", &buffers.vertices},
              std::tuple{indices.size() * 4U, " mesh indices", &buffers.indices},
              std::tuple{std::size_t{16U}, " mesh draw", &buffers.args},
              std::tuple{table.size() * 4U, " mesh models", &buffers.models}}) {
            Result<Buffer> created = device.create_buffer(bytes, "ajfx " + emitter.name + label);
            if (!created) {
                return created.error();
            }
            *target = created.value();
        }
        device.write_buffer(buffers.vertices, 0U,
                            std::span{reinterpret_cast<const std::uint8_t*>(vertices.data()), vertices.size() * 4U});
        device.write_buffer(
            buffers.indices, 0U,
            std::span{reinterpret_cast<const std::uint8_t*>(indices.data()), indices.size() * 4U});
        device.write_buffer(buffers.models, 0U,
                            std::span{reinterpret_cast<const std::uint8_t*>(table.data()), table.size() * 4U});
        out = buffers;
    }
    std::map<std::string, Texture, std::less<>> by_name;
    const auto upload = [&](const std::string& name) -> Result<Texture> {
        if (name.empty()) {
            return Texture{};
        }
        if (const auto found = by_name.find(name); found != by_name.end()) {
            return found->second;
        }
        Result<Texture> created = device.create_texture(textures.find(name)->second, "ajfx texture " + name);
        if (!created) {
            return created.error();
        }
        state->uploaded.push_back(created.value());
        by_name.emplace(name, created.value());
        return created.value();
    };
    std::vector<float> looks;
    for (const Emitter& emitter : effect.emitters) {
        Result<Texture> texture = upload(emitter.renderer.texture);
        if (!texture) {
            return texture.error();
        }
        Result<Texture> lightmap = upload(emitter.renderer.lightmap);
        if (!lightmap) {
            return lightmap.error();
        }
        state->emitter_textures.push_back(texture.value());
        state->emitter_lightmaps.push_back(lightmap.value());
        looks.insert(looks.end(), {emitter.renderer.curvature, emitter.renderer.wrap, emitter.renderer.ambient, 0.0F});
    }
    Result<Buffer> look_buffer =
        device.create_buffer(std::max<std::size_t>(looks.size(), 4U) * sizeof(float), "ajfx looks");
    if (!look_buffer) {
        return look_buffer.error();
    }
    state->looks = look_buffer.value();
    if (!looks.empty()) {
        device.write_buffer(state->looks, 0U,
                            std::span{reinterpret_cast<const std::uint8_t*>(looks.data()), looks.size() * sizeof(float)});
    }
    for (const auto& [blend, cull, out, name] :
         {std::tuple{Blend::Premultiplied, false, &state->blended, "ajfx sprites (blended)"},
          std::tuple{Blend::Additive, false, &state->additive, "ajfx sprites (additive)"},
          std::tuple{Blend::Premultiplied, true, &state->blended_mesh, "ajfx meshes (blended)"},
          std::tuple{Blend::Additive, true, &state->additive_mesh, "ajfx meshes (additive)"}}) {
        Result<Pipeline> pipeline = device.create_graphics_pipeline(
            GraphicsPipelineDesc{kernels.sprite_vertex, kernels.sprite_pixel, blend, format, cull}, name);
        if (!pipeline) {
            return pipeline.error();
        }
        *out = pipeline.value();
    }
    if (!effect.volumes.empty()) {
        if (kernels.volume_vertex.empty() || kernels.volume_pixel.empty()) {
            return Error{ErrorCode::GpuUnavailable, "the kernel set has no volume shaders for this device"};
        }
        Result<Pipeline> pipeline = device.create_graphics_pipeline(
            GraphicsPipelineDesc{kernels.volume_vertex, kernels.volume_pixel, Blend::Premultiplied, format, false},
            "ajfx volumes");
        if (!pipeline) {
            return pipeline.error();
        }
        state->volume = pipeline.value();
    }
    Result<Buffer> camera = device.create_buffer(camera_floats * sizeof(float), "ajfx camera");
    if (!camera) {
        return camera.error();
    }
    state->camera = camera.value();
    return ParticleRenderer{std::move(state)};
}

void ParticleRenderer::render(const GpuSimulation& simulation, ParticleSorter& sorter, const Camera& camera,
                              Texture target, const std::array<float, 4>* clear_color, const SceneLight& light) {
    Device& device = *state_->device;
    float data[camera_floats]{};
    std::memcpy(data, camera.view_projection.data(), sizeof(float) * 16U);
    for (const auto& [at, axis] :
         {std::pair{16, camera.right}, std::pair{20, camera.up}, std::pair{24, camera.forward},
          std::pair{28, light.direction}, std::pair{32, light.color}, std::pair{36, light.sky},
          std::pair{40, light.ground}}) {
        data[at] = axis.x;
        data[at + 1] = axis.y;
        data[at + 2] = axis.z;
    }
    const std::array<float, 16> unprojection = inverse(camera.view_projection);
    std::memcpy(data + 44, unprojection.data(), sizeof(float) * 16U);
    device.write_buffer(state_->camera, 0U,
                        std::span{reinterpret_cast<const std::uint8_t*>(data), sizeof(data)});

    // Sorting is compute work, recorded before the drawing begins; so is
    // each mesh's draw arguments: its index count as the vertex count, the
    // simulation's live count as the instance count.
    const Effect& effect = simulation.effect();
    std::vector<Buffer> orders(effect.emitters.size());
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        orders[i] = sorter.sort(simulation, i, camera.forward);
        if (i < state_->meshes.size() && state_->meshes[i]) {
            const State::MeshBuffers& mesh = *state_->meshes[i];
            const std::uint32_t args[4] = {mesh.index_count, 0U, 0U, 0U};
            device.write_buffer(mesh.args, 0U, std::span{reinterpret_cast<const std::uint8_t*>(args), sizeof(args)});
            const EmitterBuffers buffers = simulation.emitter_buffers(i);
            device.copy_buffer(mesh.args, 4U, buffers.args, buffers.draw_args_offset + 4U, 4U);
        }
    }

    device.begin_rendering(target, clear_color);
    // The grid volumes first, raymarched; the particles draw over them.
    for (std::size_t v = 0U; state_->volume.valid() && v < effect.volumes.size(); ++v) {
        const Volume& volume = effect.volumes[v];
        const VolumeBuffers cells = simulation.volume_buffers(v);
        if (!cells.buffer.valid()) {
            continue;
        }
        Draw draw;
        draw.pipeline = state_->volume;
        draw.constants = {cells.cells_at,
                          cells.nx,
                          cells.ny,
                          cells.nz,
                          std::bit_cast<std::uint32_t>(cells.origin.x),
                          std::bit_cast<std::uint32_t>(cells.origin.y),
                          std::bit_cast<std::uint32_t>(cells.origin.z),
                          std::bit_cast<std::uint32_t>(cells.cell_size),
                          std::bit_cast<std::uint32_t>(volume.extinction),
                          std::bit_cast<std::uint32_t>(volume.albedo),
                          std::bit_cast<std::uint32_t>(volume.color.x),
                          std::bit_cast<std::uint32_t>(volume.color.y),
                          std::bit_cast<std::uint32_t>(volume.color.z),
                          volume_ray_steps,
                          volume_light_steps,
                          0U};
        draw.buffers[0] = cells.buffer;
        draw.buffers[1] = state_->camera;
        draw.vertex_count = 3U;
        draw.instance_count = 1U;
        device.draw(draw);
    }
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        const SpriteRenderer& settings = effect.emitters[i].renderer;
        const EmitterBuffers buffers = simulation.emitter_buffers(i);
        Draw draw;
        const bool mesh = settings.geometry == Geometry::Mesh;
        draw.pipeline = settings.blend == BlendMode::Additive ? (mesh ? state_->additive_mesh : state_->additive)
                                                              : (mesh ? state_->blended_mesh : state_->blended);
        draw.constants[0] = buffers.layout.stride;
        draw.constants[1] = offset_or_absent(buffers.layout.position);
        draw.constants[2] = offset_or_absent(buffers.layout.velocity);
        draw.constants[3] = offset_or_absent(buffers.layout.color);
        draw.constants[4] = offset_or_absent(buffers.layout.size);
        draw.constants[5] = offset_or_absent(buffers.layout.rotation);
        draw.constants[6] = settings.facing == Facing::Velocity ? 1U : 0U;
        draw.constants[7] = settings.blend == BlendMode::Premultiplied ? 1U : 0U;
        draw.constants[8] = std::bit_cast<std::uint32_t>(settings.stretch);
        draw.constants[9] = orders[i].valid() ? 1U : 0U;
        draw.constants[10] = offset_or_absent(buffers.layout.age);
        draw.constants[11] = offset_or_absent(buffers.layout.lifetime);
        draw.constants[12] = settings.flipbook_columns | (settings.flipbook_rows << 16U);
        const Texture texture = i < state_->emitter_textures.size() ? state_->emitter_textures[i] : Texture{};
        draw.constants[13] = (texture.valid() ? 1U : 0U) | (settings.flipbook_blend ? 2U : 0U) |
                             (static_cast<std::uint32_t>(settings.lighting) << 4U) |
                             (static_cast<std::uint32_t>(i) << 8U);
        draw.constants[14] = static_cast<std::uint32_t>(settings.geometry);
        draw.constants[15] = offset_or_absent(buffers.layout.orientation);
        draw.textures[0] = texture;
        draw.textures[1] = i < state_->emitter_lightmaps.size() ? state_->emitter_lightmaps[i] : Texture{};
        draw.buffers[0] = buffers.particles;
        draw.buffers[1] = state_->camera;
        draw.buffers[2] = orders[i];
        draw.buffers[3] = buffers.counters;
        draw.buffers[7] = state_->looks;
        draw.indirect = buffers.args;
        draw.indirect_offset = buffers.draw_args_offset;
        if (i < state_->meshes.size() && state_->meshes[i]) {
            draw.buffers[4] = state_->meshes[i]->vertices;
            draw.buffers[5] = state_->meshes[i]->indices;
            draw.buffers[6] = state_->meshes[i]->models;
            draw.indirect = state_->meshes[i]->args;
            draw.indirect_offset = 0U;
        }
        device.draw(draw);
    }
    device.end_rendering();
}

} // namespace ajfx::gpu
