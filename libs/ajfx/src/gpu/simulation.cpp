#include <ajfx/gpu/simulation.hpp>

#include <ajfx/random.hpp>

#include "../step_inputs.hpp"
#include "pack.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>

namespace ajfx::gpu {
namespace {

constexpr std::uint32_t group_size = 256U;
constexpr std::uint32_t block_size = 4096U;
constexpr std::uint32_t max_groups_x = 65535U;
constexpr std::uint64_t counters_bytes = 16U; // alive, compacted, refused, unused
constexpr std::uint64_t args_bytes = 40U;     // particle groups, block groups, draw
constexpr std::uint64_t particle_args = 0U;
constexpr std::uint64_t block_args = 12U;
constexpr std::uint64_t draw_args = 24U;

struct EmitterGpu final {
    Buffer particles[2];
    Buffer counters;
    Buffer flags;
    Buffer offsets;
    Buffer blocks;
    Buffer data;
    Buffer args;
    Pipeline update;
    Pipeline spawn;
    ParticleLayout layout;
    std::uint32_t module_floats{0U};
    std::uint32_t seed{0U};
    std::uint64_t spawned{0U};
    std::vector<double> rate_carry;
};

template <class T>
std::span<const std::uint8_t> bytes_of(std::span<const T> values) {
    return {reinterpret_cast<const std::uint8_t*>(values.data()), values.size_bytes()};
}

Error device_error(const std::string& what, const Error& cause) {
    return Error{cause.code, what + ": " + cause.message};
}

} // namespace

struct GpuCheckpoint::Data final {
    Device* device{nullptr};
    std::uint64_t tick{0U};
    std::uint32_t current{0U};
    struct Emitter final {
        Buffer particles;
        Buffer counters;
        Buffer args;
        std::uint64_t spawned{0U};
        std::vector<double> rate_carry;
    };
    std::vector<Emitter> emitters;

    ~Data() {
        for (const Emitter& emitter : emitters) {
            device->destroy_buffer(emitter.particles);
            device->destroy_buffer(emitter.counters);
            device->destroy_buffer(emitter.args);
        }
    }
};

GpuCheckpoint::GpuCheckpoint(std::unique_ptr<Data> data) : data_{std::move(data)} {}
GpuCheckpoint::GpuCheckpoint(GpuCheckpoint&&) noexcept = default;
GpuCheckpoint& GpuCheckpoint::operator=(GpuCheckpoint&&) noexcept = default;
GpuCheckpoint::~GpuCheckpoint() = default;

std::uint64_t GpuCheckpoint::tick() const noexcept {
    return data_->tick;
}

struct GpuSimulation::State final {
    Device* device{nullptr};
    Effect effect;
    std::uint32_t instance_seed{0U};
    float dt{0.0F};
    std::uint64_t tick{0U};
    std::uint32_t current{0U}; // which particle buffer holds the live particles
    Pipeline scan_blocks;
    Pipeline scan_totals;
    Pipeline scatter;
    Pipeline finalize;
    std::vector<EmitterGpu> emitters;
    std::vector<ParameterValue> values;
    std::vector<float> scratch;

    ~State() {
        if (device == nullptr) {
            return;
        }
        for (const Pipeline pipeline : {scan_blocks, scan_totals, scatter, finalize}) {
            device->destroy_pipeline(pipeline);
        }
        for (const EmitterGpu& e : emitters) {
            for (const Buffer buffer : {e.particles[0], e.particles[1], e.counters, e.flags, e.offsets, e.blocks,
                                        e.data, e.args}) {
                device->destroy_buffer(buffer);
            }
            device->destroy_pipeline(e.update);
            device->destroy_pipeline(e.spawn);
        }
    }

    Dispatch dispatch(const EmitterGpu& e, Pipeline pipeline, std::uint32_t births) const {
        Dispatch d;
        d.pipeline = pipeline;
        d.constants[0] = e.layout.stride;
        d.constants[1] = effect.emitters[static_cast<std::size_t>(&e - emitters.data())].capacity;
        d.constants[2] = births;
        d.constants[3] = static_cast<std::uint32_t>(e.spawned);
        d.constants[4] = e.seed;
        d.constants[5] = std::bit_cast<std::uint32_t>(dt);
        d.constants[6] = e.module_floats;
        d.buffers = {e.particles[current], e.particles[1U - current], e.counters, e.flags, e.offsets, e.blocks, e.data,
                     Buffer{}};
        return d;
    }

    void zero_counters(const EmitterGpu& e) const {
        const std::uint8_t zeros[args_bytes]{};
        device->write_buffer(e.counters, 0U, std::span{zeros, counters_bytes});
        device->write_buffer(e.args, 0U, std::span{zeros, args_bytes});
    }
};

GpuSimulation::GpuSimulation(std::unique_ptr<State> state) : state_{std::move(state)} {}
GpuSimulation::GpuSimulation(GpuSimulation&&) noexcept = default;
GpuSimulation& GpuSimulation::operator=(GpuSimulation&&) noexcept = default;
GpuSimulation::~GpuSimulation() = default;

Result<GpuSimulation> GpuSimulation::create(Device& device, Effect effect, const KernelSet& kernels,
                                            std::uint32_t instance_seed) {
    Result<bool> valid = validate(effect);
    if (!valid) {
        return valid.error();
    }
    if (kernels.format != device.shader_format() || kernels.emitters.size() != effect.emitters.size()) {
        return Error{ErrorCode::GpuUnavailable, "the kernel set was not compiled for this effect and device"};
    }

    auto state = std::make_unique<State>();
    state->device = &device;
    state->instance_seed = instance_seed;
    state->dt = 1.0F / static_cast<float>(effect.simulation_rate);

    const auto pipeline = [&device](const std::vector<std::uint8_t>& code, const std::string& name,
                                    Pipeline& out) -> std::optional<Error> {
        Result<Pipeline> created = device.create_compute_pipeline(code, name);
        if (!created) {
            return device_error("creating " + name, created.error());
        }
        out = created.value();
        return std::nullopt;
    };
    for (const auto& [code, name, out] :
         {std::tuple{&kernels.scan_blocks, "ajfx scan_blocks", &state->scan_blocks},
          std::tuple{&kernels.scan_totals, "ajfx scan_totals", &state->scan_totals},
          std::tuple{&kernels.scatter, "ajfx scatter", &state->scatter},
          std::tuple{&kernels.finalize, "ajfx finalize", &state->finalize}}) {
        if (std::optional<Error> error = pipeline(*code, name, *out)) {
            return *std::move(error);
        }
    }

    state->emitters.resize(effect.emitters.size());
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        const Emitter& emitter = effect.emitters[i];
        EmitterGpu& e = state->emitters[i];
        const std::string name = "ajfx " + effect.name + "/" + emitter.name;
        e.layout = particle_layout(emitter);
        e.module_floats = detail::module_float_count(effect, emitter);
        e.seed = hash_mix(instance_seed, emitter.seed);
        e.rate_carry.assign(detail::rate_module_count(emitter), 0.0);
        if (std::optional<Error> error = pipeline(kernels.emitters[i].update, name + " update", e.update)) {
            return *std::move(error);
        }
        if (std::optional<Error> error = pipeline(kernels.emitters[i].spawn, name + " spawn", e.spawn)) {
            return *std::move(error);
        }

        const std::uint64_t capacity = emitter.capacity;
        const std::uint64_t record_bytes = capacity * e.layout.stride;
        for (const auto& [bytes, label, out] :
             {std::tuple{record_bytes, " particles A", &e.particles[0]},
              std::tuple{record_bytes, " particles B", &e.particles[1]},
              std::tuple{counters_bytes, " counters", &e.counters}, std::tuple{capacity * 4U, " flags", &e.flags},
              std::tuple{capacity * 4U, " offsets", &e.offsets},
              std::tuple{std::uint64_t{2U * block_size * 4U}, " blocks", &e.blocks},
              std::tuple{(e.module_floats + capacity) * 4U, " data", &e.data},
              std::tuple{args_bytes, " args", &e.args}}) {
            Result<Buffer> buffer = device.create_buffer(bytes, name + label);
            if (!buffer) {
                return device_error("creating " + name + label, buffer.error());
            }
            *out = buffer.value();
        }
    }
    state->effect = std::move(effect);
    return GpuSimulation{std::move(state)};
}

const Effect& GpuSimulation::effect() const noexcept {
    return state_->effect;
}

std::uint64_t GpuSimulation::tick() const noexcept {
    return state_->tick;
}

void GpuSimulation::step(const ParameterProvider* parameters) {
    State& s = *state_;
    Device& device = *s.device;
    detail::resolve_values(s.effect, parameters, s.tick, s.values);
    const detail::Inputs in{s.effect, s.values};
    const double rate = static_cast<double>(s.effect.simulation_rate);
    const double start = static_cast<double>(s.tick) / rate;
    const double end = static_cast<double>(s.tick + 1U) / rate;

    for (std::size_t i = 0U; i < s.emitters.size(); ++i) {
        const Emitter& emitter = s.effect.emitters[i];
        EmitterGpu& e = s.emitters[i];

        // This step's module data, then one age per birth.
        s.scratch.clear();
        detail::pack_emitter(emitter, detail::PackContext{in, s.dt, start}, s.scratch);
        std::uint32_t births = 0U;
        detail::for_each_birth(emitter, in, e.rate_carry, start, end, rate, [&](double age) {
            // Births past capacity are refused by the kernels; they need no age.
            if (births < emitter.capacity) {
                s.scratch.push_back(static_cast<float>(age));
            }
            ++births;
        });
        if (!s.scratch.empty()) {
            device.write_buffer(e.data, 0U, bytes_of(std::span<const float>{s.scratch}));
        }

        Dispatch update = s.dispatch(e, e.update, births);
        update.indirect = e.args;
        update.indirect_offset = particle_args;
        device.dispatch(update);
        device.barrier();

        Dispatch scan = s.dispatch(e, s.scan_blocks, births);
        scan.indirect = e.args;
        scan.indirect_offset = block_args;
        device.dispatch(scan);
        device.barrier();

        device.dispatch(s.dispatch(e, s.scan_totals, births));
        device.barrier();

        Dispatch scatter = s.dispatch(e, s.scatter, births);
        scatter.indirect = e.args;
        scatter.indirect_offset = particle_args;
        device.dispatch(scatter);
        device.barrier();

        if (births > 0U) {
            const std::uint32_t groups = (std::min(births, emitter.capacity) + group_size - 1U) / group_size;
            Dispatch spawn = s.dispatch(e, e.spawn, births);
            spawn.groups_x = std::min(groups, max_groups_x);
            spawn.groups_y = (groups + max_groups_x - 1U) / max_groups_x;
            device.dispatch(spawn);
            device.barrier();
        }

        Dispatch finalize = s.dispatch(e, s.finalize, births);
        finalize.buffers[7] = e.args;
        device.dispatch(finalize);
        device.barrier();

        e.spawned += births;
    }
    s.current = 1U - s.current;
    ++s.tick;
}

void GpuSimulation::advance_to(std::uint64_t target, const ParameterProvider* parameters) {
    while (state_->tick < target) {
        step(parameters);
    }
}

void GpuSimulation::reset() {
    State& s = *state_;
    s.tick = 0U;
    s.current = 0U;
    for (EmitterGpu& e : s.emitters) {
        e.spawned = 0U;
        std::fill(e.rate_carry.begin(), e.rate_carry.end(), 0.0);
        s.zero_counters(e);
    }
    s.device->barrier();
}

EmitterBuffers GpuSimulation::emitter_buffers(std::size_t emitter) const {
    const EmitterGpu& e = state_->emitters[emitter];
    return {e.particles[state_->current], e.counters, e.args, draw_args, e.layout,
            state_->effect.emitters[emitter].capacity};
}

Result<std::vector<std::uint8_t>> GpuSimulation::read_particle_bytes(std::size_t emitter) {
    const EmitterGpu& e = state_->emitters[emitter];
    Result<std::vector<std::uint8_t>> counters = state_->device->read_buffer(e.counters, 0U, 4U);
    if (!counters) {
        return counters.error();
    }
    std::uint32_t alive = 0U;
    std::memcpy(&alive, counters.value().data(), 4U);
    if (alive == 0U) {
        return std::vector<std::uint8_t>{};
    }
    return state_->device->read_buffer(e.particles[state_->current], 0U,
                                       static_cast<std::uint64_t>(alive) * e.layout.stride);
}

Result<EmitterState> GpuSimulation::read_emitter(std::size_t emitter) {
    const EmitterGpu& e = state_->emitters[emitter];
    Result<std::vector<std::uint8_t>> counters = state_->device->read_buffer(e.counters, 0U, counters_bytes);
    if (!counters) {
        return counters.error();
    }
    std::uint32_t words[4]{};
    std::memcpy(words, counters.value().data(), sizeof(words));
    Result<std::vector<std::uint8_t>> records = read_particle_bytes(emitter);
    if (!records) {
        return records.error();
    }

    const ParticleLayout& l = e.layout;
    const std::vector<std::uint8_t>& bytes = records.value();
    const auto read = [&bytes](std::size_t at, auto& out) { std::memcpy(&out, bytes.data() + at, sizeof(out)); };
    EmitterState out;
    const std::size_t count = bytes.size() / l.stride;
    for (std::size_t i = 0U; i < count; ++i) {
        const std::size_t base = i * l.stride;
        const auto field = [&](std::int32_t offset, auto fallback) {
            if (offset >= 0) {
                read(base + static_cast<std::size_t>(offset), fallback);
            }
            return fallback;
        };
        out.position.push_back(field(l.position, Vec3{}));
        out.velocity.push_back(field(l.velocity, Vec3{}));
        out.age.push_back(field(l.age, 0.0F));
        out.lifetime.push_back(field(l.lifetime, 1.0F));
        out.spawn_index.push_back(field(l.spawn_index, std::uint32_t{0U}));
        out.color.push_back(field(l.color, Color{}));
        out.base_color.push_back(l.base_color >= 0 ? field(l.base_color, Color{}) : out.color.back());
        out.size.push_back(field(l.size, 0.1F));
        out.base_size.push_back(l.base_size >= 0 ? field(l.base_size, 0.0F) : out.size.back());
        out.rotation.push_back(field(l.rotation, 0.0F));
    }
    out.spawned = e.spawned;
    out.refused = words[2];
    out.rate_carry = e.rate_carry;
    return out;
}

Result<GpuCheckpoint> GpuSimulation::allocate_checkpoint() {
    State& s = *state_;
    auto data = std::make_unique<GpuCheckpoint::Data>();
    data->device = s.device;
    for (std::size_t i = 0U; i < s.emitters.size(); ++i) {
        const EmitterGpu& e = s.emitters[i];
        const std::uint64_t record_bytes =
            static_cast<std::uint64_t>(s.effect.emitters[i].capacity) * e.layout.stride;
        GpuCheckpoint::Data::Emitter& saved = data->emitters.emplace_back();
        for (const auto& [bytes, out] : {std::pair{record_bytes, &saved.particles},
                                         std::pair{counters_bytes, &saved.counters},
                                         std::pair{args_bytes, &saved.args}}) {
            Result<Buffer> buffer = s.device->create_buffer(bytes, "ajfx checkpoint");
            if (!buffer) {
                return device_error("creating a checkpoint", buffer.error());
            }
            *out = buffer.value();
        }
    }
    return GpuCheckpoint{std::move(data)};
}

Result<GpuCheckpoint> GpuSimulation::save() {
    Result<GpuCheckpoint> checkpoint = allocate_checkpoint();
    if (checkpoint) {
        save_into(checkpoint.value());
    }
    return checkpoint;
}

void GpuSimulation::save_into(GpuCheckpoint& checkpoint) {
    State& s = *state_;
    GpuCheckpoint::Data& data = *checkpoint.data_;
    data.tick = s.tick;
    data.current = s.current;
    for (std::size_t i = 0U; i < s.emitters.size() && i < data.emitters.size(); ++i) {
        const EmitterGpu& e = s.emitters[i];
        GpuCheckpoint::Data::Emitter& saved = data.emitters[i];
        const std::uint64_t record_bytes =
            static_cast<std::uint64_t>(s.effect.emitters[i].capacity) * e.layout.stride;
        saved.spawned = e.spawned;
        saved.rate_carry = e.rate_carry;
        s.device->copy_buffer(saved.particles, 0U, e.particles[s.current], 0U, record_bytes);
        s.device->copy_buffer(saved.counters, 0U, e.counters, 0U, counters_bytes);
        s.device->copy_buffer(saved.args, 0U, e.args, 0U, args_bytes);
    }
}

void GpuSimulation::restore(const GpuCheckpoint& checkpoint) {
    State& s = *state_;
    const GpuCheckpoint::Data& data = *checkpoint.data_;
    s.tick = data.tick;
    s.current = data.current;
    for (std::size_t i = 0U; i < s.emitters.size(); ++i) {
        EmitterGpu& e = s.emitters[i];
        const GpuCheckpoint::Data::Emitter& saved = data.emitters[i];
        const std::uint64_t record_bytes =
            static_cast<std::uint64_t>(s.effect.emitters[i].capacity) * e.layout.stride;
        e.spawned = saved.spawned;
        e.rate_carry = saved.rate_carry;
        s.device->copy_buffer(e.particles[s.current], 0U, saved.particles, 0U, record_bytes);
        s.device->copy_buffer(e.counters, 0U, saved.counters, 0U, counters_bytes);
        s.device->copy_buffer(e.args, 0U, saved.args, 0U, args_bytes);
    }
    s.device->barrier();
}

} // namespace ajfx::gpu
