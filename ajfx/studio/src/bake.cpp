#include "bake.hpp"

#include "performance.hpp"
#include "scene_properties.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>

namespace ajfx::studio {
namespace {

std::optional<std::size_t> emitter_index(const Compiled& compiled, const std::string& emitter) {
    if (!compiled.effect) {
        return std::nullopt;
    }
    for (std::size_t i = 0U; i < compiled.effect->emitters.size(); ++i) {
        if (compiled.effect->emitters[i].name == emitter) {
            return i;
        }
    }
    return std::nullopt;
}

std::string called(const SceneItem& item) {
    return item.name.empty() ? std::string{"an item"} : item.name;
}

// What changed in `now` since `then`, the same item at the bake, as the
// artist would say it. Playback-time values are passed over: they never
// reach the cache.
std::string item_changed(const SceneItem& then, const SceneItem& now) {
    if (then.data.index() != now.data.index()) {
        return "it is something else now";
    }
    if (then.simulate != now.simulate) {
        return now.simulate ? "it was switched on" : "it was switched off";
    }
    if (then.seed != now.seed) {
        return "its seed changed";
    }
    for (const SceneProperty& property : scene_properties(now)) {
        if (property.control.timing == ParameterTiming::Bake && !(property.read(then) == property.read(now))) {
            return "its \"" + std::string{property.control.label} + "\" changed";
        }
    }
    if (const auto* system = std::get_if<ParticleSystem>(&now.data)) {
        const auto& before = std::get<ParticleSystem>(then.data);
        if (before.sources != system->sources) {
            return "its sources changed";
        }
        if (before.motion != system->motion) {
            return "its motion changed";
        }
        if (before.size_driver != system->size_driver || before.size_over_age != system->size_over_age) {
            return "its size over its age changed";
        }
    }
    if (const auto* force = std::get_if<Force>(&now.data)) {
        const auto& before = std::get<Force>(then.data);
        if (before.all_systems != force->all_systems || before.targets != force->targets) {
            return "what it acts on changed";
        }
    }
    if (!(then.data == now.data)) {
        return "it changed";
    }
    return {};
}

// Why item `uid` of `now` is not what it was in `then`, the scene at the
// bake: its own change first, then what it depends on -- the forces acting
// on it, the rules it is born of, the scene's blasts and physics, and the
// solid things it lands on -- as the artist would say it.
std::string why_stale(const Document& then, const Document& now, Uid uid) {
    const Scene& before = *then.scene;
    const Scene& after = *now.scene;
    const SceneItem* was = before.item(uid);
    const SceneItem* is = after.item(uid);
    if (was == nullptr || is == nullptr) {
        return "it is new since the bake";
    }
    if (std::string why = item_changed(*was, *is); !why.empty()) {
        return why;
    }
    if (then.scale != now.scale) {
        return "the scene's scale changed";
    }
    if (!(before.physics == after.physics)) {
        return "the scene's physics changed";
    }
    // A force or a blast added, taken away or changed.
    const auto forces_changed = [&]() -> std::string {
        for (const SceneItem& item : after.items) {
            if (!std::holds_alternative<Force>(item.data)) {
                continue;
            }
            const SceneItem* old = before.item(item.uid);
            if (old == nullptr) {
                return called(item) + " was added";
            }
            if (std::string why = item_changed(*old, item); !why.empty()) {
                return called(item) + " changed: " + why;
            }
        }
        for (const SceneItem& item : before.items) {
            if (std::holds_alternative<Force>(item.data) && after.item(item.uid) == nullptr) {
                return called(item) + " was taken away";
            }
        }
        return {};
    };
    if (std::string why = forces_changed(); !why.empty()) {
        return why;
    }
    std::vector<Relationship> rules_then = before.relationships;
    std::vector<Relationship> rules_now = after.relationships;
    const auto by_uid = [](const Relationship& a, const Relationship& b) { return a.uid < b.uid; };
    std::sort(rules_then.begin(), rules_then.end(), by_uid);
    std::sort(rules_now.begin(), rules_now.end(), by_uid);
    if (rules_then != rules_now) {
        return "what is born of what changed";
    }
    // What it lands on, or is born of: another item.
    for (const SceneItem& item : after.items) {
        if (item.uid == uid || std::holds_alternative<Group>(item.data)) {
            continue;
        }
        const SceneItem* old = before.item(item.uid);
        if (old == nullptr) {
            return called(item) + " was added";
        }
        if (!item_changed(*old, item).empty()) {
            return called(item) + ", which it plays beside, changed";
        }
    }
    return {};
}

// An item's digest as Studio compares it: its emitter's, and the cuts that
// emitter and those it reads are made with, which are made into them only
// once their models are read -- so a scene compiled before that still sees a
// cut's settings change.
std::uint64_t item_digest(const Compiled& compiled, std::size_t index) {
    std::uint64_t hash = emitter_digest(*compiled.effect, index, compiled.meshes, compiled.fields);
    const auto mix = [&hash](std::uint64_t value) {
        for (std::uint32_t i = 0U; i < 8U; ++i) {
            hash ^= (value >> (8U * i)) & 0xffU;
            hash *= 0x100000001b3ULL;
        }
    };
    const auto bits = [](float v) { return static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(v)); };
    // The blasts it answers: a Shockwave names a blast, and the runtime's
    // digest holds the name but not where, when or how hard the blast goes
    // off, which are the effect's (Effect::blasts).
    const Effect& effect = *compiled.effect;
    for (const UpdateModule& module : effect.emitters[index].update) {
        const auto* shockwave = std::get_if<Shockwave>(&module);
        if (shockwave == nullptr) {
            continue;
        }
        for (const Blast& blast : effect.blasts) {
            if (blast.name != shockwave->blast) {
                continue;
            }
            for (const char c : blast.name) {
                mix(static_cast<std::uint64_t>(static_cast<unsigned char>(c)));
            }
            mix(bits(blast.center.x));
            mix(bits(blast.center.y));
            mix(bits(blast.center.z));
            mix(bits(blast.power));
            mix(bits(blast.radius));
            mix(bits(blast.time));
        }
    }
    // The cuts of its own emitter and of every emitter it reads (born of its
    // hits or bodies, meeting its bodies): an Object's fracture is made only
    // once its model is read, so a scene compiled before that sees it here.
    std::vector<std::string> reads{effect.emitters[index].name};
    const auto read = [&reads](const std::string& name) {
        if (!name.empty() && std::find(reads.begin(), reads.end(), name) == reads.end()) {
            reads.push_back(name);
        }
    };
    for (const SpawnModule& module : effect.emitters[index].spawn) {
        std::visit(
            [&](const auto& typed) {
                if constexpr (requires { typed.emitter; }) {
                    read(typed.emitter);
                }
            },
            module);
    }
    for (const UpdateModule& module : effect.emitters[index].update) {
        std::visit(
            [&](const auto& typed) {
                if constexpr (requires { typed.emitter; }) {
                    read(typed.emitter);
                }
            },
            module);
    }
    for (const FractureCut& cut : compiled.cuts) {
        if (std::find(reads.begin(), reads.end(), cut.emitter) == reads.end()) {
            continue;
        }
        for (const char c : cut.model) {
            mix(static_cast<std::uint64_t>(static_cast<unsigned char>(c)));
        }
        mix(cut.body);
        mix(cut.id);
        mix(cut.cells);
        mix(cut.seed);
        mix(bits(cut.spread));
        mix(bits(cut.threshold));
        mix(bits(cut.blast_threshold));
        mix(bits(cut.scatter));
        mix(bits(cut.spin));
    }
    return hash;
}

// Bytes a particle that lives on costs a step in a cached particle system.
std::uint64_t particle_step_bytes(const Emitter& emitter) {
    const std::uint32_t mask = required_attributes(emitter);
    std::uint64_t bytes = 6U;
    if (emitter.renderer.facing == Facing::Velocity && has_attribute(mask, Attribute::Velocity)) {
        bytes += 6U;
    }
    if (has_attribute(mask, Attribute::Rotation)) {
        bytes += 2U;
    }
    if (has_attribute(mask, Attribute::Orientation)) {
        bytes += 8U;
    }
    return bytes;
}

// Whether the emitter deposits into a grid volume (VolumeSource), which no
// cache can hold.
bool feeds_smoke(const Emitter& emitter) {
    return std::any_of(emitter.update.begin(), emitter.update.end(),
                       [](const UpdateModule& module) { return std::holds_alternative<VolumeSource>(module); });
}

} // namespace

const BakeRecord::Item* BakeRecord::item(Uid uid) const {
    for (const Item& i : items) {
        if (i.item == uid) {
            return &i;
        }
    }
    return nullptr;
}

const ItemBake* BakeStatus::item(Uid uid) const {
    for (const ItemBake& i : items) {
        if (i.item == uid) {
            return &i;
        }
    }
    return nullptr;
}

std::vector<CacheMode> bake_modes(const Document& document, const Compiled& compiled) {
    static_cast<void>(document);
    std::vector<CacheMode> modes;
    if (!compiled.effect) {
        return modes;
    }
    modes.resize(compiled.effect->emitters.size(), CacheMode::Live);
    for (std::size_t i = 0U; i < modes.size(); ++i) {
        const Emitter& emitter = compiled.effect->emitters[i];
        // Cached when every item playing in it is baked: the scene's Objects
        // are one world, which is cached whole or not at all.
        bool any = false;
        bool all = true;
        for (const ItemReport& report : compiled.items) {
            if (!report.included || report.emitter != emitter.name) {
                continue;
            }
            any = true;
            all = all && report.baked;
        }
        if (any && all) {
            modes[i] = rigid_physics(emitter) != nullptr ? CacheMode::Rigid : CacheMode::Particles;
        }
        // An emitter feeding a smoke grid stays live: a cache keeps its
        // particles, not what they deposit, and the runtime refuses to
        // cache it (ajfx::bake).
        if (feeds_smoke(emitter)) {
            modes[i] = CacheMode::Live;
        }
    }
    // A live system cannot be born of cached bodies or stirred by them (a
    // cache keeps bodies, not their hits or speeds), so one that reads a
    // cached world is cached with it, as particles.
    for (std::size_t i = 0U; i < modes.size(); ++i) {
        if (modes[i] != CacheMode::Live) {
            continue;
        }
        for (const UpdateModule& module : compiled.effect->emitters[i].update) {
            if (const auto* stirred = std::get_if<Wake>(&module)) {
                const std::optional<std::size_t> source = emitter_index(compiled, stirred->emitter);
                if (source && modes[*source] != CacheMode::Live && rigid_physics(compiled.effect->emitters[i]) == nullptr) {
                    modes[i] = CacheMode::Particles;
                }
            }
        }
        for (const SpawnModule& module : compiled.effect->emitters[i].spawn) {
            std::string reads;
            if (const auto* hits = std::get_if<SpawnFromContacts>(&module)) {
                reads = hits->emitter;
            } else if (const auto* moving = std::get_if<SpawnFromBodies>(&module)) {
                reads = moving->emitter;
            } else if (const auto* trail = std::get_if<SpawnFromParticles>(&module)) {
                reads = trail->emitter;
            }
            if (reads.empty()) {
                continue;
            }
            const std::optional<std::size_t> source = emitter_index(compiled, reads);
            if (source && modes[*source] != CacheMode::Live) {
                modes[i] = CacheMode::Particles;
            }
        }
    }
    return modes;
}

std::uint64_t bake_frames(const Document& document, const Compiled& compiled) {
    const double seconds = static_cast<double>(loop_seconds(document));
    const std::uint32_t rate = compiled.effect ? compiled.effect->simulation_rate : 60U;
    return std::max<std::uint64_t>(1U, static_cast<std::uint64_t>(std::ceil(seconds * rate)));
}

BakeStatus bake_status(const Document& document, const Compiled& compiled, const BakeRecord* record,
                       const std::map<Uid, std::uint64_t>* peaks) {
    BakeStatus status;
    status.cost.frames = bake_frames(document, compiled);
    status.cached_frames = record != nullptr ? record->frames : 0U;
    // The bake replays the performance it was taken with: a cache of another
    // performance holds another scene.
    const bool same_performance = record == nullptr || !record->document.scene || !document.scene ||
                                  record->document.scene->performance == document.scene->performance;
    bool all_current = true;
    std::uint64_t per_step = 0U;
    for (const ItemReport& report : compiled.items) {
        ItemBake bake;
        bake.item = report.item;
        const SceneItem* item = document.scene ? document.scene->item(report.item) : nullptr;
        if (!report.baked || item == nullptr) {
            status.items.push_back(bake);
            continue;
        }
        if (!item->simulate || !report.included) {
            // Not played, so nothing to cache; it is baked when it plays.
            bake.state = BakeState::Unbaked;
            bake.why = item->simulate ? "it cannot play as it is" : "it is switched off";
            status.items.push_back(bake);
            continue;
        }
        const std::optional<std::size_t> index = emitter_index(compiled, report.emitter);
        if (!index) {
            status.any_baked = true;
            bake.state = BakeState::Unbaked;
            status.items.push_back(bake);
            all_current = false;
            continue;
        }
        const Emitter& emitter = compiled.effect->emitters[*index];
        if (feeds_smoke(emitter)) {
            // Live, and says why: it is not waiting for a bake, and the
            // cache is current without it.
            bake.state = BakeState::Live;
            bake.mode = CacheMode::Live;
            bake.why = "it feeds smoke, which a cache cannot keep, so it stays live";
            status.items.push_back(bake);
            continue;
        }
        status.any_baked = true;
        const bool rigid = rigid_physics(emitter) != nullptr;
        bake.mode = rigid ? CacheMode::Rigid : CacheMode::Particles;
        if (!rigid && emitter.capacity > max_cached_particles) {
            bake.state = BakeState::TooLarge;
            bake.why = "it can hold " + std::to_string(emitter.capacity) + " particles and a baked system at most " +
                       std::to_string(max_cached_particles) + "; one that big is cheaper to simulate than to stream";
            status.items.push_back(bake);
            all_current = false;
            continue;
        }
        // What a step of it costs to play from a cache: its own share of
        // an emitter it shares.
        const std::uint64_t own = report.capacity != 0U ? report.capacity : emitter.capacity;
        std::uint64_t alive = own;
        if (peaks != nullptr) {
            if (const auto peak = peaks->find(report.item); peak != peaks->end()) {
                alive = std::min<std::uint64_t>(peak->second, own);
            }
        }
        if (rigid) {
            per_step += alive * 36U + 32U;
        } else {
            // Its survivors, and a keyframe of all of it each second.
            per_step += alive * particle_step_bytes(emitter) + alive * 124U / 60U + 32U;
        }
        const BakeRecord::Item* baked = record != nullptr ? record->item(report.item) : nullptr;
        if (baked == nullptr) {
            bake.state = BakeState::Unbaked;
            status.items.push_back(bake);
            all_current = false;
            continue;
        }
        const std::uint64_t digest = item_digest(compiled, *index);
        if (baked->mode == bake.mode && baked->digest == digest && record->frames == status.cost.frames &&
            same_performance) {
            bake.state = BakeState::Current;
            status.items.push_back(bake);
            continue;
        }
        all_current = false;
        bake.state = BakeState::Stale;
        if (!same_performance) {
            bake.why = "things were dropped or deleted since the bake";
        } else if (record->frames != status.cost.frames) {
            bake.why = "the loop is longer or shorter than the bake";
        } else if (record->document.scene && document.scene) {
            bake.why = why_stale(record->document, document, report.item);
        }
        if (bake.why.empty()) {
            bake.why = "something it depends on changed";
        }
        status.items.push_back(bake);
    }
    status.current = status.any_baked && all_current && record != nullptr;
    const std::uint32_t rate = compiled.effect ? compiled.effect->simulation_rate : 60U;
    status.cost.bytes = per_step * status.cost.frames;
    status.cost.bytes_per_second = per_step * rate;
    if (record != nullptr && record->frames > 0U) {
        status.cost.seconds =
            record->seconds * static_cast<double>(status.cost.frames) / static_cast<double>(record->frames);
    }
    return status;
}

Result<BakeRecord> bake_scene(const Document& document, const Compiled& compiled, std::uint32_t instance_seed,
                              const RigidSolver* solver, const std::function<bool(double)>& progress) {
    if (!compiled.effect) {
        return Error{ErrorCode::InvalidEffect, "There is nothing to bake: nothing plays"};
    }
    const std::vector<CacheMode> modes = bake_modes(document, compiled);
    if (std::all_of(modes.begin(), modes.end(), [](CacheMode m) { return m == CacheMode::Live; })) {
        return Error{ErrorCode::InvalidEffect, "Nothing is baked: everything that plays is live"};
    }
    BakeSettings settings;
    settings.frames = bake_frames(document, compiled);
    settings.modes = modes;
    // A keyframe a second, and a checkpoint every two, as the preview seeks.
    settings.keyframe_interval = compiled.effect->simulation_rate;
    settings.checkpoint_interval = 2U * compiled.effect->simulation_rate;
    // The scene's performance, which the cache is keyed by (CacheIdentity).
    if (document.scene) {
        settings.actions = performance_actions(document.scene->performance, act_targets(compiled));
    }
    if (progress) {
        const double frames = static_cast<double>(settings.frames);
        settings.progress = [&progress, frames](std::uint64_t tick) {
            return progress(static_cast<double>(tick + 1U) / frames);
        };
    }
    SimulationOptions options;
    options.meshes = compiled.meshes;
    options.fields = compiled.fields;
    options.rigid = solver;
    const auto started = std::chrono::steady_clock::now();
    Result<Baked> baked = bake(*compiled.effect, instance_seed, std::move(options), settings);
    if (!baked) {
        return baked.error();
    }
    BakeRecord record;
    record.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    record.document = document;
    record.instance_seed = instance_seed;
    record.frames = settings.frames;
    record.report = baked.value().report;
    record.bytes = std::move(baked).value().bytes;
    // The digests bake_status will compare against: from the scene compiled
    // as it compiles it, before files are read and cuts made.
    const Compiled plain = compile(document);
    if (plain.effect) {
        const std::vector<CacheMode> plain_modes = bake_modes(document, plain);
        for (std::size_t i = 0U; i < plain.effect->emitters.size(); ++i) {
            if (plain_modes[i] == CacheMode::Live) {
                continue;
            }
            // Every item playing in it: the scene's Objects share one.
            const std::string& name = plain.effect->emitters[i].name;
            for (const ItemReport& report : plain.items) {
                if (report.included && report.emitter == name) {
                    record.items.push_back(
                        BakeRecord::Item{report.item, name, plain_modes[i], item_digest(plain, i)});
                }
            }
        }
    }
    return record;
}

std::string bytes_text(std::uint64_t bytes) {
    char text[32];
    if (bytes < 1024U) {
        std::snprintf(text, sizeof(text), "%llu bytes", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024U * 1024U) {
        std::snprintf(text, sizeof(text), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024ULL * 1024U * 1024U) {
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(text, sizeof(text), "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return text;
}

} // namespace ajfx::studio
