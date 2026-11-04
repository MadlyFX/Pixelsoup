#include "issues.hpp"

#include "performance.hpp"
#include "scene_stats.hpp"
#include "stage_pick.hpp"

#include <ajfx/cache.hpp>

#include <algorithm>
#include <charconv>
#include <map>
#include <set>

namespace ajfx::studio {
namespace {

std::string item_name(const Scene& scene, Uid uid) {
    const SceneItem* item = scene.item(uid);
    if (item == nullptr) {
        return "item " + std::to_string(uid);
    }
    return item->name.empty() ? default_item_name(*item) : item->name;
}

const Act* act_of(const Performance& performance, std::uint32_t serial) {
    for (const Act& act : performance.acts) {
        if (act.serial == serial) {
            return &act;
        }
    }
    return nullptr;
}

std::string_view act_word(ActKind kind) {
    switch (kind) {
    case ActKind::Drop:
        return "Drop";
    case ActKind::Delete:
        return "Delete";
    case ActKind::Push:
        return "Poke";
    case ActKind::Blast:
        return "Blast";
    case ActKind::Grab:
        return "Grab";
    case ActKind::Paint:
        return "Paint";
    }
    return "Act";
}

// The share of a budget used, as a warning once it passes the warning share.
void budget(std::vector<Issue>& out, IssueKind kind, std::size_t used, std::size_t limit, std::string what,
            std::string advice) {
    if (limit == 0U || static_cast<float>(used) < budget_warning_share * static_cast<float>(limit)) {
        return;
    }
    out.push_back(Issue{.level = used > limit ? IssueLevel::Problem : IssueLevel::Warning,
                        .kind = kind,
                        .message = what + ": " + std::to_string(used) + " of " + std::to_string(limit) + ". " +
                                   advice});
}

} // namespace

std::string_view issue_level_name(IssueLevel level) {
    switch (level) {
    case IssueLevel::Note:
        return "note";
    case IssueLevel::Warning:
        return "warning";
    case IssueLevel::Problem:
        return "problem";
    }
    return "note";
}

std::string_view issue_kind_name(IssueKind kind) {
    switch (kind) {
    case IssueKind::Scene:
        return "scene";
    case IssueKind::Unapplied:
        return "act not applied";
    case IssueKind::RefusedDrop:
        return "refused drop";
    case IssueKind::DropBudget:
        return "dropped things";
    case IssueKind::ParticlesRefused:
        return "out of room";
    case IssueKind::KindBudget:
        return "body kinds";
    case IssueKind::PropBudget:
        return "props";
    case IssueKind::WindIdle:
        return "wind";
    case IssueKind::CacheStale:
        return "stale cache";
    case IssueKind::CacheUnbaked:
        return "not baked";
    case IssueKind::CacheTooLarge:
        return "too large to bake";
    }
    return "issue";
}

Uid emitter_item(std::string_view emitter) {
    constexpr std::string_view prefix = "item_";
    if (!emitter.starts_with(prefix)) {
        return 0U;
    }
    Uid uid = 0U;
    const char* first = emitter.data() + prefix.size();
    const char* last = emitter.data() + emitter.size();
    const auto [end, error] = std::from_chars(first, last, uid);
    return error == std::errc{} && end == last ? uid : 0U;
}

std::vector<Issue> scene_issues(const Document& document, const Prepared& prepared) {
    std::vector<Issue> out;
    if (!document.scene) {
        out.push_back(Issue{.level = IssueLevel::Problem, .kind = IssueKind::Scene, .message = "There is no scene"});
        return out;
    }
    const Scene& scene = *document.scene;
    const std::string invalid = validate_scene(document, true);
    if (!invalid.empty()) {
        out.push_back(Issue{.level = IssueLevel::Problem, .kind = IssueKind::Scene, .message = invalid});
    }
    const std::string acts = validate_performance(scene);
    if (!acts.empty()) {
        out.push_back(Issue{.level = IssueLevel::Problem, .kind = IssueKind::Scene, .message = "Performance: " + acts});
    }
    if (!prepared.compiled) {
        if (prepared.error != invalid) {
            out.push_back(Issue{.level = IssueLevel::Problem,
                                .kind = IssueKind::Scene,
                                .message = prepared.error.empty() ? "The scene does not compile" : prepared.error});
        }
        return out;
    }
    const Compiled& compiled = *prepared.compiled;
    if (!compiled.error.empty() && compiled.error != invalid) {
        // Compiled with something left out, which says why.
        out.push_back(Issue{.level = IssueLevel::Warning, .kind = IssueKind::Scene, .message = compiled.error});
    }

    const SceneStats stats = scene_stats(scene, &compiled, nullptr);
    budget(out, IssueKind::KindBudget, stats.body_kinds, max_rigid_body_kinds, "Body kinds",
           "Every piece a breakable thing comes apart into is a kind of its own: fewer pieces, or fewer kinds "
           "that break, leave room.");
    budget(out, IssueKind::PropBudget, stats.props, max_rigid_props, "Props",
           "Things that stand still or move along a path are props; a scene holds so many.");

    std::vector<std::uint32_t> unapplied;
    static_cast<void>(performance_actions(scene.performance, prepared.targets, nullptr, &unapplied));
    for (const std::uint32_t serial : unapplied) {
        const Act* act = act_of(scene.performance, serial);
        if (act == nullptr) {
            continue;
        }
        std::string why;
        if (act->kind == ActKind::Drop || act->kind == ActKind::Paint) {
            why = prepared.targets.drops ? item_name(scene, act->object) + " cannot be dropped in this scene: only a "
                                                                          "loose Object of the solid world can"
                                         : "this scene takes no dropped things (Scene settings > Simulation)";
        } else if (act->kind == ActKind::Blast) {
            why = "this scene has no solid world to blast";
        } else {
            why = "the thing it names is not made by this scene any more";
        }
        out.push_back(Issue{.level = IssueLevel::Warning,
                            .kind = IssueKind::Unapplied,
                            .item = act->object,
                            .act = serial,
                            .tick = act->tick,
                            .message = std::string{act_word(act->kind)} + " at step " + std::to_string(act->tick) +
                                       " is not applied: " + why});
    }

    // Wind on things blows on loose things: Dynamic Objects, their own
    // bodies and those dropped of them.
    const bool loose = std::any_of(scene.items.begin(), scene.items.end(), [](const SceneItem& item) {
        const auto* object = std::get_if<Object>(&item.data);
        return object != nullptr && item.simulate && object->motion == ObjectMotion::Dynamic;
    });
    for (const SceneItem& item : scene.items) {
        const auto* force = std::get_if<Force>(&item.data);
        if (force == nullptr || force->kind != ForceKind::Wind || !force->bodies || !item.simulate) {
            continue;
        }
        if (!loose || prepared.targets.emitter.empty()) {
            out.push_back(Issue{.level = IssueLevel::Warning,
                                .kind = IssueKind::WindIdle,
                                .item = item.uid,
                                .message = item_name(scene, item.uid) +
                                           " blows on things, and nothing in this scene moves: add a loose Object"});
        } else if (!(force->air_speed > 0.0F)) {
            out.push_back(Issue{.level = IssueLevel::Note,
                                .kind = IssueKind::WindIdle,
                                .item = item.uid,
                                .message = item_name(scene, item.uid) + " blows on things with no air speed"});
        }
    }
    sort_issues(out);
    return out;
}

std::vector<Issue> play_issues(const Document& document, const Prepared& prepared, const RigidSolver* solver,
                               std::uint32_t seed, std::uint64_t to) {
    std::vector<Issue> out;
    if (!document.scene || !prepared.compiled) {
        return out;
    }
    const Scene& scene = *document.scene;
    const ActTargets& targets = prepared.targets;
    Played played = play_scene(document, prepared, solver, seed, 0U);
    if (!played.error.empty() || !played.simulation) {
        out.push_back(Issue{.level = IssueLevel::Problem,
                            .kind = IssueKind::Scene,
                            .message = played.error.empty() ? "The scene does not play" : played.error});
        return out;
    }
    const std::shared_ptr<const ActionList> actions = performance_actions(scene.performance, targets);
    Simulation& simulation = *played.simulation;

    // The acts that make bodies and that this compile applies, each with the
    // serials of what it makes: each thing it makes is born at the end of
    // its act's step (a step-0 thing before the first), so it is looked for
    // in the world once that step has run.
    std::map<std::uint64_t, std::uint32_t> asked; // things asked for at each step
    std::vector<const Act*> making;
    for (const Act& act : scene.performance.acts) {
        if ((act.kind != ActKind::Drop && act.kind != ActKind::Paint) || act.tick >= to || !targets.drops ||
            !targets.kinds.contains(act.object)) {
            continue;
        }
        making.push_back(&act);
        asked[act.tick] += act.kind == ActKind::Drop ? 1U : act.count;
    }
    std::uint32_t most_held = 0U;
    std::size_t refused_for_room = 0U;
    for (const Act* act : making) {
        simulation.advance_to(act->tick + 1U, nullptr, actions.get());
        const EmitterState& world = simulation.emitter(*played.world);
        std::set<std::uint32_t> alive;
        std::uint32_t held = 0U;
        for (std::size_t i = 0U; i < world.alive(); ++i) {
            alive.insert(world.spawn_index[i]);
            held += spawn_source(world.spawn_index[i]) == *targets.drops ? 1U : 0U;
        }
        most_held = std::max(most_held, held);
        std::vector<std::uint32_t> serials;
        if (act->kind == ActKind::Drop) {
            serials.push_back(act->serial);
        } else {
            for (const PaintedThing& thing : painted_things(*act)) {
                serials.push_back(thing.serial);
            }
        }
        std::size_t missing = 0U;
        for (const std::uint32_t serial : serials) {
            missing += alive.contains(spawn_index_of(*targets.drops, serial)) ? 0U : 1U;
        }
        if (missing == 0U) {
            continue;
        }
        std::string why;
        if (held >= targets.max_dropped) {
            why = "the scene holds at most " + std::to_string(targets.max_dropped) +
                  " dropped things at once (Scene settings > Simulation)";
            ++refused_for_room;
        } else if (asked[act->tick] > SolidWorld::drops_per_step) {
            why = "more than " + std::to_string(SolidWorld::drops_per_step) + " things were asked for in one step";
        } else {
            why = "the solid world had no room for them";
            ++refused_for_room;
        }
        const std::string what = item_name(scene, act->object);
        out.push_back(Issue{.level = IssueLevel::Warning,
                            .kind = IssueKind::RefusedDrop,
                            .item = act->object,
                            .act = act->serial,
                            .tick = act->tick,
                            .message = std::string{act_word(act->kind)} + " at step " + std::to_string(act->tick) +
                                       ": " +
                                       (serials.size() == 1U ? what + " was refused"
                                                             : std::to_string(missing) + " of " +
                                                                   std::to_string(serials.size()) + " " + what +
                                                                   " were refused") +
                                       ": " + why});
    }
    simulation.advance_to(std::max<std::uint64_t>(simulation.tick(), to), nullptr, actions.get());
    if (targets.drops) {
        const EmitterState& world = simulation.emitter(*played.world);
        std::uint32_t held = 0U;
        for (std::size_t i = 0U; i < world.alive(); ++i) {
            held += spawn_source(world.spawn_index[i]) == *targets.drops ? 1U : 0U;
        }
        most_held = std::max(most_held, held);
        if (targets.max_dropped > 0U && most_held >= targets.max_dropped && refused_for_room == 0U) {
            out.push_back(Issue{.level = IssueLevel::Note,
                                .kind = IssueKind::DropBudget,
                                .message = "The scene is holding its most dropped things at once (" +
                                           std::to_string(targets.max_dropped) +
                                           "): the next drop will be refused until something is deleted"});
        }
    }
    const Effect& effect = *prepared.compiled->effect;
    for (std::size_t e = 0U; e < effect.emitters.size(); ++e) {
        if (played.world && e == *played.world) {
            continue;
        }
        const EmitterState& s = simulation.emitter(e);
        if (s.refused == 0U) {
            continue;
        }
        const Uid item = emitter_item(effect.emitters[e].name);
        out.push_back(Issue{.level = IssueLevel::Warning,
                            .kind = IssueKind::ParticlesRefused,
                            .item = item,
                            .tick = to,
                            .message = (item != 0U ? item_name(scene, item) : effect.emitters[e].name) +
                                       " ran out of room: " + std::to_string(s.refused) + " particles were not born by step " +
                                       std::to_string(to) + " (it holds " + std::to_string(effect.emitters[e].capacity) +
                                       ")"});
    }
    sort_issues(out);
    return out;
}

std::vector<Issue> bake_issues(const Document& document, const BakeStatus& status) {
    std::vector<Issue> out;
    const auto name = [&](Uid uid) {
        return document.scene ? item_name(*document.scene, uid) : "layer " + std::to_string(uid);
    };
    for (const LayerBake& layer : status.layers) {
        switch (layer.state) {
        case BakeState::Live:
        case BakeState::Current:
            break;
        case BakeState::Stale:
            out.push_back(Issue{.level = IssueLevel::Warning,
                                .kind = IssueKind::CacheStale,
                                .item = layer.layer,
                                .message = name(layer.layer) + ": the cache holds it as it was" +
                                           (layer.why.empty() ? std::string{} : " (" + layer.why + ")") +
                                           "; bake again"});
            break;
        case BakeState::Unbaked:
            out.push_back(Issue{.level = IssueLevel::Note,
                                .kind = IssueKind::CacheUnbaked,
                                .item = layer.layer,
                                .message = name(layer.layer) + " is baked and no cache holds it yet" +
                                           (layer.why.empty() ? std::string{} : ": " + layer.why)});
            break;
        case BakeState::TooLarge:
            out.push_back(Issue{.level = IssueLevel::Problem,
                                .kind = IssueKind::CacheTooLarge,
                                .item = layer.layer,
                                .message = name(layer.layer) + " cannot be baked: " + layer.why});
            break;
        }
    }
    sort_issues(out);
    return out;
}

std::optional<Issue> cache_issue(const Document& document, const Prepared& prepared,
                                 std::span<const std::uint8_t> bytes, std::uint32_t seed) {
    if (!document.scene || !prepared.compiled) {
        return std::nullopt;
    }
    const Compiled& compiled = *prepared.compiled;
    const std::shared_ptr<const ActionList> actions =
        performance_actions(document.scene->performance, prepared.targets);
    Result<Cache> opened = Cache::open(std::vector<std::uint8_t>(bytes.begin(), bytes.end()), *compiled.effect, seed, {},
                                       compiled.meshes, compiled.fields, actions.get());
    if (opened) {
        return std::nullopt;
    }
    const bool stale = opened.error().code == ErrorCode::StaleCache;
    return Issue{.level = stale ? IssueLevel::Warning : IssueLevel::Problem,
                 .kind = IssueKind::CacheStale,
                 .message = stale ? "The cache no longer holds the scene as it is (" + opened.error().message +
                                        "): bake again"
                                  : "The cache cannot be read: " + opened.error().message};
}

void sort_issues(std::vector<Issue>& issues) {
    std::stable_sort(issues.begin(), issues.end(), [](const Issue& a, const Issue& b) {
        if (a.level != b.level) {
            return a.level > b.level;
        }
        return a.tick < b.tick;
    });
}

} // namespace ajfx::studio
