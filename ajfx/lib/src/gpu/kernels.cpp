#include <ajfx/gpu/kernels.hpp>

#include "hlsl_sources.hpp"
#include "pack.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>

namespace ajfx::gpu {
namespace {

// Pastes the library's HLSL headers in place of their #include lines, so a
// compiler needs no include handler and the source is one self-contained text.
std::string expand_includes(std::string_view source) {
    std::string out;
    std::size_t at = 0U;
    while (at < source.size()) {
        std::size_t end = source.find('\n', at);
        end = end == std::string_view::npos ? source.size() : end + 1U;
        const std::string_view line = source.substr(at, end - at);
        if (line.starts_with("#include \"ajfx_random.hlsli\"")) {
            out += detail::hlsl_random;
        } else if (line.starts_with("#include \"ajfx_particles.hlsli\"")) {
            out += expand_includes(detail::hlsl_particles);
        } else if (line.starts_with("#include \"ajfx_volume.hlsli\"")) {
            out += detail::hlsl_volume;
        } else {
            out += line;
        }
        at = end;
    }
    return out;
}

std::string printable(std::string_view text) {
    std::string out;
    for (const char c : text) {
        out.push_back(static_cast<unsigned char>(c) < 0x20U ? '?' : c);
    }
    return out;
}

enum class FieldType {
    Float,
    Float3,
    Float4,
    Uint,
};

struct Field final {
    std::string_view name;
    std::int32_t offset;
    FieldType type;
};

std::vector<Field> stored_fields(const ParticleLayout& l) {
    std::vector<Field> fields{
        {"position", l.position, FieldType::Float3},
        {"velocity", l.velocity, FieldType::Float3},
        {"age", l.age, FieldType::Float},
        {"lifetime", l.lifetime, FieldType::Float},
        {"spawn_index", l.spawn_index, FieldType::Uint},
        {"color", l.color, FieldType::Float4},
        {"base_color", l.base_color, FieldType::Float4},
        {"size", l.size, FieldType::Float},
        {"base_size", l.base_size, FieldType::Float},
        {"rotation", l.rotation, FieldType::Float},
        {"orientation", l.orientation, FieldType::Float4},
        {"angular_velocity", l.angular_velocity, FieldType::Float3},
        {"spin", l.spin, FieldType::Float},
    };
    std::erase_if(fields, [](const Field& field) { return field.offset < 0; });
    return fields;
}

std::string_view load_suffix(FieldType type) {
    switch (type) {
    case FieldType::Float3:
        return "3";
    case FieldType::Float4:
        return "4";
    case FieldType::Float:
    case FieldType::Uint:
        return "";
    }
    return "";
}

void generate_access(std::string& out, const ParticleLayout& layout) {
    const std::vector<Field> fields = stored_fields(layout);

    out += "Particle ajfx_load(uint base) {\n    Particle p = ajfx_default_particle();\n";
    for (const Field& field : fields) {
        const std::string load = "ajfx_src.Load" + std::string{load_suffix(field.type)} + "(base + " +
                                 std::to_string(field.offset) + "u)";
        out += "    p." + std::string{field.name} + " = ";
        out += field.type == FieldType::Uint ? load : "asfloat(" + load + ")";
        out += ";\n";
    }
    // Without colour- or size-over-life the base value is the value.
    if (layout.base_color < 0 && layout.color >= 0) {
        out += "    p.base_color = p.color;\n";
    }
    if (layout.base_size < 0 && layout.size >= 0) {
        out += "    p.base_size = p.size;\n";
    }
    out += "    return p;\n}\n\n";

    for (const std::string_view buffer : {"src", "dst"}) {
        out += "void ajfx_store_" + std::string{buffer} + "(uint base, Particle p) {\n";
        for (const Field& field : fields) {
            out += "    ajfx_" + std::string{buffer} + ".Store" + std::string{load_suffix(field.type)} + "(base + " +
                   std::to_string(field.offset) + "u, ";
            out += field.type == FieldType::Uint ? "p." + std::string{field.name}
                                                 : "asuint(p." + std::string{field.name} + ")";
            out += ");\n";
        }
        out += "}\n\n";
    }
}

// A module's data starts with its id (see pack_module), so the kernel reads
// the id rather than baking it in. A module with a scope (Scope) has it
// next, and acts only on the particles in it.
template <class Module>
std::string call(const Module& module, std::uint32_t offset) {
    static_cast<void>(module);
    if constexpr (requires { module.only; }) {
        return "        if (ajfx_in_scope(p, " + std::to_string(offset + 1U) + "u)) {\n            ajfx_" +
               std::string{Module::kind} + "(p, " + std::to_string(offset + 1U + detail::scope_floats) +
               "u, ajfx_u32(" + std::to_string(offset) + "u));\n        }\n";
    } else {
        return "        ajfx_" + std::string{Module::kind} + "(p, " + std::to_string(offset + 1U) + "u, ajfx_u32(" +
               std::to_string(offset) + "u));\n";
    }
}

bool is_appearance(const UpdateModule& module) {
    return std::holds_alternative<ColorOverLife>(module) || std::holds_alternative<SizeOverLife>(module);
}

// Where a stored field of another emitter's record is, as HLSL: its byte
// offset, or the no-such-field mark for one it does not store.
std::string field_offset(std::int32_t offset) {
    return offset < 0 ? std::string{"0xffffffffu"} : std::to_string(offset) + "u";
}

// The shed kernel: the births one SpawnFromParticles module makes this step,
// worked out from the named emitter's particles as this step left them,
// exactly as detail::particle_births works them out (float throughout, the
// same hash rolls, the same clamp at max_per_step in particle order) and
// written where the spawn kernel reads births: after the ones the CPU
// scheduled (ajfx_birth_count) and the ones earlier shed dispatches of this
// step made. One group walks the source's particles in chunks of 4096, four
// a thread, a group scan turning each particle's count into its place in the
// step's births.
//
// Bound for a shed dispatch: u0 the source emitter's particles after its
// step, u1 the source's counters (live count first), u2 this emitter's
// counters (word 3 counts the step's shed births), u3 its shed state
// (shed_state_words), u6 its data and u7 its args (spawn groups at 40).
// ajfx_shed_module (a constant) picks the module; one dispatch per module,
// in stack order.
template <class Shedding>
void generate_shed(std::string& out, const Emitter& emitter, const std::vector<Shedding>& shedding) {
    static_cast<void>(emitter);
    out += "\n// Births shed from other emitters' particles (SpawnFromParticles).\n"
           "#define ajfx_shed_module ajfx_reserved0\n"
           "groupshared uint ajfx_scan[1024];\n\n"
           "// Inclusive scan across the group, saturating well above any max_per_step.\n"
           "uint ajfx_shed_scan(uint value, uint thread) {\n"
           "    ajfx_scan[thread] = value;\n"
           "    GroupMemoryBarrierWithGroupSync();\n"
           "    for (uint offset = 1u; offset < 1024u; offset <<= 1u) {\n"
           "        uint add = thread >= offset ? ajfx_scan[thread - offset] : 0u;\n"
           "        GroupMemoryBarrierWithGroupSync();\n"
           "        ajfx_scan[thread] = min(ajfx_scan[thread] + add, 0x40000000u);\n"
           "        GroupMemoryBarrierWithGroupSync();\n"
           "    }\n"
           "    return ajfx_scan[thread];\n"
           "}\n\n"
           "// Whether the source particle is in the module's scope: [every particle, 8\n"
           "// words of one bit per spawn source], as pack_scope packs it.\n"
           "bool ajfx_shed_in_scope(uint spawn_index, uint o) {\n"
           "    if (ajfx_u32(o) != 0u) {\n"
           "        return true;\n"
           "    }\n"
           "    uint source = spawn_index >> 24u;\n"
           "    return ((ajfx_u32(o + 1u + (source >> 5u)) >> (source & 31u)) & 1u) != 0u;\n"
           "}\n\n"
           "// How many a source particle sheds this step, before the step's cap:\n"
           "// detail::particle_births and rounded_count_f, operation for operation.\n"
           "uint ajfx_shed_count(uint o, uint id, uint key, float3 v, uint spawn_index) {\n"
           "    if (!ajfx_shed_in_scope(spawn_index, o + 5u)) {\n"
           "        return 0u;\n"
           "    }\n"
           "    float speed_squared = ajfx_dot(v, v);\n"
           "    if (speed_squared < ajfx_f(o)) {\n"
           "        return 0u;\n"
           "    }\n"
           "    float speed = sqrt(speed_squared);\n"
           "    float wanted = (ajfx_f(o + 1u) + ajfx_f(o + 2u) * speed) * ajfx_dt;\n"
           "    float roll = ajfx_particle_unit(ajfx_seed, ajfx_hash_mix(key, spawn_index), ajfx_hash_mix(id, 0x8000a71cu));\n"
           "    float count = floor(wanted + roll);\n"
           "    uint max_per_step = ajfx_u32(o + 3u);\n"
           "    return count >= (float)max_per_step ? max_per_step : (uint)count;\n"
           "}\n\n"
           "[numthreads(1024, 1, 1)]\n"
           "void shed(uint thread : SV_GroupIndex) {\n"
           "    uint o = 0u;\n"
           "    uint id = 0u;\n"
           "    uint slot = 0u;\n"
           "    uint source_stride = 0u;\n"
           "    uint at_position = 0u;\n"
           "    uint at_velocity = 0xffffffffu;\n"
           "    uint at_spawn_index = 0u;\n";
    for (std::size_t k = 0U; k < shedding.size(); ++k) {
        const Shedding& m = shedding[k];
        out += std::string{k == 0U ? "    if" : " else if"} + " (ajfx_shed_module == " + std::to_string(k) +
               "u) {\n"
               "        o = " +
               std::to_string(m.offset + 1U) + "u;\n        id = ajfx_u32(" + std::to_string(m.offset) +
               "u);\n        slot = " + std::to_string(m.slot) + "u;\n        source_stride = " +
               std::to_string(m.source.stride) + "u;\n        at_position = " + field_offset(m.source.position) +
               ";\n        at_velocity = " + field_offset(m.source.velocity) +
               ";\n        at_spawn_index = " + field_offset(m.source.spawn_index) + ";\n    }";
    }
    out += "\n"
           "    uint key = ajfx_hash_mix(ajfx_tick_lo, ajfx_tick_hi);\n"
           "    uint shed_before = ajfx_shed_module == 0u ? 0u : ajfx_counters.Load(12u);\n"
           "    uint base = ajfx_birth_count + shed_before;\n"
           "    uint alive = ajfx_dst.Load(0u);\n"
           "    uint max_per_step = ajfx_u32(o + 3u);\n"
           "    float inherit = ajfx_f(o + 4u);\n"
           "    uint counted = ajfx_flags.Load((2u + ajfx_shed_module) * 4u);\n"
           "    uint made = 0u;\n"
           "    for (uint first = 0u; first < alive && made < max_per_step; first += 4096u) {\n"
           "        uint counts[4];\n"
           "        uint sum = 0u;\n"
           "        for (uint k = 0u; k < 4u; ++k) {\n"
           "            uint i = first + thread * 4u + k;\n"
           "            counts[k] = 0u;\n"
           "            if (i < alive) {\n"
           "                uint record = i * source_stride;\n"
           "                float3 v = at_velocity != 0xffffffffu ? asfloat(ajfx_src.Load3(record + at_velocity))\n"
           "                                                      : float3(0.0f, 0.0f, 0.0f);\n"
           "                counts[k] = ajfx_shed_count(o, id, key, v, ajfx_src.Load(record + at_spawn_index));\n"
           "            }\n"
           "            sum += counts[k];\n"
           "        }\n"
           "        uint inclusive = ajfx_shed_scan(sum, thread);\n"
           "        uint chunk_total = ajfx_scan[1023u];\n"
           "        uint running = inclusive >= 0x40000000u ? 0x40000000u : made + (inclusive - sum);\n"
           "        for (uint k = 0u; k < 4u; ++k) {\n"
           "            uint i = first + thread * 4u + k;\n"
           "            if (i < alive && counts[k] != 0u) {\n"
           "                uint before = min(running, max_per_step);\n"
           "                uint n = min(counts[k], max_per_step - before);\n"
           "                uint record = i * source_stride;\n"
           "                float3 position = asfloat(ajfx_src.Load3(record + at_position));\n"
           "                float3 v = at_velocity != 0xffffffffu ? asfloat(ajfx_src.Load3(record + at_velocity))\n"
           "                                                      : float3(0.0f, 0.0f, 0.0f);\n"
           "                for (uint j = 0u; j < n; ++j) {\n"
           "                    uint b = base + before + j;\n"
           "                    if (b < ajfx_capacity) {\n"
           "                        // Along the step's stretch of the path, the first born the oldest.\n"
           "                        float age = ajfx_dt * (1.0f - ((float)j + 0.5f) / (float)n);\n"
           "                        float3 at = position - v * age;\n"
           "                        float3 thrown = v * inherit;\n"
           "                        ajfx_data.Store((ajfx_ages_offset + b) * 4u, asuint(age));\n"
           "                        ajfx_data.Store((ajfx_ages_offset + ajfx_capacity + b) * 4u,\n"
           "                                        (slot << 24u) | ((counted + before + j) & 0x00ffffffu));\n"
           "                        uint origin = (ajfx_ages_offset + 2u * ajfx_capacity + b * " +
           std::to_string(detail::birth_origin_floats) +
           "u) * 4u;\n"
           "                        ajfx_data.Store3(origin, asuint(at));\n"
           "                        ajfx_data.Store3(origin + 12u, asuint(thrown));\n"
           "                        ajfx_data.Store(origin + 24u, 1u);\n"
           "                    }\n"
           "                }\n"
           "                running = min(running + counts[k], 0x40000000u);\n"
           "            }\n"
           "        }\n"
           "        made = min(made + chunk_total, max_per_step);\n"
           "        GroupMemoryBarrierWithGroupSync();\n"
           "    }\n"
           "    if (thread == 0u) {\n"
           "        ajfx_flags.Store((2u + ajfx_shed_module) * 4u, counted + made);\n"
           "        uint lo = ajfx_flags.Load(0u) + made;\n"
           "        uint hi = ajfx_flags.Load(4u) + (lo < made ? 1u : 0u);\n"
           "        ajfx_flags.Store2(0u, uint2(lo, hi));\n"
           "        uint shed_total = shed_before + made;\n"
           "        ajfx_counters.Store(12u, shed_total);\n"
           "        ajfx_write_groups(40u, min(ajfx_birth_count + shed_total, ajfx_capacity), ajfx_group_size);\n"
           "    }\n"
           "}\n";
}

EmitterKernelSource generate_emitter(const Effect& effect, const Emitter& emitter) {
    EmitterKernelSource result;
    result.layout = particle_layout(emitter);

    // A rigid physics emitter's particles are simulated on the CPU and
    // written into its buffers (see GpuSimulation); its kernels do nothing.
    if (rigid_physics(emitter) != nullptr) {
        result.module_floats = detail::module_float_count(effect, emitter);
        result.source = "// Generated by ajfx for rigid physics emitter '" + printable(emitter.name) +
                        "' of effect '" + printable(effect.name) +
                        "'; the CPU simulates it.\n\n"
                        "[numthreads(1, 1, 1)]\nvoid update() {}\n\n"
                        "[numthreads(1, 1, 1)]\nvoid spawn() {}\n";
        return result;
    }

    // Each module's data offset, in the order pack_emitter writes them.
    std::vector<float> scratch;
    std::vector<ParameterValue> values;
    detail::resolve_values(effect, nullptr, 0U, values);
    const detail::Inputs in{effect, values};
    const detail::PackContext context{in, 1.0F / static_cast<float>(effect.simulation_rate), 0.0};
    detail::Packer packer{scratch};
    const auto offset_of = [&](const auto& module) {
        const auto offset = static_cast<std::uint32_t>(scratch.size());
        detail::pack_module(module, context, packer);
        return offset;
    };

    std::string init_calls;
    std::string turn_call;
    for (const InitModule& module : emitter.init) {
        std::visit(
            [&](const auto& m) {
                const std::uint32_t offset = offset_of(m);
                if constexpr (std::is_same_v<std::decay_t<decltype(m)>, InitSpin>) {
                    turn_call = "        ajfx_spin_turn(p, " + std::to_string(offset + 1U) + "u);\n";
                }
                init_calls += call(m, offset);
            },
            module);
    }
    std::string force_calls;
    std::string after_calls;
    std::string appearance_calls;
    std::string defines;
    // A glowing emitter's colour is worked out afresh after everything
    // else: its base, every colour over life, then every heat (see
    // detail::glow), so a glow is never added to the last step's.
    std::string color_calls;
    std::string heat_calls;
    for (const UpdateModule& module : emitter.update) {
        std::visit(
            [&](const auto& m) {
                using M = std::decay_t<decltype(m)>;
                const std::uint32_t offset = offset_of(m);
                if constexpr (std::is_same_v<M, RigidBody>) {
                    defines += "#define AJFX_RIGID_DATA " + std::to_string(offset + 1U) + "u\n";
                }
                const std::string line = call(m, offset);
                if constexpr (std::is_same_v<M, Heat>) {
                    heat_calls += line;
                } else {
                    if constexpr (std::is_same_v<M, ColorOverLife>) {
                        color_calls += line;
                    }
                    (is_force(module) ? force_calls : after_calls) += line;
                    if (is_appearance(module)) {
                        appearance_calls += line;
                    }
                }
            },
            module);
    }
    const std::string glow_calls =
        heat_calls.empty() ? std::string{} : "        p.color = p.base_color;\n" + color_calls + heat_calls;
    // The spawn modules shedding from other emitters' particles, packed after
    // the stacks (pack_emitter): each one's data offset, its place in the
    // spawn stack and the layout of the emitter it sheds from, which its
    // shed kernel reads directly.
    struct Shedding final {
        std::uint32_t offset;
        std::uint32_t slot;
        ParticleLayout source;
    };
    std::vector<Shedding> shedding;
    for (std::size_t slot = 0U; slot < emitter.spawn.size(); ++slot) {
        const auto* shed = std::get_if<SpawnFromParticles>(&emitter.spawn[slot]);
        if (shed == nullptr) {
            continue;
        }
        ParticleLayout source;
        for (const Emitter& other : effect.emitters) {
            if (other.name == shed->emitter) {
                source = particle_layout(other);
                break;
            }
        }
        shedding.push_back(Shedding{offset_of(*shed), static_cast<std::uint32_t>(slot), source});
    }
    result.module_floats = static_cast<std::uint32_t>(scratch.size());
    result.sheds = !shedding.empty();

    std::string& out = result.source;
    out += "// Generated by ajfx for emitter '" + printable(emitter.name) + "' of effect '" +
           printable(effect.name) + "'.\n\n";
    out += defines;
    out += expand_includes(detail::hlsl_particles);
    out += "\n";
    generate_access(out, result.layout);

    out += "[numthreads(256, 1, 1)]\n"
           "void update(uint3 group : SV_GroupID, uint thread : SV_GroupIndex) {\n"
           "    uint i = ajfx_index(group, thread);\n"
           "    if (i >= ajfx_counters.Load(0u)) {\n"
           "        return;\n"
           "    }\n"
           "    Particle p = ajfx_load(i * ajfx_stride);\n"
           "    p.age += ajfx_dt;\n"
           "    bool live = p.age < p.lifetime;\n"
           "    if (live) {\n";
    out += force_calls;
    out += "        p.position += p.velocity * ajfx_dt;\n";
    out += turn_call;
    out += after_calls;
    out += glow_calls;
    out += "        live = p.age < p.lifetime;\n"
           "    }\n"
           "    ajfx_store_src(i * ajfx_stride, p);\n"
           "    ajfx_flags.Store(i * 4u, live ? 1u : 0u);\n"
           "}\n\n";

    out += "[numthreads(256, 1, 1)]\n"
           "void spawn(uint3 group : SV_GroupID, uint thread : SV_GroupIndex) {\n"
           "    uint b = ajfx_index(group, thread);\n"
           "    uint compacted = ajfx_counters.Load(4u);\n"
           "    if (b >= ajfx_births() || b >= ajfx_capacity - compacted) {\n"
           "        return;\n"
           "    }\n"
           "    Particle p = ajfx_default_particle();\n"
           "    p.age = ajfx_f(ajfx_ages_offset + b);\n"
           "    p.spawn_index = ajfx_u32(ajfx_ages_offset + ajfx_capacity + b);\n"
           "    {\n";
    out += init_calls;
    out += "    }\n";
    if (detail::has_birth_origins(emitter)) {
        // Born of a contact or a moving body: the init modules' position is
        // an offset from where it put the particle, their velocity added to
        // what it gave, as the reference does it.
        out += "    {\n"
               "        uint origin = ajfx_ages_offset + 2u * ajfx_capacity + b * " +
               std::to_string(detail::birth_origin_floats) +
               "u;\n"
               "        if (ajfx_u32(origin + 6u) != 0u) {\n"
               "            p.position = ajfx_f3(origin) + p.position;\n"
               "            p.velocity = p.velocity + ajfx_f3(origin + 3u);\n"
               "        }\n"
               "    }\n";
    }
    out += "    p.color = p.base_color;\n"
           "    p.size = p.base_size;\n"
           "    p.position += p.velocity * p.age;\n"
           "    p.rotation += p.spin * p.age;\n"
           "    {\n";
    out += appearance_calls;
    out += glow_calls;
    out += "    }\n"
           "    ajfx_store_dst((compacted + b) * ajfx_stride, p);\n"
           "}\n";
    if (!shedding.empty()) {
        generate_shed(out, emitter, shedding);
    }
    return result;
}

} // namespace

ParticleLayout particle_layout(const Emitter& emitter) {
    const std::uint32_t mask = required_attributes(emitter);
    bool color_over_life = false;
    bool size_over_life = false;
    for (const UpdateModule& module : emitter.update) {
        // A glow is worked out from the base colour every step too.
        color_over_life = color_over_life || std::holds_alternative<ColorOverLife>(module) ||
                          std::holds_alternative<Heat>(module);
        size_over_life = size_over_life || std::holds_alternative<SizeOverLife>(module);
    }

    ParticleLayout layout;
    std::uint32_t next = 0U;
    const auto place = [&next](std::int32_t& field, std::uint32_t bytes) {
        field = static_cast<std::int32_t>(next);
        next += bytes;
    };
    place(layout.position, 12U);
    if (has_attribute(mask, Attribute::Velocity)) {
        place(layout.velocity, 12U);
    }
    place(layout.age, 4U);
    place(layout.lifetime, 4U);
    place(layout.spawn_index, 4U);
    if (has_attribute(mask, Attribute::Color)) {
        place(layout.color, 16U);
        if (color_over_life) {
            place(layout.base_color, 16U);
        }
    }
    if (has_attribute(mask, Attribute::Size)) {
        place(layout.size, 4U);
        if (size_over_life) {
            place(layout.base_size, 4U);
        }
    }
    if (has_attribute(mask, Attribute::Rotation)) {
        place(layout.rotation, 4U);
    }
    if (has_attribute(mask, Attribute::Orientation)) {
        place(layout.orientation, 16U);
    }
    if (has_attribute(mask, Attribute::AngularVelocity)) {
        place(layout.angular_velocity, 12U);
    }
    if (has_attribute(mask, Attribute::Spin)) {
        place(layout.spin, 4U);
    }
    if (models_by_kind(emitter)) {
        place(layout.body_kind, 4U);
    }
    layout.stride = next;
    return layout;
}

std::vector<EmitterKernelSource> generate_kernel_sources(const Effect& effect) {
    std::vector<EmitterKernelSource> sources;
    sources.reserve(effect.emitters.size());
    for (const Emitter& emitter : effect.emitters) {
        sources.push_back(generate_emitter(effect, emitter));
    }
    return sources;
}

std::string runtime_kernel_source() {
    return expand_includes(detail::hlsl_runtime);
}

std::string sprite_shader_source() {
    return std::string{detail::hlsl_sprite};
}

std::string sort_kernel_source() {
    return std::string{detail::hlsl_sort};
}

std::string volume_kernel_source() {
    return expand_includes(detail::hlsl_volume_kernels);
}

std::string volume_shader_source() {
    return expand_includes(detail::hlsl_volume_draw);
}

Result<KernelSet> compile_kernels(const Effect& effect, ShaderCompiler& compiler) {
    Result<bool> valid = validate(effect);
    if (!valid) {
        return valid.error();
    }
    KernelSet set;
    set.format = compiler.format();
    const std::string runtime = runtime_kernel_source();
    for (const auto& [entry, out] : {std::pair{"scan_blocks", &set.scan_blocks}, std::pair{"scan_totals", &set.scan_totals},
                                     std::pair{"scatter", &set.scatter}, std::pair{"finalize", &set.finalize}}) {
        Result<std::vector<std::uint8_t>> code =
            compiler.compile(runtime, entry, ShaderStage::Compute, std::string{"ajfx "} + entry);
        if (!code) {
            return code.error();
        }
        *out = std::move(code).value();
    }
    const std::string sort = sort_kernel_source();
    for (const auto& [entry, out] : {std::pair{"sort_keys", &set.sort_keys}, std::pair{"sort_count", &set.sort_count},
                                     std::pair{"sort_scan", &set.sort_scan},
                                     std::pair{"sort_scatter", &set.sort_scatter},
                                     std::pair{"sort_range", &set.sort_range},
                                     std::pair{"sort_slices", &set.sort_slices}}) {
        Result<std::vector<std::uint8_t>> code =
            compiler.compile(sort, entry, ShaderStage::Compute, std::string{"ajfx "} + entry);
        if (!code) {
            return code.error();
        }
        *out = std::move(code).value();
    }
    const std::string sprite = sprite_shader_source();
    for (const auto& [entry, stage, out] : {std::tuple{"sprite_vertex", ShaderStage::Vertex, &set.sprite_vertex},
                                            std::tuple{"sprite_pixel", ShaderStage::Pixel, &set.sprite_pixel}}) {
        Result<std::vector<std::uint8_t>> code = compiler.compile(sprite, entry, stage, std::string{"ajfx "} + entry);
        if (!code) {
            return code.error();
        }
        *out = std::move(code).value();
    }
    if (!effect.volumes.empty()) {
        const std::string volume = volume_kernel_source();
        for (std::size_t k = 0U; k < volume_kernel_count; ++k) {
            const std::string entry{volume_kernel_names[k]};
            Result<std::vector<std::uint8_t>> code =
                compiler.compile(volume, entry, ShaderStage::Compute, "ajfx " + entry);
            if (!code) {
                return code.error();
            }
            set.volume[k] = std::move(code).value();
        }
        const std::string draw = volume_shader_source();
        for (const auto& [entry, stage, out] : {std::tuple{"volume_vertex", ShaderStage::Vertex, &set.volume_vertex},
                                                std::tuple{"volume_pixel", ShaderStage::Pixel, &set.volume_pixel}}) {
            Result<std::vector<std::uint8_t>> code = compiler.compile(draw, entry, stage, std::string{"ajfx "} + entry);
            if (!code) {
                return code.error();
            }
            *out = std::move(code).value();
        }
    }
    const std::vector<EmitterKernelSource> sources = generate_kernel_sources(effect);
    for (std::size_t i = 0U; i < sources.size(); ++i) {
        const std::string name = "ajfx " + effect.name + "/" + effect.emitters[i].name;
        EmitterKernels& kernels = set.emitters.emplace_back();
        Result<std::vector<std::uint8_t>> update =
            compiler.compile(sources[i].source, "update", ShaderStage::Compute, name + " update");
        if (!update) {
            return update.error();
        }
        Result<std::vector<std::uint8_t>> spawn =
            compiler.compile(sources[i].source, "spawn", ShaderStage::Compute, name + " spawn");
        if (!spawn) {
            return spawn.error();
        }
        kernels.update = std::move(update).value();
        kernels.spawn = std::move(spawn).value();
        if (sources[i].sheds) {
            Result<std::vector<std::uint8_t>> shed =
                compiler.compile(sources[i].source, "shed", ShaderStage::Compute, name + " shed");
            if (!shed) {
                return shed.error();
            }
            kernels.shed = std::move(shed).value();
        }
    }
    return set;
}

namespace {

std::string_view format_name(ShaderFormat format) {
    switch (format) {
    case ShaderFormat::Dxil:
        return "dxil";
    }
    return "unknown";
}

void hash_bytes(std::uint64_t& hash, std::string_view bytes) {
    for (const char c : bytes) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 0x100000001b3ULL;
    }
}

// Every kernel set entry the effect needs and its name within
// kernels/<format>/: a shed kernel only for an emitter that sheds from
// particles.
std::vector<std::pair<std::string, std::vector<std::uint8_t>*>> named_kernels(KernelSet& set, const Effect& effect) {
    std::vector<std::pair<std::string, std::vector<std::uint8_t>*>> named{
        {"runtime/scan_blocks", &set.scan_blocks}, {"runtime/scan_totals", &set.scan_totals},
        {"runtime/scatter", &set.scatter},         {"runtime/finalize", &set.finalize},
        {"sort/keys", &set.sort_keys},             {"sort/count", &set.sort_count},
        {"sort/scan", &set.sort_scan},             {"sort/scatter", &set.sort_scatter},
        {"sort/range", &set.sort_range},           {"sort/slices", &set.sort_slices},
        {"sprite/vertex", &set.sprite_vertex},     {"sprite/pixel", &set.sprite_pixel},
    };
    // The grid volumes' kernels and shaders, for an effect with volumes.
    if (!effect.volumes.empty()) {
        for (std::size_t k = 0U; k < volume_kernel_count; ++k) {
            named.emplace_back("volume/" + std::string{volume_kernel_names[k]}, &set.volume[k]);
        }
        named.emplace_back("volume/vertex", &set.volume_vertex);
        named.emplace_back("volume/pixel", &set.volume_pixel);
    }
    const std::size_t emitters = effect.emitters.size();
    set.emitters.resize(emitters);
    for (std::size_t i = 0U; i < emitters; ++i) {
        named.emplace_back("emitter/" + std::to_string(i) + "/update", &set.emitters[i].update);
        named.emplace_back("emitter/" + std::to_string(i) + "/spawn", &set.emitters[i].spawn);
        if (!detail::shedding_slots(effect.emitters[i]).empty()) {
            named.emplace_back("emitter/" + std::to_string(i) + "/shed", &set.emitters[i].shed);
        }
    }
    return named;
}

} // namespace

std::uint64_t kernel_source_hash(const Effect& effect) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    // Lengths separate the pieces so moving text between them changes the hash.
    const auto add = [&hash](std::string_view text) {
        hash_bytes(hash, std::to_string(text.size()) + ":");
        hash_bytes(hash, text);
    };
    add(runtime_kernel_source());
    add(sort_kernel_source());
    add(sprite_shader_source());
    if (!effect.volumes.empty()) {
        add(volume_kernel_source());
        add(volume_shader_source());
    }
    for (const EmitterKernelSource& source : generate_kernel_sources(effect)) {
        add(source.source);
    }
    return hash;
}

std::vector<PackageEntry> kernel_package_entries(const Effect& effect, const KernelSet& kernels) {
    const std::string prefix = "kernels/" + std::string{format_name(kernels.format)} + "/";
    KernelSet copy = kernels;
    std::vector<PackageEntry> entries;
    for (auto& [name, code] : named_kernels(copy, effect)) {
        entries.push_back({prefix + name, std::move(*code)});
    }
    std::vector<std::uint8_t> hash(8U);
    const std::uint64_t value = kernel_source_hash(effect);
    for (std::size_t i = 0U; i < 8U; ++i) {
        hash[i] = static_cast<std::uint8_t>(value >> (8U * i));
    }
    entries.push_back({prefix + "source_hash", std::move(hash)});
    return entries;
}

Result<KernelSet> read_package_kernels(std::span<const std::uint8_t> package, const Effect& effect,
                                       ShaderFormat format) {
    Result<std::vector<PackageEntry>> entries = read_container(package);
    if (!entries) {
        return entries.error();
    }
    const std::string prefix = "kernels/" + std::string{format_name(format)} + "/";
    std::map<std::string, const std::vector<std::uint8_t>*> by_name;
    for (const PackageEntry& entry : entries.value()) {
        by_name.emplace(entry.name, &entry.data);
    }
    const auto unavailable = [&](const std::string& why) {
        return Error{ErrorCode::KernelsUnavailable,
                     "package kernels for " + std::string{format_name(format)} + " cannot be used: " + why};
    };
    const auto hash = by_name.find(prefix + "source_hash");
    if (hash == by_name.end()) {
        return unavailable("the package has none");
    }
    std::uint64_t stored = 0U;
    for (std::size_t i = 0U; i < 8U && i < hash->second->size(); ++i) {
        stored |= static_cast<std::uint64_t>((*hash->second)[i]) << (8U * i);
    }
    if (hash->second->size() != 8U || stored != kernel_source_hash(effect)) {
        return unavailable("they were compiled from different source; recompile them with this version of ajfx");
    }
    KernelSet set;
    set.format = format;
    for (auto& [name, code] : named_kernels(set, effect)) {
        const auto found = by_name.find(prefix + name);
        if (found == by_name.end() || found->second->empty()) {
            return unavailable("'" + prefix + name + "' is missing");
        }
        *code = *found->second;
    }
    return set;
}

} // namespace ajfx::gpu
