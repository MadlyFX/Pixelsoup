#include "performance.hpp"

#include "stage.hpp"

#include <algorithm>
#include <cmath>
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

std::uint32_t append(Performance& performance, Act act) {
    act.serial = performance.next_serial++;
    performance.acts.insert(place_for(performance.acts, act.tick), act);
    return act.serial;
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
        if (act.kind == ActKind::Drop) {
            const auto kind = targets.kinds.find(act.object);
            if (!targets.drops || kind == targets.kinds.end()) {
                ++left_out;
                continue;
            }
            action.kind = ActionKind::Spawn;
            action.serial = act.serial;
            action.body = kind->second;
            action.position = act.position;
            action.orientation = prop_orientation(act.turn);
            action.velocity = act.velocity;
            action.release = act.tick;
        } else {
            const std::optional<std::uint32_t> target = body_index(targets, act.target);
            if (!target) {
                ++left_out;
                continue;
            }
            action.kind = ActionKind::Remove;
            action.target = *target;
        }
        list->add(targets.emitter, action);
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

bool remove_act(Performance& performance, std::uint32_t serial) {
    const Act* act = find_act(performance, serial);
    if (act == nullptr) {
        return false;
    }
    const bool drop = act->kind == ActKind::Drop;
    std::erase_if(performance.acts, [serial, drop](const Act& a) {
        return a.serial == serial || (drop && a.kind == ActKind::Delete && a.target.drop == serial);
    });
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
    moved.tick = tick;
    performance.acts.erase(at);
    performance.acts.insert(place_for(performance.acts, tick), moved);
    return true;
}

void forget_object(Performance& performance, Uid object) {
    std::set<std::uint32_t> drops;
    for (const Act& act : performance.acts) {
        if (act.kind == ActKind::Drop && act.object == object) {
            drops.insert(act.serial);
        }
    }
    std::erase_if(performance.acts, [&](const Act& a) {
        return (a.kind == ActKind::Drop && a.object == object) ||
               (a.kind == ActKind::Delete && (a.target.object == object || drops.contains(a.target.drop)));
    });
}

const Act* find_act(const Performance& performance, std::uint32_t serial) {
    const auto at = std::find_if(performance.acts.begin(), performance.acts.end(),
                                 [serial](const Act& a) { return a.serial == serial; });
    return at == performance.acts.end() ? nullptr : &*at;
}

std::optional<std::uint64_t> first_difference(const Performance& before, const Performance& after) {
    // Both are in tick order and agree up to the first place they differ,
    // so everything before the earlier of the two ticks there is the same.
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

std::size_t dropped_count(const Performance& performance) {
    std::size_t drops = 0U;
    std::set<std::uint32_t> deleted;
    for (const Act& act : performance.acts) {
        if (act.kind == ActKind::Drop) {
            ++drops;
        } else if (act.target.drop != 0U) {
            deleted.insert(act.target.drop);
        }
    }
    return drops - std::min(drops, deleted.size());
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
        return "more than " + std::to_string(max_acts) + " drops and deletions";
    }
    if (performance.next_serial == 0U || performance.next_serial > spawn_count_mask) {
        return "invalid serial allocator";
    }
    std::set<std::uint32_t> serials;
    std::uint64_t last = 0U;
    for (const Act& act : performance.acts) {
        if (act.serial == 0U || act.serial >= performance.next_serial || !serials.insert(act.serial).second) {
            return "an act with an invalid or repeated serial";
        }
        if (act.tick < last) {
            return "acts out of order";
        }
        last = act.tick;
        if (act.kind == ActKind::Drop) {
            if (act.object == 0U || !finite(act.position) || !finite(act.turn) || !finite(act.velocity)) {
                return "an invalid drop";
            }
        } else if (act.kind == ActKind::Delete) {
            if ((act.target.object == 0U) == (act.target.drop == 0U) || act.target.count > spawn_count_mask) {
                return "a deletion that names no one body";
            }
        } else {
            return "an act of an unknown kind";
        }
    }
    return {};
}

} // namespace ajfx::studio
