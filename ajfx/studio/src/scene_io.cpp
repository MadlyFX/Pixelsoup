#include "scene_io.hpp"
#include "scene_properties.hpp"

#include <algorithm>
#include <array>
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
    if (p.control.type != ControlType::Vec3 && p.control.type != ControlType::Color &&
        p.control.type != ControlType::Range) {
        return numeric(v.x);
    }
    Json array = Json::array();
    array.push(numeric(v.x));
    array.push(numeric(v.y));
    if (p.control.type == ControlType::Range) {
        return array;
    }
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
    } else if (p.control.type == ControlType::Vec3 || p.control.type == ControlType::Color ||
               p.control.type == ControlType::Range) {
        const std::size_t n = p.control.type == ControlType::Color ? 4U : p.control.type == ControlType::Range ? 2U : 3U;
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
Json numbers(std::initializer_list<float> values) {
    Json array = Json::array();
    for (const float v : values) {
        array.push(numeric(v));
    }
    return array;
}
// An array of exactly N finite numbers.
template <std::size_t N> bool numbers(const Json *in, std::array<float, N> &out) {
    if (!in || in->type != Json::Type::Array || in->items.size() != N) {
        return false;
    }
    for (std::size_t k = 0U; k < N; ++k) {
        const Json &v = in->items[k];
        if (v.type != Json::Type::Number || !std::isfinite(v.number)) {
            return false;
        }
        out[k] = static_cast<float>(v.number);
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
    root.add("version", numeric(2));
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
                                 : std::holds_alternative<Force>(item.data)          ? "force"
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
            Json motion = Json::array();
            for (const MotionBehavior &b : system->motion) {
                Json m = Json::object();
                m.add("uid", numeric(b.uid));
                m.add("kind", numeric(static_cast<int>(b.kind)));
                m.add("enabled", Json::of(b.enabled));
                m.add("strength", numbers({b.strength.min, b.strength.max}));
                m.add("heading", numeric(b.heading));
                m.add("reach", numeric(b.reach));
                m.add("seed", numeric(b.seed));
                m.add("size", numeric(b.size));
                m.add("churn", numeric(b.churn));
                m.add("center", numbers({b.center.x, b.center.y, b.center.z}));
                m.add("axis", numeric(static_cast<int>(b.axis)));
                motion.push(std::move(m));
            }
            out.add("motion", std::move(motion));
            Json over_age = Json::object();
            over_age.add("size_driver", numeric(static_cast<int>(system->size_driver)));
            Json size_keys = Json::array();
            for (const CurveKey &key : system->size_over_age) {
                size_keys.push(numbers({key.t, key.value}));
            }
            over_age.add("size", std::move(size_keys));
            over_age.add("tint_driver", numeric(static_cast<int>(system->tint_driver)));
            Json tint_keys = Json::array();
            for (const ColorKey &key : system->tint_over_age) {
                tint_keys.push(numbers({key.t, key.value.r, key.value.g, key.value.b, key.value.a}));
            }
            over_age.add("tint", std::move(tint_keys));
            out.add("over_age", std::move(over_age));
        }
        if (const auto *force = std::get_if<Force>(&item.data)) {
            Json targets = Json::array();
            for (const std::uint32_t target : force->targets) {
                targets.push(numeric(target));
            }
            out.add("targets", std::move(targets));
        }
        items.push(std::move(out));
    }
    root.add("items", std::move(items));
    Json rules = Json::array();
    for (const Relationship &r : document.scene->relationships) {
        Json rule = Json::object();
        rule.add("uid", numeric(r.uid));
        rule.add("kind", numeric(static_cast<int>(r.kind)));
        rule.add("object", numeric(r.object));
        rule.add("system", numeric(r.system));
        rule.add("hardness", numeric(r.hardness));
        rule.add("per_hit", numeric(r.per_hit));
        rule.add("per_speed", numeric(r.per_speed));
        rule.add("spread", numeric(r.spread));
        rule.add("min_speed", numeric(r.min_speed));
        rule.add("rate", numeric(r.rate));
        rule.add("pieces", Json::of(r.pieces));
        rule.add("seed", numeric(r.seed));
        rules.push(std::move(rule));
    }
    root.add("relationships", std::move(rules));
    return json::write(root);
}

Loaded load_scene(const Json &root) {
    const auto fail = [](std::string reason) { return Loaded{{}, "Scene: " + std::move(reason)}; };
    if (!keys(root, {"format", "version", "name", "next_uid", "range_seconds", "scale", "physics", "items",
                     "relationships"})) {
        return fail("unknown or duplicate document field");
    }
    if (root.number_or("version", 0) != 2) {
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
        if (!keys(in, {"uid", "parent", "name", "seed", "simulate", "type", "properties", "sources", "motion",
                       "over_age", "targets"})) {
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
        } else if (type == "force") {
            item.data = Force{};
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
            const Json *motion = in.find("motion");
            const Json *over_age = in.find("over_age");
            if (!motion || motion->type != Json::Type::Array || !over_age ||
                !keys(*over_age, {"size_driver", "size", "tint_driver", "tint"})) {
                return fail(item.name + ": missing motion stack or appearance over age");
            }
            for (const Json &m : motion->items) {
                MotionBehavior b;
                std::uint32_t kind{};
                std::uint32_t axis{};
                std::array<float, 3> center{};
                std::array<float, 2> strength{};
                if (!keys(m, {"uid", "kind", "enabled", "strength", "size", "churn", "center", "axis", "heading",
                              "reach", "seed"}) ||
                    !integer(m, "uid", b.uid) || !integer(m, "kind", kind) || kind > 5U ||
                    !integer(m, "axis", axis) || axis > 1U || !integer(m, "seed", b.seed) ||
                    !has_type(m, "enabled", Json::Type::Bool) || !numbers(m.find("strength"), strength) ||
                    !has_type(m, "size", Json::Type::Number) || !has_type(m, "churn", Json::Type::Number) ||
                    !has_type(m, "heading", Json::Type::Number) || !has_type(m, "reach", Json::Type::Number) ||
                    !numbers(m.find("center"), center)) {
                    return fail(item.name + ": invalid motion behavior");
                }
                b.kind = static_cast<BehaviorKind>(kind);
                b.axis = static_cast<VortexAxis>(axis);
                b.enabled = m.bool_or("enabled", true);
                b.strength = {strength[0], strength[1]};
                b.heading = static_cast<float>(m.number_or("heading", 0));
                b.reach = static_cast<float>(m.number_or("reach", 0));
                b.size = static_cast<float>(m.number_or("size", 0));
                b.churn = static_cast<float>(m.number_or("churn", 0));
                b.center = {center[0], center[1], center[2]};
                system->motion.push_back(b);
            }
            std::uint32_t driver{};
            if (!integer(*over_age, "size_driver", driver) || driver > 1U) {
                return fail(item.name + ": invalid size driver");
            }
            system->size_driver = static_cast<Driver>(driver);
            if (!integer(*over_age, "tint_driver", driver) || driver > 1U) {
                return fail(item.name + ": invalid color driver");
            }
            system->tint_driver = static_cast<Driver>(driver);
            const Json *size_keys = over_age->find("size");
            const Json *tint_keys = over_age->find("tint");
            if (!size_keys || size_keys->type != Json::Type::Array || !tint_keys ||
                tint_keys->type != Json::Type::Array) {
                return fail(item.name + ": invalid keys over age");
            }
            system->size_over_age.clear();
            for (const Json &k : size_keys->items) {
                std::array<float, 2> key{};
                if (!numbers(&k, key)) {
                    return fail(item.name + ": invalid size key");
                }
                system->size_over_age.push_back(CurveKey{key[0], key[1]});
            }
            system->tint_over_age.clear();
            for (const Json &k : tint_keys->items) {
                std::array<float, 5> key{};
                if (!numbers(&k, key)) {
                    return fail(item.name + ": invalid color key");
                }
                system->tint_over_age.push_back(ColorKey{key[0], Color{key[1], key[2], key[3], key[4]}});
            }
        } else if (in.find("sources") || in.find("motion") || in.find("over_age")) {
            return fail(item.name + ": sources, motion and appearance over age belong to particle systems");
        }
        if (auto *force = std::get_if<Force>(&item.data)) {
            const Json *targets = in.find("targets");
            if (!targets || targets->type != Json::Type::Array) {
                return fail(item.name + ": missing targets");
            }
            for (const Json &t : targets->items) {
                if (t.type != Json::Type::Number || !std::isfinite(t.number) || t.number < 1.0 ||
                    std::floor(t.number) != t.number || t.number > std::numeric_limits<std::uint32_t>::max()) {
                    return fail(item.name + ": invalid target");
                }
                force->targets.push_back(static_cast<std::uint32_t>(t.number));
            }
        } else if (in.find("targets")) {
            return fail(item.name + ": targets belong to forces");
        }
        scene.items.push_back(std::move(item));
    }
    const Json *rules = root.find("relationships");
    if (!rules || rules->type != Json::Type::Array) {
        return fail("missing relationships");
    }
    for (const Json &rule : rules->items) {
        Relationship r;
        std::uint32_t kind{};
        if (!keys(rule, {"uid", "kind", "object", "system", "hardness", "per_hit", "per_speed", "spread", "min_speed",
                         "rate", "pieces", "seed"}) ||
            !integer(rule, "uid", r.uid) || !integer(rule, "kind", kind) || kind > 1U ||
            !integer(rule, "object", r.object) || !integer(rule, "system", r.system) || !integer(rule, "seed", r.seed) ||
            !has_type(rule, "pieces", Json::Type::Bool)) {
            return fail("invalid relationship");
        }
        for (const std::string_view key : {"hardness", "per_hit", "per_speed", "spread", "min_speed", "rate"}) {
            if (!has_type(rule, key, Json::Type::Number)) {
                return fail("invalid relationship");
            }
        }
        r.kind = static_cast<RelationKind>(kind);
        r.hardness = static_cast<float>(rule.number_or("hardness", 0));
        r.per_hit = static_cast<float>(rule.number_or("per_hit", 0));
        r.per_speed = static_cast<float>(rule.number_or("per_speed", 0));
        r.spread = static_cast<float>(rule.number_or("spread", 0));
        r.min_speed = static_cast<float>(rule.number_or("min_speed", 0));
        r.rate = static_cast<float>(rule.number_or("rate", 0));
        r.pieces = rule.bool_or("pieces", false);
        scene.relationships.push_back(r);
    }
    if (const std::string error = validate_scene(document, false); !error.empty()) {
        return fail(error);
    }
    return Loaded{std::move(document), {}};
}
} // namespace ajfx::studio
