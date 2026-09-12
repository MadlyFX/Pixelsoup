#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/linked_light.hpp>
#include <mediaserver/scene/instance_batch.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace mediaserver::scene {
namespace {
namespace ls = link_schema;
using Nodes = std::map<doc::NodeId, doc::Node>;

const doc::PropertyValue* property(const doc::Node& n, std::string_view key) {
    const auto p = n.properties.find(key);
    return p == n.properties.end() ? nullptr : &p->second;
}
std::string text(const doc::Node& n, std::string_view key) {
    const auto* p = property(n, key);
    const auto* v = p ? std::get_if<std::string>(p) : nullptr;
    return v ? *v : std::string{};
}
doc::NodeId ref(const doc::Node& n, std::string_view key) {
    const auto* p = property(n, key);
    const auto* v = p ? std::get_if<std::int64_t>(p) : nullptr;
    return v && *v > 0 ? doc::NodeId{static_cast<core::u64>(*v)} : doc::NodeId{};
}
bool boolean(const doc::Node& n, std::string_view key, bool fallback) {
    const auto* p = property(n, key);
    if (!p) return fallback;
    if (const auto* v = std::get_if<bool>(p)) return *v;
    if (const auto* v = std::get_if<double>(p)) return *v >= 0.5;
    if (const auto* v = std::get_if<std::int64_t>(p)) return *v != 0;
    return fallback;
}
void reference(doc::Node& n, std::string_view key, doc::NodeId id) {
    n.properties.insert_or_assign(std::string{key}, static_cast<std::int64_t>(id.value()));
}
Nodes nodes(const doc::Root& root) {
    Nodes result;
    for (const auto& c : doc::diff({}, root).changes) result.emplace(c.id, *c.after);
    return result;
}
bool uuid_valid(std::string_view id) {
    if (id.size() != 36) return false;
    bool nonzero = false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return false;
        } else {
            if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
            nonzero = nonzero || id[i] != '0';
        }
    }
    return nonzero;
}
bool matrix(const doc::Node& n, std::string_view prefix, Mat4& out) {
    out = identity();
    std::size_t count = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        const auto* p = property(n, std::string{prefix} + std::to_string(i));
        if (!p) continue;
        const auto* v = std::get_if<double>(p);
        if (!v || !std::isfinite(*v)) return false;
        out.m[i] = static_cast<float>(*v);
        ++count;
    }
    return (count == 0 || count == 16) && valid_link_affine(out);
}
bool factors(const doc::Node& n, Material& m, core::u32* mask = nullptr) {
    const std::pair<std::string_view, float*> fields[] = {
        {schema::color_r, &m.base_color.x}, {schema::color_g, &m.base_color.y},
        {schema::color_b, &m.base_color.z}, {schema::roughness, &m.roughness},
        {schema::metallic, &m.metallic}, {schema::emissive_r, &m.emissive.x},
        {schema::emissive_g, &m.emissive.y}, {schema::emissive_b, &m.emissive.z}};
    core::u32 bit = 1;
    for (const auto& [key, target] : fields) {
        const auto* p = property(n, key);
        const auto flag = bit; bit <<= 1U;
        if (!p) continue;
        const auto* v = std::get_if<double>(p);
        if (!v || !std::isfinite(*v) || *v < 0 || *v > 1'000'000) return false;
        if ((key == schema::roughness || key == schema::metallic) && *v > 1) return false;
        *target = static_cast<float>(*v);
        if (mask) *mask |= flag;
    }
    return true;
}
doc::NodeId allocate(const doc::Root& root, core::u64& next) {
    constexpr auto maximum = static_cast<core::u64>(INT64_MAX);
    while (next > 0 && next <= maximum && root.contains(doc::NodeId{next})) ++next;
    if (next == 0 || next > maximum) return {};
    return doc::NodeId{next++};
}
core::u64 next_id(const doc::Root& root) {
    const auto all = nodes(root);
    return all.empty() ? 1 : all.rbegin()->first.value() + 1;
}
LinkedPlanResult fail(doc::NodeId node, std::string message) {
    return LinkedPlanResult::failure({node, std::move(message)});
}
LinkedPlanResult populate(doc::Root root, doc::NodeId attachment, core::u64& next) {
    const auto* link = root.get(attachment);
    if (!link) return fail(attachment, "Missing link instance");
    const auto source = ref(*link, ls::source), scene = ref(*link, schema::scene_id);
    const auto all = nodes(root);
    std::set<doc::NodeId> present;
    for (const auto& [id, n] : all) {
        static_cast<void>(id);
        if (is_linked_occurrence(n) && ref(n, ls::instance) == attachment) present.insert(ref(n, ls::object));
    }
    for (const auto& [id, n] : all) {
        if (n.type != ls::object_type || ref(n, ls::source) != source || present.contains(id)
            || boolean(n, ls::removed, false)) continue;
        const auto new_id = allocate(root, next);
        if (new_id.value() == 0) return fail(attachment, "Document identity space exhausted");
        doc::Node occurrence{new_id, std::string{n.properties.contains(schema::light_kind) ? schema::light_type : schema::model_type}, {}};
        reference(occurrence, ls::instance, attachment);
        reference(occurrence, ls::object, id);
        reference(occurrence, schema::scene_id, scene);
        occurrence.properties[std::string{ls::policy}] = std::string{ls::follow};
        root = root.set(std::move(occurrence));
    }
    return LinkedPlanResult::success(std::move(root));
}
} // namespace

bool is_linked_occurrence(const doc::Node& n) noexcept {
    return (n.type == schema::model_type || n.type == schema::light_type) && n.properties.contains(link_schema::instance);
}
Material model_part_material(const ModelInstance& model, const Material& imported) {
    auto result = imported;
    const auto mask = model.material_override_mask;
    if (mask & 1U) result.base_color.x = model.material.base_color.x;
    if (mask & 2U) result.base_color.y = model.material.base_color.y;
    if (mask & 4U) result.base_color.z = model.material.base_color.z;
    if (mask & 8U) result.roughness = model.material.roughness;
    if (mask & 16U) result.metallic = model.material.metallic;
    if (mask & 32U) result.emissive.x = model.material.emissive.x;
    if (mask & 64U) result.emissive.y = model.material.emissive.y;
    if (mask & 128U) result.emissive.z = model.material.emissive.z;
    return result;
}
bool is_link_record(const doc::Node& n) noexcept {
    return n.type.starts_with("link.") || is_linked_occurrence(n);
}
bool valid_link_affine(const Mat4& m) noexcept {
    for (const auto v : m.m) if (!std::isfinite(v)) return false;
    // Exact affine row is intentional: admitting a small perspective residual
    // and then treating it as affine would silently alter the source matrix.
    if (m.m[3] != 0 || m.m[7] != 0 || m.m[11] != 0 || m.m[15] != 1) return false;
    double c[3][3]{};
    double product = 1;
    for (std::size_t i = 0; i < 3; ++i) {
        double squared = 0;
        for (std::size_t j = 0; j < 3; ++j) { c[i][j] = m.m[i * 4 + j]; squared += c[i][j] * c[i][j]; }
        const double len = std::sqrt(squared);
        if (len < 1e-6 || len > 1e6) return false;
        product *= len;
    }
    const double det = c[0][0]*(c[1][1]*c[2][2]-c[1][2]*c[2][1])
        - c[1][0]*(c[0][1]*c[2][2]-c[0][2]*c[2][1])
        + c[2][0]*(c[0][1]*c[1][2]-c[0][2]*c[1][1]);
    return std::abs(det) / product > 1e-6;
}
void write_link_matrix(doc::Node& n, std::string_view prefix, const Mat4& m) {
    for (std::size_t i = 0; i < m.m.size(); ++i)
        n.properties.insert_or_assign(std::string{prefix} + std::to_string(i), static_cast<double>(m.m[i]));
}
std::string link_instance_uuid(doc::NodeId id) {
    // Project-local attachment namespace, using the allocated persistent node
    // identity. Never an array index, source name, pointer, or asset hash.
    char value[37]{};
    const auto n = id.value();
    std::snprintf(value, sizeof(value), "6d736c6b-%04x-4000-8000-%012llx",
        static_cast<unsigned>((n >> 48U) & 0xffffU),
        static_cast<unsigned long long>(n & 0xffffffffffffULL));
    return value;
}

LinkedStateResult read_linked_scene(const doc::Root& root) {
    const auto all = nodes(root);
    const auto error = [](doc::NodeId id, std::string message) { return LinkedStateResult::failure({id, std::move(message)}); };
    std::set<std::tuple<std::string, doc::NodeId, std::string>> identities;
    std::map<std::pair<doc::NodeId, doc::NodeId>, doc::NodeId> placements;
    const auto lookup = [&](doc::NodeId id, std::string_view type) -> const doc::Node* {
        const auto it = all.find(id);
        return it != all.end() && it->second.type == type ? &it->second : nullptr;
    };
    for (const auto& [id, n] : all) {
        if (n.properties.contains(instance_batch_property)) {
            std::vector<Mat4> batch;
            if ((n.type != ls::object_type && n.type != schema::model_type)
                || !decode_instance_batch(text(n,instance_batch_property),batch))
                return error(id,"Invalid instance batch: expected 1..4096 positive affine transforms");
        }
        if (!is_link_record(n)) continue;
        if (!id.value() || id.value() > static_cast<core::u64>(INT64_MAX)) return error(id, "Invalid persistent document identity");
        if (is_linked_occurrence(n)) {
            const auto* link = lookup(ref(n, ls::instance), ls::instance_type);
            const auto* obj = lookup(ref(n, ls::object), ls::object_type);
            if (!link || !obj || ref(*link, ls::source) != ref(*obj, ls::source)
                || ref(n, schema::scene_id) != ref(*link, schema::scene_id)) return error(id, "Invalid occurrence ownership");
            if (!placements.emplace(std::pair{link->id, obj->id}, id).second) return error(id, "Duplicate occurrence in a link instance");
            if (property(n, schema::parent_group)) return error(id, "Linked parenting belongs to the source hierarchy");
            for (const auto key : {schema::position_x, schema::position_y, schema::position_z,
                    schema::rotation_yaw, schema::rotation_pitch, schema::rotation_roll,
                    schema::scale_x, schema::scale_y, schema::scale_z})
                if (property(n, key)) return error(id, "Linked placement requires an affine shot edit");
            const auto policy = text(n, ls::policy);
            if (policy != ls::follow && policy != ls::hold) return error(id, "Unknown linked placement policy");
            if (policy == ls::hold && !property(n, std::string{ls::held_world} + "0")) return error(id, "Hold placement requires a captured world matrix");
            Mat4 m;
            if (!matrix(n, ls::offset, m) || !matrix(n, ls::held_world, m)) return error(id, "Invalid shot transform");
        } else {
            if (n.type != ls::source_type && n.type != ls::object_type && n.type != ls::prototype_type
                && n.type != ls::material_type && n.type != ls::instance_type) return error(id, "Unknown linked schema node");
            const auto uuid = text(n, ls::uuid);
            const auto scope = n.type == ls::source_type || n.type == ls::instance_type ? doc::NodeId{} : ref(n, ls::source);
            if (!uuid_valid(uuid) || !identities.emplace(n.type, scope, uuid).second) return error(id, "Missing or duplicate persistent UUID");
            if (n.type == ls::source_type) {
                const auto* v = property(n, ls::version);
                if (!v || !std::holds_alternative<std::int64_t>(*v) || std::get<std::int64_t>(*v) != 1) return error(id, "Unsupported scene link contract version");
            } else if (!lookup(ref(n, ls::source), ls::source_type)) return error(id, "Missing source scene");
            if (n.type == ls::instance_type) {
                Mat4 m;
                if (!lookup(ref(n, schema::scene_id), schema::stage_type) || !matrix(n, ls::alignment, m)) return error(id, "Invalid stage alignment or scene");
            }
            if (n.type == ls::object_type) {
                if (boolean(n, schema::animation_enabled, false) && (!ref(n,ls::prototype).value()
                    || property(n,instance_batch_property))) return error(id,"Animation requires one assembly prototype without scatter");
                if (property(n,schema::light_kind)) {
                    const auto kind=text(n,schema::light_kind), unit=text(n,schema::light_unit);
                    if ((kind != schema::directional_kind && kind != schema::point_kind && kind != schema::spot_kind && kind != schema::rectangle_kind)
                        || unit != (kind == schema::directional_kind ? "lux" : "lumens")
                        || property(n,ls::prototype) || !linked_light_values_valid(n.properties))
                        return error(id,"Invalid linked light type, photometry or dimensions");
                }
                Mat4 m;
                if (!matrix(n, ls::local, m)) return error(id, "Nonfinite, nonaffine, singular or ill-conditioned source transform");
                for (const auto& [key, type] : {std::pair{ls::parent, ls::object_type}, {ls::prototype, ls::prototype_type}, {ls::material, ls::material_type}}) {
                    if (!property(n, key)) continue;
                    const auto* target = lookup(ref(n, key), type);
                    if (!target || ref(*target, ls::source) != ref(n, ls::source)) return error(id, "Missing or cross-source hierarchy/asset reference");
                }
            }
            if ((n.type == ls::prototype_type || n.type == ls::material_type) && text(n, ls::revision).empty()) return error(id, "Asset identity requires a separate content revision");
            if (n.type == ls::prototype_type && text(n, schema::source).empty()) return error(id, "Prototype requires a local-space model resource");
        }
        for (const auto key : {ls::removed, schema::visible, schema::animation_enabled}) {
            const auto* p = property(n, key);
            if (p && !(std::holds_alternative<bool>(*p) || std::holds_alternative<std::int64_t>(*p)
                || (std::holds_alternative<double>(*p) && std::isfinite(std::get<double>(*p))))) return error(id, "Invalid boolean property");
        }
        Material material;
        if (!factors(n, material)) return error(id, "Invalid material factor");
    }
    // Iterative topological evaluation also validates uninstantiated source
    // graphs. No recursion/stack overflow for a maliciously deep hierarchy.
    std::vector<doc::NodeId> order;
    std::set<doc::NodeId> ready;
    std::map<doc::NodeId, std::vector<doc::NodeId>> children;
    std::size_t object_count = 0;
    for (const auto& [id, n] : all) {
        if (n.type != ls::object_type) continue;
        ++object_count;
        if (property(n, ls::parent)) children[ref(n, ls::parent)].push_back(id);
        else ready.insert(id);
    }
    if (object_count > 8192 || placements.size() > 16384) return error({}, "Linked scene exceeds step 2 document capacity");
    while (!ready.empty()) {
        const auto id = *ready.begin(); ready.erase(ready.begin()); order.push_back(id);
        if (const auto it = children.find(id); it != children.end())
            for (const auto child : it->second) ready.insert(child);
    }
    if (order.size() != object_count) return error({}, "Cycle in source hierarchy");
    for (const auto& [id, n] : all) {
        if (n.type != schema::channel_type) continue;
        const auto target = ref(n, schema::channel_target);
        if (const auto it = all.find(target); it != all.end() && is_link_record(it->second)) return error(id, "Animated linked parents/placements require step 11");
    }
    LinkedSceneState state;
    std::map<SceneId, std::size_t> draw_counts;
    SceneId default_scene{};
    for (const auto& [id, n] : all) if (n.type == schema::stage_type) { default_scene = id; break; }
    for (const auto& [id, n] : all) {
        static_cast<void>(id);
        if (n.type == schema::model_type && !is_linked_occurrence(n)) {
            const auto owner = ref(n, schema::scene_id);
            const auto count = n.properties.contains(instance_batch_property)
                ? text(n,instance_batch_property).size()/instance_batch_stride : 1;
            draw_counts[owner.value() ? owner : default_scene] += count;
            if (draw_counts[owner.value() ? owner : default_scene] > max_scene_models)
                return error(id,"Instance batches exceed the whole scene model budget (4096)");
        }
    }
    for (const auto& [link_id, link] : all) {
        if (link.type != ls::instance_type) continue;
        Mat4 alignment;
        static_cast<void>(matrix(link, ls::alignment, alignment));
        struct Parent { Mat4 world; bool visible, removed; };
        std::map<doc::NodeId, Parent> worlds;
        for (const auto object_id : order) {
            const auto& obj = all.at(object_id);
            if (ref(obj, ls::source) != ref(link, ls::source)) continue;
            Mat4 local;
            static_cast<void>(matrix(obj, ls::local, local));
            const auto p = worlds.find(ref(obj, ls::parent));
            const auto basis = multiply(p == worlds.end() ? alignment : p->second.world, local);
            const bool removed = boolean(obj, ls::removed, false) || (p != worlds.end() && p->second.removed);
            const bool parent_visible = p == worlds.end() || p->second.visible;
            const auto occurrence = placements.find({link_id, object_id});
            Mat4 world = basis;
            bool visible = boolean(obj, schema::visible, true) && parent_visible;
            if (occurrence != placements.end()) {
                const auto& n = all.at(occurrence->second);
                Mat4 offset, held;
                static_cast<void>(matrix(n, ls::offset, offset));
                static_cast<void>(matrix(n, ls::held_world, held));
                const bool hold = text(n, ls::policy) == ls::hold;
                world = hold ? held : multiply(basis, offset);
                visible = boolean(n, schema::visible, boolean(obj, schema::visible, true)) && parent_visible;
                LinkedOccurrence result{n.id, link_id, object_id, ref(link, schema::scene_id), world, basis, hold, removed, visible && !removed,
                    text(obj, schema::group_name), {}, {}};
                if (const auto* prototype = lookup(ref(obj, ls::prototype), ls::prototype_type)) result.source = text(*prototype, schema::source);
                result.animated=boolean(obj,schema::animation_enabled,false);
                result.animation.enabled=boolean(n,schema::animation_enabled,result.animated);
                result.animation.loop=boolean(n,schema::animation_loop,boolean(obj,schema::animation_loop,true));
                const auto integer_value=[&](std::string_view key, core::u64 fallback, core::u64 maximum) {
                    const auto* p=property(n,key); if (!p) p=property(obj,key);
                    if (!p) return fallback;
                    const auto* v=std::get_if<core::i64>(p);
                    return v && *v >= 0 ? std::min(static_cast<core::u64>(*v),maximum) : fallback;
                };
                result.animation.start_frame=integer_value(schema::animation_start_frame,0,INT64_MAX);
                result.animation.clip=static_cast<core::u32>(integer_value(schema::animation_clip,0,63));
                result.animation.wind_period_frames=static_cast<core::u32>(std::max(core::u64{2},integer_value(schema::wind_period_frames,120,1000000)));
                const auto wind_value=[&](std::string_view key) {
                    const auto* p=property(n,key); const auto* v=p ? std::get_if<double>(p) : nullptr;
                    return v && std::isfinite(*v) ? static_cast<float>(std::clamp(*v,-100.0,100.0)) : 0.0F;
                };
                result.animation.wind={wind_value(schema::wind_x),wind_value(schema::wind_y),wind_value(schema::wind_z)};
                if (!result.source.empty()) {
                    result.batch = text(obj,instance_batch_property);
                    draw_counts[result.scene] += result.batch.empty() ? 1 : result.batch.size()/instance_batch_stride;
                    if (draw_counts[result.scene] > max_scene_models) return error(n.id, "Linked occurrences exceed the scene model budget (4096 including batches)");
                    const Vec3 x{world.m[0],world.m[1],world.m[2]}, y{world.m[4],world.m[5],world.m[6]}, z{world.m[8],world.m[9],world.m[10]};
                    if (dot(x,cross(y,z)) < 0) return error(n.id, "Reflected linked occurrence is pending renderer parity qualification (step 1)");
                }
                if (const auto* material = lookup(ref(obj, ls::material), ls::material_type)) static_cast<void>(factors(*material, result.material, &result.material_mask));
                static_cast<void>(factors(n, result.material, &result.material_mask));
                if (property(obj,schema::light_kind)) {
                    if (n.type != schema::light_type) return error(n.id,"Source changed between light and mesh; create a new source identity");
                    result.light[std::string{schema::light_kind}]=text(obj,schema::light_kind);
                    result.light[std::string{schema::light_unit}]=text(obj,schema::light_unit);
                    for (auto key : linked_light_fields) if (const auto* v=property(obj,key)) result.light[std::string{key}]=*v;
                    const Vec3 x{world.m[0],world.m[1],world.m[2]}, y{world.m[4],world.m[5],world.m[6]}, z{world.m[8],world.m[9],world.m[10]};
                    const auto nx=normalize(x), ny=normalize(y), nz=normalize(z);
                    if (dot(nx,cross(ny,nz))<0.9999F || std::abs(dot(nx,ny))>1e-4F || std::abs(dot(nx,nz))>1e-4F || std::abs(dot(ny,nz))>1e-4F)
                        return error(n.id,"Linked lights require positive orthogonal placement; shear/reflection is unsupported");
                    const auto direction=ny * -1.0F;
                    result.light[std::string{schema::position_x}]=double(world.m[12]);
                    result.light[std::string{schema::position_y}]=double(world.m[13]);
                    result.light[std::string{schema::position_z}]=double(world.m[14]);
                    result.light[std::string{schema::direction_x}]=double(direction.x);
                    result.light[std::string{schema::direction_y}]=double(direction.y);
                    result.light[std::string{schema::direction_z}]=double(direction.z);
                    for (auto [key,scale] : {std::pair{schema::light_width,std::sqrt(dot(x,x))},std::pair{schema::light_height,std::sqrt(dot(z,z))}})
                        if (auto it=result.light.find(key); it!=result.light.end()) it->second=std::get<double>(it->second)*scale;
                    const auto axis=std::abs(direction.x)<0.9F ? Vec3{1,0,0} : Vec3{0,0,1};
                    const auto tangent=normalize(axis-direction*dot(axis,direction));
                    result.light[std::string{schema::area_rotation_turns}]=double(std::atan2(dot(nx,cross(direction,tangent)),dot(nx,tangent))/(2*3.14159265358979323846));
                    for (auto key : linked_light_overrides) if (const auto* v=property(n,key)) result.light[std::string{key}]=*v;
                    if (!linked_light_values_valid(result.light)) return error(n.id,"Invalid linked light shot values");
                    result.light[std::string{schema::visible}]=result.visible;
                } else if (n.type == schema::light_type) return error(n.id,"Source changed between light and mesh; create a new source identity");
                state.occurrences.emplace(n.id, std::move(result));
            }
            if (!valid_link_affine(basis) || !valid_link_affine(world)) return error(object_id, "Hierarchy composition or hold conversion is singular/ill-conditioned");
            worlds.emplace(object_id, Parent{world, visible, removed});
        }
    }
    // Count before SceneModel's fixed arrays can truncate. Hidden/orphan lights
    // remain authored capacity, just like linked geometry.
    std::map<SceneId,std::pair<std::size_t,std::size_t>> lights;
    for (const auto& [id,n] : all) if (n.type==schema::stage_type && boolean(n,schema::sky_physical_enabled,false)) lights[id].first=1;
    for (const auto& [id,n] : all) if (n.type==schema::light_type) {
        auto owner=ref(n,schema::scene_id); if (!owner.value()) owner=default_scene;
        const auto p=state.occurrences.find(id);
        const auto kind=text(n,schema::light_kind);
        const bool directional=p!=state.occurrences.end() ? text(all.at(p->second.object),schema::light_kind)==schema::directional_kind
            : kind.empty() || kind==schema::directional_kind;
        auto& count=lights[owner]; if (directional) ++count.first; else ++count.second;
        if (count.first>max_scene_directional_lights || count.second>max_scene_point_lights) return error(id,"Linked scene exceeds 4 directional / 64 local lights");
    }
    return LinkedStateResult::success(std::move(state));
}

LinkedPlanResult plan_linked_change(const doc::Root& before, const doc::Root& candidate) {
    for (const auto& c : doc::diff(before, candidate).changes) {
        if (!c.before || !is_link_record(*c.before)) continue;
        if (!c.after) {
            if (c.before->type != ls::instance_type && !is_linked_occurrence(*c.before)) return fail(c.id, "Source removal requires a recoverable tombstone");
            continue;
        }
        const auto& a = *c.before; const auto& b = *c.after;
        if (a.type != b.type || text(a, ls::uuid) != text(b, ls::uuid) || ref(a, ls::source) != ref(b, ls::source)
            || ref(a, ls::instance) != ref(b, ls::instance) || ref(a, ls::object) != ref(b, ls::object)) return fail(c.id, "Persistent linked identity cannot be reassigned");
        if (a.type == ls::object_type && text(a, ls::origin) != text(b, ls::origin)) return fail(c.id, "Origin changes require an explicit geometry/placement rebase; candidate retained for resolution");
    }
    auto parsed = read_linked_scene(candidate);
    if (!parsed) return LinkedPlanResult::failure(parsed.error());
    return LinkedPlanResult::success(candidate);
}
LinkedPlanResult plan_link_source_update(const doc::Root& root, std::span<const doc::Node> records, doc::NodeId first_id) {
    auto candidate = root;
    std::set<doc::NodeId> seen;
    for (const auto& n : records) {
        if ((n.type != ls::source_type && n.type != ls::object_type && n.type != ls::prototype_type && n.type != ls::material_type)
            || !seen.insert(n.id).second) return fail(n.id, "Source transaction contains a shot record or duplicate document ID");
        if (const auto* old = root.get(n.id); old && old->type != n.type) return fail(n.id, "Source transaction collides with a document identity");
        candidate = candidate.set(n);
    }
    auto next = first_id.value();
    for (const auto& [id, n] : nodes(candidate)) {
        if (n.type != ls::instance_type) continue;
        auto populated = populate(candidate, id, next);
        if (!populated) return populated;
        candidate = std::move(populated).value();
    }
    return plan_linked_change(root, candidate);
}
LinkedPlanResult plan_attach_link(const doc::Root& root, doc::NodeId source, SceneId scene, doc::NodeId first_id, const Mat4& alignment) {
    auto next = first_id.value();
    const auto id = allocate(root, next);
    if (!id.value()) return fail(first_id, "Invalid first document identity");
    doc::Node link{id, std::string{ls::instance_type}, {}};
    reference(link, ls::source, source); reference(link, schema::scene_id, scene);
    link.properties[std::string{ls::uuid}] = link_instance_uuid(id);
    write_link_matrix(link, ls::alignment, alignment);
    auto candidate = populate(root.set(std::move(link)), id, next);
    if (!candidate) return candidate;
    return plan_linked_change(root, candidate.value());
}
LinkedPlanResult plan_linked_world(const doc::Root& root, doc::NodeId occurrence, const Mat4& world) {
    auto state = read_linked_scene(root);
    if (!state) return LinkedPlanResult::failure(state.error());
    const auto it = state.value().occurrences.find(occurrence);
    if (it == state.value().occurrences.end()) return fail(occurrence, "Select a linked occurrence");
    auto n = *root.get(occurrence);
    if (boolean(n, schema::locked, false)) return fail(occurrence, "Linked occurrence is locked");
    if (const auto* link = root.get(it->second.instance); link && boolean(*link,schema::locked,false))
        return fail(occurrence,"Linked collection is locked");
    // invert_affine uses a full 3x3 cofactor inverse (including shear). Validate
    // first so its singular-input identity fallback can never count as success.
    const auto local = it->second.held ? world : multiply(invert_affine(it->second.basis), world);
    if (!valid_link_affine(world) || !valid_link_affine(local)) return fail(occurrence, "Shot placement cannot be converted through this hierarchy");
    write_link_matrix(n, it->second.held ? ls::held_world : ls::offset, local);
    return plan_linked_change(root, root.set(std::move(n)));
}
LinkedPlanResult plan_link_alignment(const doc::Root& root, doc::NodeId instance, const Mat4& alignment) {
    const auto* existing = root.get(instance);
    if (!existing || existing->type != ls::instance_type) return fail(instance, "Select a linked scene attachment");
    if (boolean(*existing, schema::locked, false)) return fail(instance, "Linked collection is locked");
    auto n = *existing; write_link_matrix(n, ls::alignment, alignment);
    return plan_linked_change(root, root.set(std::move(n)));
}
LinkedPlanResult snapshot_link(const doc::Root& root, doc::NodeId attachment) {
    const auto* link = root.get(attachment);
    if (!link || link->type != ls::instance_type) return fail(attachment, "Select a linked collection");
    auto state = read_linked_scene(root);
    if (!state) return LinkedPlanResult::failure(state.error());
    doc::Root snapshot;
    doc::Node group{attachment, std::string{schema::group_type}, {}};
    reference(group, schema::scene_id, ref(*link, schema::scene_id));
    const auto* source = root.get(ref(*link, ls::source));
    auto name = source ? text(*source, schema::group_name) : std::string{};
    group.properties[std::string{schema::group_name}] = (name.empty() ? "Blender collection" : name) + " (independent)";
    snapshot = snapshot.set(std::move(group));
    for (const auto& [id, placed] : state.value().occurrences) {
        if (placed.instance != attachment || placed.removed || (placed.source.empty() && placed.light.empty())) continue;
        auto n = *root.get(id);
        std::erase_if(n.properties, [](const auto& p) { return p.first.starts_with("link_"); });
        if (!placed.light.empty()) {
            for (const auto& [key,value] : placed.light) n.properties[key]=value;
            n.properties[std::string{schema::group_name}]=placed.name;
            snapshot=snapshot.set(std::move(n));
            continue;
        }
        n.properties[std::string{schema::source}] = placed.source;
        n.properties[std::string{schema::animation_enabled}]=placed.animation.enabled;
        n.properties[std::string{schema::animation_loop}]=placed.animation.loop;
        n.properties[std::string{schema::animation_start_frame}]=static_cast<core::i64>(placed.animation.start_frame);
        if (placed.animated) n.properties[std::string{schema::bake_static}]=false;
        if (!placed.batch.empty()) n.properties[std::string{instance_batch_property}] = placed.batch;
        n.properties[std::string{schema::group_name}] = placed.name;
        n.properties[std::string{schema::visible}] = placed.visible;
        reference(n, schema::parent_group, attachment);
        n.properties[std::string{schema::position_x}] = double(placed.world.m[12]);
        n.properties[std::string{schema::position_y}] = double(placed.world.m[13]);
        n.properties[std::string{schema::position_z}] = double(placed.world.m[14]);
        auto residual = placed.world; residual.m[12] = residual.m[13] = residual.m[14] = 0;
        write_link_matrix(n, schema::model_affine, residual);
        n.properties[std::string{schema::model_factor_overrides}] = true;
        const auto& m = placed.material;
        const std::pair<std::string_view, float> fields[] = {
            {schema::color_r,m.base_color.x},{schema::color_g,m.base_color.y},{schema::color_b,m.base_color.z},
            {schema::roughness,m.roughness},{schema::metallic,m.metallic},
            {schema::emissive_r,m.emissive.x},{schema::emissive_g,m.emissive.y},{schema::emissive_b,m.emissive.z}};
        core::u32 bit = 1;
        for (const auto& [key,value] : fields) {
            if (placed.material_mask & bit) n.properties[std::string{key}] = double(value);
            else n.properties.erase(std::string{key});
            bit <<= 1U;
        }
        snapshot = snapshot.set(std::move(n));
    }
    return LinkedPlanResult::success(std::move(snapshot));
}
LinkedPlanResult paste_link_snapshot(const doc::Root& root, const doc::Root& snapshot, SceneId target) {
    const auto* stage = root.get(target);
    if (!stage || stage->type != schema::stage_type) return fail(target, "Select a Preview scene");
    auto result = root;
    auto next = next_id(root);
    std::map<doc::NodeId,doc::NodeId> ids;
    for (const auto& c : doc::diff({}, snapshot).changes) {
        if (c.after->type != schema::group_type && c.after->type != schema::model_type
            && c.after->type != schema::light_type)
            return fail(c.id, "Invalid independent collection snapshot");
        const auto id = allocate(result, next);
        if (!id.value()) return fail(c.id, "Document identity space exhausted");
        ids.emplace(c.id,id);
    }
    if (ids.empty()) return fail({}, "Copy a linked collection first");
    for (const auto& c : doc::diff({}, snapshot).changes) {
        auto n = *c.after; n.id = ids.at(c.id);
        reference(n,schema::scene_id,target);
        const auto parent = ref(n,schema::parent_group);
        if (parent.value()) {
            if (!ids.contains(parent)) return fail(c.id, "Snapshot parent is missing");
            reference(n,schema::parent_group,ids.at(parent));
        }
        result = result.set(std::move(n));
    }
    return plan_linked_change(root,result);
}
LinkedPlanResult plan_linked_edit(const doc::Root& root, doc::NodeId occurrence, LinkedEdit edit) {
    auto state = read_linked_scene(root);
    if (!state) return LinkedPlanResult::failure(state.error());
    const auto* selected = root.get(occurrence);
    const auto it = state.value().occurrences.find(occurrence);
    const auto attachment = selected && selected->type == ls::instance_type ? occurrence
        : it != state.value().occurrences.end() ? it->second.instance : doc::NodeId{};
    if (attachment.value()) {
        const auto* link = root.get(attachment);
        if (boolean(*link, schema::locked, false)) return fail(attachment, "Linked collection is locked");
        if (edit == LinkedEdit::Pause || edit == LinkedEdit::Resume) {
            auto n = *link; n.properties[std::string{ls::paused}] = edit == LinkedEdit::Pause;
            return plan_linked_change(root,root.set(std::move(n)));
        }
        if (edit == LinkedEdit::Unlink) {
            for (const auto& [id,p] : state.value().occurrences)
                if (p.instance == attachment && boolean(*root.get(id),schema::locked,false))
                    return fail(id,"Unlock the collection's objects before unlinking");
            auto snapshot = snapshot_link(root,attachment);
            if (!snapshot) return snapshot;
            auto candidate = *root.erase(attachment);
            for (const auto& [id,p] : state.value().occurrences)
                if (p.instance == attachment) candidate = *candidate.erase(id);
            for (const auto& c : doc::diff({},snapshot.value()).changes) candidate = candidate.set(*c.after);
            // This explicit conversion is the sole identity-changing link edit.
            auto valid = read_linked_scene(candidate);
            if (!valid) return LinkedPlanResult::failure(valid.error());
            return LinkedPlanResult::success(std::move(candidate));
        }
        if (edit == LinkedEdit::DuplicateLink) {
            auto candidate = root;
            auto next = next_id(root);
            const auto id = allocate(candidate,next);
            if (!id.value()) return fail(attachment,"Document identity space exhausted");
            auto copy = *link; copy.id = id;
            copy.properties[std::string{ls::uuid}] = link_instance_uuid(id);
            candidate = candidate.set(std::move(copy));
            for (const auto& [old_id,p] : state.value().occurrences) {
                if (p.instance != attachment) continue;
                auto object = *root.get(old_id); object.id = allocate(candidate,next);
                if (!object.id.value()) return fail(attachment,"Document identity space exhausted");
                reference(object,ls::instance,id); candidate = candidate.set(std::move(object));
            }
            return plan_linked_change(root,candidate);
        }
    }
    if (it == state.value().occurrences.end()) return fail(occurrence, "Select a linked occurrence");
    const auto& placed = it->second;
    auto n = *root.get(occurrence);
    if (boolean(n, schema::locked, false)) return fail(occurrence, "Linked occurrence is locked");
    switch (edit) {
    case LinkedEdit::Hold:
        n.properties[std::string{ls::policy}] = std::string{ls::hold};
        write_link_matrix(n, ls::held_world, placed.world); break;
    case LinkedEdit::Follow:
        n.properties[std::string{ls::policy}] = std::string{ls::follow};
        write_link_matrix(n, ls::offset, multiply(invert_affine(placed.basis), placed.world)); break;
    case LinkedEdit::ResetPlacement:
        n.properties[std::string{ls::policy}] = std::string{ls::follow};
        write_link_matrix(n, ls::offset, identity());
        for (std::size_t i = 0; i < 16; ++i) n.properties.erase(std::string{ls::held_world} + std::to_string(i));
        break;
    case LinkedEdit::Hide: n.properties[std::string{schema::visible}] = false; break;
    case LinkedEdit::Show: n.properties[std::string{schema::visible}] = true; break;
    case LinkedEdit::FollowVisibility: n.properties.erase(std::string{schema::visible}); break;
    case LinkedEdit::ResetMaterial:
        if (!placed.light.empty()) for (auto key : linked_light_overrides) n.properties.erase(std::string{key});
        for (const auto key : {schema::color_r, schema::color_g, schema::color_b, schema::roughness, schema::metallic, schema::emissive_r, schema::emissive_g, schema::emissive_b}) n.properties.erase(std::string{key});
        break;
    case LinkedEdit::ForgetRemoved:
        if (!placed.removed) return fail(occurrence, "Only removed source objects can discard their recovery record");
        return plan_linked_change(root, *root.erase(occurrence));
    case LinkedEdit::DuplicateLink: break;
    case LinkedEdit::Pause: case LinkedEdit::Resume: case LinkedEdit::Unlink: break;
    }
    return plan_linked_change(root, root.set(std::move(n)));
}
} // namespace mediaserver::scene
