#include <mediaserver/render/particle_pass.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/particle_sprite_frag_dxil.h>
#include <mediaserver/shaders/particle_sprite_vert_dxil.h>
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
};
static_assert(sizeof(ViewConstants) == 224U);

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

ajfx::gpu::GpuSimulation& ParticleInstance::simulation() noexcept {
    return *state_->simulation;
}

// ------------------------------------------------------------ pass

struct ParticlePass::State final {
    rhi::Device* device{};
    std::unique_ptr<ParticleDevice> particles;
    rhi::GraphicsPipeline blended;  // alpha and premultiplied emitters
    rhi::GraphicsPipeline additive;
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
    vertex.storage_buffer_count = 1U;
    vertex.uniform_buffer_count = 1U;
    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = rhi::ShaderFormat::Dxil;
    fragment.bytecode = std::span<const core::u8>{reinterpret_cast<const core::u8*>(mediaserver_particle_sprite_frag_dxil),
                                                  sizeof(mediaserver_particle_sprite_frag_dxil)};
    // Tested against the scene, never written: a particle is not a surface
    // anything behind it should be hidden by.
    rhi::DepthState depth{};
    depth.enabled = true;
    depth.write = false;
    depth.greater = true;
    auto blended = device.create_pipeline(vertex, fragment, color_format, rhi::ColorBlend::PremultipliedNormal, depth);
    auto additive = device.create_pipeline(vertex, fragment, color_format, rhi::ColorBlend::PremultipliedEmit, depth);
    if (!blended || !additive) {
        return Result::failure(failure(ParticleErrorCode::Unsupported, "the particle sprite pipelines could not be made"));
    }
    state->blended = std::move(blended).value();
    state->additive = std::move(additive).value();
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
                                                                           core::u32 seed) {
    using Result = core::Result<ParticleInstance, ParticleError>;
    State& s = *state_;
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

bool ParticlePass::draw(rhi::CommandList& commands, const ParticleInstance& instance, const scene::ViewMatrices& view,
                        const scene::Mat4& object_to_world, const ParticleTargets& targets) {
    State& s = *state_;
    if (!instance.caught_up() || targets.color == nullptr || targets.depth == nullptr) {
        return false;
    }
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
    const std::span<const core::u8> uniform{reinterpret_cast<const core::u8*>(&constants), sizeof(constants)};

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

        const rhi::StorageBuffer* vertex_buffers[] = {records};
        // The counts come from the simulation; this batch only carries them.
        const std::array<rhi::DrawBatch, 1U> batches{rhi::DrawBatch{6U, 1U}};
        rhi::PulledDraw draw{};
        draw.pipeline = sprite.blend == ajfx::BlendMode::Additive ? &s.additive : &s.blended;
        draw.vertex_buffers = vertex_buffers;
        draw.vertex_uniform = uniform;
        draw.batches = batches;
        draw.indirect = rhi::IndirectArguments{arguments, static_cast<core::u32>(buffers.draw_args_offset)};
        draw.load = first ? targets.load : true;
        draw.store_depth = true;
        draw.viewport = targets.viewport;
        draw.raster_viewport = targets.raster_viewport;
        if (!commands.draw_pulled(*targets.color, targets.clear, *targets.depth, targets.depth_clear, draw)) {
            s.note("a particle draw was refused");
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
