#include "sandbox.hpp"

#include "blast.hpp"
#include "catalog.hpp"

#include <ajfx/random.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <utility>
#include <variant>

namespace ajfx::studio {
namespace {

// How far above the floor a dropped thing starts, in the page's metres.
constexpr float drop_height = 1.0F;

float page_scale(const Document& document) {
    return std::isfinite(document.scale) && document.scale > 0.0F ? document.scale : 1.0F;
}

// Sets a control through the catalog, so a value is held to its range as one
// typed would be.
void put(Block& block, std::string_view control, Value value) {
    const BlockDef* def = find_block_def(block.kind);
    const ControlDef* found = def != nullptr ? find_control(*def, control) : nullptr;
    if (found != nullptr) {
        block.set(control, sanitize(*found, std::move(value)));
    }
}

Vec3 unit(Vec3 v, Vec3 fallback) {
    const float length = std::sqrt(dot(v, v));
    return length > 1.0e-6F && std::isfinite(length) ? v * (1.0F / length) : fallback;
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

// The layer a tool writes an event into: `hint` where it is one, else the
// page's first; 0 on a page with none.
Uid event_layer(const Document& document, Uid hint) {
    if (document.layer(hint) != nullptr) {
        return hint;
    }
    return document.layers.empty() ? 0U : document.layers.front().uid;
}

std::optional<SandboxEdit> add_event(Document& document, Uid hint, BlockKind kind,
                                     std::initializer_list<std::pair<std::string_view, Value>> values) {
    const Uid layer_uid = event_layer(document, hint);
    std::optional<Block> block = make_block(document, kind);
    Layer* layer = document.layer(layer_uid);
    if (!block || layer == nullptr) {
        return std::nullopt;
    }
    for (const auto& [control, value] : values) {
        put(*block, control, value);
    }
    conform_block(*block, layer->kind);
    const Uid uid = insert_block(*layer, layer->blocks.size(), std::move(*block));
    return SandboxEdit{layer_uid, uid};
}

std::uint32_t next_seed(std::uint32_t seed, Uid uid) {
    // Kept to six digits, as the seed field shows them, and never 0 (the
    // layer's own) or the seed it was.
    std::uint32_t next = hash_mix(hash_mix(0x5eed5eedU, seed), uid) % 1000000U;
    if (next == 0U || next == seed) {
        next = seed % 999999U + 1U;
    }
    return next;
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
    // The block each spawn module came from, by its id.
    std::map<ModuleId, Uid> origins;
    for (const LayerReport& report : compiled.layers) {
        for (const ModuleLine& line : report.modules) {
            if (const auto* spawn = std::get_if<SpawnModule>(&line.module)) {
                origins.emplace(id_of(*spawn), line.origin);
            }
        }
    }
    const auto layer_of = [&](std::uint32_t spawn_index, std::uint32_t kind) -> Uid {
        for (const SolidWorld::Member& member : compiled.solids.layers) {
            if (member.bodies.empty() || in_scope(member.bodies, spawn_index, kind)) {
                return member.layer;
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
        Uid block = 0U;
        if (source < emitter.spawn.size()) {
            if (const auto found = origins.find(id_of(emitter.spawn[source])); found != origins.end()) {
                block = found->second;
            }
        }
        out.push_back(StageBody{.center = p,
                                .radius = std::max(0.5F * size * scale * 1.15F, 0.03F),
                                .layer = layer_of(spawn_index, kind),
                                .block = block});
    }
    if (physics != nullptr) {
        // The props, by the Object each is: the world's lists its members'
        // in the order of their layers' uids, as SolidWorld lists them.
        std::vector<std::pair<Uid, Uid>> owners;
        for (const SolidWorld::Member& member : compiled.solids.layers) {
            if (const LayerReport* report = compiled.layer(member.layer)) {
                for (const Uid block : report->props) {
                    owners.emplace_back(member.layer, block);
                }
            }
        }
        for (std::size_t p = 0U; p < physics->props.size() && p < world.props.size(); ++p) {
            const Vec3 at = world.props[p].position;
            if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) {
                continue;
            }
            out.push_back(StageBody{.center = at,
                                    .radius = prop_reach(physics->props[p]),
                                    .layer = p < owners.size() ? owners[p].first : 0U,
                                    .block = p < owners.size() ? owners[p].second : 0U});
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
    return document.world.ground ? document.world.ground_height * page_scale(document) : 0.0F;
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
                        .layer = met->layer,
                        .block = met->block,
                        .on_body = true};
    }
    if (ground) {
        const Vec3 point = origin + direction * *ground;
        return StageHit{.point = point, .center = point, .layer = 0U, .block = 0U, .on_body = false};
    }
    return std::nullopt;
}

std::string sandbox_material(const Document& document, Uid selected, Uid layer_uid) {
    const auto material_of = [](const Block& block) -> std::string {
        const std::string material = choice_of(block, "material");
        return find_material(material) != nullptr ? material : std::string{};
    };
    if (const Block* block = document.block(selected); block != nullptr && block->kind == BlockKind::Object) {
        if (std::string material = material_of(*block); !material.empty()) {
            return material;
        }
    }
    if (const Layer* layer = document.layer(layer_uid)) {
        for (auto it = layer->blocks.rbegin(); it != layer->blocks.rend(); ++it) {
            if (it->kind == BlockKind::Object) {
                if (std::string material = material_of(*it); !material.empty()) {
                    return material;
                }
            }
        }
    }
    return "wood";
}

std::optional<SandboxEdit> drop_thing(Document& document, Uid hint, Vec3 floor_point, float seconds,
                                      std::string_view material) {
    // Into a solid layer: the one asked for, the page's first, or a new one.
    Uid layer_uid = 0U;
    if (const Layer* asked = document.layer(hint); asked != nullptr && asked->kind == LayerKind::Rigid) {
        layer_uid = hint;
    } else {
        for (const Layer& layer : document.layers) {
            if (layer.kind == LayerKind::Rigid && layer.enabled) {
                layer_uid = layer.uid;
                break;
            }
        }
    }
    if (layer_uid == 0U) {
        Layer made = make_layer(document, LayerWhat::Solid, "Dropped things");
        layer_uid = made.uid;
        document.layers.push_back(std::move(made));
    }
    std::optional<Block> block = make_block(document, BlockKind::Object);
    Layer* layer = document.layer(layer_uid);
    if (!block || layer == nullptr) {
        return std::nullopt;
    }
    const float s = page_scale(document);
    const float size = number_of(*block, "size");
    const Vec3 at = floor_point * (1.0F / s) + Vec3{0.0F, size * 0.5F + drop_height, 0.0F};
    put(*block, "model", choice("cube"));
    put(*block, "at", vec3(at.x, at.y, at.z));
    put(*block, "appear", number(std::max(seconds, 0.0F)));
    put(*block, "material", choice(std::string{material.empty() ? std::string_view{"wood"} : material}));
    conform_block(*block, layer->kind);
    const Uid uid = insert_block(*layer, layer->blocks.size(), std::move(*block));
    // Its weight, grip and bounce, as picking the material would set them.
    static_cast<void>(apply_material(document, uid));
    return SandboxEdit{layer_uid, uid};
}

std::optional<SandboxEdit> explode_here(Document& document, Uid hint, Vec3 point, float seconds) {
    const Vec3 at = point * (1.0F / page_scale(document));
    return add_event(document, hint, BlockKind::Explosion,
                     {{"at", vec3(at.x, at.y, at.z)}, {"time", number(std::max(seconds, 0.0F))}});
}

std::optional<SandboxEdit> push_here(Document& document, Uid hint, Vec3 point, Vec3 away, float seconds) {
    const Vec3 at = point * (1.0F / page_scale(document));
    const Vec3 way = unit(away, Vec3{0.0F, 0.0F, -1.0F});
    return add_event(document, hint, BlockKind::Push,
                     {{"at", vec3(at.x, at.y, at.z)},
                      {"toward", vec3(way.x, way.y, way.z)},
                      {"time", number(std::max(seconds, 0.0F))}});
}

bool reroll_seed(Document& document, Uid layer_uid) {
    const Layer* asked = layer_uid != 0U ? document.layer(layer_uid) : nullptr;
    if (layer_uid != 0U && asked == nullptr) {
        return false;
    }
    const bool solid = asked != nullptr && asked->kind == LayerKind::Rigid;
    bool changed = false;
    for (Layer& layer : document.layers) {
        const bool chosen = layer_uid == 0U || layer.uid == layer_uid || (solid && layer.kind == LayerKind::Rigid);
        if (chosen) {
            layer.seed = next_seed(layer.seed, layer.uid);
            changed = true;
        }
    }
    return changed;
}

std::vector<PageEvent> page_events(const Document& document) {
    std::vector<PageEvent> events;
    for (const Layer& layer : document.layers) {
        if (!layer.enabled) {
            continue;
        }
        for (const Block& block : layer.blocks) {
            if (!block.enabled) {
                continue;
            }
            float seconds = -1.0F;
            if (block.kind == BlockKind::Explosion || block.kind == BlockKind::Push) {
                seconds = std::max(number_of(block, "time"), 0.0F);
            } else if (block.kind == BlockKind::Object && layer.kind == LayerKind::Rigid &&
                       choice_of(block, "arrange") == "one" && number_of(block, "appear") > 0.0F) {
                seconds = number_of(block, "appear");
            }
            if (seconds < 0.0F) {
                continue;
            }
            std::string label = block_title(block);
            label += block.kind == BlockKind::Object ? " appears at " : " at ";
            label += format_number(seconds, 2) + " s";
            events.push_back(PageEvent{layer.uid, block.uid, seconds, std::move(label)});
        }
    }
    std::sort(events.begin(), events.end(), [](const PageEvent& a, const PageEvent& b) {
        return a.seconds != b.seconds ? a.seconds < b.seconds : a.block < b.block;
    });
    return events;
}

} // namespace ajfx::studio
