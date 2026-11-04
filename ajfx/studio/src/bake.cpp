#include "bake.hpp"

#include "catalog.hpp"
#include "performance.hpp"

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

const Block* block_with(const Layer& layer, Uid uid) {
    for (const Block& block : layer.blocks) {
        if (block.uid == uid) {
            return &block;
        }
    }
    return nullptr;
}

// What changed in `now` since `then`, the same layer at the bake, as the
// artist would say it; the block at fault in `block`. Playback-time controls
// are passed over: they never reach the cache.
std::string what_changed(const Layer& then, const Layer& now, Uid& block) {
    for (const Block& b : now.blocks) {
        const Block* before = block_with(then, b.uid);
        if (before == nullptr) {
            block = b.uid;
            return block_title(b) + " was added";
        }
        if (before->enabled != b.enabled) {
            block = b.uid;
            return block_title(b) + (b.enabled ? " was turned on" : " was turned off");
        }
        if (*before == b) {
            continue;
        }
        if (const BlockDef* def = find_block_def(b.kind)) {
            for (const ControlDef& control : def->controls) {
                if (control.timing == ParameterTiming::Playback) {
                    continue;
                }
                if (!(value_of(*before, control) == value_of(b, control))) {
                    block = b.uid;
                    return block_title(b) + "'s \"" + std::string{control.label} + "\" changed";
                }
            }
        }
        if (before->raw != b.raw || before->published != b.published) {
            block = b.uid;
            return block_title(b) + " changed";
        }
    }
    for (const Block& b : then.blocks) {
        if (block_with(now, b.uid) == nullptr) {
            return block_title(b) + " was removed";
        }
    }
    if (then.life_min != now.life_min || then.life_max != now.life_max) {
        return "how long each lives changed";
    }
    if (then.seed != now.seed) {
        return "its seed changed";
    }
    if (!(then.world == now.world)) {
        return "its world's settings changed";
    }
    return {};
}

// Whether a blast on the page (an Explosion or a Push, in any layer) was
// added, moved, changed or taken away since `then`, as the artist would say
// it; the block at fault in `block`.
std::string blast_changed(const Document& then, const Document& now, Uid& block) {
    const auto is_blast = [](const Block& b) { return b.kind == BlockKind::Explosion || b.kind == BlockKind::Push; };
    for (const Layer& layer : now.layers) {
        const std::string where = layer.name.empty() ? std::string{} : " in " + layer.name;
        for (const Block& b : layer.blocks) {
            if (!is_blast(b)) {
                continue;
            }
            const Block* before = then.block(b.uid);
            const Layer* layer_then = then.layer(layer.uid);
            const bool playing = b.enabled && layer.enabled;
            const bool played = before != nullptr && before->enabled && layer_then != nullptr && layer_then->enabled;
            if (!playing && !played) {
                continue;
            }
            if (before == nullptr || !played) {
                block = b.uid;
                return block_title(b) + where + " was added";
            }
            if (!playing) {
                block = b.uid;
                return block_title(b) + where + " was turned off";
            }
            if (!(*before == b)) {
                block = b.uid;
                return block_title(b) + where + " changed";
            }
        }
    }
    for (const Layer& layer : then.layers) {
        for (const Block& b : layer.blocks) {
            if (is_blast(b) && b.enabled && layer.enabled && now.block(b.uid) == nullptr) {
                return block_title(b) + " was removed";
            }
        }
    }
    if (then.scale != now.scale) {
        return "the page's scale changed";
    }
    return {};
}

// A layer's digest as Studio compares it: its emitter's, and the cuts it
// asks for, which are made into the emitter only once its models are read --
// so a page compiled before that still sees a Break's settings change.
std::uint64_t layer_digest(const Compiled& compiled, std::size_t index, Uid layer) {
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
    // off, which are the effect's (Effect::blasts). Moving an Explosion in
    // another layer changes what this layer does all the same.
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
    for (const FractureCut& cut : compiled.cuts) {
        if (cut.layer != layer) {
            continue;
        }
        for (const char c : cut.model) {
            mix(static_cast<std::uint64_t>(static_cast<unsigned char>(c)));
        }
        mix(cut.block);
        mix(cut.body);
        mix(cut.id);
        mix(cut.cells);
        mix(cut.seed);
        mix(bits(cut.spread));
        mix(bits(cut.threshold));
        mix(bits(cut.scatter));
        mix(bits(cut.spin));
    }
    return hash;
}

// Bytes a particle that lives on costs a step in a cached particle layer.
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

} // namespace

const BakeRecord::Layer* BakeRecord::layer(Uid uid) const {
    for (const Layer& l : layers) {
        if (l.layer == uid) {
            return &l;
        }
    }
    return nullptr;
}

const LayerBake* BakeStatus::layer(Uid uid) const {
    for (const LayerBake& l : layers) {
        if (l.layer == uid) {
            return &l;
        }
    }
    return nullptr;
}

namespace {
// Whether the emitter deposits into a grid volume (VolumeSource), which no
// cache can hold.
bool feeds_smoke(const Emitter& emitter) {
    return std::any_of(emitter.update.begin(), emitter.update.end(),
                       [](const UpdateModule& module) { return std::holds_alternative<VolumeSource>(module); });
}
} // namespace

std::vector<CacheMode> bake_modes(const Document& document, const Compiled& compiled) {
    if (document.scene) { return bake_modes(lower_scene(document), compiled); }
    std::vector<CacheMode> modes;
    if (!compiled.effect) {
        return modes;
    }
    modes.resize(compiled.effect->emitters.size(), CacheMode::Live);
    for (std::size_t i = 0U; i < modes.size(); ++i) {
        const Emitter& emitter = compiled.effect->emitters[i];
        // Cached when every layer playing in it is baked: the page's solid
        // layers are one world, which is cached whole or not at all.
        bool any = false;
        bool all = true;
        for (const LayerReport& report : compiled.layers) {
            if (!report.included || report.emitter != emitter.name) {
                continue;
            }
            const Layer* layer = document.layer(report.layer);
            any = true;
            all = all && layer != nullptr && layer->baked;
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
    // A live layer cannot be born of cached bodies or stirred by them (a
    // cache keeps bodies, not their hits or speeds), so one that reads a
    // cached world is cached with it, as particles. Smoke and dust beside a
    // solid world are stirred by it by default (Wake), and a solid world is
    // baked by default, so this is the usual case rather than a rare one.
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
    const double seconds = static_cast<double>(loop_seconds(document, compiled));
    const std::uint32_t rate = compiled.effect ? compiled.effect->simulation_rate : 60U;
    return std::max<std::uint64_t>(1U, static_cast<std::uint64_t>(std::ceil(seconds * rate)));
}

BakeStatus bake_status(const Document& document, const Compiled& compiled, const BakeRecord* record,
                       const std::map<Uid, std::uint64_t>* peaks) {
    if (document.scene) {
        if (record && record->page.scene) {
            BakeRecord lowered;
            lowered.layers = record->layers;
            lowered.frames = record->frames;
            lowered.seconds = record->seconds;
            lowered.page = lower_scene(record->page);
            BakeStatus status = bake_status(lower_scene(document), compiled, &lowered, peaks);
            // The bake replays the drops and deletions it was taken with; a
            // cache of another performance holds another scene.
            if (record->page.scene->performance != document.scene->performance) {
                for (LayerBake& layer : status.layers) {
                    if (layer.state == BakeState::Current) {
                        layer.state = BakeState::Stale;
                        layer.why = "things were dropped or deleted since the bake";
                    }
                }
                status.current = false;
            }
            return status;
        }
        return bake_status(lower_scene(document), compiled, record, peaks);
    }
    BakeStatus status;
    status.cost.frames = bake_frames(document, compiled);
    status.cached_frames = record != nullptr ? record->frames : 0U;
    bool all_current = true;
    std::uint64_t per_step = 0U;
    for (const Layer& layer : document.layers) {
        LayerBake bake;
        bake.layer = layer.uid;
        if (!layer.baked) {
            status.layers.push_back(bake);
            continue;
        }
        const LayerReport* report = compiled.layer(layer.uid);
        if (!layer.enabled || report == nullptr || !report->included) {
            // Not played, so nothing to cache; it is baked when it plays.
            bake.state = BakeState::Unbaked;
            bake.why = layer.enabled ? "it cannot play as it is" : "it is turned off";
            status.layers.push_back(bake);
            continue;
        }
        const std::optional<std::size_t> index = emitter_index(compiled, report->emitter);
        if (!index) {
            status.any_baked = true;
            bake.state = BakeState::Unbaked;
            status.layers.push_back(bake);
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
            status.layers.push_back(bake);
            continue;
        }
        status.any_baked = true;
        const bool rigid = rigid_physics(emitter) != nullptr;
        bake.mode = rigid ? CacheMode::Rigid : CacheMode::Particles;
        // The page's solid layers are one world, cached whole: a live one
        // keeps every other one live too.
        const Layer* live = nullptr;
        for (const LayerReport& other : compiled.layers) {
            const Layer* of = document.layer(other.layer);
            if (other.included && other.emitter == report->emitter && of != nullptr && !of->baked) {
                live = of;
                break;
            }
        }
        if (live != nullptr) {
            bake.state = BakeState::Unbaked;
            bake.why = "its world is shared with " + (live->name.empty() ? std::string{"a live layer"} : live->name) +
                       ", which is live";
            status.layers.push_back(bake);
            all_current = false;
            continue;
        }
        if (!rigid && emitter.capacity > max_cached_particles) {
            bake.state = BakeState::TooLarge;
            bake.why = "it can hold " + std::to_string(emitter.capacity) + " particles and a baked layer at most " +
                       std::to_string(max_cached_particles) + "; one that big is cheaper to simulate than to stream";
            status.layers.push_back(bake);
            all_current = false;
            continue;
        }
        // What a step of it costs to play from a cache.
        // Its own share of an emitter it shares.
        const std::uint64_t own = report->capacity != 0U ? report->capacity : emitter.capacity;
        std::uint64_t alive = own;
        if (peaks != nullptr) {
            if (const auto peak = peaks->find(layer.uid); peak != peaks->end()) {
                alive = std::min<std::uint64_t>(peak->second, own);
            }
        }
        if (rigid) {
            per_step += alive * 36U + 32U;
        } else {
            // Its survivors, and a keyframe of all of it each second.
            per_step += alive * particle_step_bytes(emitter) + alive * 124U / 60U + 32U;
        }
        const BakeRecord::Layer* baked = record != nullptr ? record->layer(layer.uid) : nullptr;
        if (baked == nullptr) {
            bake.state = BakeState::Unbaked;
            status.layers.push_back(bake);
            all_current = false;
            continue;
        }
        const std::uint64_t digest = layer_digest(compiled, *index, layer.uid);
        if (baked->mode == bake.mode && baked->digest == digest && record->frames == status.cost.frames) {
            bake.state = BakeState::Current;
            status.layers.push_back(bake);
            continue;
        }
        all_current = false;
        bake.state = BakeState::Stale;
        if (record->frames != status.cost.frames) {
            bake.why = "the loop is longer or shorter than the bake";
        } else if (const Layer* then = record->page.layer(layer.uid)) {
            bake.why = what_changed(*then, layer, bake.block);
        }
        if (bake.why.empty()) {
            // Nothing of its own: a blast anywhere on the page, then, which
            // every layer answers wherever its block is.
            bake.why = blast_changed(record->page, document, bake.block);
        }
        if (bake.why.empty()) {
            // Nothing of its own: what it lands on, then.
            for (const Layer& other : document.layers) {
                const Layer* then = record->page.layer(other.uid);
                Uid ignored{};
                if (other.kind == LayerKind::Rigid && other.uid != layer.uid &&
                    (then == nullptr || !what_changed(*then, other, ignored).empty())) {
                    bake.why = (other.name.empty() ? std::string{"a solid layer"} : other.name) +
                               ", which it lands on, changed";
                    break;
                }
            }
        }
        if (bake.why.empty()) {
            bake.why = "something it depends on changed";
        }
        status.layers.push_back(bake);
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

Result<BakeRecord> bake_page(const Document& document, const Compiled& compiled, std::uint32_t instance_seed,
                             const RigidSolver* solver, const std::function<bool(double)>& progress) {
    if (!compiled.effect) {
        return Error{ErrorCode::InvalidEffect, "There is nothing to bake: no layer plays"};
    }
    const std::vector<CacheMode> modes = bake_modes(document, compiled);
    if (std::all_of(modes.begin(), modes.end(), [](CacheMode m) { return m == CacheMode::Live; })) {
        return Error{ErrorCode::InvalidEffect, "No layer is baked: mark a layer Baked on its heading first"};
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
    record.page = document;
    record.instance_seed = instance_seed;
    record.frames = settings.frames;
    record.report = baked.value().report;
    record.bytes = std::move(baked).value().bytes;
    // The digests bake_status will compare against: from the page compiled
    // as it compiles it, before files are read and cuts made.
    const Compiled plain = compile(document, Purpose::Export);
    if (plain.effect) {
        const std::vector<CacheMode> plain_modes = bake_modes(document, plain);
        for (std::size_t i = 0U; i < plain.effect->emitters.size(); ++i) {
            if (plain_modes[i] == CacheMode::Live) {
                continue;
            }
            // Every layer playing in it: the page's solid layers share one.
            const std::string& name = plain.effect->emitters[i].name;
            for (const LayerReport& report : plain.layers) {
                if (report.included && report.emitter == name) {
                    record.layers.push_back(
                        BakeRecord::Layer{report.layer, name, plain_modes[i], layer_digest(plain, i, report.layer)});
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
