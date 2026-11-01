#include <ajfx/package.hpp>

#include "json.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <iterator>
#include <optional>
#include <set>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace ajfx {
namespace {

using detail::Json;

constexpr std::string_view effect_entry = "effect.json";

// ---------------------------------------------------------------- writing

Json number(float value) {
    char buffer[32];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    return Json::number(std::string{buffer, result.ptr});
}

Json number(std::uint32_t value) {
    char buffer[16];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    return Json::number(std::string{buffer, result.ptr});
}

Json numbers(std::initializer_list<float> values) {
    Json array = Json::array();
    for (const float value : values) {
        array.items.push_back(number(value));
    }
    return array;
}

Json to_json(float value) { return number(value); }
Json to_json(std::uint32_t value) { return number(value); }
Json to_json(bool value) { return Json::flag(value); }
Json to_json(const std::string& value) { return Json::string(value); }
Json to_json(Vec3 value) { return numbers({value.x, value.y, value.z}); }
Json to_json(Color value) { return numbers({value.r, value.g, value.b, value.a}); }

template <class E>
    requires std::is_enum_v<E>
Json to_json(E value) {
    return Json::string(std::string{EnumNames<E>::names[static_cast<std::size_t>(value)]});
}

template <class T>
Json to_json(const Input<T>& input) {
    if (!input.bound()) {
        return to_json(input.value);
    }
    Json binding = Json::object();
    binding.add("parameter", Json::string(input.parameter));
    return binding;
}

template <class Key>
Json to_json(const std::vector<Key>& keys) {
    Json array = Json::array();
    for (const Key& key : keys) {
        Json item = Json::object();
        item.add("t", number(key.t));
        item.add("value", to_json(key.value));
        array.items.push_back(std::move(item));
    }
    return array;
}

// A body kind and a prop each write their own fields, the same list
// validation and reading use.
Json to_json(const RigidBodyKind& body) {
    Json object = Json::object();
    RigidBodyKind::fields(body, [&object](std::string_view name, const auto& field) {
        object.add(std::string{name}, to_json(field));
    });
    return object;
}

Json to_json(const std::vector<RigidBodyKind>& bodies) {
    Json array = Json::array();
    for (const RigidBodyKind& body : bodies) {
        array.items.push_back(to_json(body));
    }
    return array;
}

Json to_json(const RigidProp& prop) {
    Json object = Json::object();
    RigidProp::fields(prop, [&object](std::string_view name, const auto& field) {
        object.add(std::string{name}, to_json(field));
    });
    return object;
}

Json to_json(const std::vector<RigidProp>& props) {
    Json array = Json::array();
    for (const RigidProp& prop : props) {
        array.items.push_back(to_json(prop));
    }
    return array;
}

template <class Module>
Json module_to_json(const Module& module) {
    Json object = Json::object();
    object.add("type", Json::string(std::string{Module::kind}));
    if constexpr (requires { module.id; }) {
        object.add("id", number(module.id));
    }
    Module::fields(module, [&object](std::string_view name, const auto& field) {
        object.add(std::string{name}, to_json(field));
    });
    return object;
}

template <class Variant>
Json stack_to_json(const std::vector<Variant>& stack) {
    Json array = Json::array();
    for (const Variant& module : stack) {
        array.items.push_back(std::visit([](const auto& m) { return module_to_json(m); }, module));
    }
    return array;
}

std::string_view type_name(ParameterType type) {
    switch (type) {
    case ParameterType::Float:
        return "float";
    case ParameterType::Vec3:
        return "vec3";
    case ParameterType::Color:
        return "color";
    }
    return "float";
}

Json parameter_to_json(const ParameterDesc& parameter) {
    Json object = Json::object();
    object.add("name", Json::string(parameter.name));
    object.add("type", Json::string(std::string{type_name(parameter.type)}));
    const ParameterValue& v = parameter.default_value;
    switch (parameter.type) {
    case ParameterType::Float:
        object.add("default", number(v[0]));
        break;
    case ParameterType::Vec3:
        object.add("default", numbers({v[0], v[1], v[2]}));
        break;
    case ParameterType::Color:
        object.add("default", numbers({v[0], v[1], v[2], v[3]}));
        break;
    }
    object.add("min", number(parameter.min));
    object.add("max", number(parameter.max));
    return object;
}

// ---------------------------------------------------------------- reading

// Reads typed values out of the tree, recording the first problem with the
// path it was found at.
class Reader final {
public:
    [[nodiscard]] bool failed() const noexcept { return error_.has_value(); }
    [[nodiscard]] Error error() const { return *error_; }

    void fail(const std::string& path, const std::string& problem, ErrorCode code = ErrorCode::MalformedPackage) {
        if (!error_) {
            error_ = Error{code, std::string{effect_entry} + ": " + path + " " + problem};
        }
    }

    bool read(const std::string& path, const Json& json, float& out) {
        if (json.type != Json::Type::Number) {
            fail(path, "must be a number");
            return false;
        }
        const char* first = json.text.data();
        const char* last = first + json.text.size();
        const auto result = std::from_chars(first, last, out);
        if (result.ec != std::errc{} || result.ptr != last) {
            fail(path, "is not a representable number");
            return false;
        }
        return true;
    }

    bool read(const std::string& path, const Json& json, std::uint32_t& out) {
        const char* first = json.text.data();
        const char* last = first + json.text.size();
        if (json.type != Json::Type::Number || std::from_chars(first, last, out).ptr != last) {
            fail(path, "must be a whole number from 0 to 4294967295");
            return false;
        }
        return true;
    }

    bool read(const std::string& path, const Json& json, bool& out) {
        if (json.type != Json::Type::Bool) {
            fail(path, "must be true or false");
            return false;
        }
        out = json.boolean;
        return true;
    }

    bool read(const std::string& path, const Json& json, std::string& out) {
        if (json.type != Json::Type::String) {
            fail(path, "must be a string");
            return false;
        }
        out = json.text;
        return true;
    }

    bool read_floats(const std::string& path, const Json& json, float* out, std::size_t count) {
        if (json.type != Json::Type::Array || json.items.size() != count) {
            fail(path, "must be an array of " + std::to_string(count) + " numbers");
            return false;
        }
        for (std::size_t i = 0U; i < count; ++i) {
            if (!read(path + "[" + std::to_string(i) + "]", json.items[i], out[i])) {
                return false;
            }
        }
        return true;
    }

    bool read(const std::string& path, const Json& json, Vec3& out) {
        float v[3]{};
        if (!read_floats(path, json, v, 3U)) {
            return false;
        }
        out = {v[0], v[1], v[2]};
        return true;
    }

    bool read(const std::string& path, const Json& json, Color& out) {
        float v[4]{};
        if (!read_floats(path, json, v, 4U)) {
            return false;
        }
        out = {v[0], v[1], v[2], v[3]};
        return true;
    }

    template <class E>
        requires std::is_enum_v<E>
    bool read(const std::string& path, const Json& json, E& out) {
        if (json.type == Json::Type::String) {
            const auto& names = EnumNames<E>::names;
            for (std::size_t i = 0U; i < std::size(names); ++i) {
                if (names[i] == json.text) {
                    out = static_cast<E>(i);
                    return true;
                }
            }
        }
        fail(path, "has an unknown value");
        return false;
    }

    template <class T>
    bool read(const std::string& path, const Json& json, Input<T>& out) {
        if (json.type == Json::Type::Object) {
            const Json* parameter = json.find("parameter");
            if (parameter == nullptr || json.members.size() != 1U) {
                fail(path, "must be a value or {\"parameter\": name}");
                return false;
            }
            std::string name;
            if (!read(path + ".parameter", *parameter, name)) {
                return false;
            }
            out = Input<T>{bind(std::move(name))};
            return true;
        }
        T value{};
        if (!read(path, json, value)) {
            return false;
        }
        out = Input<T>{value};
        return true;
    }

    template <class Key>
        requires std::is_same_v<Key, ColorKey> || std::is_same_v<Key, CurveKey>
    bool read(const std::string& path, const Json& json, std::vector<Key>& out) {
        if (json.type != Json::Type::Array) {
            fail(path, "must be an array of keys");
            return false;
        }
        out.clear();
        for (std::size_t i = 0U; i < json.items.size(); ++i) {
            const std::string item_path = path + "[" + std::to_string(i) + "]";
            const Json& item = json.items[i];
            const Json* t = item.find("t");
            const Json* value = item.find("value");
            if (item.type != Json::Type::Object || t == nullptr || value == nullptr || item.members.size() != 2U) {
                fail(item_path, "must be {\"t\": number, \"value\": value}");
                return false;
            }
            Key key{};
            if (!read(item_path + ".t", *t, key.t) || !read(item_path + ".value", *value, key.value)) {
                return false;
            }
            out.push_back(key);
        }
        return true;
    }

    // A body kind is an object of RigidBodyKind's own fields, so a misspelt
    // one is refused exactly as a module's is, as a prop's is below.
    bool read(const std::string& path, const Json& json, std::vector<RigidBodyKind>& out) {
        if (json.type != Json::Type::Array) {
            fail(path, "must be an array of body kinds");
            return false;
        }
        out.clear();
        out.reserve(json.items.size());
        for (std::size_t i = 0U; i < json.items.size(); ++i) {
            RigidBodyKind body;
            read_fields(path + "[" + std::to_string(i) + "]", json.items[i], body, {});
            out.push_back(std::move(body));
        }
        return !failed();
    }

    // Each prop is an object of RigidProp's own fields, so a misspelt one is
    // refused exactly as a module's is.
    bool read(const std::string& path, const Json& json, std::vector<RigidProp>& out) {
        if (json.type != Json::Type::Array) {
            fail(path, "must be an array of props");
            return false;
        }
        out.clear();
        out.reserve(json.items.size());
        for (std::size_t i = 0U; i < json.items.size(); ++i) {
            RigidProp prop;
            read_fields(path + "[" + std::to_string(i) + "]", json.items[i], prop, {});
            out.push_back(std::move(prop));
        }
        return !failed();
    }

    // Reads the named fields of a described object. Missing fields keep their
    // defaults; members that are not fields (or in `extra`) are refused, so a
    // misspelt field is reported rather than ignored.
    template <class Described>
    void read_fields(const std::string& path, const Json& json, Described& out,
                     std::initializer_list<std::string_view> extra) {
        if (json.type != Json::Type::Object) {
            fail(path, "must be an object");
            return;
        }
        std::set<std::string_view> known{extra};
        Described::fields(out, [&](std::string_view name, auto& field) {
            known.insert(name);
            if (const Json* member = json.find(name)) {
                read(path + "." + std::string{name}, *member, field);
            }
        });
        for (const auto& [name, value] : json.members) {
            if (!known.contains(name)) {
                fail(path, "has unknown field '" + name + "'");
            }
        }
    }

    template <class Variant, std::size_t... I>
    std::optional<Variant> make_module(std::string_view kind, std::index_sequence<I...>) {
        std::optional<Variant> module;
        ((std::variant_alternative_t<I, Variant>::kind == kind ? (void)module.emplace(std::in_place_index<I>)
                                                               : void()),
         ...);
        return module;
    }

    template <class Variant>
    void read_stack(const std::string& path, const Json* json, std::vector<Variant>& out) {
        if (json == nullptr) {
            return;
        }
        if (json->type != Json::Type::Array) {
            fail(path, "must be an array of modules");
            return;
        }
        for (std::size_t i = 0U; i < json->items.size() && !failed(); ++i) {
            const std::string item_path = path + "[" + std::to_string(i) + "]";
            const Json& item = json->items[i];
            const Json* type = item.find("type");
            std::string kind;
            if (type == nullptr || !read(item_path + ".type", *type, kind)) {
                fail(item_path, "needs a module type");
                return;
            }
            std::optional<Variant> module =
                make_module<Variant>(kind, std::make_index_sequence<std::variant_size_v<Variant>>{});
            if (!module) {
                fail(item_path, "has unknown module type '" + kind + "' for this stack");
                return;
            }
            std::visit(
                [&](auto& m) {
                    if (const Json* id = item.find("id")) {
                        read(item_path + ".id", *id, m.id);
                    }
                    read_fields(item_path, item, m, {"type", "id"});
                },
                *module);
            out.push_back(*std::move(module));
        }
    }

    void read_parameter(const std::string& path, const Json& json, ParameterDesc& out) {
        if (json.type != Json::Type::Object) {
            fail(path, "must be an object");
            return;
        }
        for (const auto& [name, value] : json.members) {
            if (name != "name" && name != "type" && name != "default" && name != "min" && name != "max") {
                fail(path, "has unknown field '" + name + "'");
                return;
            }
        }
        const Json* name = json.find("name");
        const Json* type = json.find("type");
        const Json* value = json.find("default");
        const Json* min = json.find("min");
        const Json* max = json.find("max");
        if (name == nullptr || type == nullptr || value == nullptr || min == nullptr || max == nullptr) {
            fail(path, "needs name, type, default, min and max");
            return;
        }
        std::string type_text;
        if (!read(path + ".name", *name, out.name) || !read(path + ".type", *type, type_text) ||
            !read(path + ".min", *min, out.min) || !read(path + ".max", *max, out.max)) {
            return;
        }
        if (type_text == "float") {
            out.type = ParameterType::Float;
            read(path + ".default", *value, out.default_value[0]);
        } else if (type_text == "vec3") {
            out.type = ParameterType::Vec3;
            read_floats(path + ".default", *value, out.default_value.data(), 3U);
        } else if (type_text == "color") {
            out.type = ParameterType::Color;
            read_floats(path + ".default", *value, out.default_value.data(), 4U);
        } else {
            fail(path + ".type", "must be float, vec3 or color");
        }
    }

    void read_emitter(const std::string& path, const Json& json, Emitter& out) {
        if (json.type != Json::Type::Object) {
            fail(path, "must be an object");
            return;
        }
        for (const auto& [name, value] : json.members) {
            if (name != "name" && name != "seed" && name != "capacity" && name != "spawn" && name != "init" &&
                name != "update" && name != "renderer") {
                fail(path, "has unknown field '" + name + "'");
                return;
            }
        }
        const Json* name = json.find("name");
        if (name == nullptr || !read(path + ".name", *name, out.name)) {
            fail(path, "needs a name");
            return;
        }
        if (const Json* seed = json.find("seed")) {
            read(path + ".seed", *seed, out.seed);
        }
        if (const Json* capacity = json.find("capacity")) {
            read(path + ".capacity", *capacity, out.capacity);
        }
        read_stack(path + ".spawn", json.find("spawn"), out.spawn);
        read_stack(path + ".init", json.find("init"), out.init);
        read_stack(path + ".update", json.find("update"), out.update);
        if (const Json* renderer = json.find("renderer")) {
            const Json* type = renderer->find("type");
            if (type == nullptr || type->type != Json::Type::String || type->text != SpriteRenderer::kind) {
                fail(path + ".renderer", "must have type 'sprite'");
                return;
            }
            read_fields(path + ".renderer", *renderer, out.renderer, {"type"});
        }
    }

private:
    std::optional<Error> error_;
};

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint64_t fnv1a(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

class ByteReader final {
public:
    explicit ByteReader(std::span<const std::uint8_t> bytes) : bytes_{bytes} {}

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - at_; }

    bool u32(std::uint32_t& out) {
        if (remaining() < 4U) {
            return false;
        }
        out = 0U;
        for (std::uint32_t i = 0U; i < 4U; ++i) {
            out |= static_cast<std::uint32_t>(bytes_[at_++]) << (8U * i);
        }
        return true;
    }

    bool u64(std::uint64_t& out) {
        if (remaining() < 8U) {
            return false;
        }
        out = 0U;
        for (std::uint32_t i = 0U; i < 8U; ++i) {
            out |= static_cast<std::uint64_t>(bytes_[at_++]) << (8U * i);
        }
        return true;
    }

    bool take(std::size_t count, std::span<const std::uint8_t>& out) {
        if (remaining() < count) {
            return false;
        }
        out = bytes_.subspan(at_, count);
        at_ += count;
        return true;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t at_{0U};
};

constexpr std::uint8_t magic[4] = {'A', 'J', 'F', 'X'};
constexpr std::uint32_t max_entries = 4096U;
constexpr std::uint32_t max_name_length = 1024U;

Error malformed(const std::string& problem) {
    return Error{ErrorCode::MalformedPackage, "package " + problem};
}

} // namespace

std::string effect_to_json(const Effect& effect) {
    Json root = Json::object();
    root.add("format", number(effect_format_version));
    root.add("name", Json::string(effect.name));
    root.add("simulation_rate", number(effect.simulation_rate));
    Json& parameters = root.add("parameters", Json::array());
    for (const ParameterDesc& parameter : effect.parameters) {
        parameters.items.push_back(parameter_to_json(parameter));
    }
    Json& emitters = root.add("emitters", Json::array());
    for (const Emitter& emitter : effect.emitters) {
        Json object = Json::object();
        object.add("name", Json::string(emitter.name));
        object.add("seed", number(emitter.seed));
        object.add("capacity", number(emitter.capacity));
        object.add("spawn", stack_to_json(emitter.spawn));
        object.add("init", stack_to_json(emitter.init));
        object.add("update", stack_to_json(emitter.update));
        object.add("renderer", module_to_json(emitter.renderer));
        emitters.items.push_back(std::move(object));
    }
    return detail::write_json(root);
}

Result<Effect> effect_from_json(std::string_view json) {
    detail::JsonError syntax;
    std::optional<Json> root = detail::parse_json(json, syntax);
    if (!root) {
        return Error{ErrorCode::MalformedPackage, std::string{effect_entry} + ": " + syntax.message + " at byte " +
                                                      std::to_string(syntax.offset)};
    }
    Reader reader;
    if (root->type != Json::Type::Object) {
        return Error{ErrorCode::MalformedPackage, std::string{effect_entry} + ": must be an object"};
    }
    const Json* format = root->find("format");
    std::uint32_t version = 0U;
    if (format == nullptr || !reader.read("format", *format, version)) {
        return Error{ErrorCode::MalformedPackage, std::string{effect_entry} + ": has no format version"};
    }
    if (version != effect_format_version) {
        return Error{ErrorCode::UnsupportedVersion, std::string{effect_entry} + ": format " + std::to_string(version) +
                                                        " is not supported; this library reads format " +
                                                        std::to_string(effect_format_version)};
    }
    for (const auto& [name, value] : root->members) {
        if (name != "format" && name != "name" && name != "simulation_rate" && name != "parameters" &&
            name != "emitters") {
            reader.fail("", "has unknown field '" + name + "'");
        }
    }

    Effect effect;
    if (const Json* name = root->find("name")) {
        reader.read("name", *name, effect.name);
    }
    if (const Json* rate = root->find("simulation_rate")) {
        reader.read("simulation_rate", *rate, effect.simulation_rate);
    }
    if (const Json* parameters = root->find("parameters")) {
        if (parameters->type != Json::Type::Array) {
            reader.fail("parameters", "must be an array");
        } else {
            for (std::size_t i = 0U; i < parameters->items.size() && !reader.failed(); ++i) {
                reader.read_parameter("parameters[" + std::to_string(i) + "]", parameters->items[i],
                                      effect.parameters.emplace_back());
            }
        }
    }
    if (const Json* emitters = root->find("emitters")) {
        if (emitters->type != Json::Type::Array) {
            reader.fail("emitters", "must be an array");
        } else {
            for (std::size_t i = 0U; i < emitters->items.size() && !reader.failed(); ++i) {
                reader.read_emitter("emitters[" + std::to_string(i) + "]", emitters->items[i],
                                    effect.emitters.emplace_back());
            }
        }
    }
    if (reader.failed()) {
        return reader.error();
    }
    Result<bool> valid = validate(effect);
    if (!valid) {
        return valid.error();
    }
    return effect;
}

std::vector<std::uint8_t> write_container(std::vector<PackageEntry> entries) {
    std::sort(entries.begin(), entries.end(),
              [](const PackageEntry& a, const PackageEntry& b) { return a.name < b.name; });
    std::vector<std::uint8_t> out(std::begin(magic), std::end(magic));
    put_u32(out, package_container_version);
    put_u32(out, static_cast<std::uint32_t>(entries.size()));
    for (const PackageEntry& entry : entries) {
        put_u32(out, static_cast<std::uint32_t>(entry.name.size()));
        out.insert(out.end(), entry.name.begin(), entry.name.end());
        put_u64(out, entry.data.size());
        out.insert(out.end(), entry.data.begin(), entry.data.end());
    }
    put_u64(out, fnv1a(out));
    return out;
}

Result<std::vector<PackageEntry>> read_container(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < sizeof(magic) + 4U + 4U + 8U || !std::equal(std::begin(magic), std::end(magic), bytes.begin())) {
        return malformed("is not an ajfx package");
    }
    ByteReader trailer{bytes.last(8U)};
    std::uint64_t checksum = 0U;
    (void)trailer.u64(checksum);
    const std::span<const std::uint8_t> body = bytes.first(bytes.size() - 8U);
    if (fnv1a(body) != checksum) {
        return malformed("is damaged: its checksum does not match");
    }

    ByteReader reader{body.subspan(sizeof(magic))};
    std::uint32_t version = 0U;
    std::uint32_t count = 0U;
    (void)reader.u32(version);
    if (version != package_container_version) {
        return Error{ErrorCode::UnsupportedVersion, "package container version " + std::to_string(version) +
                                                        " is not supported; this library reads version " +
                                                        std::to_string(package_container_version)};
    }
    if (!reader.u32(count) || count > max_entries) {
        return malformed("has an invalid entry count");
    }
    std::vector<PackageEntry> entries;
    std::set<std::string> names;
    for (std::uint32_t i = 0U; i < count; ++i) {
        std::uint32_t name_length = 0U;
        std::uint64_t data_length = 0U;
        std::span<const std::uint8_t> name;
        std::span<const std::uint8_t> data;
        if (!reader.u32(name_length) || name_length == 0U || name_length > max_name_length ||
            !reader.take(name_length, name) || !reader.u64(data_length) || data_length > reader.remaining() ||
            !reader.take(static_cast<std::size_t>(data_length), data)) {
            return malformed("is truncated or has a malformed entry");
        }
        PackageEntry& entry = entries.emplace_back();
        entry.name.assign(name.begin(), name.end());
        entry.data.assign(data.begin(), data.end());
        if (!names.insert(entry.name).second) {
            return malformed("has entry '" + entry.name + "' twice");
        }
    }
    if (reader.remaining() != 0U) {
        return malformed("has bytes after its last entry");
    }
    return entries;
}

Result<std::vector<std::uint8_t>> write_package(const Effect& effect, std::vector<PackageEntry> extra) {
    Result<bool> valid = validate(effect);
    if (!valid) {
        return valid.error();
    }
    std::set<std::string_view> names;
    for (const PackageEntry& entry : extra) {
        if (entry.name.empty() || entry.name == effect_entry || !names.insert(entry.name).second) {
            return malformed("entry '" + entry.name + "' is empty, reserved or repeated");
        }
    }
    const std::string json = effect_to_json(effect);
    extra.push_back({std::string{effect_entry}, std::vector<std::uint8_t>(json.begin(), json.end())});
    return write_container(std::move(extra));
}

Result<Effect> read_package(std::span<const std::uint8_t> bytes) {
    Result<std::vector<PackageEntry>> entries = read_container(bytes);
    if (!entries) {
        return entries.error();
    }
    for (const PackageEntry& entry : entries.value()) {
        if (entry.name == effect_entry) {
            return effect_from_json(
                std::string_view{reinterpret_cast<const char*>(entry.data.data()), entry.data.size()});
        }
    }
    return malformed("has no effect.json");
}

} // namespace ajfx
