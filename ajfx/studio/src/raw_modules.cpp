#include "raw_modules.hpp"

#include "catalog.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>

namespace ajfx::studio {
namespace {

template <class Stage, class M>
RawModule make_module() {
    return RawModule{Stage{M{}}};
}

std::string_view title_of(std::string_view kind) {
    for (const RawKindDef& def : raw_kinds()) {
        if (def.kind == kind) {
            return def.title;
        }
    }
    return kind;
}

std::string text_of(float value) {
    return format_number(value, 3);
}

std::string text_of(Vec3 value) {
    return "(" + text_of(value.x) + ", " + text_of(value.y) + ", " + text_of(value.z) + ")";
}

std::string text_of(Color value) {
    return "(" + text_of(value.r) + ", " + text_of(value.g) + ", " + text_of(value.b) + ", " + text_of(value.a) +
           ")";
}

template <class T>
std::string text_of(const Input<T>& input) {
    return input.bound() ? "bound to " + input.parameter : text_of(input.value);
}

struct Describe final {
    std::string out;

    void separator() {
        if (!out.empty()) {
            out += ", ";
        }
    }

    template <class T>
    void operator()(std::string_view name, const T& field) {
        separator();
        out += name;
        out += ' ';
        if constexpr (std::is_same_v<T, bool>) {
            out += field ? "on" : "off";
        } else if constexpr (std::is_same_v<T, std::uint32_t>) {
            out += std::to_string(field);
        } else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, Vec3>) {
            out += text_of(field);
        } else if constexpr (std::is_same_v<T, std::string>) {
            out += field.empty() ? std::string{"none"} : "'" + field + "'";
        } else if constexpr (NamedEnum<T>) {
            out += EnumNames<T>::names[static_cast<std::size_t>(field)];
        } else if constexpr (std::is_same_v<T, FloatInput> || std::is_same_v<T, Vec3Input> ||
                             std::is_same_v<T, ColorInput>) {
            out += text_of(field);
        } else if constexpr (std::is_same_v<T, std::vector<ColorKey>>) {
            out += std::to_string(field.size()) + " keys";
        } else if constexpr (std::is_same_v<T, std::vector<CurveKey>>) {
            out += "[";
            for (std::size_t i = 0U; i < field.size(); ++i) {
                out += (i == 0U ? "" : " ") + text_of(field[i].t) + ":" + text_of(field[i].value);
            }
            out += "]";
        } else if constexpr (std::is_same_v<T, Scope>) {
            out += field.empty() ? std::string{"all"}
                                 : std::to_string(field.sources.size()) + " sources, " +
                                       std::to_string(field.kinds.size()) + " kinds";
        }
    }
};

json::Value indices_json(const std::vector<std::uint32_t>& indices) {
    json::Value array = json::Value::array();
    for (const std::uint32_t i : indices) {
        array.push(json::Value::of(static_cast<double>(i)));
    }
    return array;
}

bool read_indices(const json::Value* value, std::vector<std::uint32_t>& out) {
    out.clear();
    if (value == nullptr) {
        return true;
    }
    if (value->type != json::Value::Type::Array) {
        return false;
    }
    for (const json::Value& item : value->items) {
        if (item.type != json::Value::Type::Number || item.number < 0.0 || item.number > 4294967295.0) {
            return false;
        }
        out.push_back(static_cast<std::uint32_t>(item.number));
    }
    return true;
}

json::Value vec_json(Vec3 v) {
    json::Value array = json::Value::array();
    array.push(json::Value::of(static_cast<double>(v.x)));
    array.push(json::Value::of(static_cast<double>(v.y)));
    array.push(json::Value::of(static_cast<double>(v.z)));
    return array;
}

json::Value color_json(Color c) {
    json::Value array = json::Value::array();
    for (const float component : {c.r, c.g, c.b, c.a}) {
        array.push(json::Value::of(static_cast<double>(component)));
    }
    return array;
}

bool read_floats(const json::Value& value, float* out, std::size_t count) {
    if (value.type != json::Value::Type::Array || value.items.size() != count) {
        return false;
    }
    for (std::size_t i = 0U; i < count; ++i) {
        if (value.items[i].type != json::Value::Type::Number) {
            return false;
        }
        out[i] = static_cast<float>(value.items[i].number);
    }
    return true;
}

struct ToJson final {
    json::Value& out;

    template <class T>
    static json::Value plain(const T& field) {
        if constexpr (std::is_same_v<T, bool>) {
            return json::Value::of(field);
        } else if constexpr (std::is_same_v<T, std::uint32_t> || std::is_same_v<T, float>) {
            return json::Value::of(static_cast<double>(field));
        } else if constexpr (std::is_same_v<T, Vec3>) {
            return vec_json(field);
        } else if constexpr (std::is_same_v<T, Color>) {
            return color_json(field);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return json::Value::of(field);
        } else if constexpr (NamedEnum<T>) {
            return json::Value::of(std::string{EnumNames<T>::names[static_cast<std::size_t>(field)]});
        } else {
            return json::Value{};
        }
    }

    template <class T>
    void operator()(std::string_view name, const T& field) {
        if constexpr (std::is_same_v<T, FloatInput> || std::is_same_v<T, Vec3Input> || std::is_same_v<T, ColorInput>) {
            if (field.bound()) {
                json::Value binding = json::Value::object();
                binding.add("bind", json::Value::of(field.parameter));
                out.add(std::string{name}, std::move(binding));
            } else {
                out.add(std::string{name}, plain(field.value));
            }
        } else if constexpr (std::is_same_v<T, std::vector<ColorKey>>) {
            json::Value keys = json::Value::array();
            for (const ColorKey& key : field) {
                json::Value entry = json::Value::object();
                entry.add("t", json::Value::of(static_cast<double>(key.t)));
                entry.add("value", color_json(key.value));
                keys.push(std::move(entry));
            }
            out.add(std::string{name}, std::move(keys));
        } else if constexpr (std::is_same_v<T, std::vector<CurveKey>>) {
            json::Value keys = json::Value::array();
            for (const CurveKey& key : field) {
                json::Value entry = json::Value::object();
                entry.add("t", json::Value::of(static_cast<double>(key.t)));
                entry.add("value", json::Value::of(static_cast<double>(key.value)));
                keys.push(std::move(entry));
            }
            out.add(std::string{name}, std::move(keys));
        } else if constexpr (std::is_same_v<T, Scope>) {
            // An empty scope means everything and is left out.
            if (!field.empty()) {
                json::Value scope = json::Value::object();
                scope.add("sources", indices_json(field.sources));
                scope.add("kinds", indices_json(field.kinds));
                out.add(std::string{name}, std::move(scope));
            }
        } else {
            out.add(std::string{name}, plain(field));
        }
    }
};

struct FromJson final {
    const json::Value& in;
    std::string& error;

    template <class T>
    bool plain(const json::Value& value, T& field) {
        if constexpr (std::is_same_v<T, bool>) {
            if (value.type != json::Value::Type::Bool) {
                return false;
            }
            field = value.boolean;
            return true;
        } else if constexpr (std::is_same_v<T, std::uint32_t>) {
            if (value.type != json::Value::Type::Number || value.number < 0.0 || value.number > 4294967295.0) {
                return false;
            }
            field = static_cast<std::uint32_t>(value.number);
            return true;
        } else if constexpr (std::is_same_v<T, float>) {
            if (value.type != json::Value::Type::Number) {
                return false;
            }
            field = static_cast<float>(value.number);
            return true;
        } else if constexpr (std::is_same_v<T, Vec3>) {
            return read_floats(value, &field.x, 3U);
        } else if constexpr (std::is_same_v<T, Color>) {
            return read_floats(value, &field.r, 4U);
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (value.type != json::Value::Type::String) {
                return false;
            }
            field = value.text;
            return true;
        } else if constexpr (NamedEnum<T>) {
            if (value.type != json::Value::Type::String) {
                return false;
            }
            for (std::size_t i = 0U; i < std::size(EnumNames<T>::names); ++i) {
                if (EnumNames<T>::names[i] == value.text) {
                    field = static_cast<T>(i);
                    return true;
                }
            }
            return false;
        } else {
            return false;
        }
    }

    template <class T>
    void operator()(std::string_view name, T& field) {
        const json::Value* value = in.find(name);
        if (value == nullptr || !error.empty()) {
            return; // a missing field keeps its default
        }
        bool ok = false;
        if constexpr (std::is_same_v<T, FloatInput> || std::is_same_v<T, Vec3Input> || std::is_same_v<T, ColorInput>) {
            if (value->type == json::Value::Type::Object) {
                // As ajfx::bind leaves it: no constant behind the binding.
                field.value = {};
                field.parameter = value->string_or("bind", "");
                ok = !field.parameter.empty();
            } else {
                field.parameter.clear();
                ok = plain(*value, field.value);
            }
        } else if constexpr (std::is_same_v<T, std::vector<ColorKey>> || std::is_same_v<T, std::vector<CurveKey>>) {
            ok = value->type == json::Value::Type::Array;
            field.clear();
            for (const json::Value& entry : value->items) {
                typename T::value_type key{};
                const json::Value* t = entry.find("t");
                const json::Value* v = entry.find("value");
                ok = ok && t != nullptr && v != nullptr && plain(*t, key.t) && plain(*v, key.value);
                field.push_back(key);
            }
        } else if constexpr (std::is_same_v<T, Scope>) {
            ok = value->type == json::Value::Type::Object && read_indices(value->find("sources"), field.sources) &&
                 read_indices(value->find("kinds"), field.kinds);
        } else {
            ok = plain(*value, field);
        }
        if (!ok) {
            error = "field '" + std::string{name} + "' has the wrong kind of value";
        }
    }
};

} // namespace

const std::vector<RawKindDef>& raw_kinds() {
    static const std::vector<RawKindDef> kinds = {
        {SpawnRate::kind, "Spawn rate", "spawn", make_module<SpawnModule, SpawnRate>},
        {SpawnBurst::kind, "Spawn burst", "spawn", make_module<SpawnModule, SpawnBurst>},
        {SpawnFromContacts::kind, "Spawn from contacts", "spawn", make_module<SpawnModule, SpawnFromContacts>},
        {SpawnFromBodies::kind, "Spawn from bodies", "spawn", make_module<SpawnModule, SpawnFromBodies>},
        {SpawnFromParticles::kind, "Spawn from particles", "spawn", make_module<SpawnModule, SpawnFromParticles>},
        {Lifetime::kind, "Lifetime", "init", make_module<InitModule, Lifetime>},
        {ShapeLocation::kind, "Shape location", "init", make_module<InitModule, ShapeLocation>},
        {Velocity::kind, "Velocity", "init", make_module<InitModule, Velocity>},
        {InitColor::kind, "Initial color", "init", make_module<InitModule, InitColor>},
        {InitSize::kind, "Initial size", "init", make_module<InitModule, InitSize>},
        {InitRotation::kind, "Initial rotation", "init", make_module<InitModule, InitRotation>},
        {InitTumble::kind, "Initial tumble", "init", make_module<InitModule, InitTumble>},
        {InitSpin::kind, "Initial spin", "init", make_module<InitModule, InitSpin>},
        {Gravity::kind, "Gravity (acceleration)", "update", make_module<UpdateModule, Gravity>},
        {Drag::kind, "Drag", "update", make_module<UpdateModule, Drag>},
        {CurlNoise::kind, "Curl noise", "update", make_module<UpdateModule, CurlNoise>},
        {Vortex::kind, "Vortex", "update", make_module<UpdateModule, Vortex>},
        {Attractor::kind, "Attractor", "update", make_module<UpdateModule, Attractor>},
        {Converge::kind, "Converge", "update", make_module<UpdateModule, Converge>},
        {RigidBody::kind, "Rigid body", "update", make_module<UpdateModule, RigidBody>},
        {PlaneCollision::kind, "Plane collision", "update", make_module<UpdateModule, PlaneCollision>},
        {SphereCollision::kind, "Sphere collision", "update", make_module<UpdateModule, SphereCollision>},
        {BoxCollision::kind, "Box collision", "update", make_module<UpdateModule, BoxCollision>},
        {SdfCollision::kind, "Distance field collision", "update", make_module<UpdateModule, SdfCollision>},
        {ColorOverLife::kind, "Color over life", "update", make_module<UpdateModule, ColorOverLife>},
        {SizeOverLife::kind, "Size over life", "update", make_module<UpdateModule, SizeOverLife>},
        {Heat::kind, "Heat", "update", make_module<UpdateModule, Heat>},
        {Wake::kind, "Wake", "update", make_module<UpdateModule, Wake>},
    };
    return kinds;
}

const RawKindDef* find_raw_kind(std::string_view kind) {
    for (const RawKindDef& def : raw_kinds()) {
        if (def.kind == kind) {
            return &def;
        }
    }
    return nullptr;
}

std::string_view raw_kind(const RawModule& module) {
    return std::visit(
        [](const auto& stage) {
            return std::visit([](const auto& typed) { return std::decay_t<decltype(typed)>::kind; }, stage);
        },
        module);
}

std::string_view raw_stage(const RawModule& module) {
    switch (module.index()) {
    case 0U: return "spawn";
    case 1U: return "init";
    default: return "update";
    }
}

std::string_view raw_title(const RawModule& module) {
    return title_of(raw_kind(module));
}

ModuleId raw_id(const RawModule& module) {
    return std::visit(
        [](const auto& stage) { return std::visit([](const auto& typed) { return typed.id; }, stage); }, module);
}

void set_raw_id(RawModule& module, ModuleId id) {
    std::visit([id](auto& stage) { std::visit([id](auto& typed) { typed.id = id; }, stage); }, module);
}

std::string describe_module(const RawModule& module) {
    Describe describe;
    visit_fields(module, describe);
    return describe.out;
}

std::string describe_renderer(const SpriteRenderer& renderer) {
    Describe describe;
    SpriteRenderer::fields(renderer, describe);
    return describe.out;
}

json::Value raw_to_json(const RawModule& module) {
    json::Value out = json::Value::object();
    out.add("stage", json::Value::of(std::string{raw_stage(module)}));
    out.add("kind", json::Value::of(std::string{raw_kind(module)}));
    json::Value fields = json::Value::object();
    ToJson writer{fields};
    visit_fields(module, writer);
    out.add("fields", std::move(fields));
    return out;
}

std::optional<RawModule> raw_from_json(const json::Value& value, std::string& error) {
    const RawKindDef* def = find_raw_kind(value.string_or("kind", ""));
    if (def == nullptr) {
        error = "unknown module kind '" + value.string_or("kind", "") + "'";
        return std::nullopt;
    }
    RawModule module = def->make();
    if (const json::Value* fields = value.find("fields"); fields != nullptr) {
        FromJson reader{*fields, error};
        visit_fields(module, reader);
        if (!error.empty()) {
            error = std::string{def->kind} + " " + error;
            return std::nullopt;
        }
    }
    return module;
}

} // namespace ajfx::studio
