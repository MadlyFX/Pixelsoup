#include <ajfx/gpu/renderer.hpp>

#include <bit>
#include <cmath>
#include <cstring>
#include <map>
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
    Buffer camera;
    // Each emitter's texture, invalid for an untextured one; one upload per
    // name.
    std::vector<Texture> emitter_textures;
    std::vector<Texture> uploaded;

    ~State() {
        if (device != nullptr) {
            device->destroy_pipeline(blended);
            device->destroy_pipeline(additive);
            device->destroy_buffer(camera);
            for (const Texture texture : uploaded) {
                device->destroy_texture(texture);
            }
        }
    }
};

ParticleRenderer::ParticleRenderer(std::unique_ptr<State> state) : state_{std::move(state)} {}
ParticleRenderer::ParticleRenderer(ParticleRenderer&&) noexcept = default;
ParticleRenderer& ParticleRenderer::operator=(ParticleRenderer&&) noexcept = default;
ParticleRenderer::~ParticleRenderer() = default;

Result<ParticleRenderer> ParticleRenderer::create(Device& device, const Effect& effect, const KernelSet& kernels,
                                                  const TextureSet& textures, TextureFormat format) {
    if (kernels.format != device.shader_format() || kernels.sprite_vertex.empty() || kernels.sprite_pixel.empty()) {
        return Error{ErrorCode::GpuUnavailable, "the kernel set has no sprite shaders for this device"};
    }
    if (Result<bool> checked = check_textures(effect, textures); !checked) {
        return checked.error();
    }
    auto state = std::make_unique<State>();
    state->device = &device;
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
    for (const auto& [blend, out, name] : {std::tuple{Blend::Premultiplied, &state->blended, "ajfx sprites (blended)"},
                                           std::tuple{Blend::Additive, &state->additive, "ajfx sprites (additive)"}}) {
        Result<Pipeline> pipeline = device.create_graphics_pipeline(
            GraphicsPipelineDesc{kernels.sprite_vertex, kernels.sprite_pixel, blend, format}, name);
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

    // Sorting is compute work, recorded before the drawing begins.
    const Effect& effect = simulation.effect();
    std::vector<Buffer> orders(effect.emitters.size());
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        orders[i] = sorter.sort(simulation, i, camera.forward);
    }

    device.begin_rendering(target, clear_color);
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        const SpriteRenderer& settings = effect.emitters[i].renderer;
        const EmitterBuffers buffers = simulation.emitter_buffers(i);
        Draw draw;
        draw.pipeline = settings.blend == BlendMode::Additive ? state_->additive : state_->blended;
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
        draw.constants[14] = settings.geometry == Geometry::Ribbon ? 1U : 0U;
        draw.textures[0] = texture;
        draw.buffers[0] = buffers.particles;
        draw.buffers[1] = state_->camera;
        draw.buffers[2] = orders[i];
        draw.buffers[3] = buffers.counters;
        draw.indirect = buffers.args;
        draw.indirect_offset = buffers.draw_args_offset;
        device.draw(draw);
    }
    device.end_rendering();
}

} // namespace ajfx::gpu
