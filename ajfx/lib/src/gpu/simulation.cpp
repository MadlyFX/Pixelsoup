#include <ajfx/gpu/simulation.hpp>

#include <ajfx/cache.hpp>

#include <ajfx/random.hpp>

#include "../step_inputs.hpp"
#include "../volume.hpp"
#include "pack.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <map>
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
constexpr std::uint64_t args_bytes = 56U;     // particle groups, block groups, draw, spawn groups
constexpr std::uint64_t particle_args = 0U;
constexpr std::uint64_t block_args = 12U;
constexpr std::uint64_t draw_args = 24U;
// Spawn groups, written by the shed kernel for an emitter that sheds from
// particles: only then is the step's birth count known on the GPU alone.
constexpr std::uint64_t shed_args = 40U;

// One grid volume's region of the volumes buffer, in 32-bit words
// (shaders/ajfx_volume.hlsl): two cell buffers of five floats a cell, ten
// words of deposits a cell, then the step's scratch (curl, its size, the
// divergence and two pressures).
constexpr std::uint64_t volume_region_words(std::uint64_t cells) {
    return 27U * cells;
}
constexpr std::uint64_t volume_grid_words(std::uint64_t cells) {
    return 5U * cells;
}

// One grid volume on the GPU: where its region starts, and which of its two
// cell buffers holds the grid now.
struct VolumeGpu final {
    detail::VolumeStep step;
    std::uint32_t base{0U};
    std::uint32_t current{0U};
};

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
    // Shedding from other emitters' particles (SpawnFromParticles): the
    // kernel that works those births out, the spawn slots it serves in stack
    // order, and what it keeps between steps (detail::shed_state_words).
    Pipeline shed;
    std::vector<std::uint32_t> shed_slots;
    Buffer shed_state;
    // The distance fields its colliders use, one after another, and where
    // each starts; the floats wait here until the first step writes them.
    // After them, the bodies its BodyCollision and Wake modules meet, written
    // every step (FieldLayout::body_offsets).
    Buffer fields;
    detail::FieldLayout field_layout;
    std::vector<float> unwritten_fields;
    ParticleLayout layout;
    std::uint32_t module_floats{0U};
    std::uint32_t seed{0U};
    std::uint64_t spawned{0U};
    std::vector<double> rate_carry;
    // Births by source (EmitterState::spawn_counts), counted here on the
    // CPU, which hands each newborn its spawn index.
    std::vector<std::uint64_t> spawn_counts;
    // Whether its newborns may start where a contact or a moving body puts
    // them (detail::has_birth_origins), which the data then carries.
    bool origins{false};
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
        std::vector<std::uint64_t> spawn_counts;
        // Invalid for an emitter that sheds from no particles.
        Buffer shed_state;
    };
    std::vector<Emitter> emitters;
    // The rigid physics emitters' CPU simulation, solver state included.
    std::optional<SimulationState> rigid;
    // The grid volumes' whole buffer, and which cell buffer each was in.
    Buffer volumes;
    std::vector<std::uint32_t> volume_current;

    ~Data() {
        for (const Emitter& emitter : emitters) {
            device->destroy_buffer(emitter.particles);
            device->destroy_buffer(emitter.counters);
            device->destroy_buffer(emitter.args);
            if (emitter.shed_state.valid()) {
                device->destroy_buffer(emitter.shed_state);
            }
        }
        if (volumes.valid()) {
            device->destroy_buffer(volumes);
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
    // The order the emitters step in (step_order), the same as the CPU
    // reference's: an emitter shedding from another's particles after it.
    std::vector<std::size_t> order;
    std::vector<ParameterValue> values;
    std::vector<float> scratch;
    std::vector<std::uint32_t> spawn_indices;
    std::vector<float> origins;
    // The rigid physics emitters, simulated on the CPU, and each emitter's
    // index among them (none for the rest).
    std::optional<Simulation> rigid;
    std::vector<std::optional<std::size_t>> rigid_index;
    // The slowest contact the effect reads of each emitter (contact_floor).
    std::vector<std::optional<float>> floors;
    std::vector<std::uint8_t> records;
    // SimulationOptions::body_broadphase, and the bodies' records as they
    // are written.
    bool body_broadphase{true};
    std::vector<float> body_floats;
    // The grid volumes: one buffer holding every grid's region
    // (volume_region_words), the grids in Effect::volumes order, and the
    // kernels that step them (volume_kernel_names).
    Buffer volumes;
    std::uint64_t volume_words{0U};
    std::vector<VolumeGpu> grids;
    Pipeline volume_kernels[volume_kernel_count];
    std::vector<float> volume_floats;

    ~State() {
        if (device == nullptr) {
            return;
        }
        for (const Pipeline pipeline : {scan_blocks, scan_totals, scatter, finalize}) {
            device->destroy_pipeline(pipeline);
        }
        for (const Pipeline pipeline : volume_kernels) {
            if (pipeline.valid()) {
                device->destroy_pipeline(pipeline);
            }
        }
        if (volumes.valid()) {
            device->destroy_buffer(volumes);
        }
        for (const EmitterGpu& e : emitters) {
            for (const Buffer buffer : {e.particles[0], e.particles[1], e.counters, e.flags, e.offsets, e.blocks,
                                        e.data, e.args, e.fields, e.shed_state}) {
                device->destroy_buffer(buffer);
            }
            device->destroy_pipeline(e.update);
            device->destroy_pipeline(e.spawn);
            device->destroy_pipeline(e.shed);
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
        d.constants[7] = static_cast<std::uint32_t>(tick);
        d.constants[8] = static_cast<std::uint32_t>(tick >> 32U);
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
            if (l.body_kind >= 0 && i < particles.body_kind.size()) {
                put(base, l.body_kind, particles.body_kind[i]);
            }
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

    // Records one step. `hosted` is the host's frame of the tick the step
    // reaches, under hosted rigid physics; inline, the CPU simulation holds
    // the rigid physics emitters.
    void record(const ParameterProvider* parameters, const RigidFrame* hosted);

    void zero_counters(const EmitterGpu& e) const {
        const std::uint8_t zeros[args_bytes]{};
        device->write_buffer(e.counters, 0U, std::span{zeros, counters_bytes});
        device->write_buffer(e.args, 0U, std::span{zeros, args_bytes});
        if (e.shed_state.valid()) {
            const std::vector<std::uint8_t> none(4U * (2U + e.shed_slots.size()), 0U);
            device->write_buffer(e.shed_state, 0U, none);
        }
    }

    // Empties every grid, its deposits and its scratch: a step from here
    // starts as the reference starts.
    void zero_volumes() {
        if (!volumes.valid()) {
            return;
        }
        constexpr std::uint64_t chunk = 1U << 20U;
        const std::vector<std::uint8_t> zeros(static_cast<std::size_t>(std::min(volume_words * 4U, chunk)), 0U);
        for (std::uint64_t at = 0U; at < volume_words * 4U; at += chunk) {
            device->write_buffer(volumes, at, std::span{zeros.data(), static_cast<std::size_t>(std::min(chunk, volume_words * 4U - at))});
        }
        for (VolumeGpu& grid : grids) {
            grid.current = 0U;
        }
    }

    // Where each grid is this step, for the emitters' module data.
    [[nodiscard]] detail::VolumeLayouts volume_layouts() const {
        detail::VolumeLayouts layouts;
        for (std::size_t i = 0U; i < grids.size(); ++i) {
            const VolumeGpu& grid = grids[i];
            layouts.emplace(effect.volumes[i].name,
                            detail::VolumeLayout{
                                static_cast<std::uint32_t>(grid.base + grid.current * volume_grid_words(grid.step.cells)),
                                static_cast<std::uint32_t>(grid.base + 2U * volume_grid_words(grid.step.cells))});
        }
        return layouts;
    }

    // One pass of a grid's step, over every cell.
    void dispatch_volume(const VolumeGpu& grid, std::size_t kernel, std::uint32_t parity) const {
        const detail::VolumeStep& g = grid.step;
        Dispatch d;
        d.pipeline = volume_kernels[kernel];
        d.constants = {g.nx,
                       g.ny,
                       g.nz,
                       g.cells,
                       std::bit_cast<std::uint32_t>(g.buoyancy_dt),
                       std::bit_cast<std::uint32_t>(g.weight_dt),
                       std::bit_cast<std::uint32_t>(g.confinement),
                       std::bit_cast<std::uint32_t>(g.inv_two_cells),
                       std::bit_cast<std::uint32_t>(g.cell_squared),
                       std::bit_cast<std::uint32_t>(g.advect_scale),
                       std::bit_cast<std::uint32_t>(g.keep_density),
                       std::bit_cast<std::uint32_t>(g.keep_temperature),
                       std::bit_cast<std::uint32_t>(g.keep_velocity),
                       grid.base,
                       grid.current,
                       parity};
        d.buffers[0] = volumes;
        d.groups_x = (g.cells + group_size - 1U) / group_size;
        device->dispatch(d);
        device->barrier();
    }

    // The grid's step, as step_volume takes it: inject, confinement where it
    // is on, advect, then the pressure, then the gradient off.
    void step_volume(VolumeGpu& grid) {
        dispatch_volume(grid, 0U, 0U);
        if (grid.step.confinement > 0.0F) {
            dispatch_volume(grid, 1U, 0U);
            dispatch_volume(grid, 2U, 0U);
        }
        dispatch_volume(grid, 3U, 0U);
        dispatch_volume(grid, 4U, 0U);
        std::uint32_t from = 0U;
        for (std::uint32_t iteration = 0U; iteration < grid.step.pressure_iterations; ++iteration) {
            dispatch_volume(grid, 5U, from);
            from = 1U - from;
        }
        dispatch_volume(grid, 6U, from);
        grid.current = 1U - grid.current;
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
    // Playing a cache on the GPU is a host's to wire: under External hosting
    // it hands cached bodies in as it would a runner's (Cache::emitter_at).
    // Inline, this simulation would quietly simulate what was cached.
    if (options.cache != nullptr && hosting == RigidHosting::Inline) {
        return Error{ErrorCode::InvalidEffect,
                     "GpuSimulation does not play caches itself; host rigid physics externally and supply the "
                     "cache's frames, or play the cache with the CPU Simulation"};
    }
    std::vector<std::optional<float>> floors(effect.emitters.size());
    std::vector<std::optional<float>> part_floors;
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        floors[i] = contact_floor(effect, i);
        if (rigid_index[i]) {
            part_floors.push_back(floors[i]);
        }
    }
    if (rigid_count != 0U && hosting == RigidHosting::Inline) {
        Effect cpu = rigid_physics_part(effect);
        // The part's worlds collect what the whole effect reads of them.
        Result<Simulation> created =
            Simulation::create(std::move(cpu), instance_seed,
                               SimulationOptions{.meshes = options.meshes,
                                                 .rigid = options.rigid,
                                                 .contact_floors = std::move(part_floors)});
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
    state->body_broadphase = options.body_broadphase;
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

    // The grid volumes: one region each in one buffer, and the kernels that
    // step them.
    if (!effect.volumes.empty()) {
        std::uint64_t words = 0U;
        for (const Volume& volume : effect.volumes) {
            VolumeGpu grid;
            grid.step = detail::volume_step(volume, state->dt);
            grid.base = static_cast<std::uint32_t>(words);
            words += volume_region_words(grid.step.cells);
            state->grids.push_back(grid);
        }
        state->volume_words = words;
        Result<Buffer> buffer = device.create_buffer(words * 4U, "ajfx " + effect.name + " volumes");
        if (!buffer) {
            return device_error("creating the volumes buffer", buffer.error());
        }
        state->volumes = buffer.value();
        for (std::size_t k = 0U; k < volume_kernel_count; ++k) {
            if (kernels.volume[k].empty()) {
                return Error{ErrorCode::GpuUnavailable, "the kernel set has no grid volume kernels"};
            }
            if (std::optional<Error> error =
                    pipeline(kernels.volume[k], "ajfx " + std::string{volume_kernel_names[k]}, state->volume_kernels[k])) {
                return *std::move(error);
            }
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
        e.spawn_counts.assign(detail::spawn_count_slots(emitter), 0U);
        e.origins = detail::has_birth_origins(emitter);
        if (std::optional<Error> error = pipeline(kernels.emitters[i].update, name + " update", e.update)) {
            return *std::move(error);
        }
        if (std::optional<Error> error = pipeline(kernels.emitters[i].spawn, name + " spawn", e.spawn)) {
            return *std::move(error);
        }
        e.shed_slots = detail::shedding_slots(emitter);
        if (!e.shed_slots.empty()) {
            if (kernels.emitters[i].shed.empty()) {
                return Error{ErrorCode::GpuUnavailable, "the kernel set has no shed kernel for emitter " + emitter.name};
            }
            if (std::optional<Error> error = pipeline(kernels.emitters[i].shed, name + " shed", e.shed)) {
                return *std::move(error);
            }
            Result<Buffer> buffer = device.create_buffer(4U * detail::shed_state_words(emitter), name + " shed state");
            if (!buffer) {
                return device_error("creating " + name + " shed state", buffer.error());
            }
            e.shed_state = buffer.value();
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
        // Room after the fields for the bodies of every emitter it meets or
        // is stirred by, each at the most it may hold.
        std::uint64_t world_floats = e.unwritten_fields.size();
        for (const UpdateModule& module : emitter.update) {
            const std::string* named = nullptr;
            if (const auto* meets = std::get_if<BodyCollision>(&module)) {
                named = &meets->emitter;
            } else if (const auto* stirred = std::get_if<Wake>(&module)) {
                named = &stirred->emitter;
            }
            if (named == nullptr || e.field_layout.body_offsets.contains(*named)) {
                continue;
            }
            for (const Emitter& other : effect.emitters) {
                if (other.name == *named) {
                    e.field_layout.body_offsets.emplace(*named, static_cast<std::uint32_t>(world_floats));
                    world_floats += detail::body_region_floats(other.capacity);
                    break;
                }
            }
        }
        if (world_floats != 0U) {
            Result<Buffer> buffer = device.create_buffer(world_floats * 4U, name + " fields");
            if (!buffer) {
                return device_error("creating " + name + " fields", buffer.error());
            }
            e.fields = buffer.value();
        }

        const std::uint64_t capacity = emitter.capacity;
        const std::uint64_t record_bytes = capacity * e.layout.stride;
        const std::uint64_t origin_floats = e.origins ? detail::birth_origin_floats * capacity : 0U;
        for (const auto& [bytes, label, out] :
             {std::tuple{record_bytes, " particles A", &e.particles[0]},
              std::tuple{record_bytes, " particles B", &e.particles[1]},
              std::tuple{counters_bytes, " counters", &e.counters}, std::tuple{capacity * 4U, " flags", &e.flags},
              std::tuple{capacity * 4U, " offsets", &e.offsets},
              std::tuple{std::uint64_t{2U * block_size * 4U}, " blocks", &e.blocks},
              // Module data, then an age and a spawn index per birth, then
              // where each starts if contacts or bodies bear them.
              std::tuple{(e.module_floats + 2U * capacity + origin_floats) * 4U, " data", &e.data},
              std::tuple{args_bytes, " args", &e.args}}) {
            Result<Buffer> buffer = device.create_buffer(bytes, name + label);
            if (!buffer) {
                return device_error("creating " + name + label, buffer.error());
            }
            *out = buffer.value();
        }
    }
    state->order = step_order(effect);
    state->effect = std::move(effect);
    state->rigid = std::move(rigid);
    state->rigid_index = std::move(rigid_index);
    state->floors = std::move(floors);
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
    state_->record(parameters, nullptr);
}

void GpuSimulation::step(const ParameterProvider* parameters, const RigidFrame& rigid) {
    state_->record(parameters, state_->rigid ? nullptr : &rigid);
}

void GpuSimulation::State::record(const ParameterProvider* parameters, const RigidFrame* hosted) {
    State& s = *this;
    detail::resolve_values(s.effect, parameters, s.tick, s.values);
    const detail::Inputs in{s.effect, s.values};
    const double rate = static_cast<double>(s.effect.simulation_rate);
    const double start = static_cast<double>(s.tick) / rate;
    const double end = static_cast<double>(s.tick + 1U) / rate;
    if (s.rigid) {
        s.rigid->step(parameters);
    }

    // Each rigid physics emitter's particles at the end of this step, and
    // its world's contacts from it: the CPU simulation's, inline, or the
    // host's frame. Neither when the host stepped without a frame.
    const auto rigid_particles = [&](std::size_t index) -> const EmitterState* {
        if (const std::optional<std::size_t> cpu = s.rigid_index[index]; cpu && s.rigid) {
            return &s.rigid->emitter(*cpu);
        }
        if (hosted != nullptr && index < hosted->emitters.size() && s.rigid_index[index]) {
            return &hosted->emitters[index];
        }
        return nullptr;
    };
    const auto rigid_contacts = [&](std::size_t index) -> std::span<const RigidContact> {
        if (const std::optional<std::size_t> cpu = s.rigid_index[index]; cpu && s.rigid) {
            return s.rigid->contacts(*cpu);
        }
        if (hosted != nullptr && index < hosted->contacts.size()) {
            return hosted->contacts[index];
        }
        return {};
    };

    // The bodies this step's colliders and wakes meet, in their grids, and
    // the bodies and contacts its spawn modules bear particles from, from the
    // rigid emitters as this step left them. Gathered once: several emitters
    // may read the same bodies, and they must all see the same step's.
    detail::BodyGrids bodies;
    detail::BodySources sources;
    for (const Emitter& emitter : s.effect.emitters) {
        for (const UpdateModule& module : emitter.update) {
            const std::string* met = nullptr;
            if (const auto* meets = std::get_if<BodyCollision>(&module)) {
                met = &meets->emitter;
            } else if (const auto* stirred = std::get_if<Wake>(&module)) {
                met = &stirred->emitter;
            }
            if (met == nullptr || bodies.contains(*met)) {
                continue;
            }
            for (std::size_t other = 0U; other < s.effect.emitters.size(); ++other) {
                const RigidPhysics* physics = rigid_physics(s.effect.emitters[other]);
                const EmitterState* particles = rigid_particles(other);
                if (physics == nullptr || particles == nullptr || s.effect.emitters[other].name != *met) {
                    continue;
                }
                bodies.emplace(*met, detail::make_body_grid(body_colliders(*physics, *particles,
                                                                           s.effect.emitters[other].renderer.geometry),
                                                            s.body_broadphase));
                break;
            }
        }
        for (const SpawnModule& module : emitter.spawn) {
            const std::string* named = nullptr;
            if (const auto* contacts = std::get_if<SpawnFromContacts>(&module)) {
                named = &contacts->emitter;
            } else if (const auto* shed = std::get_if<SpawnFromBodies>(&module)) {
                named = &shed->emitter;
            }
            if (named == nullptr || sources.contains(*named)) {
                continue;
            }
            for (std::size_t other = 0U; other < s.effect.emitters.size(); ++other) {
                const RigidPhysics* physics = rigid_physics(s.effect.emitters[other]);
                const EmitterState* particles = rigid_particles(other);
                if (physics == nullptr || particles == nullptr || s.effect.emitters[other].name != *named) {
                    continue;
                }
                // What the effect reads of them, worked out from the world's
                // list exactly as the CPU reference works it out.
                std::vector<RigidContact> read;
                if (s.floors[other]) {
                    read = detail::effect_contacts(rigid_contacts(other), *s.floors[other],
                                                   detail::effect_contact_limit(s.effect.emitters[other]));
                }
                sources.emplace(*named, detail::body_source(*physics, *particles, std::move(read),
                                                            s.effect.emitters[other].capacity));
                break;
            }
        }
    }
    const detail::VolumeLayouts volume_layouts = s.volume_layouts();
    for (const std::size_t i : s.order) {
        const Emitter& emitter = s.effect.emitters[i];
        EmitterGpu& e = s.emitters[i];
        if (s.rigid_index[i]) {
            if (const EmitterState* particles = rigid_particles(i)) {
                s.upload(e, *particles, 1U - s.current);
                e.spawned = particles->spawned;
                e.rate_carry = particles->rate_carry;
                e.spawn_counts = particles->spawn_counts;
            }
            continue;
        }

        if (!e.unwritten_fields.empty()) {
            device->write_buffer(e.fields, 0U, bytes_of(std::span<const float>{e.unwritten_fields}));
            e.unwritten_fields = {};
        }
        // The bodies it meets, where pack_grid tells its kernel they are:
        // each cell's first body and the bodies by cell where the grid is
        // used, and every body's record.
        for (const auto& [met, offset] : e.field_layout.body_offsets) {
            const auto grid = bodies.find(met);
            if (grid == bodies.end() || grid->second.bodies.empty()) {
                continue;
            }
            const std::uint64_t at = offset;
            const std::uint64_t order_at = at + detail::body_grid_cells + 1U;
            std::uint64_t capacity = 0U;
            for (const Emitter& other : s.effect.emitters) {
                if (other.name == met) {
                    capacity = other.capacity;
                    break;
                }
            }
            if (grid->second.usable) {
                device->write_buffer(e.fields, at * 4U, bytes_of(std::span<const std::uint32_t>{grid->second.start}));
                device->write_buffer(e.fields, order_at * 4U,
                                     bytes_of(std::span<const std::uint32_t>{grid->second.order}));
            }
            detail::body_records(grid->second, s.body_floats);
            device->write_buffer(e.fields, (order_at + capacity) * 4U,
                                 bytes_of(std::span<const float>{s.body_floats}));
        }

        // This step's module data, then one age per birth.
        s.scratch.clear();
        detail::pack_emitter(
            emitter, detail::PackContext{in, s.dt, start, &e.field_layout, &bodies, s.tick, &volume_layouts},
            s.scratch);
        std::uint32_t births = 0U;
        s.spawn_indices.clear();
        s.origins.clear();
        // A body kind is the CPU's business: these emitters have no bodies.
        // Each birth is counted by its own source, as the reference counts
        // it, refused or not, and starts where a contact or a body puts it,
        // as the reference starts it.
        const detail::BirthContext context{s.tick, e.seed, s.dt, &sources};
        detail::for_each_birth(emitter, in, e.rate_carry, e.spawn_counts, start, end, rate, context,
                               [&](double age, std::uint32_t, std::uint32_t, std::uint32_t spawn_index,
                                   const detail::BirthOrigin* origin) {
                                   // Births past capacity are refused by the
                                   // kernels; they need no age or index.
                                   if (births < emitter.capacity) {
                                       s.scratch.push_back(static_cast<float>(age));
                                       s.spawn_indices.push_back(spawn_index);
                                       if (e.origins) {
                                           const detail::BirthOrigin at =
                                               origin != nullptr ? *origin : detail::BirthOrigin{};
                                           s.origins.insert(s.origins.end(),
                                                            {at.position.x, at.position.y, at.position.z,
                                                             at.velocity.x, at.velocity.y, at.velocity.z,
                                                             std::bit_cast<float>(origin != nullptr ? 1U : 0U)});
                                       }
                                   }
                                   ++births;
                               });
        if (!s.scratch.empty()) {
            device->write_buffer(e.data, 0U, bytes_of(std::span<const float>{s.scratch}));
        }
        if (!s.spawn_indices.empty()) {
            const std::uint64_t at = (static_cast<std::uint64_t>(e.module_floats) + emitter.capacity) * 4U;
            device->write_buffer(e.data, at, bytes_of(std::span<const std::uint32_t>{s.spawn_indices}));
        }
        if (!s.origins.empty()) {
            const std::uint64_t at = (static_cast<std::uint64_t>(e.module_floats) + 2U * emitter.capacity) * 4U;
            device->write_buffer(e.data, at, bytes_of(std::span<const float>{s.origins}));
        }
        // The births shed from other emitters' particles, after those: one
        // dispatch per shedding module, in stack order, each reading the
        // emitter it names where this step left it (it stepped earlier, by
        // step_order) and appending to the step's births. The last also
        // sizes the spawn dispatch.
        for (std::size_t k = 0U; k < e.shed_slots.size(); ++k) {
            const auto& module = std::get<SpawnFromParticles>(emitter.spawn[e.shed_slots[k]]);
            const EmitterGpu* source = nullptr;
            for (std::size_t other = 0U; other < s.emitters.size(); ++other) {
                if (s.effect.emitters[other].name == module.emitter) {
                    source = &s.emitters[other];
                    break;
                }
            }
            if (source == nullptr) {
                continue;
            }
            Dispatch shedding = s.dispatch(e, e.shed, births);
            shedding.constants[9] = static_cast<std::uint32_t>(k);
            shedding.buffers = {source->particles[1U - s.current], source->counters, e.counters, e.shed_state,
                                e.offsets, e.blocks, e.data, e.args};
            device->dispatch(shedding);
            device->barrier();
        }

        Dispatch update = s.dispatch(e, e.update, births);
        update.buffers[7] = e.fields;
        // The grids it feeds and follows, where the update kernel never
        // touches the bookkeeping kernels' offsets.
        if (s.volumes.valid() && uses_volumes(emitter)) {
            update.buffers[4] = s.volumes;
        }
        update.indirect = e.args;
        update.indirect_offset = particle_args;
        device->dispatch(update);
        device->barrier();

        Dispatch scan = s.dispatch(e, s.scan_blocks, births);
        scan.indirect = e.args;
        scan.indirect_offset = block_args;
        device->dispatch(scan);
        device->barrier();

        device->dispatch(s.dispatch(e, s.scan_totals, births));
        device->barrier();

        Dispatch scattering = s.dispatch(e, s.scatter, births);
        scattering.indirect = e.args;
        scattering.indirect_offset = particle_args;
        device->dispatch(scattering);
        device->barrier();

        if (!e.shed_slots.empty()) {
            // Only the GPU knows how many were shed: the shed kernel sized it.
            Dispatch spawn = s.dispatch(e, e.spawn, births);
            spawn.indirect = e.args;
            spawn.indirect_offset = shed_args;
            device->dispatch(spawn);
            device->barrier();
        } else if (births > 0U) {
            const std::uint32_t groups = (std::min(births, emitter.capacity) + group_size - 1U) / group_size;
            Dispatch spawn = s.dispatch(e, e.spawn, births);
            spawn.groups_x = std::min(groups, max_groups_x);
            spawn.groups_y = (groups + max_groups_x - 1U) / max_groups_x;
            device->dispatch(spawn);
            device->barrier();
        }

        Dispatch finalizing = s.dispatch(e, s.finalize, births);
        finalizing.buffers[7] = e.args;
        device->dispatch(finalizing);
        device->barrier();

        e.spawned += births;
    }
    // Then every grid, in the effect's order, as the reference steps them.
    for (VolumeGpu& grid : s.grids) {
        s.step_volume(grid);
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
        std::fill(e.spawn_counts.begin(), e.spawn_counts.end(), 0U);
        s.zero_counters(e);
    }
    s.zero_volumes();
    s.device->barrier();
    write_inline_rigid();
}

VolumeBuffers GpuSimulation::volume_buffers(std::size_t volume) const {
    const State& s = *state_;
    VolumeBuffers out;
    if (volume >= s.grids.size()) {
        return out;
    }
    const VolumeGpu& grid = s.grids[volume];
    out.buffer = s.volumes;
    out.cells_at = static_cast<std::uint32_t>(grid.base + grid.current * volume_grid_words(grid.step.cells));
    out.nx = grid.step.nx;
    out.ny = grid.step.ny;
    out.nz = grid.step.nz;
    out.origin = grid.step.origin;
    out.cell_size = s.effect.volumes[volume].cell_size;
    return out;
}

void GpuSimulation::write_volume(std::size_t volume, const VolumeState& cells) {
    State& s = *state_;
    if (volume >= s.grids.size()) {
        return;
    }
    const VolumeGpu& grid = s.grids[volume];
    const std::uint32_t n = grid.step.cells;
    if (cells.density.size() != n || cells.temperature.size() != n || cells.velocity.size() != n) {
        return;
    }
    s.volume_floats.clear();
    s.volume_floats.reserve(static_cast<std::size_t>(volume_grid_words(n)));
    s.volume_floats.insert(s.volume_floats.end(), cells.density.begin(), cells.density.end());
    s.volume_floats.insert(s.volume_floats.end(), cells.temperature.begin(), cells.temperature.end());
    for (const Vec3& v : cells.velocity) {
        s.volume_floats.insert(s.volume_floats.end(), {v.x, v.y, v.z});
    }
    s.device->write_buffer(s.volumes, (grid.base + grid.current * volume_grid_words(n)) * 4U,
                           bytes_of(std::span<const float>{s.volume_floats}));
    s.device->barrier();
}

Result<VolumeState> GpuSimulation::read_volume(std::size_t volume) {
    const State& s = *state_;
    if (volume >= s.grids.size()) {
        return Error{ErrorCode::InvalidEffect, "the effect has no volume " + std::to_string(volume)};
    }
    const VolumeGpu& grid = s.grids[volume];
    const std::uint32_t n = grid.step.cells;
    Result<std::vector<std::uint8_t>> bytes =
        s.device->read_buffer(s.volumes, (grid.base + grid.current * volume_grid_words(n)) * 4U, volume_grid_words(n) * 4U);
    if (!bytes) {
        return bytes.error();
    }
    VolumeState out;
    out.nx = grid.step.nx;
    out.ny = grid.step.ny;
    out.nz = grid.step.nz;
    out.density.resize(n);
    out.temperature.resize(n);
    out.velocity.resize(n);
    const std::uint8_t* at = bytes.value().data();
    std::memcpy(out.density.data(), at, n * 4U);
    std::memcpy(out.temperature.data(), at + n * 4U, n * 4U);
    std::memcpy(out.velocity.data(), at + 2U * n * 4U, n * 12U);
    return out;
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
                s.emitters[i].spawn_counts = particles.spawn_counts;
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
    e.spawn_counts = particles.spawn_counts;
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
    out.spawn_counts = e.spawn_counts;
    if (e.shed_state.valid()) {
        // What the shed kernel counted: its births so far and each shedding
        // slot's count, which the CPU-side counts know nothing of.
        Result<std::vector<std::uint8_t>> shed =
            state_->device->read_buffer(e.shed_state, 0U, 4U * (2U + e.shed_slots.size()));
        if (!shed) {
            return shed.error();
        }
        std::vector<std::uint32_t> state(2U + e.shed_slots.size());
        std::memcpy(state.data(), shed.value().data(), state.size() * 4U);
        out.spawned += static_cast<std::uint64_t>(state[0]) | (static_cast<std::uint64_t>(state[1]) << 32U);
        for (std::size_t k = 0U; k < e.shed_slots.size(); ++k) {
            if (e.shed_slots[k] < out.spawn_counts.size()) {
                out.spawn_counts[e.shed_slots[k]] = state[2U + k];
            }
        }
    }
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
        if (e.shed_state.valid()) {
            Result<Buffer> buffer =
                s.device->create_buffer(4U * (2U + e.shed_slots.size()), "ajfx checkpoint shed state");
            if (!buffer) {
                return device_error("creating a checkpoint", buffer.error());
            }
            saved.shed_state = buffer.value();
        }
    }
    if (s.volumes.valid()) {
        Result<Buffer> buffer = s.device->create_buffer(s.volume_words * 4U, "ajfx checkpoint volumes");
        if (!buffer) {
            return device_error("creating a checkpoint", buffer.error());
        }
        data->volumes = buffer.value();
        data->volume_current.resize(s.grids.size());
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
        saved.spawn_counts = e.spawn_counts;
        s.device->copy_buffer(saved.particles, 0U, e.particles[s.current], 0U, record_bytes);
        s.device->copy_buffer(saved.counters, 0U, e.counters, 0U, counters_bytes);
        s.device->copy_buffer(saved.args, 0U, e.args, 0U, args_bytes);
        if (e.shed_state.valid() && saved.shed_state.valid()) {
            s.device->copy_buffer(saved.shed_state, 0U, e.shed_state, 0U, 4U * (2U + e.shed_slots.size()));
        }
    }
    if (s.volumes.valid() && data.volumes.valid()) {
        s.device->copy_buffer(data.volumes, 0U, s.volumes, 0U, s.volume_words * 4U);
        for (std::size_t i = 0U; i < s.grids.size() && i < data.volume_current.size(); ++i) {
            data.volume_current[i] = s.grids[i].current;
        }
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
        e.spawn_counts = saved.spawn_counts;
        s.device->copy_buffer(e.particles[s.current], 0U, saved.particles, 0U, record_bytes);
        s.device->copy_buffer(e.counters, 0U, saved.counters, 0U, counters_bytes);
        s.device->copy_buffer(e.args, 0U, saved.args, 0U, args_bytes);
        if (e.shed_state.valid() && saved.shed_state.valid()) {
            s.device->copy_buffer(e.shed_state, 0U, saved.shed_state, 0U, 4U * (2U + e.shed_slots.size()));
        }
    }
    if (s.volumes.valid() && data.volumes.valid()) {
        s.device->copy_buffer(s.volumes, 0U, data.volumes, 0U, s.volume_words * 4U);
        for (std::size_t i = 0U; i < s.grids.size() && i < data.volume_current.size(); ++i) {
            s.grids[i].current = data.volume_current[i];
        }
    }
    s.device->barrier();
}

} // namespace ajfx::gpu
