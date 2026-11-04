#include "hands.hpp"

#include "stage.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <set>
#include <variant>

namespace ajfx::studio {
namespace {

bool finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v, Vec3 fallback) {
    const float l = length(v);
    return l > 1.0e-6F && std::isfinite(l) ? v * (1.0F / l) : fallback;
}

float page_scale(const Document& document) {
    return std::isfinite(document.scale) && document.scale > 0.0F ? document.scale : 1.0F;
}

// The things the performance's drops and paint bear at `tick`: births of
// the world's action source in that one step, which it bears at most
// SolidWorld::drops_per_step of.
std::uint32_t births_at(const Performance& performance, std::uint64_t tick) {
    std::uint32_t births = 0U;
    for (const Act& act : performance.acts) {
        if (act.tick == tick) {
            births += act.kind == ActKind::Drop ? 1U : act.kind == ActKind::Paint ? std::max(act.count, 1U) : 0U;
        }
    }
    return births;
}

bool deleted(const Performance& performance, const BodyRef& ref) {
    return std::any_of(performance.acts.begin(), performance.acts.end(),
                       [&ref](const Act& act) { return act.kind == ActKind::Delete && act.target == ref; });
}

void mix(std::uint64_t& hash, std::uint64_t value) {
    for (std::uint32_t i = 0U; i < 8U; ++i) {
        hash ^= static_cast<std::uint8_t>(value >> (8U * i));
        hash *= 0x100000001b3ULL;
    }
}

void mix(std::uint64_t& hash, float value) {
    mix(hash, static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(value)));
}

void mix(std::uint64_t& hash, Vec3 v) {
    mix(hash, v.x);
    mix(hash, v.y);
    mix(hash, v.z);
}

} // namespace

// ------------------------------------------------------------------ brush

BrushDab brush_dab(Document& document, const ActTargets& targets, const Brush& brush, Uid kind, Vec3 point,
                   Vec3 normal, std::span<const StageBody> bodies, std::uint64_t tick, std::uint32_t stroke) {
    BrushDab dab{.serials = {}, .stroke = stroke, .tick = tick, .refusal = {}};
    if (!document.scene) {
        dab.refusal = "Only a scene can be painted";
        return dab;
    }
    if (!finite(point) || !finite(normal) || !std::isfinite(brush.radius) || brush.radius < 0.0F) {
        dab.refusal = "The brush is nowhere";
        return dab;
    }
    Scene& scene = *document.scene;
    const Vec3 up = normalized(normal, Vec3{0.0F, 1.0F, 0.0F});
    if (brush.mode == BrushMode::Paint) {
        const SceneItem* item = scene.item(kind);
        const auto* object = item != nullptr ? std::get_if<Object>(&item->data) : nullptr;
        if (object == nullptr) {
            dab.refusal = "Select an object to paint copies of it";
            return dab;
        }
        if (!targets.drops || scene.physics.max_dropped == 0U) {
            dab.refusal = "This scene takes no painted things: raise Most dropped at once in Scene settings";
            return dab;
        }
        if (!droppable(targets, kind)) {
            dab.refusal = item->name + " stands still or is moved: only a Dynamic object can be painted";
            return dab;
        }
        if (brush.count == 0U || brush.count > SolidWorld::drops_per_step) {
            dab.refusal = "A dab lays down 1 to " + std::to_string(SolidWorld::drops_per_step) + " things";
            return dab;
        }
        if (dropped_count(scene.performance) + brush.count > scene.physics.max_dropped) {
            dab.refusal = "The scene holds at most " + std::to_string(scene.physics.max_dropped) +
                          " dropped and painted things at once: delete some, or raise Most dropped at once";
            return dab;
        }
        // The world bears so many births a step (the stage, at step 0, is
        // set up whole): a dab with no room left in its step goes in the
        // next step that has room.
        if (tick != 0U) {
            while (births_at(scene.performance, dab.tick) + brush.count > SolidWorld::drops_per_step) {
                ++dab.tick;
            }
        }
        const float scale = page_scale(document);
        const float above = object->size * scale * 0.5F + brush.lift * scale;
        PaintDab paint{.tick = dab.tick,
                       .object = kind,
                       .position = point + up * above,
                       .normal = up,
                       .turn = object->rotation,
                       .velocity = {},
                       .radius = brush.radius,
                       .count = brush.count,
                       .falloff = std::clamp(brush.falloff, 0.0F, 1.0F),
                       .paint_turn = brush.turn,
                       .stroke = stroke};
        const std::uint32_t serial = add_paint(scene.performance, paint);
        dab.serials.push_back(serial);
        dab.stroke = stroke != 0U ? stroke : serial;
        return dab;
    }

    // Erase, Push and Pull: the bodies whose middles are under the brush,
    // in the order they stand in the world.
    for (const StageBody& body : bodies) {
        if (!body.spawn_index) {
            continue;
        }
        const Vec3 out = body.center - point;
        const float distance = length(out);
        if (!(distance <= brush.radius)) {
            continue;
        }
        const std::optional<BodyRef> ref = body_ref(targets, *body.spawn_index);
        if (!ref || deleted(scene.performance, *ref)) {
            continue;
        }
        if (brush.mode == BrushMode::Erase) {
            dab.serials.push_back(add_delete(scene.performance, tick, *ref));
            continue;
        }
        // Along the surface, out from the brush's middle (in for Pull).
        const Vec3 along = out - up * dot(out, up);
        if (!(length(along) > 1.0e-4F)) {
            continue;
        }
        const float edge = brush.radius > 0.0F ? distance / brush.radius : 0.0F;
        const float speed = brush.strength * (1.0F - std::clamp(brush.falloff, 0.0F, 1.0F) * edge);
        const Vec3 way = normalized(along, {}) * (brush.mode == BrushMode::Pull ? -speed : speed);
        dab.serials.push_back(add_push(scene.performance, tick, *ref, way, Vec3{}));
    }
    if (dab.serials.empty()) {
        dab.refusal = brush.mode == BrushMode::Erase ? "Nothing under the brush to take away"
                                                     : "Nothing under the brush to push";
    }
    return dab;
}

// ------------------------------------------------------------------- grab

std::optional<Vec3> on_plane(const GrabPlane& plane, Vec3 origin, Vec3 direction) {
    const float facing = dot(direction, plane.normal);
    if (!(std::fabs(facing) > 1.0e-6F)) {
        return std::nullopt;
    }
    const float t = dot(plane.point - origin, plane.normal) / facing;
    if (!(t > 0.0F) || !std::isfinite(t)) {
        return std::nullopt;
    }
    return origin + direction * t;
}

GrabStart begin_grab(Document& document, const ActTargets& targets, const StageHit& hit, Quat held, Vec3 view,
                     std::uint64_t tick, float gain, float max_speed, bool firm) {
    GrabStart start;
    if (!document.scene) {
        start.refusal = "Only a scene can be acted on";
        return start;
    }
    if (!hit.on_body || !hit.spawn_index) {
        start.refusal = "Press on a thing to pick it up";
        return start;
    }
    const std::optional<BodyRef> ref = body_ref(targets, *hit.spawn_index);
    if (!ref) {
        start.refusal = "Only objects that move and dropped things can be picked up, not what stands still or "
                        "the pieces of something broken";
        return start;
    }
    if (!finite(hit.point) || !finite(hit.center) || !(gain > 0.0F) || !(max_speed > 0.0F)) {
        start.refusal = "There is nowhere to hold it";
        return start;
    }
    start.hold.plane = GrabPlane{hit.point, normalized(view, Vec3{0.0F, 0.0F, -1.0F})};
    start.hold.offset = hit.center - hit.point;
    start.hold.serial =
        add_grab(document.scene->performance, tick, *ref, hit.center, turn_of(held), gain, max_speed, firm);
    return start;
}

bool carry(Document& document, const Hold& hold, Vec3 origin, Vec3 direction, std::uint64_t tick) {
    if (!document.scene || hold.serial == 0U) {
        return false;
    }
    const std::optional<Vec3> at = on_plane(hold.plane, origin, direction);
    if (!at) {
        return false;
    }
    return extend_grab(document.scene->performance, hold.serial, tick, *at + hold.offset);
}

void push_deeper(Hold& hold, float metres) {
    if (std::isfinite(metres)) {
        hold.plane.point += hold.plane.normal * metres;
    }
}

// ------------------------------------------------------------ poke, blast

StageAct poke(Document& document, const ActTargets& targets, const StageHit& hit, Vec3 direction, float speed,
              float radius, std::uint64_t tick) {
    if (!document.scene) {
        return StageAct{0U, "Only a scene can be acted on"};
    }
    if (!hit.on_body || !hit.spawn_index) {
        return StageAct{0U, "Click on a thing to push it"};
    }
    const std::optional<BodyRef> ref = body_ref(targets, *hit.spawn_index);
    if (!ref) {
        return StageAct{0U, "Only objects that move and dropped things can be pushed, not what stands still or the "
                            "pieces of something broken"};
    }
    if (!std::isfinite(speed) || !(speed > 0.0F)) {
        return StageAct{0U, "A push needs some speed"};
    }
    const Vec3 push = normalized(direction, Vec3{0.0F, 0.0F, -1.0F}) * speed;
    // A push at a lever arm r spins it by r x push over its inertia; as a
    // solid ball of the body's radius, I / m = 2/5 r^2.
    Vec3 twist{};
    if (radius > 1.0e-4F && finite(hit.point) && finite(hit.center)) {
        twist = cross(hit.point - hit.center, push) * (1.0F / (0.4F * radius * radius));
    }
    return StageAct{add_push(document.scene->performance, tick, *ref, push, twist), {}};
}

StageAct set_off_blast(Document& document, const ActTargets& targets, Vec3 center, float radius, float power,
                       std::uint64_t tick) {
    if (!document.scene) {
        return StageAct{0U, "Only a scene can be acted on"};
    }
    if (targets.emitter.empty()) {
        return StageAct{0U, "There is nothing solid in the scene to blast"};
    }
    if (!finite(center) || !std::isfinite(radius) || !(radius > 0.0F) || !std::isfinite(power) ||
        !(power > 0.0F)) {
        return StageAct{0U, "A blast needs a reach and a strength"};
    }
    return StageAct{add_blast(document.scene->performance, tick, center, radius, power), {}};
}

// ------------------------------------------------- playing the performance

Prepared prepare_scene(const Document& document) {
    Prepared prepared;
    Compiled compiled = compile(without_performance(document), Purpose::Export);
    if (!compiled.error.empty()) {
        prepared.error = compiled.error;
        return prepared;
    }
    if (!compiled.effect) {
        prepared.error = "Nothing in the scene plays";
        return prepared;
    }
    if (!compiled.requests.empty()) {
        prepared.error = "The scene uses a model file (" + compiled.requests.front().path +
                         "), which only the Studio window reads";
        return prepared;
    }
    std::string issue;
    make_cuts(compiled, issue);
    if (!issue.empty()) {
        prepared.error = issue;
        return prepared;
    }
    prepared.targets = act_targets(compiled);
    prepared.compiled = std::move(compiled);
    return prepared;
}

Played play_scene(const Document& document, const Prepared& prepared, const RigidSolver* solver,
                  std::uint32_t seed, std::uint64_t to) {
    Played played;
    if (!prepared.compiled || !document.scene) {
        played.error = prepared.error.empty() ? "The scene is not compiled" : prepared.error;
        return played;
    }
    const Compiled& compiled = *prepared.compiled;
    auto created = Simulation::create(*compiled.effect, seed,
                                      SimulationOptions{.fields = compiled.fields,
                                                        .meshes = compiled.meshes,
                                                        .rigid = solver});
    if (!created) {
        played.error = created.error().message;
        return played;
    }
    played.simulation.emplace(std::move(created).value());
    played.world = solid_world_index(compiled);
    const std::shared_ptr<const ActionList> actions =
        performance_actions(document.scene->performance, prepared.targets, &played.skipped);
    played.simulation->reset(nullptr, actions.get());
    played.simulation->advance_to(to, nullptr, actions.get());
    return played;
}

void play_on(Played& played, const Document& document, const Prepared& prepared, std::uint64_t to) {
    if (!played.simulation || !document.scene) {
        return;
    }
    const std::shared_ptr<const ActionList> actions =
        performance_actions(document.scene->performance, prepared.targets, &played.skipped);
    played.simulation->advance_to(to, nullptr, actions.get());
}

std::vector<WorldBody> world_bodies(const Played& played, const Prepared& prepared) {
    std::vector<WorldBody> bodies;
    if (!played.simulation || !played.world || !prepared.compiled) {
        return bodies;
    }
    const EmitterState& s = played.simulation->emitter(*played.world);
    const RigidPhysics* physics = rigid_physics(prepared.compiled->effect->emitters[*played.world]);
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        const std::uint32_t kind = i < s.body_kind.size() ? s.body_kind[i] : 0U;
        // A body's size is in its kind's units; the kind's scale makes it
        // metres, as the renderer and picking take it.
        const float scale = physics != nullptr && kind >= 1U && kind <= physics->bodies.size()
                                ? physics->bodies[kind - 1U].scale
                                : 1.0F;
        bodies.push_back(WorldBody{.spawn_index = s.spawn_index[i],
                                   .ref = body_ref(prepared.targets, s.spawn_index[i]),
                                   .position = s.position[i],
                                   .orientation = s.orientation[i],
                                   .velocity = s.velocity[i],
                                   .size = s.size[i] * scale,
                                   .kind = kind});
    }
    return bodies;
}

std::optional<WorldBody> find_body(const Played& played, const Prepared& prepared, const BodyRef& ref) {
    const std::optional<std::uint32_t> index = body_index(prepared.targets, ref);
    if (!index) {
        return std::nullopt;
    }
    for (const WorldBody& body : world_bodies(played, prepared)) {
        if (body.spawn_index == *index) {
            return body;
        }
    }
    return std::nullopt;
}

std::vector<StageBody> played_stage(const Played& played, const Prepared& prepared) {
    if (!played.simulation || !played.world || !prepared.compiled) {
        return {};
    }
    return stage_bodies(*prepared.compiled, played.simulation->emitter(*played.world));
}

std::uint64_t state_digest(const Simulation& simulation) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    mix(hash, simulation.tick());
    for (std::size_t e = 0U; e < simulation.effect().emitters.size(); ++e) {
        const EmitterState& s = simulation.emitter(e);
        mix(hash, static_cast<std::uint64_t>(s.alive()));
        mix(hash, s.spawned);
        mix(hash, static_cast<std::uint64_t>(s.refused));
        for (std::size_t i = 0U; i < s.alive(); ++i) {
            mix(hash, static_cast<std::uint64_t>(s.spawn_index[i]));
            mix(hash, s.position[i]);
            mix(hash, s.velocity[i]);
            mix(hash, s.orientation[i].x);
            mix(hash, s.orientation[i].y);
            mix(hash, s.orientation[i].z);
            mix(hash, s.orientation[i].w);
            mix(hash, s.angular_velocity[i]);
            mix(hash, s.age[i]);
            mix(hash, s.size[i]);
            mix(hash, s.color[i].r);
            mix(hash, s.color[i].g);
            mix(hash, s.color[i].b);
            mix(hash, s.color[i].a);
        }
    }
    return hash;
}

} // namespace ajfx::studio
