#include "scene.hpp"

#include "catalog.hpp"
#include "compile.hpp"
#include "document.hpp"
#include "scene_properties.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <set>

namespace ajfx::studio {
namespace {
constexpr Uid component_stride = 32U;
constexpr Uid max_scene_uid = 0x07ffffc0U;

Uid reserve(Document &document) {
    if (document.next_uid == 0U || document.next_uid > max_scene_uid) {
        return 0U;
    }
    const Uid uid = document.next_uid;
    document.next_uid += component_stride;
    return uid;
}

Block block(Uid uid, std::string kind, std::initializer_list<std::pair<std::string_view, Value>> values = {}) {
    Block out;
    out.uid = uid;
    out.kind = std::move(kind);
    for (const auto &[key, value] : values) {
        out.set(key, value);
    }
    return out;
}

Value value(Vec3 v) { return vec3(v.x, v.y, v.z); }
float length(Vec3 v) { return std::sqrt(dot(v, v)); }
void launch(Layer &layer, Uid uid, Vec3 v, float spread = 0.0F) {
    const float speed = length(v);
    layer.blocks.push_back(block(uid, "launch",
                                 {{"direction", choice("custom")},
                                  {"custom", value(v)},
                                  {"speed", range(speed, speed)},
                                  {"spread", range(0.0F, spread)}}));
}
} // namespace

SceneItem *Scene::item(std::uint32_t uid) {
    const auto found = std::find_if(items.begin(), items.end(), [uid](const SceneItem &i) { return i.uid == uid; });
    return found == items.end() ? nullptr : &*found;
}
const SceneItem *Scene::item(std::uint32_t uid) const {
    const auto found = std::find_if(items.begin(), items.end(), [uid](const SceneItem &i) { return i.uid == uid; });
    return found == items.end() ? nullptr : &*found;
}

Document empty_scene() {
    Document document;
    document.name = "Untitled scene";
    document.scene.emplace();
    document.loop_auto = false;
    document.world.ground = false;
    document.backdrop = Backdrop::Studio;
    return document;
}

Document new_scene() {
    Document document = empty_scene();
    static_cast<void>(add_floor(document));
    return document;
}

Uid add_floor(Document &document) {
    const Uid uid = add_object(document, "floor");
    if (uid == 0U) {
        return 0U;
    }
    SceneItem &item = *document.scene->item(uid);
    item.name = "Ground";
    Object &floor = std::get<Object>(item.data);
    // The built-in floor is 1 x 0.01 x 1. At 20 m its top is exactly Y=0.
    floor.size = 20.0F;
    floor.position = {0.0F, -0.1F, 0.0F};
    floor.motion = ObjectMotion::Static;
    floor.collider = ObjectCollider::Hull;
    floor.restitution = 0.0F;
    return uid;
}

Uid add_object(Document &document, std::string mesh) {
    if (!document.scene) {
        return 0U;
    }
    const Uid uid = reserve(document);
    if (uid == 0U) {
        return 0U;
    }
    SceneItem item;
    item.uid = uid;
    item.name = mesh == "cube" ? "Object" : std::filesystem::path{mesh}.stem().string();
    item.seed = uid;
    item.data = Object{.mesh = std::move(mesh)};
    document.scene->items.push_back(std::move(item));
    return uid;
}
Uid add_particle_system(Document &document) {
    if (!document.scene) {
        return 0U;
    }
    const Uid uid = reserve(document);
    const Uid source = reserve(document);
    if (uid == 0U || source == 0U) {
        return 0U;
    }
    SceneItem item;
    item.uid = uid;
    item.name = "Particle system";
    item.seed = uid;
    ParticleSystem system;
    system.sources.push_back(ParticleSource{.uid = source});
    item.data = std::move(system);
    document.scene->items.push_back(std::move(item));
    return uid;
}
Uid add_group(Document &document) {
    if (!document.scene) {
        return 0U;
    }
    const Uid uid = reserve(document);
    if (uid == 0U) {
        return 0U;
    }
    document.scene->items.push_back(SceneItem{.uid = uid, .name = "Group", .data = Group{}});
    return uid;
}
Uid add_source(Document &document, Uid uid, SourceMode mode) {
    SceneItem *item = document.scene ? document.scene->item(uid) : nullptr;
    auto *system = item ? std::get_if<ParticleSystem>(&item->data) : nullptr;
    if (!system || system->sources.size() >= 32U || (system->persistent && mode == SourceMode::Continuous)) {
        return 0U;
    }
    const Uid fresh = reserve(document);
    if (fresh == 0U) {
        return 0U;
    }
    system->sources.push_back(ParticleSource{.uid = fresh, .mode = mode});
    return fresh;
}
bool remove_item(Document &document, Uid uid) {
    if (!document.scene || document.scene->item(uid) == nullptr) {
        return false;
    }
    // Removing an organizational group leaves its contents at the same poses.
    const Uid parent = document.scene->item(uid)->parent;
    for (SceneItem &item : document.scene->items) {
        if (item.parent == uid) {
            item.parent = parent;
        }
    }
    std::erase_if(document.scene->items, [uid](const SceneItem &i) { return i.uid == uid; });
    return true;
}
bool group_item(Document &document, Uid uid, Uid parent) {
    if (!document.scene) {
        return false;
    }
    SceneItem *item = document.scene->item(uid);
    if (item == nullptr || uid == parent) {
        return false;
    }
    for (Uid at = parent; at != 0U;) {
        const SceneItem *group = document.scene->item(at);
        if (group == nullptr || !std::holds_alternative<Group>(group->data) || at == uid) {
            return false;
        }
        at = group->parent;
    }
    item->parent = parent;
    return true;
}
Uid duplicate_item(Document &document, Uid uid) {
    if (!document.scene || document.scene->item(uid) == nullptr) {
        return 0U;
    }
    SceneItem copy = *document.scene->item(uid);
    const Uid fresh = reserve(document);
    if (fresh == 0U) {
        return 0U;
    }
    copy.uid = fresh;
    copy.name += " copy";
    copy.seed = fresh;
    if (auto *system = std::get_if<ParticleSystem>(&copy.data)) {
        for (ParticleSource &source : system->sources) {
            source.uid = reserve(document);
            if (source.uid == 0U) {
                return 0U;
            }
        }
    }
    document.scene->items.push_back(std::move(copy));
    return fresh;
}

std::string validate_scene(const Document &document, bool for_playback) {
    if (!document.scene) {
        return "This document is not a scene";
    }
    if (!document.layers.empty()) {
        return "Scene: legacy layers cannot be a second authored owner";
    }
    if (document.next_uid == 0U || document.next_uid > max_scene_uid + component_stride) {
        return "Scene: invalid identity allocator";
    }
    const Scene &scene = *document.scene;
    if (!std::isfinite(scene.physics.gravity) || scene.physics.gravity < 0 || scene.physics.gravity > 10 ||
        !std::isfinite(scene.physics.angular_damping) || scene.physics.angular_damping < 0 ||
        scene.physics.angular_damping > 1) {
        return "Scene / Simulation: invalid gravity or angular damping";
    }
    if (for_playback && scene.items.size() > 4096U) {
        return "Scene: item capacity exceeded (4096)";
    }
    std::set<Uid> used;
    const auto identity = [&](Uid uid) {
        if (uid == 0U || uid > max_scene_uid || uid + component_stride > document.next_uid) {
            return false;
        }
        for (Uid k = uid; k < uid + component_stride; ++k) {
            if (!used.insert(k).second) {
                return false;
            }
        }
        return true;
    };
    for (const SceneItem &item : scene.items) {
        if (const std::string error = validate_scene_properties(item); !error.empty()) {
            return error;
        }
        const std::string where = item.name + ": ";
        if (!identity(item.uid)) {
            return where + "invalid or overlapping item identity";
        }
        std::set<Uid> parents{item.uid};
        for (Uid at = item.parent; at != 0U;) {
            const SceneItem *parent = scene.item(at);
            if (parent == nullptr || !std::holds_alternative<Group>(parent->data) || !parents.insert(at).second) {
                return where + "missing group or organizational cycle";
            }
            at = parent->parent;
            if (parents.size() > 65U) {
                return where + "group nesting exceeds the supported 64 levels";
            }
        }
        if (const auto *object = std::get_if<Object>(&item.data)) {
            if (for_playback && object->mesh.empty()) {
                return where + "Geometry: choose a model";
            }
            if (for_playback && object->motion != ObjectMotion::Dynamic && object->fracture) {
                return where + "Fracture requires Dynamic motion";
            }
            if (for_playback && object->motion != ObjectMotion::Dynamic &&
                object->distribution != Distribution::Single) {
                return where + "repeated Static or Animated objects are not supported by this target";
            }
        } else if (const auto *system = std::get_if<ParticleSystem>(&item.data)) {
            if (for_playback && system->sources.empty()) {
                return where + "Initial state / Sources: add a source";
            }
            if (for_playback && system->sources.size() > 32U) {
                return where + "source capacity exceeded (32)";
            }
            for (const ParticleSource &source : system->sources) {
                if (source.count == 0U || source.count > 1000000U || !std::isfinite(source.rate) || source.rate < 0 ||
                    source.rate > 1000000 || !std::isfinite(source.time) || source.time < 0 || source.time > 3600) {
                    return where + "invalid source count, rate or time";
                }
                if (!identity(source.uid)) {
                    return where + "invalid or overlapping source identity";
                }
                if (for_playback && system->persistent && source.mode == SourceMode::Continuous) {
                    return where + "persistent populations require bounded Initial or Burst sources";
                }
            }
        }
    }
    return {};
}

Document lower_scene(const Document &document) {
    if (!document.scene) {
        return document;
    }
    Document out = document;
    out.scene.reset();
    out.layers.clear();
    out.world = PageWorld{.ground = false, .gravity = document.scene->physics.gravity};
    std::vector<const SceneItem *> ordered;
    for (const SceneItem &item : document.scene->items) {
        ordered.push_back(&item);
    }
    std::sort(ordered.begin(), ordered.end(), [](const SceneItem *a, const SceneItem *b) { return a->uid < b->uid; });
    for (const SceneItem *item : ordered) {
        if (std::holds_alternative<Group>(item->data)) {
            continue;
        }
        Layer layer;
        layer.uid = item->uid;
        // Names and presentation order never change runtime identity.
        layer.name = "item_" + std::to_string(item->uid);
        layer.enabled = item->simulate;
        layer.seed = item->seed;
        layer.baked = true;
        if (const auto *object = std::get_if<Object>(&item->data)) {
            layer.kind = LayerKind::Rigid;
            layer.seed = 1U; // one explicit shared world; per-object streams use their own seed below
            layer.what = LayerWhat::Solid;
            // The largest finite lifetime is the runtime's persistent encoding.
            // Only bounded sources use it, so capacity is finite and exact.
            layer.life_min = layer.life_max = std::numeric_limits<float>::max();
            layer.world = block(item->uid + 1U, "physics",
                                {{"density", number(object->density)},
                                 {"friction", number(object->friction)},
                                 {"bounce", number(object->restitution)},
                                 {"damping", number(document.scene->physics.angular_damping)},
                                 {"rest", toggle(document.scene->physics.allow_sleep)}});
            const bool file =
                object->mesh != "cube" && object->mesh != "slab" && object->mesh != "rock" && object->mesh != "shard" &&
                object->mesh != "floor";
            layer.blocks.push_back(block(item->uid + 2U, "object",
                                         {{"seed", number(static_cast<float>(item->seed))},
                                          {"model", choice(file ? "file" : object->mesh)},
                                          {"model_file", choice(file ? object->mesh : "")},
                                          {"at", value(object->position)},
                                          {"turn", value(object->rotation)},
                                          {"size", number(object->size)},
                                          {"motion", choice(object->motion == ObjectMotion::Static     ? "fixed"
                                                            : object->motion == ObjectMotion::Animated ? "driven"
                                                                                                       : "loose")},
                                          {"collides", choice(object->collider == ObjectCollider::Sphere ? "ball"
                                                              : object->collider == ObjectCollider::Box  ? "block"
                                                                                                         : "own")},
                                          {"radius", number(object->size * 0.5F)},
                                          {"extent", vec3(object->size, object->size, object->size)},
                                          {"own_material", toggle(true)},
                                          {"density", number(object->density)},
                                          {"friction", number(object->friction)},
                                          {"bounce", number(object->restitution)},
                                          {"release", number(object->release)},
                                          {"move_to", value(object->move_to)},
                                          {"turn_to", value(object->turn_to)},
                                          {"move_at", number(object->move_at)},
                                          {"move_over", number(object->move_over)},
                                          {"arrange", choice(object->distribution == Distribution::Single ? "one"
                                                             : object->distribution == Distribution::Grid ? "stack"
                                                                                                          : "heap")},
                                          {"count", number(static_cast<float>(object->count))},
                                          {"width", number(object->width)},
                                          {"depth", number(object->depth)},
                                          {"spacing", number(object->spacing)},
                                          {"jitter", number(object->variation)}}));
            if (object->fracture) {
                layer.blocks.push_back(block(item->uid + 11U, "break",
                                             {{"into", choice("cut")},
                                              {"cells", number(static_cast<float>(object->fragments))},
                                              {"force", number(object->fracture_speed)},
                                              {"cut_seed", number(static_cast<float>(item->seed))}}));
            }
            layer.blocks.push_back(
                block(item->uid + 4U, "appearance", {{"material", choice("model")}, {"model", choice("cube")}}));
            launch(layer, item->uid + 6U, object->velocity);
        } else if (const auto *system = std::get_if<ParticleSystem>(&item->data)) {
            layer.life_min = layer.life_max = system->persistent ? std::numeric_limits<float>::max() : system->lifetime;
            std::vector<ParticleSource> sources = system->sources;
            std::sort(sources.begin(), sources.end(),
                      [](const ParticleSource &a, const ParticleSource &b) { return a.uid < b.uid; });
            for (const ParticleSource &source : sources) {
                layer.blocks.push_back(
                    source.mode == SourceMode::Continuous
                        ? block(source.uid, "stream", {{"rate", number(source.rate)}})
                        : block(source.uid, "burst",
                                {{"count", number(static_cast<float>(source.count))},
                                 {"time", number(source.mode == SourceMode::Initial ? 0.0F : source.time)}}));
            }
            layer.blocks.push_back(block(item->uid + 7U, "shape",
                                         {{"shape", choice("ball")},
                                          {"offset", value(system->position)},
                                          {"size", range(0.0F, system->radius)}}));
            launch(layer, item->uid + 6U, system->velocity, system->spread);
            layer.blocks.push_back(
                block(item->uid + 8U, "gravity", {{"strength", range(system->gravity, system->gravity)}}));
            layer.blocks.push_back(block(item->uid + 9U, "drag", {{"amount", range(system->drag, system->drag)}}));
            if (system->turbulence > 0.0F) {
                layer.blocks.push_back(block(item->uid + 10U, "turbulence",
                                             {{"strength", range(system->turbulence, system->turbulence)}}));
            }
            layer.blocks.push_back(
                block(item->uid + 4U, "appearance",
                      {{"material", choice(system->look == ParticleLook::Glow    ? "glow"
                                           : system->look == ParticleLook::Smoke ? "smoke"
                                                                                 : "solid")},
                       {"tint", color(system->tint.r, system->tint.g, system->tint.b, system->tint.a)}}));
            layer.blocks.push_back(block(item->uid + 3U, "color", {{"color", color(1.0F, 1.0F, 1.0F)}}));
            layer.blocks.push_back(block(item->uid + 5U, "size", {{"size", range(system->size, system->size)}}));
            if (!system->collide) {
                layer.blocks.push_back(block(item->uid + 12U, "passes_through"));
            }
        }
        out.layers.push_back(std::move(layer));
    }
    return out;
}

bool same_scene_motion(const Document &a, const Document &b) {
    if (!a.scene || !b.scene || a.scale != b.scale || a.scene->physics != b.scene->physics) {
        return false;
    }
    const auto count = [](const Scene &scene) {
        return std::count_if(scene.items.begin(), scene.items.end(),
                             [](const SceneItem &item) { return !std::holds_alternative<Group>(item.data); });
    };
    if (count(*a.scene) != count(*b.scene)) {
        return false;
    }
    for (const SceneItem &before : a.scene->items) {
        if (std::holds_alternative<Group>(before.data)) {
            continue;
        }
        const SceneItem *after = b.scene->item(before.uid);
        if (!after || before.seed != after->seed || before.simulate != after->simulate ||
            before.data.index() != after->data.index()) {
            return false;
        }
        for (const SceneProperty &property : scene_properties(before)) {
            if (property.control.timing == ParameterTiming::Bake && property.read(before) != property.read(*after)) {
                return false;
            }
        }
        if (const auto *system = std::get_if<ParticleSystem>(&before.data)) {
            std::vector<ParticleSource> one = system->sources;
            std::vector<ParticleSource> two = std::get<ParticleSystem>(after->data).sources;
            const auto order = [](const ParticleSource &x, const ParticleSource &y) { return x.uid < y.uid; };
            std::sort(one.begin(), one.end(), order);
            std::sort(two.begin(), two.end(), order);
            if (one != two) {
                return false;
            }
        }
    }
    return true;
}

Compiled compile_scene(const Document &document, Purpose purpose) {
    if (const std::string error = validate_scene(document); !error.empty()) {
        Compiled failed;
        failed.error = error;
        return failed;
    }
    Compiled out = compile(lower_scene(document), purpose);
    if (!out.error.empty()) {
        for (const LayerReport &report : out.layers) {
            if (!report.error.empty()) {
                if (const SceneItem *item = document.scene->item(report.layer)) {
                    out.error = item->name + ": " + report.error;
                }
                break;
            }
        }
        // Never replace a verified preview with a plausible incomplete scene.
        out.effect.reset();
        return out;
    }
    // A shared solver state feeds per-kind draw assignments. The assignments
    // are draw-time data, so tint edits preserve the world's motion cache.
    if (out.effect) {
        std::size_t prop_index = 0U;
        for (const LayerReport &report : out.layers) {
            const SceneItem *item = document.scene->item(report.layer);
            const Object *object = item ? std::get_if<Object>(&item->data) : nullptr;
            if (!object || !report.included) {
                continue;
            }
            for (Emitter &emitter : out.effect->emitters) {
                if (emitter.name != report.emitter) {
                    continue;
                }
                for (SpawnModule &module : emitter.spawn) {
                    if (auto *pile = std::get_if<SpawnPile>(&module);
                        pile && (report.scope.empty() || std::find(report.scope.kinds.begin(), report.scope.kinds.end(),
                                                                   pile->body) != report.scope.kinds.end())) {
                        pile->initial_velocity = object->velocity * document.scale;
                        pile->turn = object->rotation;
                    }
                }
                for (UpdateModule &module : emitter.update) {
                    auto *physics = std::get_if<RigidPhysics>(&module);
                    if (!physics) {
                        continue;
                    }
                    for (std::size_t k = 0U; k < physics->bodies.size(); ++k) {
                        if (report.scope.empty() || std::find(report.scope.kinds.begin(), report.scope.kinds.end(),
                                                              k + 1U) != report.scope.kinds.end()) {
                            physics->bodies[k].draw_tint = object->tint;
                        }
                    }
                    for (std::size_t p = 0U; p < report.props.size() && prop_index < physics->props.size(); ++p) {
                        RigidProp &prop = physics->props[prop_index++];
                        prop.draw_tint = object->tint;
                        prop.shape = object->collider == ObjectCollider::Hull  ? PropShape::Hull
                                     : object->collider == ObjectCollider::Box ? PropShape::Box
                                                                               : PropShape::Sphere;
                    }
                }
            }
        }
    }
    return out;
}
} // namespace ajfx::studio
