#include "scene.hpp"

#include "catalog.hpp"
#include "compile.hpp"
#include "document.hpp"
#include "library.hpp"
#include "scene_properties.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <set>
#include <type_traits>

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

Block raw_block(Uid uid, RawModule module) {
    Block out;
    out.uid = uid;
    out.kind = std::string{block_kind_id(BlockKind::Module)};
    out.raw = std::move(module);
    return out;
}

Value value(Vec3 v) { return vec3(v.x, v.y, v.z); }
float length(Vec3 v) { return std::sqrt(dot(v, v)); }
void launch(Layer &layer, Uid uid, Vec3 direction, Range speed, float spread = 0.0F) {
    layer.blocks.push_back(block(uid, "launch",
                                 {{"direction", choice("custom")},
                                  {"custom", value(direction)},
                                  {"speed", range(speed.min, speed.max)},
                                  {"spread", range(0.0F, spread)}}));
}
void launch(Layer &layer, Uid uid, Vec3 v) {
    const float speed = length(v);
    launch(layer, uid, v, Range{speed, speed});
}
Value value(Range r) { return range(r.min, r.max); }
bool within(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }
bool within(Vec3 v, float lo, float hi) { return within(v.x, lo, hi) && within(v.y, lo, hi) && within(v.z, lo, hi); }
bool within(Range r, float lo, float hi) { return within(r.min, lo, hi) && within(r.max, lo, hi); }
std::string validate_behavior(const MotionBehavior &b) {
    if (b.seed > 1000000U) {
        return "seed is outside its supported range";
    }
    if (!within(b.center, -10000.0F, 10000.0F)) {
        return "center is outside its supported range";
    }
    switch (b.kind) {
    case BehaviorKind::Drag:
        return within(b.strength, 0.0F, 100.0F) ? "" : "amount is outside its supported range";
    case BehaviorKind::Turbulence:
        if (!within(b.strength, 0.0F, 100.0F)) {
            return "amount is outside its supported range";
        }
        if (!within(b.size, 0.01F, 1000.0F)) {
            return "swirl size is outside its supported range";
        }
        return within(b.churn, 0.0F, 100.0F) ? "" : "churn is outside its supported range";
    case BehaviorKind::Vortex:
    case BehaviorKind::Attract:
        if (!within(b.strength, -1000.0F, 1000.0F)) {
            return "strength is outside its supported range";
        }
        return within(b.reach, 0.0F, 1000.0F) ? "" : "reach is outside its supported range";
    case BehaviorKind::Wind:
        if (!within(b.strength, 0.0F, 1000.0F)) {
            return "strength is outside its supported range";
        }
        return within(b.heading, 0.0F, 360.0F) ? "" : "heading is outside its supported range";
    case BehaviorKind::DrawIn:
        if (!within(b.strength, 0.0F, 1000.0F)) {
            return "pull is outside its supported range";
        }
        return within(b.reach, 0.0F, 1000.0F) ? "" : "fade height is outside its supported range";
    }
    return "unknown behavior";
}
template <class Key> std::string validate_keys(const std::vector<Key> &keys, float lo, float hi) {
    if (keys.size() < 2U || keys.size() > max_curve_keys) {
        return "needs between 2 and 16 keys";
    }
    float last = 0.0F;
    for (const Key &key : keys) {
        if (!within(key.t, 0.0F, 1.0F) || key.t < last) {
            return "keys must run from age 0 to 1 in order";
        }
        last = key.t;
        if constexpr (std::is_same_v<Key, ColorKey>) {
            if (!within(key.value.r, lo, hi) || !within(key.value.g, lo, hi) || !within(key.value.b, lo, hi) ||
                !within(key.value.a, 0.0F, 1.0F)) {
                return "a key's colour is outside its supported range";
            }
        } else {
            if (!within(key.value, lo, hi)) {
                return "a key's value is outside its supported range";
            }
        }
    }
    return {};
}
} // namespace

std::string_view behavior_name(BehaviorKind kind) {
    switch (kind) {
    case BehaviorKind::Drag:
        return "Drag";
    case BehaviorKind::Turbulence:
        return "Turbulence";
    case BehaviorKind::Vortex:
        return "Vortex";
    case BehaviorKind::Wind:
        return "Wind";
    case BehaviorKind::Attract:
        return "Attract";
    case BehaviorKind::DrawIn:
        return "Draw in";
    }
    return "Behavior";
}

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

void rebase_models(Document &document, const std::filesystem::path &from, const std::filesystem::path &to) {
    if (!document.scene) {
        return;
    }
    for (SceneItem &item : document.scene->items) {
        auto *object = std::get_if<Object>(&item.data);
        if (object == nullptr || object->mesh.empty() || library_mesh(object->mesh) != nullptr) {
            continue;
        }
        std::filesystem::path model{object->mesh};
        if (model.is_relative()) {
            if (from.empty()) {
                continue; // nothing to resolve it against
            }
            model = from / model;
        }
        model = model.lexically_normal();
        if (!to.empty()) {
            // Another drive has no way there: the path stays whole.
            model = model.lexically_proximate(to.lexically_normal());
        }
        object->mesh = model.generic_string();
    }
}

std::string default_item_name(const SceneItem &item) {
    if (const auto *object = std::get_if<Object>(&item.data)) {
        if (object->mesh == "cube") {
            return "Object";
        }
        if (object->mesh == "floor") {
            return "Ground";
        }
        if (library_mesh(object->mesh) != nullptr) {
            std::string name = object->mesh;
            name.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(name.front())));
            return name;
        }
        return std::filesystem::path{object->mesh}.stem().string();
    }
    if (const auto *force = std::get_if<Force>(&item.data)) {
        return std::string{force_name(force->kind)};
    }
    if (std::holds_alternative<Group>(item.data)) {
        return "Group";
    }
    if (std::holds_alternative<SmokeGrid>(item.data)) {
        return "Smoke grid";
    }
    return "Particle system";
}

Uid add_floor(Document &document) {
    const Uid uid = add_object(document, "floor");
    if (uid == 0U) {
        return 0U;
    }
    SceneItem &item = *document.scene->item(uid);
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
    item.seed = uid;
    item.data = Object{.mesh = std::move(mesh)};
    item.name = default_item_name(item);
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
Uid add_force(Document &document, ForceKind kind) {
    if (!document.scene) {
        return 0U;
    }
    const Uid uid = reserve(document);
    if (uid == 0U) {
        return 0U;
    }
    Force force{.kind = kind};
    switch (kind) {
    case ForceKind::Wind:
        force.strength = {1.5F, 1.5F};
        break;
    case ForceKind::Vortex:
        force.strength = {3.0F, 3.0F};
        break;
    case ForceKind::Attract:
        force.strength = {4.0F, 4.0F};
        break;
    case ForceKind::DrawIn:
        force.strength = {6.0F, 6.0F};
        force.position = {};
        break;
    case ForceKind::Blast:
        force.position = {0.0F, 0.5F, 0.0F};
        break;
    }
    document.scene->items.push_back(
        SceneItem{.uid = uid, .name = std::string{force_name(kind)}, .seed = uid, .data = std::move(force)});
    return uid;
}
Uid add_smoke_grid(Document &document) {
    if (!document.scene) {
        return 0U;
    }
    const Uid uid = reserve(document);
    if (uid == 0U) {
        return 0U;
    }
    document.scene->items.push_back(SceneItem{.uid = uid, .name = "Smoke grid", .seed = uid, .data = SmokeGrid{}});
    return uid;
}
std::array<std::uint32_t, 3> grid_resolution(const SmokeGrid &grid) {
    const auto along = [&grid](float extent) {
        const float cell = std::isfinite(grid.cell) && grid.cell > 0.0F ? grid.cell : 0.01F;
        // A box that is a whole number of cells, give or take a rounding
        // error, is that many cells rather than one more.
        const double count = std::ceil(static_cast<double>(extent) / static_cast<double>(cell) - 1.0e-4);
        return static_cast<std::uint32_t>(std::clamp(count, 2.0, 1.0e9));
    };
    return {along(grid.size.x), along(grid.size.y), along(grid.size.z)};
}
std::uint64_t grid_cells(const SmokeGrid &grid) {
    const std::array<std::uint32_t, 3> n = grid_resolution(grid);
    return std::uint64_t{n[0]} * n[1] * n[2];
}
std::string smoke_grid_volume(Uid item) { return volume_name(item + 1U); }
bool relation_births(RelationKind kind) { return kind != RelationKind::FeedSmoke && kind != RelationKind::RideSmoke; }
std::string_view force_name(ForceKind kind) {
    switch (kind) {
    case ForceKind::Wind:
        return "Wind";
    case ForceKind::Vortex:
        return "Vortex";
    case ForceKind::Attract:
        return "Attract";
    case ForceKind::DrawIn:
        return "Draw in";
    case ForceKind::Blast:
        return "Blast";
    }
    return "Force";
}
bool force_targets(const Force &force, Uid system) {
    return force.all_systems || std::find(force.targets.begin(), force.targets.end(), system) != force.targets.end();
}
Uid add_relationship(Document &document, RelationKind kind, Uid object, Uid system) {
    if (!document.scene || document.scene->relationships.size() >= max_relationships) {
        return 0U;
    }
    const SceneItem *target = document.scene->item(system);
    const SceneItem *source = object == 0U ? nullptr : document.scene->item(object);
    if (target == nullptr || !std::holds_alternative<ParticleSystem>(target->data)) {
        return 0U;
    }
    if (kind == RelationKind::TrailFromSystem) {
        // Another system's particles, never its own.
        if (source == nullptr || object == system || !std::holds_alternative<ParticleSystem>(source->data)) {
            return 0U;
        }
    } else if (!relation_births(kind)) {
        // A smoke grid, and one rule of each kind between a system and a grid.
        if (source == nullptr || !std::holds_alternative<SmokeGrid>(source->data)) {
            return 0U;
        }
        const bool already = std::any_of(
            document.scene->relationships.begin(), document.scene->relationships.end(),
            [&](const Relationship &r) { return r.kind == kind && r.object == object && r.system == system; });
        if (already) {
            return 0U;
        }
    } else if (object != 0U && (source == nullptr || !std::holds_alternative<Object>(source->data))) {
        return 0U;
    }
    const Uid uid = reserve(document);
    if (uid == 0U) {
        return 0U;
    }
    Relationship relationship{.uid = uid, .kind = kind, .object = object, .system = system};
    if (kind != RelationKind::EmitOnHit) {
        relationship.per_speed = 10.0F;
    }
    document.scene->relationships.push_back(relationship);
    return uid;
}
Uid add_bound_system(Document &document, RelationKind kind, Uid object) {
    const SceneItem *source = document.scene ? document.scene->item(object) : nullptr;
    const bool from_system = kind == RelationKind::TrailFromSystem;
    if (source == nullptr || (from_system ? !std::holds_alternative<ParticleSystem>(source->data)
                                          : !std::holds_alternative<Object>(source->data))) {
        return 0U;
    }
    const std::string name = source->name + (kind == RelationKind::EmitOnHit ? " hits" : " trail");
    const Uid system = add_particle_system(document);
    if (system == 0U) {
        return 0U;
    }
    SceneItem *item = document.scene->item(system);
    item->name = name;
    auto &particles = std::get<ParticleSystem>(item->data);
    particles.sources.clear();
    particles.radius = {0.0F, 0.0F};
    if (kind != RelationKind::EmitOnHit) {
        particles.speed = {0.0F, 0.2F};
        particles.lifetime = {0.8F, 1.6F};
    } else {
        particles.speed = {0.5F, 1.5F};
        particles.lifetime = {0.6F, 1.4F};
    }
    if (add_relationship(document, kind, object, system) == 0U) {
        static_cast<void>(remove_item(document, system));
        return 0U;
    }
    return system;
}
bool remove_relationship(Document &document, Uid uid) {
    if (!document.scene) {
        return false;
    }
    return std::erase_if(document.scene->relationships, [uid](const Relationship &r) { return r.uid == uid; }) > 0U;
}
bool relationship_active(const Scene &scene, const Relationship &relationship) {
    const auto dynamic = [](const SceneItem &item) {
        const auto *object = std::get_if<Object>(&item.data);
        return object != nullptr && item.simulate && object->motion == ObjectMotion::Dynamic;
    };
    if (relationship.kind == RelationKind::TrailFromSystem) {
        const SceneItem *source = scene.item(relationship.object);
        return source != nullptr && source->simulate && std::holds_alternative<ParticleSystem>(source->data);
    }
    if (!relation_births(relationship.kind)) {
        const SceneItem *grid = scene.item(relationship.object);
        return grid != nullptr && grid->simulate && std::holds_alternative<SmokeGrid>(grid->data);
    }
    if (relationship.object != 0U) {
        const SceneItem *object = scene.item(relationship.object);
        return object != nullptr && dynamic(*object);
    }
    return std::any_of(scene.items.begin(), scene.items.end(), dynamic);
}
std::string_view relation_name(RelationKind kind) {
    switch (kind) {
    case RelationKind::EmitOnHit:
        return "Emit on hit";
    case RelationKind::TrailFrom:
        return "Trail";
    case RelationKind::TrailFromSystem:
        return "Trail from system";
    case RelationKind::FeedSmoke:
        return "Feed smoke";
    case RelationKind::RideSmoke:
        return "Ride smoke";
    }
    return "Relationship";
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
Uid add_behavior(Document &document, Uid uid, BehaviorKind kind) {
    SceneItem *item = document.scene ? document.scene->item(uid) : nullptr;
    auto *system = item ? std::get_if<ParticleSystem>(&item->data) : nullptr;
    if (!system || system->motion.size() >= max_behaviors) {
        return 0U;
    }
    const Uid fresh = reserve(document);
    if (fresh == 0U) {
        return 0U;
    }
    MotionBehavior behavior{.uid = fresh, .kind = kind};
    const float strength = kind == BehaviorKind::Drag         ? 1.0F
                           : kind == BehaviorKind::Turbulence ? 1.5F
                           : kind == BehaviorKind::Vortex     ? 3.0F
                           : kind == BehaviorKind::Wind       ? 1.5F
                           : kind == BehaviorKind::Attract    ? 4.0F
                                                              : 6.0F;
    behavior.strength = {strength, strength};
    if (kind == BehaviorKind::Attract) {
        behavior.center = {0.0F, 1.0F, 0.0F};
    } else if (kind == BehaviorKind::DrawIn) {
        behavior.reach = 1.0F;
    }
    system->motion.push_back(behavior);
    return fresh;
}
bool move_behavior(Document &document, Uid uid, Uid behavior, int by) {
    SceneItem *item = document.scene ? document.scene->item(uid) : nullptr;
    auto *system = item ? std::get_if<ParticleSystem>(&item->data) : nullptr;
    if (!system || (by != -1 && by != 1)) {
        return false;
    }
    const auto at = std::find_if(system->motion.begin(), system->motion.end(),
                                 [behavior](const MotionBehavior &b) { return b.uid == behavior; });
    if (at == system->motion.end() || (by < 0 && at == system->motion.begin()) ||
        (by > 0 && at + 1 == system->motion.end())) {
        return false;
    }
    std::iter_swap(at, at + by);
    return true;
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
        // A force stops naming a system that is gone; it never retargets.
        if (auto *force = std::get_if<Force>(&item.data)) {
            std::erase(force->targets, uid);
        }
    }
    // A rule with an endpoint gone is gone too, never re-aimed at everything.
    std::erase_if(document.scene->relationships,
                  [uid](const Relationship &r) { return r.system == uid || r.object == uid; });
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
        for (MotionBehavior &behavior : system->motion) {
            behavior.uid = reserve(document);
            if (behavior.uid == 0U) {
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
            // Fed by a rule that gives it particles; a smoke rule binds the
            // particles it has, so a system that only rides smoke still
            // needs a source.
            const bool bound = std::any_of(
                scene.relationships.begin(), scene.relationships.end(),
                [&item](const Relationship &r) { return r.system == item.uid && relation_births(r.kind); });
            if (for_playback && system->sources.empty() && !bound) {
                return where + "Initial state / Sources: add a source or a relationship";
            }
            if (for_playback && system->persistent && bound) {
                return where + "a persistent population cannot be born of hits or trails";
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
            if (system->motion.size() > max_behaviors) {
                return where + "Motion: behavior capacity exceeded (16)";
            }
            for (const MotionBehavior &behavior : system->motion) {
                if (!identity(behavior.uid)) {
                    return where + "Motion: invalid or overlapping behavior identity";
                }
                if (const std::string error = validate_behavior(behavior); !error.empty()) {
                    return where + "Motion / " + std::string{behavior_name(behavior.kind)} + ": " + error;
                }
            }
            if (const std::string error = validate_keys(system->size_over_age, 0.0F, 100.0F); !error.empty()) {
                return where + "Appearance / Size over age: " + error;
            }
            if (const std::string error = validate_keys(system->tint_over_age, 0.0F, 64.0F); !error.empty()) {
                return where + "Appearance / Color over age: " + error;
            }
            std::size_t acting = 0U;
            for (const SceneItem &other : scene.items) {
                const auto *force = std::get_if<Force>(&other.data);
                if (force && force->kind != ForceKind::Blast && other.simulate && force_targets(*force, item.uid)) {
                    ++acting;
                }
            }
            if (acting > max_forces_per_system) {
                return where + "more than 16 forces act on this system";
            }
        } else if (const auto *force = std::get_if<Force>(&item.data)) {
            std::set<Uid> named;
            for (const Uid target : force->targets) {
                const SceneItem *named_system = scene.item(target);
                if (named_system == nullptr || !std::holds_alternative<ParticleSystem>(named_system->data)) {
                    return where + "Targets: a named particle system is missing";
                }
                if (!named.insert(target).second) {
                    return where + "Targets: a particle system is named twice";
                }
            }
            if (force->targets.size() > 4096U) {
                return where + "Targets: too many";
            }
        } else if (const auto *grid = std::get_if<SmokeGrid>(&item.data)) {
            // The runtime's budget, named here with the item so the artist
            // can raise the cell size or shrink the box: compile would
            // refuse the same grid, naming the volume rather than the item.
            if (for_playback) {
                const std::array<std::uint32_t, 3> n = grid_resolution(*grid);
                constexpr std::string_view axes[] = {"width", "height", "depth"};
                for (std::size_t k = 0U; k < 3U; ++k) {
                    if (n[k] > max_volume_size) {
                        return where + "Smoke: its " + std::string{axes[k]} + " is " + std::to_string(n[k]) +
                               " cells, more than the " + std::to_string(max_volume_size) +
                               " a grid may have along a side: raise the cell size or shrink the box";
                    }
                }
                if (const std::uint64_t cells = grid_cells(*grid); cells > max_volume_cells) {
                    return where + "Smoke: " + std::to_string(cells) + " cells (" + std::to_string(n[0]) + " x " +
                           std::to_string(n[1]) + " x " + std::to_string(n[2]) + "), more than the " +
                           std::to_string(max_volume_cells) +
                           " a grid may hold: raise the cell size or shrink the box";
                }
            }
            const auto grids = static_cast<std::size_t>(
                std::count_if(scene.items.begin(), scene.items.end(),
                              [](const SceneItem &other) { return std::holds_alternative<SmokeGrid>(other.data); }));
            if (for_playback && grids > max_smoke_grids) {
                return where + "more than " + std::to_string(max_smoke_grids) + " smoke grids in the scene";
            }
        }
    }
    if (scene.relationships.size() > max_relationships) {
        return "Relationships: capacity exceeded (256)";
    }
    for (const Relationship &r : scene.relationships) {
        const std::string where = "Relationship " + std::string{relation_name(r.kind)} + ": ";
        if (!identity(r.uid)) {
            return where + "invalid or overlapping identity";
        }
        const SceneItem *system = scene.item(r.system);
        if (system == nullptr || !std::holds_alternative<ParticleSystem>(system->data)) {
            return where + "its particle system is missing";
        }
        const SceneItem *object = r.object == 0U ? nullptr : scene.item(r.object);
        if (r.kind == RelationKind::TrailFromSystem) {
            if (object == nullptr || !std::holds_alternative<ParticleSystem>(object->data)) {
                return where + "its source system is missing";
            }
            if (r.object == r.system) {
                return where + "a system cannot trail itself";
            }
        } else if (!relation_births(r.kind)) {
            if (object == nullptr || !std::holds_alternative<SmokeGrid>(object->data)) {
                return where + system->name + ": its smoke grid is missing";
            }
        } else if (r.object != 0U && (object == nullptr || !std::holds_alternative<Object>(object->data))) {
            return where + "its object is missing";
        }
        if (!within(r.hardness, 0.0F, 10000.0F) || !within(r.per_hit, 0.0F, 100000.0F) ||
            !within(r.per_speed, 0.0F, 100000.0F) || !within(r.spread, 0.0F, 180.0F) ||
            !within(r.min_speed, 0.0F, 10000.0F) || !within(r.rate, 0.0F, 100000.0F) ||
            !within(r.density, 0.0F, 1000.0F) || !within(r.heat, 0.0F, 1000.0F) || !within(r.push, 0.0F, 100.0F) ||
            !within(r.strength, 0.0F, 1000.0F) || r.seed > 1000000U) {
            return where + "a value is outside its supported range";
        }
    }
    // Systems trailing one another in a ring could never agree which steps
    // first: the runtime refuses the ring, so the scene names it.
    for (const Relationship &start : scene.relationships) {
        if (start.kind != RelationKind::TrailFromSystem) {
            continue;
        }
        std::vector<Uid> seen{start.system};
        Uid at = start.object;
        while (at != 0U && std::find(seen.begin(), seen.end(), at) == seen.end()) {
            seen.push_back(at);
            Uid next = 0U;
            for (const Relationship &r : scene.relationships) {
                if (r.kind == RelationKind::TrailFromSystem && r.system == at) {
                    next = r.object;
                    break;
                }
            }
            at = next;
        }
        if (at != 0U) {
            const SceneItem *item = scene.item(at);
            return "Relationship Trail from system: " + (item ? item->name : std::string{"a system"}) +
                   " trails a system that trails it, round in a ring";
        }
    }
    return {};
}
namespace {
// The forces that act on a system, in identity order, and where each lowers
// to: the system's own reserved identities from 16 up, so adding a system or
// a force elsewhere moves nothing here.
std::vector<const SceneItem *> forces_on(const Scene &scene, Uid system) {
    std::vector<const SceneItem *> out;
    for (const SceneItem &item : scene.items) {
        const auto *force = std::get_if<Force>(&item.data);
        if (force && force->kind != ForceKind::Blast && item.simulate && force_targets(*force, system)) {
            out.push_back(&item);
        }
    }
    std::sort(out.begin(), out.end(), [](const SceneItem *a, const SceneItem *b) { return a->uid < b->uid; });
    return out;
}
Uid force_block_uid(const Scene &scene, Uid force, Uid system) {
    const std::vector<const SceneItem *> acting = forces_on(scene, system);
    for (std::size_t k = 0U; k < acting.size() && k < max_forces_per_system; ++k) {
        if (acting[k]->uid == force) {
            return system + 16U + static_cast<Uid>(k);
        }
    }
    return 0U;
}
Block force_block(const SceneItem &item, Uid uid) {
    const Force &f = std::get<Force>(item.data);
    const Value seed = number(static_cast<float>(item.seed));
    switch (f.kind) {
    case ForceKind::Wind:
        return block(uid, "wind", {{"strength", value(f.strength)}, {"heading", number(f.heading)}, {"seed", seed}});
    case ForceKind::Vortex:
        return block(uid, "swirl",
                     {{"strength", value(f.strength)},
                      {"axis", choice(f.axis == VortexAxis::Horizontal ? "horizontal" : "vertical")},
                      {"center", value(f.position)},
                      {"seed", seed}});
    case ForceKind::Attract:
        return block(uid, "attract",
                     {{"strength", value(f.strength)},
                      {"point", value(f.position)},
                      {"reach", number(f.reach)},
                      {"seed", seed}});
    case ForceKind::DrawIn:
        return block(uid, "draw_in",
                     {{"strength", value(f.strength)},
                      {"height", number(f.height)},
                      {"center", value(f.position)},
                      {"seed", seed}});
    case ForceKind::Blast:
        return block(uid, "explosion",
                     {{"at", value(f.position)},
                      {"power", number(f.power)},
                      {"radius", number(f.radius)},
                      {"time", number(f.time)}});
    }
    return block(uid, "wind");
}
// The system a force's handles drag through: the lowest identity it acts on.
Uid first_target(const Scene &scene, const SceneItem &item) {
    const Force &force = std::get<Force>(item.data);
    Uid best = 0U;
    for (const SceneItem &other : scene.items) {
        if (std::holds_alternative<ParticleSystem>(other.data) && force_targets(force, other.uid) &&
            (best == 0U || other.uid < best)) {
            best = other.uid;
        }
    }
    return best;
}
} // namespace

namespace {

// A behavior's strength control and the range validate_behavior allows it.
struct BehaviorStrength final {
    const char *control;
    float low;
    float high;
};
BehaviorStrength behavior_strength(BehaviorKind kind) {
    switch (kind) {
    case BehaviorKind::Drag: return {"amount", 0.0F, 100.0F};
    case BehaviorKind::Turbulence: return {"strength", 0.0F, 100.0F};
    case BehaviorKind::Vortex:
    case BehaviorKind::Attract: return {"strength", -1000.0F, 1000.0F};
    case BehaviorKind::Wind:
    case BehaviorKind::DrawIn: return {"strength", 0.0F, 1000.0F};
    }
    return {"strength", 0.0F, 1000.0F};
}

std::string live_name(Uid system, const char *what) { return "live" + std::to_string(system) + "_" + what; }
std::string behavior_parameter(Uid system, const MotionBehavior &b) {
    return live_name(system, "m") + std::to_string(b.uid);
}

Vec3 unit_or_up(Vec3 v) {
    const float n = std::sqrt(dot(v, v));
    return n > 1e-6F ? Vec3{v.x / n, v.y / n, v.z / n} : Vec3{0.0F, 1.0F, 0.0F};
}

// Publishes the system's live controls on the blocks lower_scene made of it.
// The ranges are the inspector's, so a value it allows is never clamped.
void publish_live(Layer &layer, const SceneItem &item, const ParticleSystem &system) {
    const auto on = [&](Uid uid, const char *control, std::string name, float low, float high) {
        for (Block &b : layer.blocks) {
            if (b.uid == uid) {
                b.publish(Published{control, std::move(name), low, high});
            }
        }
    };
    on(item.uid + 6U, "speed", live_name(item.uid, "speed"), 0.0F, 10000.0F);
    on(item.uid + 6U, "spread", live_name(item.uid, "spread"), 0.0F, 180.0F);
    on(item.uid + 6U, "custom", live_name(item.uid, "direction"), -1.0F, 1.0F);
    on(item.uid + 7U, "size", live_name(item.uid, "radius"), 0.0F, 1000.0F);
    on(item.uid + 8U, "strength", live_name(item.uid, "gravity"), 0.0F, 10.0F);
    for (const MotionBehavior &b : system.motion) {
        if (b.enabled) {
            const BehaviorStrength strength = behavior_strength(b.kind);
            on(b.uid, strength.control, behavior_parameter(item.uid, b), strength.low, strength.high);
        }
    }
    // The preview simulates. Holding bake-time values where they are set is
    // for the cache, which an export takes.
    layer.baked = false;
}

} // namespace

std::vector<LiveParameter> live_parameters(const Document &document, Uid system) {
    std::vector<LiveParameter> out;
    const SceneItem *item = document.scene ? document.scene->item(system) : nullptr;
    const auto *particles = item ? std::get_if<ParticleSystem>(&item->data) : nullptr;
    if (particles == nullptr) {
        return out;
    }
    const float s = document.scale;
    const auto range = [&](const std::string &name, float low, float high) {
        out.push_back({name + "_min", {low, 0.0F, 0.0F, 0.0F}});
        out.push_back({name + "_max", {high, 0.0F, 0.0F, 0.0F}});
    };
    range(live_name(system, "speed"), particles->speed.min * s, particles->speed.max * s);
    range(live_name(system, "spread"), 0.0F, std::clamp(particles->spread, 0.0F, 180.0F));
    const Vec3 direction = unit_or_up(particles->direction);
    out.push_back({live_name(system, "direction"), {direction.x, direction.y, direction.z, 0.0F}, 3U});
    range(live_name(system, "radius"), particles->radius.min * s, particles->radius.max * s);
    range(live_name(system, "gravity"), particles->gravity, particles->gravity);
    for (const MotionBehavior &b : particles->motion) {
        if (b.enabled) {
            range(behavior_parameter(system, b), b.strength.min, b.strength.max);
        }
    }
    return out;
}

bool same_but_live(const Document &a, const Document &b, Uid system) {
    if (!a.scene || !b.scene) {
        return false;
    }
    const SceneItem *from = a.scene->item(system);
    const SceneItem *to = b.scene->item(system);
    const auto *kept = from ? std::get_if<ParticleSystem>(&from->data) : nullptr;
    if (kept == nullptr || to == nullptr || !std::holds_alternative<ParticleSystem>(to->data)) {
        return false;
    }
    // b with the live values of a is a when nothing else differs.
    Document copy = b;
    auto &moved = std::get<ParticleSystem>(copy.scene->item(system)->data);
    if (moved.motion.size() != kept->motion.size()) {
        return false;
    }
    moved.speed = kept->speed;
    moved.spread = kept->spread;
    moved.direction = kept->direction;
    moved.radius = kept->radius;
    moved.gravity = kept->gravity;
    for (std::size_t k = 0U; k < moved.motion.size(); ++k) {
        moved.motion[k].strength = kept->motion[k].strength;
    }
    return copy == a;
}

Document focus_scene(const Document &document, Uid system) {
    Document out = document;
    if (!out.scene) {
        return out;
    }
    std::set<Uid> kept{system};
    for (bool grew = true; grew;) {
        grew = false;
        for (const Relationship &r : out.scene->relationships) {
            const SceneItem *source = out.scene->item(r.object);
            if (kept.contains(r.system) && source != nullptr &&
                std::holds_alternative<ParticleSystem>(source->data) && kept.insert(r.object).second) {
                grew = true;
            }
        }
    }
    for (SceneItem &item : out.scene->items) {
        if (std::holds_alternative<ParticleSystem>(item.data) && !kept.contains(item.uid)) {
            item.simulate = false;
        }
    }
    return out;
}

Document lower_scene(const Document &document, Uid live) {
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
    Uid host = 0U; // the first simulating layer holds every blast; a blast throws everything
    for (const SceneItem *item : ordered) {
        if (std::holds_alternative<Group>(item->data) || std::holds_alternative<Force>(item->data)) {
            continue;
        }
        if (const auto *grid = std::get_if<SmokeGrid>(&item->data)) {
            // A smoke grid is a layer of one block, so its box has guides
            // and handles as any block does; the compiler lists it on the
            // effect (page_volumes) and builds no emitter from it. Nothing
            // of it is cached, so it is never baked.
            Layer layer;
            layer.uid = item->uid;
            layer.name = "item_" + std::to_string(item->uid);
            layer.enabled = item->simulate;
            layer.seed = item->seed;
            layer.baked = false;
            layer.blocks.push_back(block(item->uid + 1U, "smoke_grid",
                                         {{"center", value(grid->position)},
                                          {"size", value(grid->size)},
                                          {"cell", number(grid->cell)},
                                          {"rises", number(grid->rises)},
                                          {"sinks", number(grid->sinks)},
                                          {"swirl", number(grid->swirl)},
                                          {"fades", number(grid->fades)},
                                          {"calms", number(grid->calms)},
                                          {"solve", number(static_cast<float>(grid->solve))},
                                          {"thickness", number(grid->thickness)},
                                          {"color", color(grid->color.r, grid->color.g, grid->color.b, grid->color.a)},
                                          {"brightness", number(grid->brightness)}}));
            out.layers.push_back(std::move(layer));
            continue;
        }
        if (host == 0U && item->simulate) {
            host = item->uid;
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
            layer.life_min = system->persistent ? std::numeric_limits<float>::max() : system->lifetime.min;
            layer.life_max = system->persistent ? std::numeric_limits<float>::max() : system->lifetime.max;
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
                                          {"size", value(system->radius)}}));
            launch(layer, item->uid + 6U, system->direction, system->speed, system->spread);
            layer.blocks.push_back(
                block(item->uid + 8U, "gravity", {{"strength", range(system->gravity, system->gravity)}}));
            // The stack's authored order is the order the forces run in.
            for (const MotionBehavior &b : system->motion) {
                if (!b.enabled) {
                    continue;
                }
                const Value seed = number(static_cast<float>(b.seed));
                switch (b.kind) {
                case BehaviorKind::Drag:
                    layer.blocks.push_back(block(b.uid, "drag", {{"amount", value(b.strength)}, {"seed", seed}}));
                    break;
                case BehaviorKind::Turbulence:
                    layer.blocks.push_back(block(b.uid, "turbulence",
                                                 {{"strength", value(b.strength)},
                                                  {"size", number(b.size)},
                                                  {"churn", number(b.churn)},
                                                  {"seed", seed}}));
                    break;
                case BehaviorKind::Vortex:
                    layer.blocks.push_back(block(b.uid, "swirl",
                                                 {{"strength", value(b.strength)},
                                                  {"axis", choice(b.axis == VortexAxis::Horizontal ? "horizontal"
                                                                                                    : "vertical")},
                                                  {"center", value(b.center)},
                                                  {"seed", seed}}));
                    break;
                case BehaviorKind::Wind:
                    layer.blocks.push_back(block(b.uid, "wind",
                                                 {{"strength", value(b.strength)},
                                                  {"heading", number(b.heading)},
                                                  {"seed", seed}}));
                    break;
                case BehaviorKind::Attract:
                    layer.blocks.push_back(block(b.uid, "attract",
                                                 {{"strength", value(b.strength)},
                                                  {"point", value(b.center)},
                                                  {"reach", number(b.reach)},
                                                  {"seed", seed}}));
                    break;
                case BehaviorKind::DrawIn:
                    layer.blocks.push_back(block(b.uid, "draw_in",
                                                 {{"strength", value(b.strength)},
                                                  {"height", number(b.reach)},
                                                  {"center", value(b.center)},
                                                  {"seed", seed}}));
                    break;
                }
            }
            layer.blocks.push_back(
                block(item->uid + 4U, "appearance",
                      {{"material", choice(system->look == ParticleLook::Glow    ? "glow"
                                           : system->look == ParticleLook::Smoke ? "smoke"
                                                                                 : "solid")},
                       {"lighting", choice(system->lighting == ParticleLighting::Lit ? "lit" : "unlit")},
                       {"curvature", number(system->curvature)},
                       {"wrap", number(system->wrap)},
                       {"ambient", number(system->ambient)},
                       {"shadowed", toggle(system->shadowed)},
                       {"tint", color(system->tint.r, system->tint.g, system->tint.b, system->tint.a)}}));
            layer.blocks.push_back(block(item->uid + 3U, "color",
                                         {{"color", color(1.0F, 1.0F, 1.0F)}, {"brightness", value(system->brightness)}}));
            layer.blocks.push_back(block(item->uid + 5U, "size", {{"size", value(system->size)}}));
            if (system->casts_light) {
                // In the particle's own colour, fading in and out with its
                // life, so a flame's light goes as the flame does.
                layer.blocks.push_back(block(item->uid + 9U, "light",
                                             {{"count", number(static_cast<float>(system->light_count))},
                                              {"intensity", value(system->light_strength)},
                                              {"reach", value(system->light_reach)},
                                              {"over_life", choice("fade")},
                                              {"color_mode", choice("particle")}}));
            }
            if (system->spin.max > 0.0F || system->spin.min > 0.0F) {
                layer.blocks.push_back(block(item->uid + 15U, "spin", {{"speed", value(system->spin)}}));
            }
            if (system->size_driver == Driver::Age) {
                layer.blocks.push_back(
                    raw_block(item->uid + 13U, UpdateModule{SizeOverLife{.keys = system->size_over_age}}));
            }
            if (system->tint_driver == Driver::Age) {
                layer.blocks.push_back(
                    raw_block(item->uid + 14U, UpdateModule{ColorOverLife{.keys = system->tint_over_age}}));
            }
            if (!system->collide) {
                layer.blocks.push_back(block(item->uid + 12U, "passes_through"));
            } else {
                // Particles meet loose bodies through the world, but a Static
                // object stands outside it, so each is met as its own shape,
                // turned as it stands: the floor as a plane at its top, a
                // sphere as a sphere, an artist's model with a hull collider
                // as its own surface (a distance field fitted and turned as
                // the model is drawn), and anything else as a box: a
                // primitive's own bounds, or a model's longest side. The
                // block keeps the object's identity, so every system meets
                // the same thing.
                for (const SceneItem *other : ordered) {
                    const auto *still = std::get_if<Object>(&other->data);
                    if (still == nullptr || !other->simulate || still->motion != ObjectMotion::Static) {
                        continue;
                    }
                                        const Uid uid = other->uid + 3U;
                    if (still->mesh == "floor") {
                        float top = 0.0F;
                        if (const MeshData *mesh = library_mesh("floor")) {
                            for (const auto &vertex : mesh->vertices) {
                                top = std::max(top, vertex.position.y);
                            }
                        }
                        layer.blocks.push_back(block(uid, "ground",
                                                     {{"response", choice("bounce")},
                                                      {"bounce", number(still->restitution)},
                                                      {"friction", number(still->friction)},
                                                      {"height", number(still->position.y + top * still->size)}}));
                    } else if (still->collider == ObjectCollider::Sphere) {
                        layer.blocks.push_back(block(uid, "sphere",
                                                     {{"center", value(still->position)},
                                                      {"radius", number(still->size * 0.5F)},
                                                      {"bounce", number(still->restitution)},
                                                      {"friction", number(still->friction)}}));
                    } else if (const MeshData *primitive = library_mesh(still->mesh);
                               primitive == nullptr && still->collider == ObjectCollider::Hull) {
                        layer.blocks.push_back(block(uid, "mesh_collider",
                                                     {{"file", choice(still->mesh)},
                                                      {"position", value(still->position)},
                                                      {"scale", number(still->size)},
                                                      {"fit", toggle(true)},
                                                      {"turn", value(still->rotation)},
                                                      {"bounce", number(still->restitution)},
                                                      {"friction", number(still->friction)}}));
                    } else {
                        Vec3 low{-0.5F, -0.5F, -0.5F};
                        Vec3 high{0.5F, 0.5F, 0.5F};
                        if (primitive != nullptr && !primitive->vertices.empty()) {
                            low = high = primitive->vertices.front().position;
                            for (const auto &vertex : primitive->vertices) {
                                const Vec3 v = vertex.position;
                                low = Vec3{std::min(low.x, v.x), std::min(low.y, v.y), std::min(low.z, v.z)};
                                high = Vec3{std::max(high.x, v.x), std::max(high.y, v.y), std::max(high.z, v.z)};
                            }
                        }
                        const Vec3 middle = rotate(prop_orientation(still->rotation), (low + high) * (0.5F * still->size));
                        const Vec3 extent = (high - low) * still->size;
                        layer.blocks.push_back(block(uid, "box",
                                                     {{"center", value(still->position + middle)},
                                                      {"size", value(extent)},
                                                      {"turn", value(still->rotation)},
                                                      {"bounce", number(still->restitution)},
                                                      {"friction", number(still->friction)}}));
                    }
                }
            }
            const std::vector<const SceneItem *> acting = forces_on(*document.scene, item->uid);
            for (std::size_t k = 0U; k < acting.size() && k < max_forces_per_system; ++k) {
                layer.blocks.push_back(force_block(*acting[k], item->uid + 16U + static_cast<Uid>(k)));
            }
            // Rules that feed this system, by identity: born where its object hits, or behind it.
            std::vector<const Relationship *> rules;
            for (const Relationship &r : document.scene->relationships) {
                if (r.system == item->uid) {
                    rules.push_back(&r);
                }
            }
            std::sort(rules.begin(), rules.end(),
                      [](const Relationship *a, const Relationship *b) { return a->uid < b->uid; });
            for (const Relationship *r : rules) {
                const Value seed = number(static_cast<float>(r->seed));
                const Value solid = number(static_cast<float>(r->object));
                if (r->kind == RelationKind::EmitOnHit) {
                    layer.blocks.push_back(block(r->uid, "hit_puffs",
                                                 {{"solid", solid},
                                                  {"hardness", number(r->hardness)},
                                                  {"per_hit", number(r->per_hit)},
                                                  {"per_speed", number(r->per_speed)},
                                                  {"spread", number(r->spread)},
                                                  {"seed", seed}}));
                } else if (r->kind == RelationKind::TrailFrom) {
                    layer.blocks.push_back(block(r->uid, "trail",
                                                 {{"solid", solid},
                                                  {"pieces", toggle(r->pieces)},
                                                  {"per_speed", number(r->per_speed)},
                                                  {"min_speed", number(r->min_speed)},
                                                  {"rate", number(r->rate)},
                                                  {"seed", seed}}));
                } else if (!relation_births(r->kind)) {
                    // The runtime's own grid modules, naming the grid's
                    // volume. A grid that is switched off is not on the
                    // effect, so a rule on it lowers to nothing rather than
                    // to a module naming a volume the effect has none of.
                    // The amounts are rates and shares, so the scene's scale
                    // leaves them alone.
                    if (!relationship_active(*document.scene, *r)) {
                        continue;
                    }
                    const std::string volume = smoke_grid_volume(r->object);
                    if (r->kind == RelationKind::FeedSmoke) {
                        layer.blocks.push_back(raw_block(
                            r->uid, RawModule{UpdateModule{VolumeSource{.volume = volume,
                                                                         .density = FloatInput{r->density},
                                                                         .temperature = FloatInput{r->heat},
                                                                         .velocity = FloatInput{r->push}}}}));
                    } else {
                        layer.blocks.push_back(raw_block(
                            r->uid, RawModule{UpdateModule{VolumeAdvect{.volume = volume,
                                                                         .strength = FloatInput{r->strength}}}}));
                    }
                } else {
                    // Shed behind another system's particles: the runtime
                    // module itself, naming that system's layer emitter.
                    // The scene's scale turns metres into the effect's units
                    // as the trail block's compiler does; the cap is the
                    // trail block's default, and is what the system's room
                    // is proved against.
                    Block shed = raw_block(r->uid, RawModule{SpawnModule{SpawnFromParticles{
                                                        .emitter = "item_" + std::to_string(r->object),
                                                        .min_speed = r->min_speed * document.scale,
                                                        .rate = r->rate,
                                                        .per_speed = r->per_speed / document.scale,
                                                        .max_per_step = 64U,
                                                        .inherit = 0.0F}}});
                    shed.set("seed", seed);
                    layer.blocks.push_back(std::move(shed));
                }
            }
        }
        if (item->uid == live) {
            if (const auto *system = std::get_if<ParticleSystem>(&item->data)) {
                publish_live(layer, *item, *system);
            }
        }
        out.layers.push_back(std::move(layer));
    }
    if (host != 0U) {
        Layer &layer = *std::find_if(out.layers.begin(), out.layers.end(), [host](const Layer &l) { return l.uid == host; });
        for (const SceneItem *item : ordered) {
            const auto *force = std::get_if<Force>(&item->data);
            if (force && force->kind == ForceKind::Blast && item->simulate) {
                layer.blocks.push_back(force_block(*item, item->uid + 1U));
            }
        }
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
    std::vector<Relationship> rules_a = a.scene->relationships;
    std::vector<Relationship> rules_b = b.scene->relationships;
    const auto by_uid = [](const Relationship &x, const Relationship &y) { return x.uid < y.uid; };
    std::sort(rules_a.begin(), rules_a.end(), by_uid);
    std::sort(rules_b.begin(), rules_b.end(), by_uid);
    if (rules_a != rules_b) {
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
        if (const auto *force = std::get_if<Force>(&before.data)) {
            const auto &next = std::get<Force>(after->data);
            if (force->all_systems != next.all_systems || force->targets != next.targets) {
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
            const auto &next = std::get<ParticleSystem>(after->data);
            // Size is read by collision; colour over age is draw-time only.
            if (system->motion != next.motion || system->size_driver != next.size_driver ||
                (system->size_driver == Driver::Age && system->size_over_age != next.size_over_age)) {
                return false;
            }
        }
    }
    return true;
}

Compiled compile_scene(const Document &document, Purpose purpose, Uid live) {
    if (const std::string error = validate_scene(document); !error.empty()) {
        Compiled failed;
        failed.error = error;
        return failed;
    }
    Compiled out = compile(lower_scene(document, live), purpose);
    if (!out.error.empty()) {
        for (const LayerReport &report : out.layers) {
            if (!report.error.empty()) {
                const SceneItem *item = document.scene->item(report.layer);
                // The runtime refuses a grid by its volume's name, from
                // whichever emitter it was checked beside: the item at
                // fault is the grid.
                if (const Uid grid = grid_block_named_in(report.error); grid != 0U) {
                    if (const SceneItem *owner = document.scene->item(grid - 1U)) {
                        item = owner;
                    }
                }
                if (item != nullptr) {
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
Uid handle_block(const Document &document, Uid item) {
    const SceneItem *owner = document.scene ? document.scene->item(item) : nullptr;
    if (owner == nullptr) {
        return 0U;
    }
    if (std::holds_alternative<Object>(owner->data)) {
        return item + 2U;
    }
    if (std::holds_alternative<ParticleSystem>(owner->data)) {
        return item + 7U;
    }
    if (std::holds_alternative<SmokeGrid>(owner->data)) {
        return item + 1U;
    }
    if (const auto *force = std::get_if<Force>(&owner->data)) {
        if (force->kind == ForceKind::Blast) {
            return item + 1U;
        }
        const Uid system = first_target(*document.scene, *owner);
        return system == 0U ? 0U : force_block_uid(*document.scene, item, system);
    }
    return 0U;
}

bool adopt_handle_edit(Document &document, const Document &lowered, Uid item) {
    SceneItem *owner = document.scene ? document.scene->item(item) : nullptr;
    const Block *dragged = lowered.block(handle_block(document, item));
    if (owner == nullptr || dragged == nullptr) {
        return false;
    }
    const auto as_vec3 = [](const Value &v) { return Vec3{v.x, v.y, v.z}; };
    if (auto *object = std::get_if<Object>(&owner->data)) {
        Object before = *object;
        object->position = as_vec3(get(*dragged, "at"));
        object->size = get(*dragged, "size").x;
        return before != *object;
    }
    if (auto *grid = std::get_if<SmokeGrid>(&owner->data)) {
        SmokeGrid before = *grid;
        grid->position = as_vec3(get(*dragged, "center"));
        grid->size = as_vec3(get(*dragged, "size"));
        return before != *grid;
    }
    if (auto *force = std::get_if<Force>(&owner->data)) {
        Force before = *force;
        switch (force->kind) {
        case ForceKind::Wind:
            force->heading = get(*dragged, "heading").x;
            break;
        case ForceKind::Vortex:
            force->position = as_vec3(get(*dragged, "center"));
            break;
        case ForceKind::Attract:
            force->position = as_vec3(get(*dragged, "point"));
            force->reach = get(*dragged, "reach").x;
            break;
        case ForceKind::DrawIn:
            force->position = as_vec3(get(*dragged, "center"));
            force->height = get(*dragged, "height").x;
            break;
        case ForceKind::Blast:
            force->position = as_vec3(get(*dragged, "at"));
            force->radius = get(*dragged, "radius").x;
            break;
        }
        return before != *force;
    }
    auto &system = std::get<ParticleSystem>(owner->data);
    ParticleSystem before = system;
    system.position = as_vec3(get(*dragged, "offset"));
    system.radius.max = get(*dragged, "size").y;
    system.radius.min = std::min(system.radius.min, system.radius.max);
    return before != system;
}

} // namespace ajfx::studio
