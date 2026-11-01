#include <ajfx/gpu/simulation.hpp>

#include <ajfx/random.hpp>

#include "../step_inputs.hpp"
#include "pack.hpp"

#include <algorithm>
#include <array>
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
    // The distance fields its colliders use, one after another, and where
    // each starts; the floats wait here until the first step writes them.
    Buffer fields;
    detail::FieldLayout field_layout;
    std::vector<float> unwritten_fields;
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
    // The rigid physics emitters' CPU simulation, solver state included.
    std::optional<SimulationState> rigid;

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
    FieldSet fields;
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
    // The rigid physics emitters, simulated on the CPU, and each emitter's
    // index among them (none for the rest).
    std::optional<Simulation> rigid;
    std::vector<std::optional<std::size_t>> rigid_index;
    std::vector<std::uint8_t> records;

    ~State() {
        if (device == nullptr) {
            return;
        }
        for (const Pipeline pipeline : {scan_blocks, scan_totals, scatter, finalize}) {
            device->destroy_pipeline(pipeline);
        }
        for (const EmitterGpu& e : emitters) {
            for (const Buffer buffer : {e.particles[0], e.particles[1], e.counters, e.flags, e.offsets, e.blocks,
                                        e.data, e.args, e.fields}) {
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

    // Writes a CPU-simulated emitter's particles into particle buffer
    // `buffer`, with the counters and dispatch and draw sizes `finalize`
    // would have written for them.
    void upload(const EmitterGpu& e, const EmitterState& particles, std::uint32_t buffer) {
        const ParticleLayout& l = e.layout;
        const std::uint32_t alive = static_cast<std::uint32_t>(particles.alive());
        records.assign(static_cast<std::size_t>(alive) * l.stride, 0U);
        const auto put = [&](std::size_t base, std::int32_t offset, const auto& value) {
            if (offset >= 0) {
                std::memcpy(records.data() + base + static_cast<std::size_t>(offset), &value, sizeof(value));
            }
        };
        for (std::size_t i = 0U; i < alive; ++i) {
            const std::size_t base = i * l.stride;
            put(base, l.position, particles.position[i]);
            put(base, l.velocity, particles.velocity[i]);
            put(base, l.age, particles.age[i]);
            put(base, l.lifetime, particles.lifetime[i]);
            put(base, l.spawn_index, particles.spawn_index[i]);
            put(base, l.color, particles.color[i]);
            put(base, l.base_color, particles.base_color[i]);
            put(base, l.size, particles.size[i]);
            put(base, l.base_size, particles.base_size[i]);
            put(base, l.rotation, particles.rotation[i]);
            put(base, l.orientation, particles.orientation[i]);
            put(base, l.angular_velocity, particles.angular_velocity[i]);
            put(base, l.spin, particles.spin[i]);
        }
        if (!records.empty()) {
            device->write_buffer(e.particles[buffer], 0U, records);
        }
        const auto groups = [](std::uint32_t threads, std::uint32_t per_group) {
            const std::uint32_t count = (threads + per_group - 1U) / per_group;
            return std::array<std::uint32_t, 3>{std::min(count, max_groups_x),
                                                count == 0U ? 1U : (count + max_groups_x - 1U) / max_groups_x, 1U};
        };
        const std::uint32_t counters[4] = {alive, alive, static_cast<std::uint32_t>(particles.refused), 0U};
        const std::array<std::uint32_t, 3> particle_groups = groups(alive, group_size);
        const std::array<std::uint32_t, 3> block_groups = groups(alive, block_size);
        const std::uint32_t args[10] = {particle_groups[0], particle_groups[1], particle_groups[2],
                                        block_groups[0],    block_groups[1],    block_groups[2],
                                        6U,                 alive,              0U,
                                        0U};
        device->write_buffer(e.counters, 0U, std::span{reinterpret_cast<const std::uint8_t*>(counters), sizeof(counters)});
        device->write_buffer(e.args, 0U, std::span{reinterpret_cast<const std::uint8_t*>(args), sizeof(args)});
        device->barrier();
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
                                            std::uint32_t instance_seed, const FieldSet& fields) {
    return create(device, std::move(effect), kernels, instance_seed, SimulationOptions{.fields = fields});
}

Result<GpuSimulation> GpuSimulation::create(Device& device, Effect effect, const KernelSet& kernels,
                                            std::uint32_t instance_seed, SimulationOptions options,
                                            RigidHosting hosting) {
    const FieldSet& fields = options.fields;
    Result<bool> valid = validate(effect);
    if (!valid) {
        return valid.error();
    }
    if (Result<bool> held = check_fields(effect, fields); !held) {
        return held.error();
    }
    // The rigid physics emitters as an effect of their own: each emitter's
    // randomness is its own, so they simulate exactly as within the whole.
    std::optional<Simulation> rigid;
    std::vector<std::optional<std::size_t>> rigid_index(effect.emitters.size());
    std::size_t rigid_count = 0U;
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        if (rigid_physics(effect.emitters[i]) != nullptr) {
            rigid_index[i] = rigid_count++;
        }
    }
    if (rigid_count != 0U && hosting == RigidHosting::Inline) {
        Effect cpu = rigid_physics_part(effect);
        Result<Simulation> created = Simulation::create(
            std::move(cpu), instance_seed, SimulationOptions{.meshes = options.meshes, .rigid = options.rigid});
        if (!created) {
            return created.error();
        }
        rigid.emplace(std::move(created).value());
    }
    if (kernels.format != device.shader_format() || kernels.emitters.size() != effect.emitters.size()) {
        return Error{ErrorCode::GpuUnavailable, "the kernel set was not compiled for this effect and device"};
    }

    auto state = std::make_unique<State>();
    state->device = &device;
    state->instance_seed = instance_seed;
    state->dt = 1.0F / static_cast<float>(effect.simulation_rate);
    for (const Emitter& emitter : effect.emitters) {
        for (const UpdateModule& module : emitter.update) {
            if (const auto* collision = std::get_if<SdfCollision>(&module)) {
                state->fields.emplace(collision->field, fields.find(collision->field)->second);
            }
        }
    }

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

        e.field_layout.fields = &state->fields;
        for (const UpdateModule& module : emitter.update) {
            const auto* collision = std::get_if<SdfCollision>(&module);
            if (collision != nullptr && !e.field_layout.offsets.contains(collision->field)) {
                e.field_layout.offsets.emplace(collision->field, static_cast<std::uint32_t>(e.unwritten_fields.size()));
                const std::vector<float>& distances = state->fields.find(collision->field)->second.distances;
                e.unwritten_fields.insert(e.unwritten_fields.end(), distances.begin(), distances.end());
            }
        }
        if (!e.unwritten_fields.empty()) {
            Result<Buffer> buffer = device.create_buffer(e.unwritten_fields.size() * 4U, name + " fields");
            if (!buffer) {
                return device_error("creating " + name + " fields", buffer.error());
            }
            e.fields = buffer.value();
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
    state->rigid = std::move(rigid);
    state->rigid_index = std::move(rigid_index);
    GpuSimulation simulation{std::move(state)};
    // Inline piles stand from step 0; the buffers need them before any step.
    simulation.write_inline_rigid();
    return simulation;
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
    if (s.rigid) {
        s.rigid->step(parameters);
    }

    for (std::size_t i = 0U; i < s.emitters.size(); ++i) {
        const Emitter& emitter = s.effect.emitters[i];
        EmitterGpu& e = s.emitters[i];
        if (const std::optional<std::size_t> cpu = s.rigid_index[i]) {
            if (s.rigid) {
                const EmitterState& particles = s.rigid->emitter(*cpu);
                s.upload(e, particles, 1U - s.current);
                e.spawned = particles.spawned;
                e.rate_carry = particles.rate_carry;
            }
            continue;
        }

        if (!e.unwritten_fields.empty()) {
            device.write_buffer(e.fields, 0U, bytes_of(std::span<const float>{e.unwritten_fields}));
            e.unwritten_fields = {};
        }

        // This step's module data, then one age per birth.
        s.scratch.clear();
        detail::pack_emitter(emitter, detail::PackContext{in, s.dt, start, &e.field_layout}, s.scratch);
        std::uint32_t births = 0U;
        // A body kind is the CPU's business: these emitters have no bodies.
        detail::for_each_birth(emitter, in, e.rate_carry, start, end, rate, [&](double age, std::uint32_t) {
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
        update.buffers[7] = e.fields;
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

void GpuSimulation::reset(const ParameterProvider* parameters) {
    State& s = *state_;
    s.tick = 0U;
    s.current = 0U;
    if (s.rigid) {
        s.rigid->reset(parameters);
    }
    for (EmitterGpu& e : s.emitters) {
        e.spawned = 0U;
        std::fill(e.rate_carry.begin(), e.rate_carry.end(), 0.0);
        s.zero_counters(e);
    }
    s.device->barrier();
    write_inline_rigid();
}

void GpuSimulation::write_inline_rigid() {
    State& s = *state_;
    if (!s.rigid) {
        return;
    }
    for (std::size_t i = 0U; i < s.emitters.size(); ++i) {
        if (const std::optional<std::size_t> cpu = s.rigid_index[i]) {
            const EmitterState& particles = s.rigid->emitter(*cpu);
            if (particles.alive() != 0U) {
                s.upload(s.emitters[i], particles, s.current);
                s.emitters[i].spawned = particles.spawned;
            }
        }
    }
}

void GpuSimulation::write_rigid(std::size_t emitter, const EmitterState& particles) {
    State& s = *state_;
    if (s.rigid || emitter >= s.emitters.size() || !s.rigid_index[emitter]) {
        return;
    }
    EmitterGpu& e = s.emitters[emitter];
    s.upload(e, particles, s.current);
    e.spawned = particles.spawned;
    e.rate_carry = particles.rate_carry;
}

EmitterBuffers GpuSimulation::emitter_buffers(std::size_t emitter) const {
    const EmitterGpu& e = state_->emitters[emitter];
    return {e.particles[state_->current], e.counters, e.args, draw_args, block_args, e.layout,
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
        out.orientation.push_back(field(l.orientation, Quat{}));
        out.angular_velocity.push_back(field(l.angular_velocity, Vec3{}));
        out.spin.push_back(field(l.spin, 0.0F));
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
    if (s.rigid) {
        data.rigid = s.rigid->state();
    }
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
    if (s.rigid && data.rigid) {
        s.rigid->restore(*data.rigid);
    }
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
