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
// the id rather than baking it in.
template <class Module>
std::string call(const Module&, std::uint32_t offset) {
    return "        ajfx_" + std::string{Module::kind} + "(p, " + std::to_string(offset + 1U) + "u, ajfx_u32(" +
           std::to_string(offset) + "u));\n";
}

bool is_appearance(const UpdateModule& module) {
    return std::holds_alternative<ColorOverLife>(module) || std::holds_alternative<SizeOverLife>(module);
}

EmitterKernelSource generate_emitter(const Effect& effect, const Emitter& emitter) {
    EmitterKernelSource result;
    result.layout = particle_layout(emitter);

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
    for (const UpdateModule& module : emitter.update) {
        std::visit(
            [&](const auto& m) {
                const std::uint32_t offset = offset_of(m);
                if constexpr (std::is_same_v<std::decay_t<decltype(m)>, RigidBody>) {
                    defines += "#define AJFX_RIGID_DATA " + std::to_string(offset + 1U) + "u\n";
                }
                const std::string line = call(m, offset);
                (is_force(module) ? force_calls : after_calls) += line;
                if (is_appearance(module)) {
                    appearance_calls += line;
                }
            },
            module);
    }
    result.module_floats = static_cast<std::uint32_t>(scratch.size());

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
    out += "        live = p.age < p.lifetime;\n"
           "    }\n"
           "    ajfx_store_src(i * ajfx_stride, p);\n"
           "    ajfx_flags.Store(i * 4u, live ? 1u : 0u);\n"
           "}\n\n";

    out += "[numthreads(256, 1, 1)]\n"
           "void spawn(uint3 group : SV_GroupID, uint thread : SV_GroupIndex) {\n"
           "    uint b = ajfx_index(group, thread);\n"
           "    uint compacted = ajfx_counters.Load(4u);\n"
           "    if (b >= ajfx_birth_count || b >= ajfx_capacity - compacted) {\n"
           "        return;\n"
           "    }\n"
           "    Particle p = ajfx_default_particle();\n"
           "    p.age = ajfx_f(ajfx_ages_offset + b);\n"
           "    p.spawn_index = ajfx_spawn_base + b;\n"
           "    {\n";
    out += init_calls;
    out += "    }\n"
           "    p.color = p.base_color;\n"
           "    p.size = p.base_size;\n"
           "    p.position += p.velocity * p.age;\n"
           "    p.rotation += p.spin * p.age;\n"
           "    {\n";
    out += appearance_calls;
    out += "    }\n"
           "    ajfx_store_dst((compacted + b) * ajfx_stride, p);\n"
           "}\n";
    return result;
}

} // namespace

ParticleLayout particle_layout(const Emitter& emitter) {
    const std::uint32_t mask = required_attributes(emitter);
    bool color_over_life = false;
    bool size_over_life = false;
    for (const UpdateModule& module : emitter.update) {
        color_over_life = color_over_life || std::holds_alternative<ColorOverLife>(module);
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
                                     std::pair{"sort_scatter", &set.sort_scatter}}) {
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

// Every kernel set entry and its name within kernels/<format>/.
std::vector<std::pair<std::string, std::vector<std::uint8_t>*>> named_kernels(KernelSet& set,
                                                                               std::size_t emitters) {
    std::vector<std::pair<std::string, std::vector<std::uint8_t>*>> named{
        {"runtime/scan_blocks", &set.scan_blocks}, {"runtime/scan_totals", &set.scan_totals},
        {"runtime/scatter", &set.scatter},         {"runtime/finalize", &set.finalize},
        {"sort/keys", &set.sort_keys},             {"sort/count", &set.sort_count},
        {"sort/scan", &set.sort_scan},             {"sort/scatter", &set.sort_scatter},
        {"sprite/vertex", &set.sprite_vertex},     {"sprite/pixel", &set.sprite_pixel},
    };
    set.emitters.resize(emitters);
    for (std::size_t i = 0U; i < emitters; ++i) {
        named.emplace_back("emitter/" + std::to_string(i) + "/update", &set.emitters[i].update);
        named.emplace_back("emitter/" + std::to_string(i) + "/spawn", &set.emitters[i].spawn);
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
    for (const EmitterKernelSource& source : generate_kernel_sources(effect)) {
        add(source.source);
    }
    return hash;
}

std::vector<PackageEntry> kernel_package_entries(const Effect& effect, const KernelSet& kernels) {
    const std::string prefix = "kernels/" + std::string{format_name(kernels.format)} + "/";
    KernelSet copy = kernels;
    std::vector<PackageEntry> entries;
    for (auto& [name, code] : named_kernels(copy, kernels.emitters.size())) {
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
    for (auto& [name, code] : named_kernels(set, effect.emitters.size())) {
        const auto found = by_name.find(prefix + name);
        if (found == by_name.end() || found->second->empty()) {
            return unavailable("'" + prefix + name + "' is missing");
        }
        *code = *found->second;
    }
    return set;
}

} // namespace ajfx::gpu
