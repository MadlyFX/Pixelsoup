#include "solid_world.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace ajfx::studio {
namespace {

const RigidPhysics* physics_in(const Emitter& emitter) {
    for (const UpdateModule& module : emitter.update) {
        if (const auto* found = std::get_if<RigidPhysics>(&module)) {
            return found;
        }
    }
    return nullptr;
}

// A module as the world compares it: its id says only which layer it came
// from, so two layers' page gravity is one gravity.
template <class Variant>
Variant without_id(Variant module) {
    std::visit([](auto& typed) { typed.id = ModuleId{}; }, module);
    return module;
}

// A kind of a layer that is not the world's base, made to stand without its
// layer's renderer: in the world, "Auto", "a hull of the layer's model" and
// "drawn as the layer's own" would mean the base layer's.
void stands_alone(RigidBodyKind& kind, const Emitter& from) {
    const bool models = from.renderer.geometry == Geometry::Mesh && !from.renderer.mesh.empty();
    if (kind.shape == RigidShape::Auto) {
        kind.shape = models ? RigidShape::Hull : RigidShape::Sphere;
    }
    if (kind.shape == RigidShape::Hull && kind.mesh.empty()) {
        if (models) {
            kind.mesh = from.renderer.mesh;
        } else {
            kind.shape = RigidShape::Sphere;
        }
    }
    if (kind.draw.empty() && models) {
        kind.draw = from.renderer.mesh;
    }
}

std::uint32_t mapped(const std::vector<std::uint32_t>& map, std::uint32_t value) {
    return value < map.size() ? map[value] : value;
}

// A layer's scope in the world: its spawn sources and kinds by the world's
// numbers, and its own kinds where it named none, so an unscoped module of
// the layer acts on the layer's particles alone.
Scope in_world(const Scope& scope, const std::vector<std::uint32_t>& sources, const std::vector<std::uint32_t>& kinds,
               const std::vector<std::uint32_t>& own) {
    Scope out;
    for (const std::uint32_t source : scope.sources) {
        out.sources.push_back(source == piece_spawn_source ? source : mapped(sources, source));
    }
    for (const std::uint32_t kind : scope.kinds) {
        out.kinds.push_back(mapped(kinds, kind));
    }
    if (out.kinds.empty()) {
        out.kinds = own;
    }
    return out;
}

void widen(Scope& scope, std::uint32_t parent, std::uint32_t first, std::uint32_t count) {
    if (scope.kinds.empty() || std::find(scope.kinds.begin(), scope.kinds.end(), parent) == scope.kinds.end()) {
        return;
    }
    for (std::uint32_t k = first; k < first + count && scope.kinds.size() < max_scope_entries; ++k) {
        if (std::find(scope.kinds.begin(), scope.kinds.end(), k) == scope.kinds.end()) {
            scope.kinds.push_back(k);
        }
    }
}

} // namespace

std::optional<MergedSolids> merge_solids(const std::vector<SolidBuild>& builds, std::string& why) {
    if (builds.empty()) {
        why = "there is no solid layer";
        return std::nullopt;
    }
    MergedSolids out;
    if (builds.size() == 1U) {
        // One layer is a world as it stands.
        out.emitter = builds.front().emitter;
        out.extra = builds.front().extra;
        out.cuts = builds.front().cuts;
        out.scopes.emplace_back();
        return out;
    }
    const Emitter& base = builds.front().emitter;
    const RigidPhysics* base_physics = physics_in(base);
    if (base_physics == nullptr) {
        why = "a solid layer has no physics world";
        return std::nullopt;
    }
    RigidPhysics world = *base_physics;

    // Every layer's kinds by the world's numbers: the base's are its own,
    // every other's own kind and kinds follow in uid order.
    std::vector<std::vector<std::uint32_t>> kind_maps(builds.size());
    for (std::size_t b = 0U; b < builds.size(); ++b) {
        const Emitter& emitter = builds[b].emitter;
        const RigidPhysics* physics = physics_in(emitter);
        if (physics == nullptr) {
            why = "a solid layer has no physics world";
            return std::nullopt;
        }
        std::vector<std::uint32_t>& map = kind_maps[b];
        if (b == 0U) {
            for (std::uint32_t k = 0U; k <= physics->bodies.size(); ++k) {
                map.push_back(k);
            }
            continue;
        }
        RigidBodyKind own{.shape = physics->shape,
                          .mesh = {},
                          .draw = {},
                          .scale = 1.0F,
                          .density = physics->density,
                          .friction = physics->friction,
                          .restitution = physics->restitution};
        stands_alone(own, emitter);
        world.bodies.push_back(std::move(own));
        map.push_back(static_cast<std::uint32_t>(world.bodies.size()));
        for (const RigidBodyKind& kind : physics->bodies) {
            RigidBodyKind copy = kind;
            stands_alone(copy, emitter);
            world.bodies.push_back(std::move(copy));
            map.push_back(static_cast<std::uint32_t>(world.bodies.size()));
        }
        for (const RigidProp& prop : physics->props) {
            world.props.push_back(prop);
        }
    }
    std::size_t kinds = world.bodies.size() + 1U;
    for (const SolidBuild& build : builds) {
        for (const FractureCut& cut : build.cuts) {
            kinds += cut.cells;
        }
    }
    if (kinds > max_rigid_body_kinds + 1U) {
        why = "the page's solid layers need " + std::to_string(kinds - 1U) + " kinds of body, and their one world " +
              "holds " + std::to_string(max_rigid_body_kinds);
        return std::nullopt;
    }
    if (world.props.size() > max_rigid_props) {
        why = "the page's solid layers hold " + std::to_string(world.props.size()) +
              " standing things, and their one world " + std::to_string(max_rigid_props);
        return std::nullopt;
    }

    // The spawn stack, in the order of the blocks the modules came from:
    // each draws its random values by its place, so a layer added later
    // (its blocks newer) leaves every other's where it was.
    struct Spawned final {
        Uid origin{};
        std::size_t build{0U};
        std::size_t local{0U};
    };
    std::vector<Spawned> order;
    std::vector<std::vector<std::uint32_t>> source_maps(builds.size());
    for (std::size_t b = 0U; b < builds.size(); ++b) {
        const std::size_t count = builds[b].emitter.spawn.size();
        source_maps[b].assign(count, 0U);
        for (std::size_t k = 0U; k < count; ++k) {
            const Uid origin = k < builds[b].spawn_origins.size() ? builds[b].spawn_origins[k] : Uid{};
            order.push_back(Spawned{origin, b, k});
        }
    }
    if (order.size() > max_spawn_modules) {
        why = "the page's solid layers make things in " + std::to_string(order.size()) +
              " ways, and their one world takes " + std::to_string(max_spawn_modules);
        return std::nullopt;
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const Spawned& a, const Spawned& b) { return a.origin < b.origin; });

    Emitter merged;
    merged.name = base.name;
    merged.seed = base.seed;
    merged.capacity = base.capacity;
    merged.renderer = base.renderer;
    for (std::size_t i = 0U; i < order.size(); ++i) {
        const Spawned& entry = order[i];
        SpawnModule module = builds[entry.build].emitter.spawn[entry.local];
        const std::vector<std::uint32_t>& map = kind_maps[entry.build];
        std::visit(
            [&map](auto& typed) {
                if constexpr (requires { typed.body; }) {
                    typed.body = mapped(map, typed.body);
                }
            },
            module);
        merged.spawn.push_back(std::move(module));
        source_maps[entry.build][entry.local] = static_cast<std::uint32_t>(i);
    }

    for (std::size_t b = 0U; b < builds.size(); ++b) {
        out.scopes.push_back(Scope{.sources = {}, .kinds = kind_maps[b]});
    }
    const auto scoped = [&](auto& typed, std::size_t b) {
        if constexpr (requires { typed.only; }) {
            typed.only = in_world(typed.only, source_maps[b], kind_maps[b], kind_maps[b]);
        }
    };

    // Each layer's init modules, acting on its own particles.
    for (std::size_t b = 0U; b < builds.size(); ++b) {
        for (InitModule module : builds[b].emitter.init) {
            std::visit([&](auto& typed) { scoped(typed, b); }, module);
            merged.init.push_back(std::move(module));
        }
    }

    // The update stack: the base's as it stands, with the world in place of
    // its own; every other layer's looks scoped to it and its breaks naming
    // the world's kinds; the world's forces and colliders once.
    for (std::size_t b = 0U; b < builds.size(); ++b) {
        const std::vector<std::uint32_t>& map = kind_maps[b];
        for (UpdateModule module : builds[b].emitter.update) {
            if (std::holds_alternative<RigidPhysics>(module)) {
                if (b == 0U) {
                    merged.update.emplace_back(world);
                }
                continue;
            }
            if (std::holds_alternative<ColorOverLife>(module) || std::holds_alternative<SizeOverLife>(module) ||
                std::holds_alternative<Heat>(module)) {
                std::visit([&](auto& typed) { scoped(typed, b); }, module);
                merged.update.push_back(std::move(module));
                continue;
            }
            if (auto* breaking = std::get_if<Break>(&module)) {
                breaking->body = mapped(map, breaking->body);
                for (BreakPiece& piece : breaking->pieces) {
                    piece.body = mapped(map, piece.body);
                }
                merged.update.push_back(std::move(module));
                continue;
            }
            if (std::holds_alternative<Glue>(module)) {
                // One glue a world.
                if (std::none_of(merged.update.begin(), merged.update.end(),
                                 [](const UpdateModule& m) { return std::holds_alternative<Glue>(m); })) {
                    merged.update.push_back(std::move(module));
                }
                continue;
            }
            const UpdateModule bare = without_id(module);
            if (b != 0U && std::any_of(merged.update.begin(), merged.update.end(),
                                       [&](const UpdateModule& m) { return without_id(m) == bare; })) {
                continue;
            }
            merged.update.push_back(std::move(module));
        }
    }

    for (std::size_t b = 0U; b < builds.size(); ++b) {
        out.extra += builds[b].extra;
        for (FractureCut cut : builds[b].cuts) {
            cut.emitter = merged.name;
            cut.body = mapped(kind_maps[b], cut.body);
            out.cuts.push_back(std::move(cut));
        }
    }
    out.emitter = std::move(merged);
    return out;
}

void widen_scopes(Compiled& compiled, const std::string& emitter, std::uint32_t parent, std::uint32_t first,
                  std::uint32_t count) {
    const auto widen_only = [&](auto& typed) {
        if constexpr (requires { typed.only; }) {
            widen(typed.only, parent, first, count);
        }
    };
    if (compiled.effect) {
        for (Emitter& e : compiled.effect->emitters) {
            if (e.name == emitter) {
                for (InitModule& module : e.init) {
                    std::visit(widen_only, module);
                }
                for (UpdateModule& module : e.update) {
                    std::visit(widen_only, module);
                }
                continue;
            }
            for (SpawnModule& module : e.spawn) {
                if (auto* hits = std::get_if<SpawnFromContacts>(&module); hits != nullptr && hits->emitter == emitter) {
                    widen(hits->bodies, parent, first, count);
                } else if (auto* moving = std::get_if<SpawnFromBodies>(&module);
                           moving != nullptr && moving->emitter == emitter) {
                    widen(moving->bodies, parent, first, count);
                }
            }
        }
    }
    for (LayerReport& report : compiled.layers) {
        if (report.emitter == emitter) {
            widen(report.scope, parent, first, count);
        }
    }
    if (compiled.solids.emitter == emitter) {
        for (SolidWorld::Member& member : compiled.solids.layers) {
            widen(member.bodies, parent, first, count);
        }
    }
}

} // namespace ajfx::studio
