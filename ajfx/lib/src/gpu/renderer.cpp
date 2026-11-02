#include <ajfx/gpu/renderer.hpp>

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
    camera.view_projection = {
        1.0F / half_width, 0.0F, 0.0F, -center.x / half_width,
        0.0F, 1.0F / half_height, 0.0F, -center.y / half_height,
        0.0F, 0.0F, 0.0F, 0.5F,
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
    Buffer camera;
    // Each emitter's texture, invalid for an untextured one; one upload per
    // name.
    std::vector<Texture> emitter_textures;
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
            device->destroy_buffer(camera);
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
    for (const Emitter& emitter : effect.emitters) {
        Texture texture;
        if (!emitter.renderer.texture.empty()) {
            const auto found = by_name.find(emitter.renderer.texture);
            if (found != by_name.end()) {
                texture = found->second;
            } else {
                Result<Texture> created = device.create_texture(textures.find(emitter.renderer.texture)->second,
                                                                "ajfx texture " + emitter.renderer.texture);
                if (!created) {
                    return created.error();
                }
                texture = created.value();
                state->uploaded.push_back(texture);
                by_name.emplace(emitter.renderer.texture, texture);
            }
        }
        state->emitter_textures.push_back(texture);
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
    Result<Buffer> camera = device.create_buffer(28U * sizeof(float), "ajfx camera");
    if (!camera) {
        return camera.error();
    }
    state->camera = camera.value();
    return ParticleRenderer{std::move(state)};
}

void ParticleRenderer::render(const GpuSimulation& simulation, ParticleSorter& sorter, const Camera& camera,
                              Texture target, const std::array<float, 4>* clear_color) {
    Device& device = *state_->device;
    float data[28]{};
    std::memcpy(data, camera.view_projection.data(), sizeof(float) * 16U);
    for (const auto& [at, axis] : {std::pair{16, camera.right}, std::pair{20, camera.up}, std::pair{24, camera.forward}}) {
        data[at] = axis.x;
        data[at + 1] = axis.y;
        data[at + 2] = axis.z;
    }
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
        draw.constants[13] = (texture.valid() ? 1U : 0U) | (settings.flipbook_blend ? 2U : 0U);
        draw.constants[14] = static_cast<std::uint32_t>(settings.geometry);
        draw.constants[15] = offset_or_absent(buffers.layout.orientation);
        draw.textures[0] = texture;
        draw.buffers[0] = buffers.particles;
        draw.buffers[1] = state_->camera;
        draw.buffers[2] = orders[i];
        draw.buffers[3] = buffers.counters;
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
