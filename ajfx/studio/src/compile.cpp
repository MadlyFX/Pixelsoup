#include "compile.hpp"

#include "build_core.hpp"
#include "catalog.hpp"
#include "layer_builder.hpp"
#include "library.hpp"
#include "solid_world.hpp"

#include <ajfx/fracture.hpp>
#include <ajfx/gpu/kernels.hpp>
#include <ajfx/random.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <span>
#include <tuple>

namespace ajfx::studio {
namespace {

std::string emitter_name(const Layer& layer, std::set<std::string>& taken) {
    std::string base;
    for (const char c : layer.name) {
        const auto u = static_cast<unsigned char>(c);
        base += std::isalnum(u) != 0 ? static_cast<char>(std::tolower(u)) : '_';
    }
    if (base.empty()) {
        base = "layer";
    }
    std::string name = base;
    for (int n = 2; !taken.insert(name).second; ++n) {
        name = base + "_" + std::to_string(n);
    }
    return name;
}

// The block a validation message names ("... module 'drag' #37 ..."), if it
// belongs to the layer.
Uid block_named_in(const std::string& message, const Layer& layer) {
    const std::size_t hash = message.find(" #");
    if (hash == std::string::npos) {
        return 0U;
    }
    std::uint64_t id = 0U;
    for (std::size_t i = hash + 2U; i < message.size() && std::isdigit(static_cast<unsigned char>(message[i])) != 0;
         ++i) {
        id = id * 10U + static_cast<std::uint64_t>(message[i] - '0');
    }
    const Uid uid = static_cast<Uid>(id / 16U);
    for (const Block& block : layer.blocks) {
        if (block.uid == uid) {
            return uid;
        }
    }
    return 0U;
}

} // namespace

const LayerReport* Compiled::layer(Uid uid) const {
    for (const LayerReport& report : layers) {
        if (report.layer == uid) {
            return &report;
        }
    }
    return nullptr;
}

std::uint32_t Compiled::total_capacity() const {
    std::uint32_t total = 0U;
    for (const LayerReport& report : layers) {
        total += report.included ? report.capacity : 0U;
    }
    return total;
}

std::string blast_name(Uid block) {
    return "blast_" + std::to_string(block);
}

std::vector<PageBlast> page_blasts(const Document& document, float scale) {
    std::vector<PageBlast> out;
    for (const Layer& layer : document.layers) {
        if (!layer.enabled) {
            continue;
        }
        for (const Block& block : layer.blocks) {
            if (block.enabled && block.kind == BlockKind::Push) {
                // A blast set half its reach behind the point, against
                // "Toward", reaching half as far again past it: what stands at
                // the point is thrown along "Toward" at Power (two thirds of
                // the way to the middle of a blast half as strong again).
                const Value at = get(block, "at");
                const Value toward = get(block, "toward");
                Vec3 way{toward.x, toward.y, toward.z};
                const float length = std::sqrt(dot(way, way));
                way = length > 1.0e-6F ? way * (1.0F / length) : Vec3{0.0F, 0.0F, -1.0F};
                const float reach = std::max(number_of(block, "radius"), 0.01F);
                const Vec3 point{at.x, at.y, at.z};
                out.push_back(PageBlast{.block = block.uid,
                                        .layer = layer.uid,
                                        .blast = Blast{.name = blast_name(block.uid),
                                                       .center = (point - way * (0.5F * reach)) * scale,
                                                       .power = std::max(number_of(block, "power"), 0.0F) * 1.5F * scale,
                                                       .radius = 1.5F * reach * scale,
                                                       .time = std::max(number_of(block, "time"), 0.0F)}});
                continue;
            }
            if (!block.enabled || block.kind != BlockKind::Explosion) {
                continue;
            }
            const Value at = get(block, "at");
            out.push_back(PageBlast{.block = block.uid,
                                    .layer = layer.uid,
                                    .blast = Blast{.name = blast_name(block.uid),
                                                   .center = Vec3{at.x, at.y, at.z} * scale,
                                                   .power = std::max(number_of(block, "power"), 0.0F) * scale,
                                                   .radius = std::max(number_of(block, "radius"), 0.01F) * scale,
                                                   .time = std::max(number_of(block, "time"), 0.0F)}});
        }
    }
    std::sort(out.begin(), out.end(), [](const PageBlast& a, const PageBlast& b) { return a.block < b.block; });
    if (out.size() > max_blasts) {
        out.resize(max_blasts);
    }
    return out;
}

std::string volume_name(Uid block) {
    return "grid_" + std::to_string(block);
}

Uid volume_block(std::string_view name) {
    constexpr std::string_view prefix = "grid_";
    if (!name.starts_with(prefix) || name.size() == prefix.size()) {
        return 0U;
    }
    Uid block = 0U;
    for (const char c : name.substr(prefix.size())) {
        if (c < '0' || c > '9' || block > 0x0fffffffU) {
            return 0U;
        }
        block = block * 10U + static_cast<Uid>(c - '0');
    }
    return block;
}

Uid grid_block_named_in(std::string_view message) {
    // The runtime quotes names in single quotes (ajfx::validate).
    constexpr std::string_view lead = "volume '";
    const std::size_t at = message.find(lead);
    if (at == std::string_view::npos) {
        return 0U;
    }
    const std::size_t from = at + lead.size();
    const std::size_t end = message.find('\'', from);
    if (end == std::string_view::npos) {
        return 0U;
    }
    return volume_block(message.substr(from, end - from));
}

bool volume_only(const Layer& layer) {
    bool any = false;
    for (const Block& block : layer.blocks) {
        if (!block.enabled) {
            continue;
        }
        if (block.kind != BlockKind::SmokeGrid) {
            return false;
        }
        any = true;
    }
    return any;
}

std::vector<PageVolume> page_volumes(const Document& document, float scale) {
    std::vector<PageVolume> volumes;
    for (const Layer& layer : document.layers) {
        if (!layer.enabled) {
            continue;
        }
        for (const Block& block : layer.blocks) {
            if (!block.enabled || block.kind != BlockKind::SmokeGrid) {
                continue;
            }
            const Value centre = get(block, "center");
            const Value size = get(block, "size");
            const Value tint = get(block, "color");
            const float cell = std::max(number_of(block, "cell"), 0.0001F);
            // As many cells as the box takes at that size, at least two a
            // side: a whole number of cells, give or take a rounding error,
            // is that many and not one more. The grid stands centred on the
            // block's centre, so a box that is not a whole number of cells
            // grows evenly by less than one.
            const auto along = [cell](float extent) {
                const double count = std::ceil(static_cast<double>(std::fabs(extent)) / static_cast<double>(cell) - 1.0e-4);
                return static_cast<std::uint32_t>(std::clamp(count, 2.0, 1.0e9));
            };
            Volume volume;
            volume.name = volume_name(block.uid);
            volume.nx = along(size.x);
            volume.ny = along(size.y);
            volume.nz = along(size.z);
            volume.cell_size = cell * scale;
            volume.origin = Vec3{centre.x * scale - 0.5F * static_cast<float>(volume.nx) * volume.cell_size,
                                 centre.y * scale - 0.5F * static_cast<float>(volume.ny) * volume.cell_size,
                                 centre.z * scale - 0.5F * static_cast<float>(volume.nz) * volume.cell_size};
            volume.buoyancy = number_of(block, "rises");
            volume.weight = number_of(block, "sinks");
            volume.vorticity = number_of(block, "swirl");
            volume.pressure_iterations = static_cast<std::uint32_t>(std::lround(std::max(number_of(block, "solve"), 0.0F)));
            volume.density_dissipation = number_of(block, "fades");
            volume.temperature_dissipation = number_of(block, "fades");
            volume.velocity_dissipation = number_of(block, "calms");
            volume.extinction = number_of(block, "thickness");
            volume.albedo = number_of(block, "brightness");
            volume.color = Vec3{tint.x, tint.y, tint.z};
            volumes.push_back(PageVolume{block.uid, layer.uid, std::move(volume)});
        }
    }
    std::sort(volumes.begin(), volumes.end(),
              [](const PageVolume& a, const PageVolume& b) { return a.block < b.block; });
    return volumes;
}

Compiled compile(const Document& document, Purpose purpose, std::uint32_t live) {
    if (document.scene) { return compile_scene(document, purpose, live); }
    Compiled compiled;
    Effect effect;
    effect.name = document.name.empty() ? std::string{"effect"} : document.name;
    effect.simulation_rate = 60U;
    const float scale = std::isfinite(document.scale) && document.scale > 0.0F ? document.scale : 1.0F;
    const bool any_solo =
        purpose == Purpose::Preview &&
        std::any_of(document.layers.begin(), document.layers.end(), [](const Layer& l) { return l.solo && l.enabled; });

    const auto wanted = [&](const Layer& layer) { return layer.enabled && (!any_solo || layer.solo); };

    // The page's explosions, which every layer answers: listed on the effect
    // before any layer is checked, since a layer's shockwave names one.
    const std::vector<PageBlast> blasts = page_blasts(document, scale);
    for (const PageBlast& blast : blasts) {
        effect.blasts.push_back(blast.blast);
    }
    // And its smoke grids, which a layer's particles feed or ride by name.
    const std::vector<PageVolume> volumes = page_volumes(document, scale);
    for (const PageVolume& grid : volumes) {
        effect.volumes.push_back(grid.volume);
    }

    // Every published range first: a parameter two layers share has to be
    // sized for both before either layer's budget is worked out.
    std::vector<ParameterDesc> parameters;
    for (const Layer& layer : document.layers) {
        if (wanted(layer) && !volume_only(layer)) {
            LayerReport scratch;
            static_cast<void>(LayerBuilder{layer, scale, parameters, document.world}.build(scratch));
        }
    }
    std::set<std::string> used;

    std::set<std::string> names;
    // Names are given in page order, so which layers are rigid never
    // changes what an emitter is called.
    std::vector<std::string> emitter_names;
    emitter_names.reserve(document.layers.size());
    for (const Layer& layer : document.layers) {
        emitter_names.push_back(emitter_name(layer, names));
    }
    // The one world every solid layer that plays is part of, which every
    // particle layer meets unless it passes through, and which blocks born
    // of solid things read. Known once the solid layers have compiled and
    // merged.
    SolidWorld solids;
    // Rigid layers compile first, each on its own, and then merge into the
    // page's one world, so a layer that meets or reads its bodies validates
    // against an emitter that already exists -- the same order the runtime
    // steps them in. The emitters are assembled in page order afterwards,
    // since that is the order they are drawn in.
    std::vector<LayerReport> reports(document.layers.size());
    std::vector<std::optional<Emitter>> built(document.layers.size());
    std::vector<std::uint64_t> extras(document.layers.size(), 0U);
    std::vector<std::vector<FractureCut>> layer_cuts(document.layers.size());
    std::vector<Emitter> so_far;
    // Merges the solid layers that compiled and play into the page's one
    // world (SolidWorld, solid_world.hpp). A layer that will not fit beside
    // the rest is left out, newest first, and says why; the others play.
    const auto merge_world = [&]() {
        SolidWorld world;
        std::vector<SolidBuild> members;
        for (std::size_t i = 0U; i < document.layers.size(); ++i) {
            if (document.layers[i].kind != LayerKind::Rigid || !reports[i].included || !built[i]) {
                continue;
            }
            SolidBuild build{.index = i,
                             .layer = document.layers[i].uid,
                             .emitter = *built[i],
                             .spawn_origins = {},
                             .extra = extras[i],
                             .cuts = layer_cuts[i]};
            // A layer's report lists its spawn modules first, in stack order.
            for (std::size_t k = 0U; k < build.emitter.spawn.size() && k < reports[i].modules.size(); ++k) {
                build.spawn_origins.push_back(reports[i].modules[k].origin);
            }
            members.push_back(std::move(build));
        }
        std::sort(members.begin(), members.end(),
                  [](const SolidBuild& a, const SolidBuild& b) { return a.layer < b.layer; });
        while (!members.empty()) {
            std::string why;
            std::optional<MergedSolids> merged = merge_solids(members, why);
            // Dropped things are born in the world, at most the scene's
            // budget at once, which its capacity is sized for.
            const bool drops = merged && document.drops > 0U;
            if (drops) {
                merged->emitter.spawn.push_back(SpawnActions{.id = module_id(members.front().layer, 12U),
                                                             .max_per_step = SolidWorld::drops_per_step,
                                                             .max_alive = document.drops});
            }
            if (merged && (members.size() > 1U || drops)) {
                // Sized and checked as one emitter, as a layer is.
                Effect alone;
                alone.name = effect.name;
                alone.simulation_rate = effect.simulation_rate;
                alone.blasts = effect.blasts;
                alone.volumes = effect.volumes;
                alone.parameters = parameters;
                const std::uint64_t bound = particle_bound(alone, merged->emitter) + merged->extra;
                if (bound > max_emitter_capacity) {
                    why = "together the solid layers could have more pieces alive than one world holds";
                } else {
                    const std::uint64_t rounded = std::max<std::uint64_t>(64U, (bound + 63U) / 64U * 64U);
                    merged->emitter.capacity =
                        static_cast<std::uint32_t>(std::min<std::uint64_t>(rounded, max_emitter_capacity));
                    alone.emitters.push_back(merged->emitter);
                    if (Result<bool> valid = validate(alone); !valid) {
                        why = valid.error().message;
                    }
                }
            }
            if (merged && why.empty()) {
                world.emitter = merged->emitter.name;
                world.capacity = merged->emitter.capacity;
                world.moves = !merged->emitter.spawn.empty();
                if (drops) {
                    world.drops = static_cast<std::uint32_t>(merged->emitter.spawn.size() - 1U);
                    world.max_dropped = document.drops;
                }
                for (std::size_t m = 0U; m < members.size(); ++m) {
                    const std::size_t at = members[m].index;
                    world.layers.push_back(SolidWorld::Member{members[m].layer, merged->scopes[m]});
                    reports[at].emitter = world.emitter;
                    reports[at].scope = merged->scopes[m];
                    reports[at].emitter_capacity = world.capacity;
                    built[at].reset();
                }
                // Drawn where the world's first layer is on the page.
                built[members.front().index] = std::move(merged->emitter);
                for (FractureCut& cut : merged->cuts) {
                    compiled.cuts.push_back(std::move(cut));
                }
                break;
            }
            const SolidBuild& last = members.back();
            LayerReport& report = reports[last.index];
            report.included = false;
            report.error = "It does not fit in the page's one solid world: " + why;
            built[last.index].reset();
            if (compiled.error.empty()) {
                compiled.error = document.layers[last.index].name + ": " + report.error;
            }
            members.pop_back();
        }
        return world;
    };
    for (const bool rigid_pass : {true, false}) {
    if (!rigid_pass) {
        solids = merge_world();
        so_far.clear();
        for (const std::optional<Emitter>& emitter : built) {
            if (emitter) {
                so_far.push_back(*emitter);
            }
        }
    }
    for (std::size_t layer_index = 0U; layer_index < document.layers.size(); ++layer_index) {
        const Layer& layer = document.layers[layer_index];
        if ((layer.kind == LayerKind::Rigid) != rigid_pass) {
            continue;
        }
        LayerReport report;
        report.layer = layer.uid;
        if (volume_only(layer)) {
            // Its grids are on the effect already; there is no emitter to
            // build. A grid the runtime refused beside an earlier emitter
            // has written its report already (grid_block_named_in), which
            // is kept.
            report.included = wanted(layer);
            report.baked = false;
            report.error = reports[layer_index].error;
            report.error_block = reports[layer_index].error_block;
            reports[layer_index] = std::move(report);
            continue;
        }
        report.emitter = emitter_names[layer_index];
        // A layer that will not play still compiles, against its own copy,
        // so what it publishes never widens what the playing layers see.
        std::vector<ParameterDesc> own = parameters;
        LayerBuilder builder{layer, scale, wanted(layer) ? parameters : own, document.world,
                             rigid_pass ? SolidWorld{} : solids, blasts};
        std::optional<Emitter> emitter = builder.build(report);
        report.parameters = builder.published();
        report.baked = layer.baked;
        report.held = builder.held();
        if (emitter) {
            emitter->name = report.emitter;
            // The most particles it can have alive, with every published
            // parameter anywhere in its range; then its capacity is exactly
            // that, rounded up.
            // This layer's emitter, after the layers already in: a block
            // may name the solid world's emitter (landing on its pieces,
            // born where they hit), and the runtime wants that one to come
            // first. Those emitters have validated already, so any error
            // here is this layer's.
            Effect alone;
            alone.name = effect.name;
            alone.simulation_rate = effect.simulation_rate;
            alone.blasts = effect.blasts;
            alone.volumes = effect.volumes;
            alone.parameters = wanted(layer) ? parameters : own;
            alone.emitters = so_far;
            // What its objects come apart into is room the layer needs: a
            // body that breaks is replaced by its pieces, and a break that
            // will not fit is refused as it happens.
            const std::uint64_t bound = particle_bound(alone, *emitter) + builder.extra_for_breaking();
            if (bound > max_emitter_capacity) {
                report.error = "This layer could have more particles alive than any layer may (" +
                               std::to_string(max_emitter_capacity) + "): lower its amount or life";
                emitter.reset();
            } else {
                const std::uint64_t rounded = std::max<std::uint64_t>(64U, (bound + 63U) / 64U * 64U);
                emitter->capacity = static_cast<std::uint32_t>(std::min<std::uint64_t>(rounded, max_emitter_capacity));
                report.capacity = emitter->capacity;
                report.emitter_capacity = emitter->capacity;
                report.bytes = static_cast<std::uint64_t>(gpu::particle_layout(*emitter).stride) * emitter->capacity;
                alone.emitters.push_back(*emitter);
                if (Result<bool> valid = validate(alone); !valid) {
                    report.error = valid.error().message;
                    report.error_block = block_named_in(report.error, layer);
                    emitter.reset();
                }
            }
        }
        if (emitter && wanted(layer)) {
            report.included = true;
            for (const FractureCut& cut : builder.cuts()) {
                FractureCut asked = cut;
                asked.emitter = report.emitter;
                // An Object with no model of its own cuts whatever the layer
                // draws, as every other body is drawn.
                if (asked.model.empty()) {
                    asked.model = emitter->renderer.mesh;
                }
                layer_cuts[layer_index].push_back(std::move(asked));
            }
            extras[layer_index] = builder.extra_for_breaking();
            if (!rigid_pass) {
                so_far.push_back(*emitter);
            }
            built[layer_index] = std::move(*emitter);
            used.insert(report.parameters.begin(), report.parameters.end());
            for (const AssetRequest& request : builder.requests()) {
                if (std::none_of(compiled.requests.begin(), compiled.requests.end(),
                                 [&](const AssetRequest& r) { return r.entry == request.entry; })) {
                    compiled.requests.push_back(request);
                }
            }
        }
        if (!report.error.empty() && wanted(layer) && compiled.error.empty()) {
            compiled.error = layer.name + ": " + report.error;
            // A grid refused by the runtime is the grid's fault, not the
            // emitter's it happened to be checked beside: its own layer
            // carries the report too, so the page names the right thing.
            if (const Uid grid = grid_block_named_in(report.error); grid != 0U) {
                for (const PageVolume& volume : volumes) {
                    if (volume.block != grid) {
                        continue;
                    }
                    for (std::size_t i = 0U; i < document.layers.size(); ++i) {
                        if (document.layers[i].uid == volume.layer && reports[i].error.empty()) {
                            reports[i].error = report.error;
                            reports[i].error_block = grid;
                            compiled.error = document.layers[i].name + ": " + report.error;
                        }
                    }
                }
            }
        }
        reports[layer_index] = std::move(report);
    }
    }
    // The cuts of the layers that play: a solid layer's name the world's
    // emitter and kinds (merge_world); any other layer's are its own.
    for (std::size_t i = 0U; i < document.layers.size(); ++i) {
        if (document.layers[i].kind != LayerKind::Rigid && reports[i].included) {
            for (FractureCut& cut : layer_cuts[i]) {
                compiled.cuts.push_back(std::move(cut));
            }
        }
    }
    // Back into page order, which is the order they are drawn in.
    for (std::size_t i = 0U; i < document.layers.size(); ++i) {
        if (built[i]) {
            effect.emitters.push_back(std::move(*built[i]));
        }
        compiled.layers.push_back(std::move(reports[i]));
    }
    compiled.solids = std::move(solids);
    // A layer that meets bodies names the solid world that was going to
    // play; where it did not compile it is not there, so its name is
    // dropped rather than left for validation to trip over.
    for (Emitter& emitter : effect.emitters) {
        std::erase_if(emitter.update, [&](const UpdateModule& module) {
            const auto* meets = std::get_if<BodyCollision>(&module);
            return meets != nullptr && std::none_of(effect.emitters.begin(), effect.emitters.end(),
                                                    [&](const Emitter& e) { return e.name == meets->emitter; });
        });
    }

    for (const Emitter& emitter : effect.emitters) {
        if (!emitter.renderer.texture.empty() && !emitter.renderer.texture.starts_with("user.")) {
            if (const TextureData* texture = library_texture_data(emitter.renderer.texture)) {
                compiled.textures.emplace(emitter.renderer.texture, *texture);
            }
        }
        // The emitter's own model, then the models its body kinds collide
        // and draw as: a part of a piece names one as readily as the layer
        // does. And the models its props are shaped by: a thing that stands
        // there as its own shape is built from its model, and a world with a
        // prop whose mesh the package lacks cannot be made.
        std::vector<std::string> models{emitter.renderer.mesh};
        if (const RigidPhysics* physics = rigid_physics(emitter)) {
            for (const RigidBodyKind& of : physics->bodies) {
                models.push_back(of.mesh);
                models.push_back(of.draw);
            }
            for (const RigidProp& prop : physics->props) {
                models.push_back(prop.mesh);
            }
        }
        for (const std::string& name : models) {
            if (name.empty() || name.starts_with("user.")) {
                continue;
            }
            if (const MeshData* mesh = library_mesh(name)) {
                compiled.meshes.emplace(name, *mesh);
            }
        }
    }
    // Only what a playing layer reads is offered to the host.
    for (const ParameterDesc& parameter : parameters) {
        if (used.contains(parameter.name)) {
            effect.parameters.push_back(parameter);
        }
    }
    if (!effect.emitters.empty()) {
        compiled.effect = std::move(effect);
    }
    return compiled;
}

// Cuts the models the compiled effect's Break blocks asked to be cut, now
// that the models are read. Each cut adds a body kind and a mesh per cell and
// the Break module that scatters them, so the pieces are the piece rather
// than models laid beside it. A cut that cannot be made leaves the layer
// playing unbroken and says why.
void make_cuts(Compiled& compiled, std::string& issue) {
    if (!compiled.effect) {
        return;
    }
    for (const FractureCut& cut : compiled.cuts) {
        const auto named = [&](const std::string& why) {
            if (issue.empty()) {
                issue = why;
            }
        };
        Emitter* emitter = nullptr;
        for (Emitter& candidate : compiled.effect->emitters) {
            if (candidate.name == cut.emitter) {
                emitter = &candidate;
            }
        }
        const auto model = compiled.meshes.find(cut.model);
        if (emitter == nullptr || model == compiled.meshes.end()) {
            named("the model to cut into pieces could not be read");
            continue;
        }
        RigidPhysics* physics = nullptr;
        for (UpdateModule& module : emitter->update) {
            if (auto* found = std::get_if<RigidPhysics>(&module)) {
                physics = found;
            }
        }
        if (physics == nullptr) {
            named("a layer that cuts its model into pieces needs a physics world");
            continue;
        }
        const FractureOptions options{.count = cut.cells,
                                      .seed = cut.seed,
                                      .focus = Vec3{},
                                      .focus_bias = std::clamp(cut.spread, 0.0F, 1.0F)};
        Result<FracturedPieces> made = cells_as_pieces(
            model->second, cut.model, options, static_cast<std::uint32_t>(physics->bodies.size()) + 1U);
        if (!made) {
            named(made.error().message);
            continue;
        }
        const auto first_cell = static_cast<std::uint32_t>(physics->bodies.size()) + 1U;
        const auto cell_count = static_cast<std::uint32_t>(made.value().bodies.size());
        // An Object's size is its kind's scale, so the cells cut from it
        // keep that scale and are as big as the thing they came from.
        const float parent_scale =
            cut.body >= 1U && cut.body <= physics->bodies.size() ? physics->bodies[cut.body - 1U].scale : 1.0F;
        const RigidBodyKind parent = cut.body >= 1U && cut.body <= physics->bodies.size()
            ? physics->bodies[cut.body - 1U] : RigidBodyKind{};
        for (RigidBodyKind& kind : made.value().bodies) {
            kind.scale *= parent_scale;
            kind.draw_tint = parent.draw_tint;
            kind.density = parent.density;
            kind.friction = parent.friction;
            kind.restitution = parent.restitution;
            physics->bodies.push_back(std::move(kind));
        }
        // A cell is in every scope the thing it was cut from is in: in the
        // page's one solid world, its layer's looks and what reads its
        // bodies.
        widen_scopes(compiled, cut.emitter, cut.body, first_cell, cell_count);
        for (auto& [name, mesh] : made.value().meshes) {
            compiled.meshes.emplace(name, std::move(mesh));
        }
        emitter->update.push_back(Break{.id = cut.id,
                                        .body = cut.body,
                                        .threshold = cut.threshold,
                                        .blast_threshold = cut.blast_threshold,
                                        .scatter = cut.scatter,
                                        .spin = cut.spin,
                                        .pieces = std::move(made).value().pieces});
    }
    if (compiled.cuts.empty()) {
        return;
    }
    // The cuts went in after the compiler validated, so what they made is
    // held to the same rules here rather than reaching a host unchecked.
    if (Result<bool> valid = validate(*compiled.effect); !valid) {
        if (issue.empty()) {
            issue = valid.error().message;
        }
        compiled.effect.reset();
    }
}

std::vector<PackageEntry> asset_entries(const Compiled& compiled) {
    std::vector<PackageEntry> entries = texture_package_entries(compiled.textures);
    for (PackageEntry& entry : mesh_package_entries(compiled.meshes)) {
        entries.push_back(std::move(entry));
    }
    for (PackageEntry& entry : field_package_entries(compiled.fields)) {
        entries.push_back(std::move(entry));
    }
    return entries;
}

namespace {

// A FloatInput's most: its value, or its parameter's top end when bound.
float most(const Effect& effect, const FloatInput& input) {
    if (!input.bound()) {
        return input.value;
    }
    for (const ParameterDesc& parameter : effect.parameters) {
        if (parameter.name == input.parameter) {
            return parameter.max;
        }
    }
    return input.value;
}

} // namespace

float natural_loop_seconds(const Compiled& compiled) {
    float end = 0.0F;
    if (compiled.effect) {
        const Effect& effect = *compiled.effect;
        // Objects that appear part way through fall and settle as a thing
        // let go does, rather than living a solid layer's whole minute.
        std::set<ModuleId> appearing;
        for (const LayerReport& report : compiled.layers) {
            appearing.insert(report.appearing.begin(), report.appearing.end());
        }
        // Whatever a blast throws is seen landing: a couple of seconds past
        // the last to go off.
        for (const Blast& blast : effect.blasts) {
            end = std::max(end, std::max(blast.time, 0.0F) + 2.0F);
        }
        for (const Emitter& emitter : effect.emitters) {
            float life = 0.0F;
            for (const InitModule& module : emitter.init) {
                if (const auto* lifetime = std::get_if<Lifetime>(&module)) {
                    life = std::max({life, most(effect, lifetime->min), most(effect, lifetime->max)});
                }
            }
            for (const SpawnModule& module : emitter.spawn) {
                if (std::holds_alternative<SpawnRate>(module)) {
                    // Building up, then a whole generation at its full.
                    end = std::max(end, life * 2.0F);
                } else if (const auto* pile = std::get_if<SpawnPile>(&module)) {
                    // Let go, then a few seconds to fall and settle.
                    end = std::max(end, std::min(life, std::max(pile->release, 0.0F) + 5.0F));
                } else if (const auto* burst = std::get_if<SpawnBurst>(&module)) {
                    const float start = std::max(burst->time, 0.0F);
                    if (appearing.count(burst->id) != 0U) {
                        end = std::max(end, std::min(start + life, start + 5.0F));
                    } else if (burst->interval > 0.0F && burst->cycles == 0U) {
                        // Repeating for ever: until bursts overlap as they will from then on.
                        end = std::max(end, start + std::max(burst->interval, life) + life);
                    } else {
                        const float last = burst->interval > 0.0F
                                               ? start + burst->interval * static_cast<float>(std::max(burst->cycles, 1U) - 1U)
                                               : start;
                        end = std::max(end, last + life);
                    }
                }
            }
        }
    }
    if (!(end > 0.0F) || !std::isfinite(end)) {
        return 2.0F;
    }
    // A moment after the last particle dies, in tenths.
    return std::clamp(std::ceil((end + 0.2F) * 10.0F) / 10.0F, 1.0F, 120.0F);
}

float loop_seconds(const Document& document, const Compiled& compiled) {
    if (document.scene) { return std::clamp(document.loop_seconds, 0.5F, 3600.0F); }
    if (!document.loop_auto) {
        return std::clamp(document.loop_seconds, 0.5F, 120.0F);
    }
    return natural_loop_seconds(compiled);
}

} // namespace ajfx::studio
