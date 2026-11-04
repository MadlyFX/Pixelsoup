#include "performance.hpp"

#include "stage.hpp"

#include <ajfx/random.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>
#include <utility>
#include <variant>

namespace ajfx::studio {
namespace {

ModuleId spawn_id(const SpawnModule& module) {
    return std::visit(
        [](const auto& typed) -> ModuleId {
            if constexpr (requires { typed.id; }) {
                return typed.id;
            } else {
                return 0U;
            }
        },
        module);
}

bool finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Where an act at `tick` goes: after every act at or before it.
std::vector<Act>::iterator place_for(std::vector<Act>& acts, std::uint64_t tick) {
    return std::upper_bound(acts.begin(), acts.end(), tick,
                            [](std::uint64_t t, const Act& act) { return t < act.tick; });
}

// How many serials an act holds: one for each thing a dab of paint lays
// down, one for any other act.
std::uint32_t serials_of(const Act& act) {
    return act.kind == ActKind::Paint ? std::max(act.count, 1U) : 1U;
}

std::uint32_t append(Performance& performance, Act act) {
    act.serial = performance.next_serial;
    performance.next_serial += serials_of(act);
    performance.acts.insert(place_for(performance.acts, act.tick), act);
    return act.serial;
}

bool targets_body(const Act& act) {
    return act.kind == ActKind::Delete || act.kind == ActKind::Push || act.kind == ActKind::Grab;
}

bool makes_bodies(const Act& act) {
    return act.kind == ActKind::Drop || act.kind == ActKind::Paint;
}

// Whether `act` names a body `maker` made.
bool on_body_of(const Act& act, const Act& maker) {
    return targets_body(act) && act.target.drop != 0U && makes_body(maker, act.target.drop);
}

Vec3 normalized(Vec3 v, Vec3 fallback) {
    const float length = std::sqrt(dot(v, v));
    return length > 1.0e-6F && std::isfinite(length) ? v * (1.0F / length) : fallback;
}

// The turn taking straight up to `n` (unit length) the shortest way.
Quat up_to(Vec3 n) {
    const Vec3 up{0.0F, 1.0F, 0.0F};
    const float c = dot(up, n);
    if (c > 1.0F - 1.0e-6F) {
        return {};
    }
    if (c < -1.0F + 1.0e-6F) {
        return Quat{1.0F, 0.0F, 0.0F, 0.0F};
    }
    const Vec3 axis = normalized(cross(up, n), Vec3{1.0F, 0.0F, 0.0F});
    const float half = 0.5F * std::acos(std::clamp(c, -1.0F, 1.0F));
    const float s = std::sin(half);
    return Quat{axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
}

// What a paint act's draws are keyed by, beside the thing's serial: fixed,
// so a dab's things land where they did whenever it is replayed.
constexpr std::uint32_t paint_seed = 0x7061696eU;
constexpr float two_pi = 6.28318530718F;

// Where a grab steers at `tick`: along the line between the samples either
// side of it, or the last sample's point from there on.
Vec3 grab_point_at(const std::vector<GrabSample>& path, std::uint64_t tick) {
    const auto after = std::upper_bound(path.begin(), path.end(), tick,
                                        [](std::uint64_t t, const GrabSample& s) { return t < s.tick; });
    if (after == path.begin()) {
        return path.front().point;
    }
    const GrabSample& from = *(after - 1);
    if (after == path.end() || from.tick == tick) {
        return from.point;
    }
    const float t = static_cast<float>(tick - from.tick) / static_cast<float>(after->tick - from.tick);
    return from.point + (after->point - from.point) * t;
}

// The first step from which two acts of one serial act differently; nothing
// when they act alike. A grab that only goes on further acts alike up to
// its old last sample.
std::optional<std::uint64_t> act_difference(const Act& before, const Act& after) {
    if (before == after) {
        return std::nullopt;
    }
    if (before.kind == ActKind::Grab && after.kind == ActKind::Grab && before.tick == after.tick &&
        !before.path.empty() && !after.path.empty()) {
        Act a = before;
        Act b = after;
        a.path.clear();
        b.path.clear();
        if (a == b) {
            const std::size_t common = std::min(before.path.size(), after.path.size());
            std::size_t i = 0U;
            while (i < common && before.path[i] == after.path[i]) {
                ++i;
            }
            // Steering after the last common sample follows the next one,
            // whichever path that is.
            return i == 0U ? before.tick : before.path[i - 1U].tick + 1U;
        }
    }
    return std::min(before.tick, after.tick);
}

} // namespace

ActTargets act_targets(const Compiled& compiled) {
    ActTargets targets;
    const std::optional<std::size_t> world = solid_world_index(compiled);
    if (!world) {
        return targets;
    }
    const Emitter& emitter = compiled.effect->emitters[*world];
    targets.emitter = emitter.name;
    targets.drops = compiled.solids.drops;
    targets.max_dropped = compiled.solids.max_dropped;
    // A scene's Object is a layer of its own; the spawn module its block
    // made is where its own bodies come from, and the kind it bears is the
    // Object's.
    for (const LayerReport& report : compiled.layers) {
        if (!report.included || report.emitter != targets.emitter) {
            continue;
        }
        for (const ModuleLine& line : report.modules) {
            const auto* spawn = std::get_if<SpawnModule>(&line.module);
            if (spawn == nullptr) {
                continue;
            }
            const ModuleId id = spawn_id(*spawn);
            for (std::uint32_t s = 0U; s < emitter.spawn.size(); ++s) {
                if (spawn_id(emitter.spawn[s]) != id || std::holds_alternative<SpawnActions>(emitter.spawn[s])) {
                    continue;
                }
                const std::uint32_t kind = std::visit([](const auto& m) { return spawn_body(m); }, emitter.spawn[s]);
                if (!targets.own.contains(report.layer)) {
                    targets.own[report.layer] = s;
                    targets.kinds[report.layer] = kind;
                }
                targets.owners[s] = report.layer;
                break;
            }
        }
    }
    return targets;
}

bool droppable(const ActTargets& targets, Uid object) {
    return targets.drops.has_value() && targets.kinds.contains(object);
}

std::optional<std::uint32_t> body_index(const ActTargets& targets, const BodyRef& ref) {
    if (ref.drop != 0U) {
        if (!targets.drops) {
            return std::nullopt;
        }
        return spawn_index_of(*targets.drops, ref.drop);
    }
    const auto own = targets.own.find(ref.object);
    if (own == targets.own.end()) {
        return std::nullopt;
    }
    return spawn_index_of(own->second, ref.count);
}

std::optional<BodyRef> body_ref(const ActTargets& targets, std::uint32_t spawn_index) {
    const std::uint32_t source = spawn_source(spawn_index);
    const std::uint32_t count = spawn_index & spawn_count_mask;
    if (targets.drops && source == *targets.drops) {
        return BodyRef{.object = 0U, .count = 0U, .drop = count};
    }
    if (const auto owner = targets.owners.find(source); owner != targets.owners.end()) {
        return BodyRef{.object = owner->second, .count = count, .drop = 0U};
    }
    return std::nullopt;
}

std::shared_ptr<const ActionList> performance_actions(const Performance& performance, const ActTargets& targets,
                                                      std::size_t* skipped) {
    auto list = std::make_shared<ActionList>();
    std::size_t left_out = 0U;
    for (const Act& act : performance.acts) {
        Action action;
        action.tick = act.tick;
        if (makes_bodies(act)) {
            const auto kind = targets.kinds.find(act.object);
            if (!targets.drops || kind == targets.kinds.end()) {
                ++left_out;
                continue;
            }
            action.kind = ActionKind::Spawn;
            action.body = kind->second;
            action.velocity = act.velocity;
            action.release = act.tick;
            if (act.kind == ActKind::Drop) {
                action.serial = act.serial;
                action.position = act.position;
                action.orientation = prop_orientation(act.turn);
                list->add(targets.emitter, action);
            } else {
                for (const PaintedThing& thing : painted_things(act)) {
                    action.serial = thing.serial;
                    action.position = thing.position;
                    action.orientation = thing.orientation;
                    list->add(targets.emitter, action);
                }
            }
            continue;
        }
        if (act.kind == ActKind::Blast) {
            if (targets.emitter.empty()) {
                ++left_out;
                continue;
            }
            action.kind = ActionKind::Blast;
            action.position = act.position;
            action.radius = act.radius;
            action.power = act.power;
            list->add(targets.emitter, action);
            continue;
        }
        const std::optional<std::uint32_t> target = body_index(targets, act.target);
        if (!target) {
            ++left_out;
            continue;
        }
        action.target = *target;
        if (act.kind == ActKind::Delete) {
            action.kind = ActionKind::Remove;
            list->add(targets.emitter, action);
        } else if (act.kind == ActKind::Push) {
            action.kind = ActionKind::Push;
            action.push = act.push;
            action.twist = act.twist;
            list->add(targets.emitter, action);
        } else if (act.kind == ActKind::Grab && !act.path.empty()) {
            action.kind = ActionKind::Steer;
            action.orientation = prop_orientation(act.turn);
            action.gain = act.gain;
            action.max_speed = act.max_speed;
            for (std::uint64_t tick = act.path.front().tick; tick <= act.path.back().tick; ++tick) {
                action.tick = tick;
                action.position = grab_point_at(act.path, tick);
                list->add(targets.emitter, action);
            }
        }
    }
    if (skipped != nullptr) {
        *skipped = left_out;
    }
    return list;
}

std::uint32_t add_drop(Performance& performance, std::uint64_t tick, Uid object, Vec3 position, Vec3 turn,
                       Vec3 velocity) {
    return append(performance, Act{.serial = 0U,
                                   .kind = ActKind::Drop,
                                   .tick = tick,
                                   .object = object,
                                   .position = position,
                                   .turn = turn,
                                   .velocity = velocity,
                                   .target = {}});
}

std::uint32_t add_delete(Performance& performance, std::uint64_t tick, BodyRef target) {
    return append(performance, Act{.serial = 0U, .kind = ActKind::Delete, .tick = tick, .target = target});
}

std::uint32_t add_push(Performance& performance, std::uint64_t tick, BodyRef target, Vec3 push, Vec3 twist) {
    Act act{.serial = 0U, .kind = ActKind::Push, .tick = tick, .target = target};
    act.push = push;
    act.twist = twist;
    return append(performance, act);
}

std::uint32_t add_blast(Performance& performance, std::uint64_t tick, Vec3 center, float radius, float power) {
    Act act{.serial = 0U, .kind = ActKind::Blast, .tick = tick, .position = center};
    act.radius = radius;
    act.power = power;
    return append(performance, act);
}

std::uint32_t add_grab(Performance& performance, std::uint64_t tick, BodyRef target, Vec3 point, Vec3 turn,
                       float gain, float max_speed) {
    Act act{.serial = 0U, .kind = ActKind::Grab, .tick = tick, .turn = turn, .target = target};
    act.path = {GrabSample{tick, point}};
    act.gain = gain;
    act.max_speed = max_speed;
    return append(performance, act);
}

bool extend_grab(Performance& performance, std::uint32_t serial, std::uint64_t tick, Vec3 point) {
    const auto at = std::find_if(performance.acts.begin(), performance.acts.end(),
                                 [serial](const Act& a) { return a.serial == serial; });
    if (at == performance.acts.end() || at->kind != ActKind::Grab || at->path.empty() ||
        tick < at->path.back().tick || !finite(point)) {
        return false;
    }
    if (tick == at->path.back().tick) {
        at->path.back().point = point;
    } else {
        at->path.push_back(GrabSample{tick, point});
    }
    return true;
}

std::uint32_t add_paint(Performance& performance, const PaintDab& dab) {
    Act act{.serial = 0U,
            .kind = ActKind::Paint,
            .tick = dab.tick,
            .object = dab.object,
            .position = dab.position,
            .turn = dab.turn,
            .velocity = dab.velocity};
    act.normal = normalized(dab.normal, Vec3{0.0F, 1.0F, 0.0F});
    act.radius = dab.radius;
    act.count = std::max(dab.count, 1U);
    act.falloff = dab.falloff;
    act.paint_turn = dab.paint_turn;
    act.stroke = dab.stroke != 0U ? dab.stroke : performance.next_serial;
    return append(performance, act);
}

std::vector<PaintedThing> painted_things(const Act& paint) {
    std::vector<PaintedThing> things;
    if (paint.kind != ActKind::Paint) {
        return things;
    }
    const Vec3 n = normalized(paint.normal, Vec3{0.0F, 1.0F, 0.0F});
    const Vec3 across = normalized(cross(std::fabs(n.y) < 0.9F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F}, n),
                                   Vec3{1.0F, 0.0F, 0.0F});
    const Vec3 along = cross(n, across);
    const Quat own = prop_orientation(paint.turn);
    // 0.5 spreads evenly over the disc; higher powers gather to the middle.
    const float gather = 0.5F + std::clamp(paint.falloff, 0.0F, 1.0F);
    for (std::uint32_t k = 0U; k < std::max(paint.count, 1U); ++k) {
        const std::uint32_t serial = paint.serial + k;
        const float out = paint.radius * std::pow(particle_unit(paint_seed, serial, 0U), gather);
        const float round = two_pi * particle_unit(paint_seed, serial, 1U);
        PaintedThing thing{.serial = serial,
                           .position = paint.position + across * (out * std::cos(round)) +
                                       along * (out * std::sin(round)),
                           .orientation = own};
        if (paint.paint_turn == PaintTurn::Realistic) {
            const float half = 0.5F * two_pi * particle_unit(paint_seed, serial, 2U);
            const Quat about_up{0.0F, std::sin(half), 0.0F, std::cos(half)};
            thing.orientation = times(up_to(n), times(about_up, own));
        } else if (paint.paint_turn == PaintTurn::Random) {
            // Shoemake's uniform turn from three draws.
            const float u1 = particle_unit(paint_seed, serial, 2U);
            const float u2 = two_pi * particle_unit(paint_seed, serial, 3U);
            const float u3 = two_pi * particle_unit(paint_seed, serial, 4U);
            const float a = std::sqrt(1.0F - u1);
            const float b = std::sqrt(u1);
            thing.orientation = Quat{a * std::sin(u2), a * std::cos(u2), b * std::sin(u3), b * std::cos(u3)};
        }
        things.push_back(thing);
    }
    return things;
}

std::uint64_t act_end(const Act& act) {
    return act.kind == ActKind::Grab && !act.path.empty() ? act.path.back().tick : act.tick;
}

bool makes_body(const Act& act, std::uint32_t drop) {
    if (act.kind == ActKind::Drop) {
        return act.serial == drop;
    }
    return act.kind == ActKind::Paint && drop >= act.serial && drop - act.serial < std::max(act.count, 1U);
}

const Act* maker_of(const Performance& performance, std::uint32_t drop) {
    const auto at = std::find_if(performance.acts.begin(), performance.acts.end(),
                                 [drop](const Act& a) { return makes_body(a, drop); });
    return at == performance.acts.end() ? nullptr : &*at;
}

Uid object_of(const Performance& performance, const BodyRef& body) {
    if (body.drop == 0U) {
        return body.object;
    }
    const Act* maker = maker_of(performance, body.drop);
    return maker != nullptr ? maker->object : 0U;
}

bool remove_act(Performance& performance, std::uint32_t serial) {
    const Act* act = find_act(performance, serial);
    if (act == nullptr) {
        return false;
    }
    const Act maker = *act;
    std::erase_if(performance.acts,
                  [&maker](const Act& a) { return a.serial == maker.serial || on_body_of(a, maker); });
    return true;
}

bool retime_act(Performance& performance, std::uint32_t serial, std::uint64_t tick) {
    const auto at = std::find_if(performance.acts.begin(), performance.acts.end(),
                                 [serial](const Act& a) { return a.serial == serial; });
    if (at == performance.acts.end()) {
        return false;
    }
    if (at->tick == tick) {
        return true;
    }
    Act moved = *at;
    // A grab moves whole, every sample by as much as its first.
    for (GrabSample& sample : moved.path) {
        sample.tick = sample.tick - moved.tick + tick;
    }
    moved.tick = tick;
    performance.acts.erase(at);
    performance.acts.insert(place_for(performance.acts, tick), moved);
    return true;
}

std::vector<std::uint32_t> stroke_acts(const Performance& performance, std::uint32_t stroke) {
    std::vector<std::uint32_t> serials;
    for (const Act& act : performance.acts) {
        if (act.kind == ActKind::Paint && act.stroke == stroke) {
            serials.push_back(act.serial);
        }
    }
    return serials;
}

bool remove_stroke(Performance& performance, std::uint32_t stroke) {
    const std::vector<std::uint32_t> serials = stroke_acts(performance, stroke);
    for (const std::uint32_t serial : serials) {
        remove_act(performance, serial);
    }
    return !serials.empty();
}

bool retime_stroke(Performance& performance, std::uint32_t stroke, std::uint64_t tick) {
    const std::vector<std::uint32_t> serials = stroke_acts(performance, stroke);
    if (serials.empty()) {
        return false;
    }
    const std::uint64_t first = find_act(performance, serials.front())->tick;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> moves;
    for (const std::uint32_t serial : serials) {
        moves.emplace_back(serial, find_act(performance, serial)->tick - first + tick);
    }
    // Each dab goes after whatever already stands at its new step, so moving
    // them in the order they were laid down keeps that order.
    for (const auto& [serial, to] : moves) {
        retime_act(performance, serial, to);
    }
    return true;
}

std::size_t rerecord_from(Performance& performance, std::uint64_t tick) {
    std::size_t changed = 0U;
    std::vector<Act> kept;
    for (Act& act : performance.acts) {
        if (act.tick >= tick) {
            ++changed;
            continue;
        }
        if (act.kind == ActKind::Grab && act_end(act) >= tick) {
            std::erase_if(act.path, [tick](const GrabSample& s) { return s.tick >= tick; });
            ++changed;
        }
        kept.push_back(std::move(act));
    }
    performance.acts = std::move(kept);
    return changed;
}

void forget_object(Performance& performance, Uid object) {
    std::vector<Act> makers;
    for (const Act& act : performance.acts) {
        if (makes_bodies(act) && act.object == object) {
            makers.push_back(act);
        }
    }
    std::erase_if(performance.acts, [&](const Act& a) {
        if (makes_bodies(a) && a.object == object) {
            return true;
        }
        if (!targets_body(a)) {
            return false;
        }
        return a.target.object == object ||
               std::any_of(makers.begin(), makers.end(), [&a](const Act& m) { return on_body_of(a, m); });
    });
}

const Act* find_act(const Performance& performance, std::uint32_t serial) {
    const auto at = std::find_if(performance.acts.begin(), performance.acts.end(),
                                 [serial](const Act& a) { return a.serial == serial; });
    return at == performance.acts.end() ? nullptr : &*at;
}

std::optional<std::uint64_t> first_difference(const Performance& before, const Performance& after) {
    // Act for act, when both hold the same acts in the same order (what a
    // hand still holding a body, or one act changed in place, leaves): the
    // earliest step any of them acts differently from.
    if (before.acts.size() == after.acts.size() &&
        std::equal(before.acts.begin(), before.acts.end(), after.acts.begin(),
                   [](const Act& a, const Act& b) { return a.serial == b.serial; })) {
        std::optional<std::uint64_t> first;
        for (std::size_t i = 0U; i < before.acts.size(); ++i) {
            if (const std::optional<std::uint64_t> at = act_difference(before.acts[i], after.acts[i])) {
                first = first ? std::min(*first, *at) : *at;
            }
        }
        return first;
    }
    // Otherwise both are in tick order and agree up to the first place they
    // differ, so everything before the earlier of the two ticks there is the
    // same.
    const std::size_t common = std::min(before.acts.size(), after.acts.size());
    for (std::size_t i = 0U; i < common; ++i) {
        if (!(before.acts[i] == after.acts[i])) {
            return std::min(before.acts[i].tick, after.acts[i].tick);
        }
    }
    if (before.acts.size() > common) {
        return before.acts[common].tick;
    }
    if (after.acts.size() > common) {
        return after.acts[common].tick;
    }
    return std::nullopt;
}

Document without_performance(Document document) {
    if (document.scene) {
        document.scene->performance = Performance{};
    }
    return document;
}

std::string performance_summary(const Performance& performance) {
    std::size_t drops = 0U;
    std::size_t deletions = 0U;
    std::size_t pushes = 0U;
    std::size_t blasts = 0U;
    std::size_t grabs = 0U;
    std::set<std::uint32_t> strokes;
    for (const Act& act : performance.acts) {
        switch (act.kind) {
        case ActKind::Drop: ++drops; break;
        case ActKind::Delete: ++deletions; break;
        case ActKind::Push: ++pushes; break;
        case ActKind::Blast: ++blasts; break;
        case ActKind::Grab: ++grabs; break;
        case ActKind::Paint: strokes.insert(act.stroke); break;
        }
    }
    std::string out;
    const auto count = [&out](std::size_t n, const char* one, const char* many) {
        if (n == 0U) {
            return;
        }
        out += (out.empty() ? "" : ", ") + std::to_string(n) + " " + (n == 1U ? one : many);
    };
    count(drops, "drop", "drops");
    count(strokes.size(), "paint stroke", "paint strokes");
    count(grabs, "grab", "grabs");
    count(pushes, "push", "pushes");
    count(blasts, "blast", "blasts");
    count(deletions, "deletion", "deletions");
    return out;
}

std::size_t dropped_count(const Performance& performance) {
    std::size_t made = 0U;
    std::set<std::uint32_t> deleted;
    for (const Act& act : performance.acts) {
        if (makes_bodies(act)) {
            made += serials_of(act);
        } else if (act.kind == ActKind::Delete && act.target.drop != 0U) {
            deleted.insert(act.target.drop);
        }
    }
    return made - std::min(made, deleted.size());
}

StageAct act_on_stage(Document& document, const ActTargets& targets, StageTool tool, Uid selected,
                      const StageHit& hit, std::uint64_t tick) {
    if (!document.scene) {
        return StageAct{0U, "Only a scene can be acted on"};
    }
    Scene& scene = *document.scene;
    if (tool == StageTool::Place) {
        const SceneItem* item = scene.item(selected);
        const auto* object = item != nullptr ? std::get_if<Object>(&item->data) : nullptr;
        if (object == nullptr) {
            return StageAct{0U, "Select an object to place copies of it"};
        }
        const float scale = std::isfinite(document.scale) && document.scale > 0.0F ? document.scale : 1.0F;
        // Standing where it lands: its middle half its size above the point.
        const Vec3 at = hit.point + Vec3{0.0F, object->size * scale * 0.5F, 0.0F};
        return place(document, targets, selected, at, object->rotation);
    }
    if (tool == StageTool::Drop) {
        const SceneItem* item = scene.item(selected);
        const auto* object = item != nullptr ? std::get_if<Object>(&item->data) : nullptr;
        if (object == nullptr) {
            return StageAct{0U, "Select an object to drop copies of it"};
        }
        if (!targets.drops || scene.physics.max_dropped == 0U) {
            return StageAct{0U, "This scene takes no drops: raise Most dropped at once in Scene settings"};
        }
        if (!droppable(targets, selected)) {
            return StageAct{0U, item->name + " stands still or is moved: only a Dynamic object can be dropped"};
        }
        if (dropped_count(scene.performance) >= scene.physics.max_dropped) {
            return StageAct{0U, "The scene holds at most " + std::to_string(scene.physics.max_dropped) +
                                    " dropped things at once: delete some, or raise Most dropped at once"};
        }
        const float scale = std::isfinite(document.scale) && document.scale > 0.0F ? document.scale : 1.0F;
        const Vec3 at = hit.point + Vec3{0.0F, drop_height * scale + object->size * scale * 0.5F, 0.0F};
        return StageAct{add_drop(scene.performance, tick, selected, at, object->rotation, Vec3{}), {}};
    }
    if (tool == StageTool::Delete) {
        if (!hit.on_body) {
            return StageAct{0U, "Click on a thing to delete it"};
        }
        const std::optional<BodyRef> ref = hit.spawn_index ? body_ref(targets, *hit.spawn_index) : std::nullopt;
        if (!ref) {
            return StageAct{0U, "Only objects that move and dropped things can be deleted, not what stands still "
                                "or the pieces of something broken"};
        }
        return StageAct{add_delete(scene.performance, tick, *ref), {}};
    }
    return StageAct{0U, {}};
}

std::string validate_performance(const Scene& scene) {
    const Performance& performance = scene.performance;
    if (performance.acts.size() > max_acts) {
        return "more than " + std::to_string(max_acts) + " acts";
    }
    if (performance.next_serial == 0U || performance.next_serial > spawn_count_mask) {
        return "invalid serial allocator";
    }
    // The serials held so far, as [first, end) by their first.
    std::map<std::uint32_t, std::uint32_t> held;
    std::uint64_t last = 0U;
    std::uint64_t steps = 0U;
    for (const Act& act : performance.acts) {
        const std::uint32_t end = act.serial + serials_of(act);
        if (act.serial == 0U || end < act.serial || end > performance.next_serial) {
            return "an act with an invalid or repeated serial";
        }
        const auto next = held.lower_bound(act.serial);
        if ((next != held.end() && next->first < end) ||
            (next != held.begin() && std::prev(next)->second > act.serial)) {
            return "an act with an invalid or repeated serial";
        }
        held.emplace(act.serial, end);
        if (act.tick < last) {
            return "acts out of order";
        }
        last = act.tick;
        const bool names_body =
            (act.target.object == 0U) != (act.target.drop == 0U) && act.target.count <= spawn_count_mask;
        switch (act.kind) {
        case ActKind::Drop:
            if (act.object == 0U || !finite(act.position) || !finite(act.turn) || !finite(act.velocity)) {
                return "an invalid drop";
            }
            break;
        case ActKind::Delete:
            if (!names_body) {
                return "a deletion that names no one body";
            }
            break;
        case ActKind::Push:
            if (!names_body || !finite(act.push) || !finite(act.twist)) {
                return "an invalid push";
            }
            break;
        case ActKind::Blast:
            if (!finite(act.position) || !std::isfinite(act.radius) || !(act.radius > 0.0F) ||
                !std::isfinite(act.power) || !(act.power > 0.0F)) {
                return "an invalid blast";
            }
            break;
        case ActKind::Grab:
            if (!names_body || act.path.empty() || act.path.front().tick != act.tick || !finite(act.turn) ||
                !std::isfinite(act.gain) || !(act.gain > 0.0F) || !std::isfinite(act.max_speed) ||
                !(act.max_speed > 0.0F)) {
                return "an invalid grab";
            }
            for (std::size_t i = 0U; i < act.path.size(); ++i) {
                if (!finite(act.path[i].point) || (i > 0U && act.path[i].tick <= act.path[i - 1U].tick)) {
                    return "a grab whose path goes back in time";
                }
            }
            steps += act.path.back().tick - act.tick + 1U;
            if (steps > max_grab_steps) {
                return "grabs holding for more than " + std::to_string(max_grab_steps) + " steps in all";
            }
            break;
        case ActKind::Paint:
            if (act.object == 0U || !finite(act.position) || !finite(act.turn) || !finite(act.velocity) ||
                !finite(act.normal) || !(dot(act.normal, act.normal) > 0.5F) || !std::isfinite(act.radius) ||
                act.radius < 0.0F || act.count == 0U || act.count > SolidWorld::drops_per_step ||
                !(act.falloff >= 0.0F && act.falloff <= 1.0F) || act.stroke == 0U || act.stroke > act.serial) {
                return "an invalid dab of paint";
            }
            break;
        default:
            return "an act of an unknown kind";
        }
    }
    return {};
}

} // namespace ajfx::studio
