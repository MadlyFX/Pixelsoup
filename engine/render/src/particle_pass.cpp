#include <mediaserver/render/particle_pass.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/particle_sprite_frag_dxil.h>
#include <mediaserver/shaders/particle_sprite_vert_dxil.h>
#include <mediaserver/shaders/particle_velocity_frag_dxil.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

constexpr core::u32 absent = 0xffffffffU;

// Matches the ParticleView constant buffer in particle_sprite.vert.hlsl.
struct ViewConstants final {
    std::array<float, 16U> view_projection{}; // column-major, as scene::Mat4
    std::array<float, 16U> world{};           // object to world, four columns
    std::array<float, 4U> camera_right{};
    std::array<float, 4U> camera_up{};
    std::array<float, 4U> camera_forward{};
    core::u32 stride{};
    core::u32 position_offset{absent};
    core::u32 velocity_offset{absent};
    core::u32 color_offset{absent};
    core::u32 size_offset{absent};
    core::u32 rotation_offset{absent};
    core::u32 facing_velocity{};
    core::u32 premultiplied{};
    std::array<float, 4U> extra{}; // x: stretch, y: 1 for additive light
    core::u32 age_offset{absent};
    core::u32 lifetime_offset{absent};
    core::u32 flipbook{};
    core::u32 flags{};
    std::array<float, 16U> canvas_view_projection{};
    std::array<float, 16U> previous_view_projection{};
    std::array<float, 4U> motion{}; // x: seconds back, y: 1 when valid, z: soft distance
    std::array<float, 4U> depth_terms{}; // projection (2,2), (2,3), (3,2), (3,3)
};
static_assert(sizeof(ViewConstants) == 400U);

// ViewConstants::flags.
constexpr core::u32 flag_textured = 1U;
constexpr core::u32 flag_blend_cells = 2U;
constexpr core::u32 flag_srgb = 4U;
constexpr core::u32 flag_sorted = 8U;
constexpr core::u32 flag_soft = 16U;
constexpr core::u32 flag_ribbon = 32U;

core::u32 offset_or_absent(std::int32_t offset) {
    return offset >= 0 ? static_cast<core::u32>(offset) : absent;
}

ParticleError failure(ParticleErrorCode code, std::string message) {
    return ParticleError{code, std::move(message)};
}

} // namespace

// ------------------------------------------------------------ instance

struct ParticleInstance::State final {
    ajfx::Effect effect;
    std::optional<ajfx::gpu::GpuSimulation> simulation;
    // Checkpoint slots, made at creation and reused; `saved` holds the step
    // each one has, if any.
    std::vector<ajfx::gpu::GpuCheckpoint> checkpoints;
    std::vector<std::optional<core::u64>> saved;
    core::u64 target{0U};
    // Set by restart: the next advance resets the simulation first.
    bool restart{};
    std::optional<ajfx::gpu::ParticleSorter> sorter;
    // One per texture name, staged a recording's upload space at a time;
    // the texels (not the size or colour space) are dropped once they have
    // all been recorded.
    struct Upload final {
        rhi::Texture texture;
        ajfx::TextureData data;
        core::u32 mip{0U};
        core::u32 row{0U};
        bool landed{};
    };
    std::vector<Upload> textures;
    // Each emitter's index in `textures`, if it has one.
    std::vector<std::optional<std::size_t>> emitter_texture;
    // Each emitter's drawing order from the last draw, for its motion.
    std::vector<ajfx::gpu::Buffer> orders;

    [[nodiscard]] bool textures_landed() const noexcept {
        return std::all_of(textures.begin(), textures.end(), [](const Upload& upload) { return upload.landed; });
    }
};

ParticleInstance::ParticleInstance(std::unique_ptr<State> state) : state_{std::move(state)} {}
ParticleInstance::ParticleInstance(ParticleInstance&&) noexcept = default;
ParticleInstance& ParticleInstance::operator=(ParticleInstance&&) noexcept = default;
ParticleInstance::~ParticleInstance() {
    // Checkpoints hold device buffers the simulation's device must release
    // first-in, last-out: drop them before the simulation.
    if (state_) {
        state_->checkpoints.clear();
        state_->simulation.reset();
    }
}

const ajfx::Effect& ParticleInstance::effect() const noexcept {
    return state_->effect;
}

core::u64 ParticleInstance::step() const noexcept {
    return state_->simulation->tick();
}

core::u64 ParticleInstance::target_step() const noexcept {
    return state_->target;
}

bool ParticleInstance::caught_up() const noexcept {
    return step() == target_step() && state_->textures_landed();
}

ajfx::gpu::GpuSimulation& ParticleInstance::simulation() noexcept {
    return *state_->simulation;
}

// ------------------------------------------------------------ pass

struct ParticlePass::State final {
    rhi::Device* device{};
    std::unique_ptr<ParticleDevice> particles;
    rhi::GraphicsPipeline blended;  // alpha and premultiplied emitters
    rhi::GraphicsPipeline additive;
    // Sprite motion into the scene's velocity target.
    rhi::GraphicsPipeline velocity;
    // Bound for emitters without a texture, which never sample it.
    std::optional<rhi::Texture> blank;
    std::optional<rhi::Sampler> sampler;
    ParticleSettings settings;
    std::optional<ParticleError> error;

    void note(std::string message) {
        if (!error) {
            error = failure(ParticleErrorCode::DeviceFailure, std::move(message));
        }
    }
};

ParticlePass::ParticlePass(std::unique_ptr<State> state) : state_{std::move(state)} {}
ParticlePass::ParticlePass(ParticlePass&&) noexcept = default;
ParticlePass& ParticlePass::operator=(ParticlePass&&) noexcept = default;
ParticlePass::~ParticlePass() = default;

core::Result<ParticlePass, ParticleError> ParticlePass::create(rhi::Device& device, rhi::TextureFormat color_format,
                                                               ParticleSettings settings) {
    using Result = core::Result<ParticlePass, ParticleError>;
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    if (settings.step_budget == 0U || settings.checkpoint_interval == 0U) {
        return Result::failure(failure(ParticleErrorCode::InvalidEffect, "particle step budget and checkpoint interval must be positive"));
    }
    auto particles = ParticleDevice::create(device);
    if (!particles) {
        return Result::failure(failure(ParticleErrorCode::Unsupported,
                                       "this renderer cannot simulate particles: it needs compute with writable "
                                       "buffers and indirect dispatch"));
    }
    auto state = std::make_unique<State>();
    state->device = &device;
    state->particles = std::move(particles).value();
    state->settings = settings;

    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = rhi::ShaderFormat::Dxil;
    vertex.bytecode = std::span<const core::u8>{reinterpret_cast<const core::u8*>(mediaserver_particle_sprite_vert_dxil),
                                                sizeof(mediaserver_particle_sprite_vert_dxil)};
    vertex.storage_buffer_count = 3U;
    vertex.uniform_buffer_count = 1U;
    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = rhi::ShaderFormat::Dxil;
    fragment.bytecode = std::span<const core::u8>{reinterpret_cast<const core::u8*>(mediaserver_particle_sprite_frag_dxil),
                                                  sizeof(mediaserver_particle_sprite_frag_dxil)};
    // The texture, and the scene depth soft particles fade against.
    fragment.sampler_count = 2U;
    // Tested against the scene, never written: a particle is not a surface
    // anything behind it should be hidden by.
    rhi::DepthState depth{};
    depth.enabled = true;
    depth.write = false;
    depth.greater = true;
    auto blended = device.create_pipeline(vertex, fragment, color_format, rhi::ColorBlend::PremultipliedNormal, depth);
    auto additive = device.create_pipeline(vertex, fragment, color_format, rhi::ColorBlend::PremultipliedEmit, depth);
    rhi::ShaderDesc motion = fragment;
    motion.bytecode = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_particle_velocity_frag_dxil),
        sizeof(mediaserver_particle_velocity_frag_dxil)};
    auto velocity = device.create_pipeline(vertex, motion, rhi::TextureFormat::Rgba32Float, rhi::ColorBlend::Disabled,
                                           depth);
    if (!blended || !additive || !velocity) {
        return Result::failure(failure(ParticleErrorCode::Unsupported, "the particle sprite pipelines could not be made"));
    }
    state->blended = std::move(blended).value();
    state->additive = std::move(additive).value();
    state->velocity = std::move(velocity).value();
    rhi::TextureDesc blank{};
    blank.width = 1U;
    blank.height = 1U;
    blank.sampled = true;
    auto blank_texture = device.create_texture(blank);
    auto sampler = device.create_sampler(rhi::SamplerDesc{});
    if (!blank_texture || !sampler) {
        return Result::failure(failure(ParticleErrorCode::Unsupported, "the particle texture sampler could not be made"));
    }
    state->blank.emplace(std::move(blank_texture).value());
    state->sampler.emplace(std::move(sampler).value());
    return Result::success(ParticlePass{std::move(state)});
#else
    (void)device;
    (void)color_format;
    (void)settings;
    return Result::failure(failure(ParticleErrorCode::Unsupported, "this build has no particle sprite shaders"));
#endif
}

core::Result<ParticleInstance, ParticleError> ParticlePass::create_instance(const ajfx::Effect& effect,
                                                                           const ajfx::gpu::KernelSet& kernels,
                                                                           const ajfx::TextureSet& textures,
                                                                           core::u32 seed) {
    using Result = core::Result<ParticleInstance, ParticleError>;
    State& s = *state_;
    if (ajfx::Result<bool> checked = ajfx::check_textures(effect, textures); !checked) {
        return Result::failure(failure(ParticleErrorCode::InvalidEffect, checked.error().message));
    }
    ajfx::Result<ajfx::gpu::GpuSimulation> simulation =
        ajfx::gpu::GpuSimulation::create(*s.particles, effect, kernels, seed);
    if (!simulation) {
        const ajfx::ErrorCode code = simulation.error().code;
        return Result::failure(failure(code == ajfx::ErrorCode::DeviceFailure ? ParticleErrorCode::DeviceFailure
                                                                                : ParticleErrorCode::InvalidEffect,
                                       simulation.error().message));
    }
    auto state = std::make_unique<ParticleInstance::State>();
    state->effect = effect;
    state->simulation.emplace(std::move(simulation).value());
    // Every checkpoint is allocated now, so saving one while recording a
    // frame copies into existing buffers. Nothing is recorded or submitted
    // here: creating an instance only allocates.
    for (core::u32 slot = 0U; slot < s.settings.checkpoint_count; ++slot) {
        ajfx::Result<ajfx::gpu::GpuCheckpoint> checkpoint = state->simulation->allocate_checkpoint();
        if (!checkpoint) {
            return Result::failure(failure(ParticleErrorCode::DeviceFailure, checkpoint.error().message));
        }
        state->checkpoints.push_back(std::move(checkpoint).value());
        state->saved.emplace_back();
    }
    ajfx::Result<ajfx::gpu::ParticleSorter> sorter = ajfx::gpu::ParticleSorter::create(*s.particles, effect, kernels);
    if (!sorter) {
        return Result::failure(failure(ParticleErrorCode::DeviceFailure, sorter.error().message));
    }
    state->sorter.emplace(std::move(sorter).value());
    // Textures are made now and filled by later advances.
    for (const ajfx::Emitter& emitter : effect.emitters) {
        std::optional<std::size_t> index;
        if (!emitter.renderer.texture.empty()) {
            const ajfx::TextureData& data = textures.find(emitter.renderer.texture)->second;
            for (std::size_t e = 0U; e < effect.emitters.size() && !index; ++e) {
                if (state->emitter_texture.size() > e && state->emitter_texture[e] &&
                    effect.emitters[e].renderer.texture == emitter.renderer.texture) {
                    index = state->emitter_texture[e];
                }
            }
            if (!index) {
                rhi::TextureDesc desc{};
                desc.width = data.width;
                desc.height = data.height;
                desc.sampled = true;
                desc.mip_levels = static_cast<core::u32>(data.mips.size());
                auto texture = s.device->create_texture(desc);
                if (!texture) {
                    return Result::failure(failure(ParticleErrorCode::DeviceFailure,
                                                   "texture '" + emitter.renderer.texture + "' could not be made"));
                }
                index = state->textures.size();
                state->textures.push_back({std::move(texture).value(), data});
            }
        }
        state->emitter_texture.push_back(index);
    }
    return Result::success(ParticleInstance{std::move(state)});
}

bool ParticlePass::advance(rhi::CommandList& commands, ParticleInstance& instance, core::u64 show_frame,
                           core::u32 rate_num, core::u32 rate_den, const ajfx::ParameterProvider* parameters) {
    State& s = *state_;
    ParticleInstance::State& i = *instance.state_;
    ajfx::gpu::GpuSimulation& simulation = *i.simulation;
    if (rate_num == 0U || rate_den == 0U) {
        s.note("advance needs a positive show rate");
        return false;
    }
    i.target = ajfx::tick_at(show_frame, rate_num, rate_den, i.effect.simulation_rate);

    // Textures first, as much as this recording's upload space holds.
    for (ParticleInstance::State::Upload& upload : i.textures) {
        while (!upload.landed) {
            const core::u32 width = ajfx::mip_size(upload.data.width, upload.mip);
            const core::u32 height = ajfx::mip_size(upload.data.height, upload.mip);
            const core::u32 room = commands.texture_upload_row_capacity(upload.texture);
            if (room == 0U) {
                break;
            }
            const core::u32 rows = std::min(height - upload.row, room);
            const std::size_t row_bytes = std::size_t{width} * 4U;
            const std::span<const core::u8> texels{upload.data.mips[upload.mip].data() + upload.row * row_bytes,
                                                   rows * row_bytes};
            if (!commands.upload_texture_rows(upload.texture, upload.mip, 0U, upload.row, rows, texels)) {
                s.note("a particle texture upload was refused");
                return false;
            }
            upload.row += rows;
            if (upload.row == height) {
                upload.row = 0U;
                if (++upload.mip == upload.data.mips.size()) {
                    upload.landed = true;
                    upload.data.mips = {};
                }
            }
        }
    }

    s.particles->begin_recording(commands);
    if (i.restart) {
        simulation.reset();
        i.restart = false;
    }
    // Start from the latest checkpoint at or before the target when that is
    // closer than where the simulation is: always when going back, and when
    // a scrub forward passes a checkpoint already made.
    std::optional<std::size_t> best;
    for (std::size_t slot = 0U; slot < i.saved.size(); ++slot) {
        if (i.saved[slot] && *i.saved[slot] <= i.target && (!best || *i.saved[slot] > *i.saved[*best])) {
            best = slot;
        }
    }
    if (i.target < simulation.tick()) {
        if (best) {
            simulation.restore(i.checkpoints[*best]);
        } else {
            simulation.reset();
        }
    } else if (best && *i.saved[*best] > simulation.tick()) {
        simulation.restore(i.checkpoints[*best]);
    }

    for (core::u32 steps = 0U; simulation.tick() < i.target && steps < s.settings.step_budget; ++steps) {
        const core::u64 tick = simulation.tick();
        const bool held = std::any_of(i.saved.begin(), i.saved.end(),
                                      [tick](const std::optional<core::u64>& saved) { return saved == tick; });
        if (!i.checkpoints.empty() && tick % s.settings.checkpoint_interval == 0U && !held) {
            // An empty slot, else the one holding the earliest step.
            std::size_t slot = 0U;
            for (std::size_t candidate = 0U; candidate < i.saved.size(); ++candidate) {
                if (!i.saved[candidate]) {
                    slot = candidate;
                    break;
                }
                if (i.saved[slot] && *i.saved[candidate] < *i.saved[slot]) {
                    slot = candidate;
                }
            }
            simulation.save_into(i.checkpoints[slot]);
            i.saved[slot] = tick;
        }
        simulation.step(parameters);
    }
    s.particles->end_recording();
    if (std::optional<ajfx::Error> error = s.particles->take_error()) {
        s.note(error->message);
        return false;
    }
    return instance.caught_up();
}

void ParticlePass::restart(ParticleInstance& instance) noexcept {
    ParticleInstance::State& i = *instance.state_;
    std::fill(i.saved.begin(), i.saved.end(), std::nullopt);
    i.restart = true;
}

bool ParticlePass::draw(rhi::CommandList& commands, ParticleInstance& instance, const scene::ViewMatrices& view,
                        const scene::Mat4& object_to_world, const ParticleTargets& targets) {
    State& s = *state_;
    if (!instance.caught_up() || targets.color == nullptr || targets.depth == nullptr) {
        return false;
    }
    ParticleInstance::State& i = *instance.state_;

    // Back to front along the camera's forward axis, in the effect's own
    // space: depth = dot(world(p), forward) is dot(p, W^T forward) plus a
    // constant, for W the 3x3 part of object_to_world (column-major).
    std::array<float, 3U> forward{};
    for (core::u32 column = 0U; column < 3U; ++column) {
        forward[column] = -view.view.at(2U, column);
    }
    ajfx::Vec3 axis{};
    float* const axis_out[3] = {&axis.x, &axis.y, &axis.z};
    for (core::u32 column = 0U; column < 3U; ++column) {
        *axis_out[column] = object_to_world.m[column * 4U + 0U] * forward[0] +
                            object_to_world.m[column * 4U + 1U] * forward[1] +
                            object_to_world.m[column * 4U + 2U] * forward[2];
    }
    i.orders.assign(i.effect.emitters.size(), ajfx::gpu::Buffer{});
    s.particles->begin_recording(commands);
    for (std::size_t e = 0U; e < i.orders.size(); ++e) {
        i.orders[e] = i.sorter->sort(*i.simulation, e, axis);
    }
    s.particles->end_recording();
    if (std::optional<ajfx::Error> error = s.particles->take_error()) {
        s.note(error->message);
        return false;
    }
    return record_sprites(commands, instance, view, object_to_world, targets, false);
}

bool ParticlePass::draw_motion(rhi::CommandList& commands, ParticleInstance& instance,
                               const scene::ViewMatrices& view, const scene::Mat4& object_to_world,
                               const ParticleTargets& targets) {
    const ParticleInstance::State& i = *instance.state_;
    if (!instance.caught_up() || targets.velocity == nullptr || targets.depth == nullptr ||
        i.orders.size() != i.effect.emitters.size()) {
        return false;
    }
    return record_sprites(commands, instance, view, object_to_world, targets, true);
}

bool ParticlePass::record_sprites(rhi::CommandList& commands, ParticleInstance& instance,
                                  const scene::ViewMatrices& view, const scene::Mat4& object_to_world,
                                  const ParticleTargets& targets, bool motion) {
    State& s = *state_;
    const ParticleInstance::State& i = *instance.state_;
    ViewConstants constants;
    constants.view_projection = view.view_projection.m;
    constants.world = object_to_world.m;
    // The view matrix's rows are the camera's axes in world space; it looks
    // down its own -Z.
    for (core::u32 column = 0U; column < 3U; ++column) {
        constants.camera_right[column] = view.view.at(0U, column);
        constants.camera_up[column] = view.view.at(1U, column);
        constants.camera_forward[column] = -view.view.at(2U, column);
    }
    constants.canvas_view_projection = targets.canvas_view_projection.m;
    constants.previous_view_projection = targets.previous_view_projection.m;
    // How reversed device depth maps back to distance, for soft particles.
    constants.depth_terms = {view.projection.at(2U, 2U), view.projection.at(2U, 3U), view.projection.at(3U, 2U),
                             view.projection.at(3U, 3U)};

    bool first = true;
    for (std::size_t e = 0U; e < i.effect.emitters.size(); ++e) {
        const ajfx::SpriteRenderer& sprite = i.effect.emitters[e].renderer;
        const ajfx::gpu::EmitterBuffers buffers = i.simulation->emitter_buffers(e);
        const rhi::StorageBuffer* records = s.particles->buffer(buffers.particles);
        const rhi::StorageBuffer* arguments = s.particles->buffer(buffers.args);
        if (records == nullptr || arguments == nullptr ||
            buffers.draw_args_offset > std::numeric_limits<core::u32>::max()) {
            s.note("an emitter's particle buffers are missing");
            return false;
        }
        constants.stride = buffers.layout.stride;
        constants.position_offset = offset_or_absent(buffers.layout.position);
        constants.velocity_offset = offset_or_absent(buffers.layout.velocity);
        constants.color_offset = offset_or_absent(buffers.layout.color);
        constants.size_offset = offset_or_absent(buffers.layout.size);
        constants.rotation_offset = offset_or_absent(buffers.layout.rotation);
        constants.facing_velocity = sprite.facing == ajfx::Facing::Velocity ? 1U : 0U;
        constants.premultiplied = sprite.blend == ajfx::BlendMode::Premultiplied ? 1U : 0U;
        constants.extra = {sprite.stretch, sprite.blend == ajfx::BlendMode::Additive ? 1.0F : 0.0F, 0.0F, 0.0F};
        constants.age_offset = offset_or_absent(buffers.layout.age);
        constants.lifetime_offset = offset_or_absent(buffers.layout.lifetime);
        constants.flipbook = sprite.flipbook_columns | (sprite.flipbook_rows << 16U);
        const bool soft = !motion && targets.scene_depth != nullptr && sprite.soft_distance > 0.0F;
        constants.motion = {targets.motion_valid ? targets.motion_seconds : 0.0F, targets.motion_valid ? 1.0F : 0.0F,
                            soft ? sprite.soft_distance : 0.0F, 0.0F};
        const std::optional<std::size_t> texture = i.emitter_texture[e];
        constants.flags = (texture ? flag_textured : 0U) | (sprite.flipbook_blend ? flag_blend_cells : 0U) |
                          (texture && i.textures[*texture].data.color_space == ajfx::ColorSpace::Srgb ? flag_srgb : 0U) |
                          (soft ? flag_soft : 0U) | (sprite.geometry == ajfx::Geometry::Ribbon ? flag_ribbon : 0U);
        const rhi::StorageBuffer* order = i.orders[e].valid() ? s.particles->buffer(i.orders[e]) : records;
        if (order == nullptr) {
            s.note("an emitter's sort order is missing");
            return false;
        }
        if (i.orders[e].valid()) {
            constants.flags |= flag_sorted;
        }
        const rhi::StorageBuffer* counters = s.particles->buffer(buffers.counters);
        if (counters == nullptr) {
            s.note("an emitter's counters are missing");
            return false;
        }
        const std::span<const core::u8> emitter_uniform{reinterpret_cast<const core::u8*>(&constants),
                                                        sizeof(constants)};
        const std::array<rhi::SampledTexture, 2U> sampled{
            rhi::SampledTexture{texture ? &i.textures[*texture].texture : &*s.blank, &*s.sampler},
            rhi::SampledTexture{soft ? targets.scene_depth : &*s.blank, &*s.sampler}};

        const rhi::StorageBuffer* vertex_buffers[] = {records, order, counters};
        // The counts come from the simulation; this batch only carries them.
        const std::array<rhi::DrawBatch, 1U> batches{rhi::DrawBatch{6U, 1U}};
        rhi::PulledDraw draw{};
        draw.pipeline = motion ? &s.velocity : sprite.blend == ajfx::BlendMode::Additive ? &s.additive : &s.blended;
        draw.vertex_buffers = vertex_buffers;
        draw.vertex_uniform = emitter_uniform;
        draw.fragment_textures = sampled;
        draw.batches = batches;
        draw.indirect = rhi::IndirectArguments{arguments, static_cast<core::u32>(buffers.draw_args_offset)};
        draw.load = motion || !first || targets.load;
        draw.store_depth = true;
        draw.viewport = targets.viewport;
        draw.raster_viewport = targets.raster_viewport;
        rhi::Texture& target = motion ? *targets.velocity : *targets.color;
        if (!commands.draw_pulled(target, targets.clear, *targets.depth, targets.depth_clear, draw)) {
            s.note(motion ? "a particle motion draw was refused" : "a particle draw was refused");
            return false;
        }
        first = false;
    }
    return true;
}

std::optional<ParticleError> ParticlePass::take_error() noexcept {
    return std::exchange(state_->error, std::nullopt);
}

} // namespace mediaserver::render
