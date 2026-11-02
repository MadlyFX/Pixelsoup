#include "scene_io.hpp"
#include "scene_properties.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace ajfx::studio {
namespace {
using Json = json::Value;
Json numeric(double v) { return Json::of(v); }
Json encode(const SceneProperty &p, const SceneItem &item) {
    const Value v = p.read(item);
    if (p.control.type == ControlType::File) {
        return Json::of(v.text);
    }
    if (p.control.type == ControlType::Toggle) {
        return Json::of(v.x != 0.0F);
    }
    if (p.control.type != ControlType::Vec3 && p.control.type != ControlType::Color) {
        return numeric(v.x);
    }
    Json array = Json::array();
    array.push(numeric(v.x));
    array.push(numeric(v.y));
    array.push(numeric(v.z));
    if (p.control.type == ControlType::Color) {
        array.push(numeric(v.w));
    }
    return array;
}
bool decode(const SceneProperty &p, const Json &in, SceneItem &item) {
    Value v;
    if (p.control.type == ControlType::File) {
        if (in.type != Json::Type::String) {
            return false;
        }
        v.text = in.text;
    } else if (p.control.type == ControlType::Toggle) {
        if (in.type != Json::Type::Bool) {
            return false;
        }
        v.x = in.boolean ? 1.0F : 0.0F;
    } else if (p.control.type == ControlType::Vec3 || p.control.type == ControlType::Color) {
        const std::size_t n = p.control.type == ControlType::Color ? 4U : 3U;
        if (in.type != Json::Type::Array || in.items.size() != n) {
            return false;
        }
        float *components[] = {&v.x, &v.y, &v.z, &v.w};
        for (std::size_t k = 0; k < n; ++k) {
            if (in.items[k].type != Json::Type::Number) {
                return false;
            }
            *components[k] = static_cast<float>(in.items[k].number);
        }
    } else {
        if (in.type != Json::Type::Number || !std::isfinite(in.number) || in.number < p.control.hard_min ||
            in.number > p.control.hard_max) {
            return false;
        }
        if ((p.control.decimals == 0 || p.control.type == ControlType::Choice) && std::floor(in.number) != in.number) {
            return false;
        }
        v.x = static_cast<float>(in.number);
    }
    p.write(item, v);
    return validate_scene_properties(item).empty();
}
bool keys(const Json &v, std::initializer_list<std::string_view> allowed) {
    if (v.type != Json::Type::Object) {
        return false;
    }
    std::set<std::string_view> seen;
    for (const auto &[key, ignored] : v.members) {
        static_cast<void>(ignored);
        if (!seen.insert(key).second || std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            return false;
        }
    }
    return true;
}
bool integer(const Json &in, std::string_view key, std::uint32_t &value) {
    const Json *v = in.find(key);
    if (!v || v->type != Json::Type::Number || !std::isfinite(v->number) || v->number < 0.0 ||
        v->number > std::numeric_limits<std::uint32_t>::max() || std::floor(v->number) != v->number) {
        return false;
    }
    value = static_cast<std::uint32_t>(v->number);
    return true;
}
} // namespace

std::string save_scene(const Document &document) {
    Json root = Json::object();
    root.add("format", Json::of("ajfx-studio-scene"));
    root.add("version", numeric(1));
    root.add("name", Json::of(document.name));
    root.add("next_uid", numeric(document.next_uid));
    root.add("range_seconds", numeric(document.loop_seconds));
    root.add("scale", numeric(document.scale));
    Json physics = Json::object();
    physics.add("gravity", numeric(document.scene->physics.gravity));
    physics.add("angular_damping", numeric(document.scene->physics.angular_damping));
    physics.add("allow_sleep", Json::of(document.scene->physics.allow_sleep));
    root.add("physics", std::move(physics));
    Json items = Json::array();
    for (const SceneItem &item : document.scene->items) {
        Json out = Json::object();
        out.add("uid", numeric(item.uid));
        out.add("parent", numeric(item.parent));
        out.add("name", Json::of(item.name));
        out.add("seed", numeric(item.seed));
        out.add("simulate", Json::of(item.simulate));
        out.add("type", Json::of(std::holds_alternative<Object>(item.data)           ? "object"
                                 : std::holds_alternative<ParticleSystem>(item.data) ? "particles"
                                                                                     : "group"));
        Json properties = Json::object();
        for (const SceneProperty &p : scene_properties(item)) {
            properties.add(std::string{p.control.id}, encode(p, item));
        }
        out.add("properties", std::move(properties));
        if (const auto *system = std::get_if<ParticleSystem>(&item.data)) {
            Json sources = Json::array();
            for (const ParticleSource &source : system->sources) {
                Json s = Json::object();
                s.add("uid", numeric(source.uid));
                s.add("mode", numeric(static_cast<int>(source.mode)));
                s.add("count", numeric(source.count));
                s.add("rate", numeric(source.rate));
                s.add("time", numeric(source.time));
                sources.push(std::move(s));
            }
            out.add("sources", std::move(sources));
        }
        items.push(std::move(out));
    }
    root.add("items", std::move(items));
    return json::write(root);
}

Loaded load_scene(const Json &root) {
    const auto fail = [](std::string reason) { return Loaded{{}, "Scene: " + std::move(reason)}; };
    if (!keys(root, {"format", "version", "name", "next_uid", "range_seconds", "scale", "physics", "items"})) {
        return fail("unknown or duplicate document field");
    }
    if (root.number_or("version", 0) != 1) {
        return fail("unsupported format version");
    }
    const auto has_type = [](const Json &object, std::string_view key, Json::Type type) {
        const Json *value = object.find(key);
        return value && value->type == type;
    };
    if (!has_type(root, "name", Json::Type::String) || !has_type(root, "range_seconds", Json::Type::Number) ||
        !has_type(root, "scale", Json::Type::Number)) {
        return fail("missing or invalid name, capture range or scale");
    }
    Document document = empty_scene();
    document.name = root.string_or("name", "Untitled scene");
    if (!integer(root, "next_uid", document.next_uid)) {
        return fail("invalid next_uid");
    }
    const double range = root.number_or("range_seconds", 6);
    const double scale = root.number_or("scale", 1);
    if (!std::isfinite(range) || range < 0.5 || range > 3600 || !std::isfinite(scale) || scale < 0.01 || scale > 100) {
        return fail("invalid capture range or scale");
    }
    document.loop_seconds = static_cast<float>(range);
    document.scale = static_cast<float>(scale);
    const Json *physics = root.find("physics");
    const Json *items = root.find("items");
    if (!physics || !items || items->type != Json::Type::Array ||
        !keys(*physics, {"gravity", "angular_damping", "allow_sleep"})) {
        return fail("invalid physics settings or items");
    }
    Scene &scene = *document.scene;
    if (!has_type(*physics, "gravity", Json::Type::Number) ||
        !has_type(*physics, "angular_damping", Json::Type::Number) ||
        !has_type(*physics, "allow_sleep", Json::Type::Bool)) {
        return fail("missing or invalid simulation setting");
    }
    scene.physics.gravity = static_cast<float>(physics->number_or("gravity", 1));
    scene.physics.angular_damping = static_cast<float>(physics->number_or("angular_damping", 0.05));
    scene.physics.allow_sleep = physics->bool_or("allow_sleep", true);
    for (const Json &in : items->items) {
        if (!keys(in, {"uid", "parent", "name", "seed", "simulate", "type", "properties", "sources"})) {
            return fail("unknown or duplicate item field");
        }
        SceneItem item;
        if (!has_type(in, "name", Json::Type::String) || !has_type(in, "simulate", Json::Type::Bool)) {
            return fail("invalid item name or simulation state");
        }
        if (!integer(in, "uid", item.uid) || !integer(in, "parent", item.parent) || !integer(in, "seed", item.seed)) {
            return fail("invalid item identity");
        }
        item.name = in.string_or("name", "Item");
        item.simulate = in.bool_or("simulate", true);
        const std::string type = in.string_or("type", "");
        if (type == "object") {
            item.data = Object{};
        } else if (type == "particles") {
            item.data = ParticleSystem{};
        } else if (type == "group") {
            item.data = Group{};
        } else {
            return fail(item.name + ": unsupported item type");
        }
        const Json *properties = in.find("properties");
        if (!properties || properties->type != Json::Type::Object) {
            return fail(item.name + ": missing properties");
        }
        std::set<std::string> seen;
        for (const auto &[key, v] : properties->members) {
            const auto defs = scene_properties(item);
            const auto p =
                std::find_if(defs.begin(), defs.end(), [&key](const SceneProperty &d) { return d.control.id == key; });
            if (!seen.insert(key).second || p == defs.end() || !decode(*p, v, item)) {
                return fail(item.name + ": invalid or unsupported property " + key);
            }
        }
        if (seen.size() != scene_properties(item).size()) {
            return fail(item.name + ": missing authored property");
        }
        if (auto *system = std::get_if<ParticleSystem>(&item.data)) {
            const Json *sources = in.find("sources");
            if (!sources || sources->type != Json::Type::Array) {
                return fail(item.name + ": missing sources");
            }
            for (const Json &s : sources->items) {
                ParticleSource source;
                std::uint32_t mode{};
                if (!keys(s, {"uid", "mode", "count", "rate", "time"}) || !integer(s, "uid", source.uid) ||
                    !integer(s, "mode", mode) || mode > 2U || !integer(s, "count", source.count)) {
                    return fail(item.name + ": invalid source");
                }
                source.mode = static_cast<SourceMode>(mode);
                source.rate = static_cast<float>(s.number_or("rate", -1));
                source.time = static_cast<float>(s.number_or("time", -1));
                system->sources.push_back(source);
            }
        } else if (in.find("sources")) {
            return fail(item.name + ": sources belong to particle systems");
        }
        scene.items.push_back(std::move(item));
    }
    if (const std::string error = validate_scene(document, false); !error.empty()) {
        return fail(error);
    }
    return Loaded{std::move(document), {}};
}
} // namespace ajfx::studio
