#include "stage_pick.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <utility>
#include <variant>

namespace ajfx::studio {
namespace {

float page_scale(const Document& document) {
    return std::isfinite(document.scale) && document.scale > 0.0F ? document.scale : 1.0F;
}

ModuleId id_of(const SpawnModule& module) {
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

// The ball a prop turns within: what a click on it is measured against.
float prop_reach(const RigidProp& prop) {
    float reach = prop.size * 0.5F;
    switch (prop.shape) {
    case PropShape::Box:
    case PropShape::HollowBox:
        reach = std::max(reach, std::sqrt(dot(prop.half_size, prop.half_size)));
        break;
    case PropShape::Sphere:
        reach = std::max(reach, prop.radius);
        break;
    case PropShape::Cylinder:
    case PropShape::HollowCylinder:
    case PropShape::Cone:
    case PropShape::Pipe:
        reach = std::max({reach, prop.radius, prop.height * 0.5F});
        break;
    case PropShape::Mesh:
    case PropShape::Hull:
        break;
    }
    return std::max(reach, 0.03F);
}

// Where the ray first meets a ball, or nothing.
std::optional<float> meet_ball(Vec3 origin, Vec3 direction, Vec3 center, float radius) {
    const Vec3 from = origin - center;
    const float b = dot(from, direction);
    const float c = dot(from, from) - radius * radius;
    const float d = b * b - c;
    if (!(d >= 0.0F)) {
        return std::nullopt;
    }
    const float root = std::sqrt(d);
    float t = -b - root;
    if (t <= 0.0F) {
        // Starting inside it: where the ray leaves.
        t = -b + root;
    }
    if (!(t > 0.0F) || t > 1.0e4F) {
        return std::nullopt;
    }
    return t;
}

} // namespace

std::optional<std::size_t> solid_world_index(const Compiled& compiled) {
    if (!compiled.effect || compiled.solids.emitter.empty()) {
        return std::nullopt;
    }
    for (std::size_t i = 0U; i < compiled.effect->emitters.size(); ++i) {
        if (compiled.effect->emitters[i].name == compiled.solids.emitter) {
            return i;
        }
    }
    return std::nullopt;
}

std::vector<StageBody> stage_bodies(const Compiled& compiled, const EmitterState& world) {
    std::vector<StageBody> out;
    const std::optional<std::size_t> index = solid_world_index(compiled);
    if (!index) {
        return out;
    }
    const Emitter& emitter = compiled.effect->emitters[*index];
    const RigidPhysics* physics = rigid_physics(emitter);
    // The item each spawn module came from, by its id.
    std::map<ModuleId, Uid> origins;
    for (const ItemReport& report : compiled.items) {
        for (const ModuleId id : report.spawns) {
            origins.emplace(id, report.item);
        }
    }
    const auto item_of = [&](std::uint32_t spawn_index, std::uint32_t kind) -> Uid {
        for (const SolidWorld::Member& member : compiled.solids.items) {
            if (member.bodies.empty() || in_scope(member.bodies, spawn_index, kind)) {
                return member.item;
            }
        }
        return 0U;
    };
    const std::size_t alive = std::min(world.alive(), max_stage_bodies);
    out.reserve(alive);
    for (std::size_t i = 0U; i < alive; ++i) {
        const Vec3 p = world.position[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            continue;
        }
        const std::uint32_t kind = i < world.body_kind.size() ? world.body_kind[i] : 0U;
        const std::uint32_t spawn_index = i < world.spawn_index.size() ? world.spawn_index[i] : 0U;
        float scale = 1.0F;
        if (physics != nullptr && kind >= 1U && kind <= physics->bodies.size()) {
            scale = physics->bodies[kind - 1U].scale;
        }
        const float size = i < world.size.size() && std::isfinite(world.size[i]) ? world.size[i] : 0.1F;
        const std::uint32_t source = spawn_source(spawn_index);
        // A piece of something broken was made by no spawn module.
        Uid item = 0U;
        if (source < emitter.spawn.size()) {
            if (const auto found = origins.find(id_of(emitter.spawn[source])); found != origins.end()) {
                item = found->second;
            }
        }
        out.push_back(StageBody{.center = p,
                                .radius = std::max(0.5F * size * scale * 1.15F, 0.03F),
                                .item = item != 0U ? item : item_of(spawn_index, kind),
                                .piece = item == 0U,
                                .spawn_index = spawn_index});
    }
    if (physics != nullptr) {
        // The props, by the Object each is: the world lists its Objects'
        // in identity order, as SolidWorld lists them.
        std::vector<Uid> owners;
        for (const SolidWorld::Member& member : compiled.solids.items) {
            if (const ItemReport* report = compiled.item(member.item); report != nullptr && report->prop) {
                owners.push_back(member.item);
            }
        }
        for (std::size_t p = 0U; p < physics->props.size() && p < world.props.size(); ++p) {
            const Vec3 at = world.props[p].position;
            if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) {
                continue;
            }
            out.push_back(StageBody{.center = at,
                                    .radius = prop_reach(physics->props[p]),
                                    .item = p < owners.size() ? owners[p] : 0U});
        }
    }
    return out;
}

const StageBodies* stage_bodies_at(const std::vector<StageBodies>& samples, std::uint64_t frame) {
    const StageBodies* best = nullptr;
    for (const StageBodies& sample : samples) {
        if (sample.tick > frame) {
            break;
        }
        best = &sample;
    }
    return best;
}

float stage_floor(const Document& document) {
    static_cast<void>(document);
    return 0.0F;
}

std::optional<StageHit> pick_stage(std::span<const StageBody> bodies, Vec3 origin, Vec3 direction, float floor) {
    std::optional<float> nearest;
    const StageBody* met = nullptr;
    for (const StageBody& body : bodies) {
        if (const std::optional<float> t = meet_ball(origin, direction, body.center, body.radius)) {
            if (!nearest || *t < *nearest) {
                nearest = t;
                met = &body;
            }
        }
    }
    std::optional<float> ground;
    if (direction.y < -1.0e-4F) {
        const float t = (floor - origin.y) / direction.y;
        if (t > 0.0F && t < 1.0e4F) {
            ground = t;
        }
    }
    if (met != nullptr && (!ground || *nearest <= *ground)) {
        return StageHit{.point = origin + direction * *nearest,
                        .center = met->center,
                        .item = met->item,
                        .piece = met->piece,
                        .on_body = true,
                        .spawn_index = met->spawn_index};
    }
    if (ground) {
        const Vec3 point = origin + direction * *ground;
        return StageHit{.point = point, .center = point, .item = 0U, .piece = false, .on_body = false};
    }
    return std::nullopt;
}

} // namespace ajfx::studio
