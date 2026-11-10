#include <mediaserver/scene/scene_project.hpp>

#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/scene/selection_set.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <tuple>

namespace mediaserver::scene {
namespace {

[[nodiscard]] const doc::PropertyValue* find(
    const doc::PropertyMap& properties,
    std::string_view name) noexcept {
    const auto found = properties.find(name);
    return found == properties.end() ? nullptr : &found->second;
}

[[nodiscard]] std::optional<doc::NodeId> referenced_id(
    const doc::Node& node,
    std::string_view property) noexcept {
    const auto* const value = find(node.properties, property);
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto* const integer = std::get_if<std::int64_t>(value);
    if (integer == nullptr || *integer < 0) {
        return std::nullopt;
    }
    return doc::NodeId{static_cast<core::u64>(*integer)};
}

[[nodiscard]] bool is_direct_scoped_type(std::string_view type) noexcept {
    return type == schema::camera_type || type == schema::object_type
        || type == schema::group_type
        || type == schema::timeline_lane_type
        || type == schema::timeline_marker_type || type == schema::media_type
        || type == schema::model_type || type == schema::particle_effect_type
        || type == link_schema::instance_type
        || type == schema::light_type || type == "publication.settings";
}

[[nodiscard]] bool is_drawable_type(std::string_view type) noexcept {
    return type == schema::object_type || type == schema::media_type
        || type == schema::model_type
        || type == schema::particle_effect_type;
}

[[nodiscard]] bool belongs_to(const doc::Node& node, SceneId source) noexcept {
    const auto* const value = find(node.properties, schema::scene_id);
    if (value == nullptr) {
        return false;
    }
    const auto* const integer = std::get_if<std::int64_t>(value);
    return integer != nullptr && *integer >= 0
        && static_cast<core::u64>(*integer) == source.value();
}

void write_reference(
    doc::PropertyMap& properties,
    std::string_view name,
    doc::NodeId id) {
    properties.insert_or_assign(
        std::string{name}, static_cast<std::int64_t>(id.value()));
}

[[nodiscard]] bool is_groupable_type(std::string_view type) noexcept {
    return type == schema::group_type || is_drawable_type(type);
}

[[nodiscard]] bool read_integer_property(
    const doc::Node& node, std::string_view property,
    std::int64_t fallback, std::int64_t& value) noexcept {
    const auto* const held = find(node.properties, property);
    if (held == nullptr) {
        value = fallback;
        return true;
    }
    const auto* const integer = std::get_if<std::int64_t>(held);
    if (integer == nullptr) {
        return false;
    }
    value = *integer;
    return true;
}

[[nodiscard]] bool read_number_property(
    const doc::Node& node, std::string_view property, float fallback,
    float& value) noexcept {
    const auto* const held = find(node.properties, property);
    if (held == nullptr) {
        value = fallback;
        return true;
    }
    double wide{};
    if (const auto* const real = std::get_if<double>(held)) {
        wide = *real;
    } else if (const auto* const integer = std::get_if<std::int64_t>(held)) {
        wide = static_cast<double>(*integer);
    } else {
        return false;
    }
    value = static_cast<float>(wide);
    return std::isfinite(value);
}

[[nodiscard]] bool read_boolean_property(
    const doc::Node& node, std::string_view property, bool fallback,
    bool& value) noexcept {
    const auto* const held = find(node.properties, property);
    if (held == nullptr) {
        value = fallback;
        return true;
    }
    if (const auto* const boolean = std::get_if<bool>(held)) {
        value = *boolean;
        return true;
    }
    if (const auto* const integer = std::get_if<std::int64_t>(held)) {
        value = *integer != 0;
        return true;
    }
    if (const auto* const real = std::get_if<double>(held)) {
        if (!std::isfinite(*real)) {
            return false;
        }
        value = *real >= 0.5;
        return true;
    }
    return false;
}

[[nodiscard]] bool read_optional_reference(
    const doc::Node& node, std::string_view property,
    std::optional<doc::NodeId>& value) noexcept {
    const auto* const held = find(node.properties, property);
    if (held == nullptr) {
        value.reset();
        return true;
    }
    const auto* const integer = std::get_if<std::int64_t>(held);
    if (integer == nullptr || *integer < 0) {
        return false;
    }
    value = doc::NodeId{static_cast<core::u64>(*integer)};
    return true;
}

[[nodiscard]] std::optional<SceneId> authored_scene(const doc::Node& node) noexcept {
    std::optional<doc::NodeId> value;
    if (!read_optional_reference(node, schema::scene_id, value)) {
        return std::nullopt;
    }
    return value;
}

struct GroupTransform final {
    Vec3 position{};
    Turns yaw{};
    Turns pitch{};
    Turns roll{};
    Vec3 scale{1.0F, 1.0F, 1.0F};
};

[[nodiscard]] bool read_group_transform(
    const doc::Node& node, GroupTransform& value) noexcept {
    return read_number_property(node, schema::position_x, 0.0F,
                                value.position.x)
        && read_number_property(node, schema::position_y, 0.0F,
                                value.position.y)
        && read_number_property(node, schema::position_z, 0.0F,
                                value.position.z)
        && read_number_property(node, schema::rotation_yaw, 0.0F, value.yaw)
        && read_number_property(node, schema::rotation_pitch, 0.0F,
                                value.pitch)
        && read_number_property(node, schema::rotation_roll, 0.0F,
                                value.roll)
        && read_number_property(node, schema::scale_x, 1.0F, value.scale.x)
        && read_number_property(node, schema::scale_y, 1.0F, value.scale.y)
        && read_number_property(node, schema::scale_z, 1.0F, value.scale.z);
}

[[nodiscard]] Mat4 transform_matrix(const GroupTransform& value) noexcept {
    return multiply(
        translation(value.position),
        multiply(rotation(value.yaw, value.pitch, value.roll),
                 scaling(value.scale)));
}

void write_number(doc::PropertyMap& properties, std::string_view name,
                  float value) {
    properties.insert_or_assign(std::string{name}, static_cast<double>(value));
}

void write_group_transform(doc::Node& node, const GroupTransform& value) {
    write_number(node.properties, schema::position_x, value.position.x);
    write_number(node.properties, schema::position_y, value.position.y);
    write_number(node.properties, schema::position_z, value.position.z);
    write_number(node.properties, schema::rotation_yaw, value.yaw);
    write_number(node.properties, schema::rotation_pitch, value.pitch);
    write_number(node.properties, schema::rotation_roll, value.roll);
    write_number(node.properties, schema::scale_x, value.scale.x);
    write_number(node.properties, schema::scale_y, value.scale.y);
    write_number(node.properties, schema::scale_z, value.scale.z);
}

// Director-local decomposition. The resulting binary32 values are authored
// and replicated, so platform libm differences cannot enter frame evaluation.
// A recompose check is the contract boundary: shear/singular/non-affine input
// fails explicitly instead of being approximated.
// `exact` demands the recomposed matrix match bit for bit, which is what an
// ungroup of authored values needs. Moving a node under a rotated parent goes
// through an inverse, whose rounding no TRS can reproduce exactly, so that path
// accepts a recomposition within a part in ten thousand of the original.
[[nodiscard]] std::optional<GroupTransform> decompose_group_transform(
    const Mat4& matrix, bool exact = true) noexcept {
    for (const float value : matrix.m) {
        if (!std::isfinite(value)) {
            return std::nullopt;
        }
    }
    if (std::abs(matrix.at(3U, 0U)) > 1.0e-5F
        || std::abs(matrix.at(3U, 1U)) > 1.0e-5F
        || std::abs(matrix.at(3U, 2U)) > 1.0e-5F
        || std::abs(matrix.at(3U, 3U) - 1.0F) > 1.0e-5F) {
        return std::nullopt;
    }
    Vec3 column0{matrix.at(0U, 0U), matrix.at(1U, 0U),
                 matrix.at(2U, 0U)};
    Vec3 column1{matrix.at(0U, 1U), matrix.at(1U, 1U),
                 matrix.at(2U, 1U)};
    Vec3 column2{matrix.at(0U, 2U), matrix.at(1U, 2U),
                 matrix.at(2U, 2U)};
    float scale_x = length(column0);
    const float scale_y = length(column1);
    const float scale_z = length(column2);
    constexpr float singular_epsilon = 1.0e-6F;
    if (!(scale_x > singular_epsilon) || !(scale_y > singular_epsilon)
        || !(scale_z > singular_epsilon)) {
        return std::nullopt;
    }
    column0 = column0 * (1.0F / scale_x);
    column1 = column1 * (1.0F / scale_y);
    column2 = column2 * (1.0F / scale_z);
    float determinant = dot(cross(column0, column1), column2);
    if (determinant < 0.0F) {
        scale_x = -scale_x;
        column0 = -column0;
        determinant = -determinant;
    }
    constexpr float orthogonal_epsilon = 2.0e-4F;
    if (std::abs(dot(column0, column1)) > orthogonal_epsilon
        || std::abs(dot(column0, column2)) > orthogonal_epsilon
        || std::abs(dot(column1, column2)) > orthogonal_epsilon
        || std::abs(determinant - 1.0F) > orthogonal_epsilon) {
        return std::nullopt;
    }

    const float sine_pitch = std::clamp(-column2.y, -1.0F, 1.0F);
    const double pitch_radians = std::asin(static_cast<double>(sine_pitch));
    const double cosine_pitch = std::cos(pitch_radians);
    double yaw_radians{};
    double roll_radians{};
    if (std::abs(cosine_pitch) > 1.0e-5) {
        yaw_radians = std::atan2(static_cast<double>(column2.x),
                                 static_cast<double>(column2.z));
        roll_radians = std::atan2(-static_cast<double>(column0.y),
                                  static_cast<double>(column1.y));
    } else {
        const double sign = sine_pitch >= 0.0F ? 1.0 : -1.0;
        yaw_radians = std::atan2(
            sign * static_cast<double>(column1.x),
            static_cast<double>(column0.x));
        roll_radians = 0.0;
    }
    constexpr double tau = 6.283185307179586476925286766559;
    GroupTransform result{};
    result.position = Vec3{matrix.at(0U, 3U), matrix.at(1U, 3U),
                           matrix.at(2U, 3U)};
    result.yaw = static_cast<float>(yaw_radians / tau);
    result.pitch = static_cast<float>(pitch_radians / tau);
    result.roll = static_cast<float>(roll_radians / tau);
    result.scale = Vec3{scale_x, scale_y, scale_z};
    if (exact) {
        return transform_matrix(result) == matrix
            ? std::optional<GroupTransform>{result}
            : std::nullopt;
    }
    const Mat4 recomposed = transform_matrix(result);
    float magnitude = 1.0F;
    for (const float value : matrix.m) {
        magnitude = std::max(magnitude, std::abs(value));
    }
    for (std::size_t index = 0U; index < matrix.m.size(); ++index) {
        if (std::abs(recomposed.m[index] - matrix.m[index])
            > 1.0e-4F * magnitude) {
            return std::nullopt;
        }
    }
    return result;
}

[[nodiscard]] bool is_transform_component(std::string_view value) noexcept {
    return value == schema::position_x || value == schema::position_y
        || value == schema::position_z || value == schema::rotation_yaw
        || value == schema::rotation_pitch || value == schema::rotation_roll
        || value == schema::scale_x || value == schema::scale_y
        || value == schema::scale_z;
}

[[nodiscard]] bool has_transform_animation(
    const std::map<doc::NodeId, doc::Node>& nodes,
    const std::set<doc::NodeId>& targets) noexcept {
    for (const auto& [id, node] : nodes) {
        static_cast<void>(id);
        if (node.type != schema::channel_type) {
            continue;
        }
        const auto target = referenced_id(node, schema::channel_target);
        const auto* const component = find(node.properties,
                                           schema::channel_component);
        const auto* const name = component == nullptr
            ? nullptr
            : std::get_if<std::string>(component);
        if (target.has_value() && targets.contains(*target)
            && name != nullptr && is_transform_component(*name)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::map<doc::NodeId, doc::Node> document_nodes(
    const doc::Root& root) {
    std::map<doc::NodeId, doc::Node> nodes;
    for (const auto& change : doc::diff(doc::Root{}, root).changes) {
        if (change.after) {
            nodes.emplace(change.id, *change.after);
        }
    }
    return nodes;
}

[[nodiscard]] doc::Root apply_group_plan(
    const doc::Root& root, const std::vector<doc::Node>& puts,
    const std::vector<doc::NodeId>& erases) {
    doc::Root result = root;
    for (const doc::Node& node : puts) {
        result = result.set(node);
    }
    for (const doc::NodeId id : erases) {
        const auto erased = result.erase(id);
        if (erased.has_value()) {
            result = *erased;
        }
    }
    return result;
}

} // namespace

std::optional<doc::NodeId> SceneClonePlan::cloned_id(
    doc::NodeId original) const noexcept {
    const auto found = std::lower_bound(
        remap.begin(), remap.end(), original,
        [](const auto& mapping, doc::NodeId id) noexcept {
            return mapping.first < id;
        });
    if (found == remap.end() || found->first != original) {
        return std::nullopt;
    }
    return found->second;
}

core::Result<SceneClonePlan, SceneCloneError> plan_clone_scene(
    const doc::Root& root,
    SceneId source,
    doc::NodeId first_id) {
    using Result = core::Result<SceneClonePlan, SceneCloneError>;
    constexpr core::u64 max_reference_id =
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());
    if (first_id.value() > max_reference_id) {
        return Result::failure(SceneCloneError::InvalidFirstId);
    }

    try {
        SceneModel model;
        model.reset(root);
        const auto authored_scenes = model.scenes();
        const auto source_info = std::lower_bound(
            authored_scenes.begin(), authored_scenes.end(), source,
            [](const SceneInfo& info, SceneId id) noexcept {
                return info.id < id;
            });
        if (source_info == authored_scenes.end() || source_info->id != source) {
            return Result::failure(SceneCloneError::SourceNotFound);
        }
        std::map<doc::NodeId, doc::Node> all;
        for (const auto& change : doc::diff(doc::Root{}, root).changes) {
            if (change.after) {
                all.emplace(change.id, *change.after);
            }
        }

        std::set<doc::NodeId> selected;
        selected.insert(source);
        std::set<doc::NodeId> selected_drawables;
        std::set<doc::NodeId> selected_direct;
        for (const auto& [id, node] : all) {
            if (!is_direct_scoped_type(node.type)
                || !belongs_to(node, source)) {
                continue;
            }
            selected.insert(id);
            selected_direct.insert(id);
            if (is_drawable_type(node.type)) {
                selected_drawables.insert(id);
            }
        }

        std::set<doc::NodeId> selected_effects;
        for (const auto& [id, node] : all) {
            if (node.type != schema::effect_type) {
                continue;
            }
            const auto target = referenced_id(node, schema::effect_target);
            if (target.has_value() && selected_drawables.contains(*target)) {
                selected.insert(id);
                selected_effects.insert(id);
            }
        }
        for (const auto& [id, node] : all) {
            if (node.type != schema::channel_type) {
                continue;
            }
            const auto target = referenced_id(node, schema::channel_target);
            if (target.has_value()
                && (selected_direct.contains(*target)
                    || selected_effects.contains(*target))) {
                selected.insert(id);
            }
        }
        // A mask belongs to the drawable or group it cuts, and a Bezier
        // mask's points belong to the mask.
        std::set<doc::NodeId> selected_masks;
        for (const auto& [id, node] : all) {
            if (node.type != schema::mask_type) {
                continue;
            }
            const auto target = referenced_id(node, schema::mask_target);
            if (target.has_value() && selected_direct.contains(*target)) {
                selected.insert(id);
                selected_masks.insert(id);
            }
        }
        for (const auto& [id, node] : all) {
            if (node.type != schema::mask_point_type) {
                continue;
            }
            const auto mask = referenced_id(node, schema::mask_id);
            if (mask.has_value() && selected_masks.contains(*mask)) {
                selected.insert(id);
            }
        }

        SceneClonePlan plan{};
        plan.source = source;
        plan.puts.reserve(selected.size());
        plan.remap.reserve(selected.size());

        core::u64 candidate = first_id.value();
        for (const doc::NodeId original : selected) {
            while (root.contains(doc::NodeId{candidate})) {
                if (candidate == max_reference_id) {
                    return Result::failure(SceneCloneError::IdExhausted);
                }
                ++candidate;
            }
            const doc::NodeId clone{candidate};
            plan.remap.emplace_back(original, clone);
            if (candidate != max_reference_id) {
                ++candidate;
            } else if (plan.remap.size() != selected.size()) {
                return Result::failure(SceneCloneError::IdExhausted);
            }
        }
        plan.destination = *plan.cloned_id(source);

        for (const auto& [original, clone] : plan.remap) {
            doc::Node node = all.at(original);
            node.id = clone;
            if (original == source) {
                node.properties.erase(std::string{schema::blender_file});
                node.properties.erase(std::string{schema::blender_link_source});
            }
            if (node.type == link_schema::instance_type)
                node.properties.insert_or_assign(std::string{link_schema::uuid}, link_instance_uuid(clone));
            if (is_linked_occurrence(node)) {
                const auto link = referenced_id(node, link_schema::instance);
                if (link && plan.cloned_id(*link))
                    write_reference(node.properties, link_schema::instance, *plan.cloned_id(*link));
            }
            if (is_direct_scoped_type(node.type)) {
                write_reference(node.properties, schema::scene_id, plan.destination);
                if (node.type == schema::group_type
                    || is_drawable_type(node.type)) {
                    const auto parent =
                        referenced_id(node, schema::parent_group);
                    if (parent.has_value()) {
                        const auto cloned_parent = plan.cloned_id(*parent);
                        if (cloned_parent.has_value()) {
                            write_reference(node.properties,
                                            schema::parent_group,
                                            *cloned_parent);
                        }
                    }
                }
                if (node.type == schema::media_type) {
                    const auto lane = referenced_id(node, schema::lane_id);
                    if (lane.has_value()) {
                        const auto cloned_lane = plan.cloned_id(*lane);
                        if (cloned_lane.has_value()) {
                            write_reference(
                                node.properties, schema::lane_id,
                                *cloned_lane);
                        }
                    }
                }
            } else if (node.type == schema::effect_type) {
                const auto target = referenced_id(node, schema::effect_target);
                write_reference(
                    node.properties, schema::effect_target,
                    *plan.cloned_id(*target));
            } else if (node.type == schema::channel_type) {
                const auto target = referenced_id(node, schema::channel_target);
                write_reference(
                    node.properties, schema::channel_target,
                    *plan.cloned_id(*target));
            } else if (node.type == schema::mask_type) {
                const auto target = referenced_id(node, schema::mask_target);
                write_reference(
                    node.properties, schema::mask_target,
                    *plan.cloned_id(*target));
                write_reference(node.properties, schema::scene_id,
                                plan.destination);
            } else if (node.type == schema::mask_point_type) {
                const auto mask = referenced_id(node, schema::mask_id);
                write_reference(
                    node.properties, schema::mask_id,
                    *plan.cloned_id(*mask));
                write_reference(node.properties, schema::scene_id,
                                plan.destination);
            }
            plan.puts.push_back(std::move(node));
        }
        return Result::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return Result::failure(SceneCloneError::AllocationFailed);
    }
}

core::Result<SceneDeletePlan, SceneDeleteError> plan_delete_scene(
    const doc::Root& root, SceneId scene) {
    using Result = core::Result<SceneDeletePlan, SceneDeleteError>;
    const doc::Node* const stage = root.get(scene);
    if (stage == nullptr || stage->type != schema::stage_type) {
        return Result::failure(SceneDeleteError::SceneNotFound);
    }

    try {
        const std::map<doc::NodeId, doc::Node> all = document_nodes(root);

        std::set<doc::NodeId> selected{scene};
        for (const auto& [id, node] : all) {
            const auto owner = referenced_id(node, schema::scene_id);
            if (owner.has_value() && *owner == scene) {
                // scene_id is the common ownership field used by scene,
                // compositor, and replicated editor metadata nodes.
                selected.insert(id);
            }
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& [id, node] : all) {
                if (selected.contains(id)) {
                    continue;
                }
                bool dependent = false;
                if (node.type == schema::effect_type) {
                    const auto target = referenced_id(node,
                                                      schema::effect_target);
                    dependent = target.has_value()
                        && selected.contains(*target);
                } else if (node.type == schema::channel_type) {
                    const auto target = referenced_id(node,
                                                      schema::channel_target);
                    dependent = target.has_value()
                        && selected.contains(*target);
                } else if (node.type == selection_set_schema::member_type) {
                    const auto set = referenced_id(
                        node, selection_set_schema::set_id);
                    dependent = set.has_value() && selected.contains(*set);
                } else if (node.type == "compositor.edge") {
                    const auto from = referenced_id(node, "from_node");
                    const auto to = referenced_id(node, "to_node");
                    dependent = (from.has_value() && selected.contains(*from))
                        || (to.has_value() && selected.contains(*to));
                }
                if (dependent) {
                    selected.insert(id);
                    changed = true;
                }
            }
        }

        SceneDeletePlan plan{};
        plan.scene = scene;
        plan.erases.assign(selected.begin(), selected.end());
        return Result::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return Result::failure(SceneDeleteError::AllocationFailed);
    }
}

core::Result<EffectDeletePlan, EffectDeletePlanError> plan_delete_effect(
    const doc::Root& root,
    doc::NodeId effect) {
    using Result = core::Result<EffectDeletePlan, EffectDeletePlanError>;
    const doc::Node* const existing = root.get(effect);
    if (existing == nullptr) {
        return Result::failure(EffectDeletePlanError::EffectNotFound);
    }
    if (existing->type != schema::effect_type) {
        return Result::failure(EffectDeletePlanError::NotAnEffect);
    }

    try {
        EffectDeletePlan plan{};
        for (const doc::NodeChange& change : doc::diff(doc::Root{}, root).changes) {
            if (!change.after || change.after->type != schema::channel_type) {
                continue;
            }
            const auto target =
                referenced_id(*change.after, schema::channel_target);
            if (target.has_value() && *target == effect) {
                plan.erases.push_back(change.after->id);
            }
        }
        std::sort(plan.erases.begin(), plan.erases.end());
        plan.erases.push_back(effect);
        return Result::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return Result::failure(EffectDeletePlanError::AllocationFailed);
    }
}

core::Result<GroupEditPlan, GroupEditError> plan_group_nodes(
    const doc::Root& root,
    SceneId scene,
    std::span<const doc::NodeId> selection,
    doc::NodeId first_id,
    std::string_view name) {
    using Result = core::Result<GroupEditPlan, GroupEditError>;
    constexpr core::u64 max_reference_id =
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());
    if (selection.empty()) {
        return Result::failure(GroupEditError::EmptySelection);
    }
    if (name.empty()) {
        return Result::failure(GroupEditError::InvalidName);
    }
    if (first_id.value() == 0U || first_id.value() > max_reference_id) {
        return Result::failure(GroupEditError::InvalidFirstId);
    }

    try {
        SceneModel model;
        model.reset(root);
        const std::vector<SceneInfo> authored_scenes = model.scenes();
        const auto authored_scene_entry = std::lower_bound(
            authored_scenes.begin(), authored_scenes.end(), scene,
            [](const SceneInfo& info, SceneId id) noexcept {
                return info.id < id;
            });
        const bool valid_scene = authored_scene_entry != authored_scenes.end()
            && authored_scene_entry->id == scene;
        if (!valid_scene) {
            return Result::failure(GroupEditError::InvalidScene);
        }

        const std::map<doc::NodeId, doc::Node> nodes = document_nodes(root);
        std::vector<doc::NodeId> selected{selection.begin(), selection.end()};
        std::sort(selected.begin(), selected.end());
        if (std::adjacent_find(selected.begin(), selected.end())
            != selected.end()) {
            return Result::failure(GroupEditError::DuplicateSelection);
        }

        struct Child final {
            doc::Node node;
            core::i64 prior_order{};
        };
        std::vector<Child> children;
        children.reserve(selected.size());
        for (const doc::NodeId id : selected) {
            const auto found = nodes.find(id);
            if (found == nodes.end() || !is_groupable_type(found->second.type)) {
                return Result::failure(GroupEditError::UnsupportedNode);
            }
            const doc::Node& source = found->second;
            if (is_linked_occurrence(source)) return Result::failure(GroupEditError::UnsupportedNode);
            const auto source_scene = authored_scene(source);
            if (!source_scene.has_value() || *source_scene != scene) {
                return Result::failure(GroupEditError::CrossSceneSelection);
            }
            std::optional<doc::NodeId> parent;
            if (!read_optional_reference(source, schema::parent_group,
                                         parent)) {
                return Result::failure(GroupEditError::InvalidHierarchy);
            }
            if (parent.has_value()) {
                return Result::failure(GroupEditError::AlreadyGrouped);
            }
            bool locked{};
            if (!read_boolean_property(source, schema::locked, false, locked)) {
                return Result::failure(GroupEditError::UnsupportedNode);
            }
            if (locked) {
                return Result::failure(GroupEditError::Locked);
            }
            core::i64 order{};
            if (!read_integer_property(source, schema::group_order, 0, order)) {
                return Result::failure(GroupEditError::InvalidHierarchy);
            }
            children.push_back(Child{source, order});
        }
        std::stable_sort(
            children.begin(), children.end(),
            [](const Child& left, const Child& right) noexcept {
                return std::pair{left.prior_order, left.node.id.value()}
                    < std::pair{right.prior_order, right.node.id.value()};
            });

        core::u64 candidate = first_id.value();
        while (root.contains(doc::NodeId{candidate})) {
            if (candidate == max_reference_id) {
                return Result::failure(GroupEditError::IdExhausted);
            }
            ++candidate;
        }
        const doc::NodeId group_id{candidate};

        doc::Node group{};
        group.id = group_id;
        group.type = std::string{schema::group_type};
        write_reference(group.properties, schema::scene_id, scene);
        group.properties.emplace(std::string{schema::group_name},
                                 std::string{name});
        group.properties.emplace(std::string{schema::group_order},
                                 children.front().prior_order);
        group.properties.emplace(std::string{schema::locked}, false);
        group.properties.emplace(std::string{schema::visible}, true);
        write_group_transform(group, GroupTransform{});

        GroupEditPlan plan{};
        plan.scene = scene;
        plan.group = group_id;
        plan.primary_after = group_id;
        plan.puts.reserve(children.size() + 1U);
        plan.puts.push_back(std::move(group));
        for (std::size_t index = 0U; index < children.size(); ++index) {
            if (index > static_cast<std::size_t>(
                            std::numeric_limits<core::i64>::max())) {
                return Result::failure(GroupEditError::OrderOverflow);
            }
            doc::Node child = std::move(children[index].node);
            write_reference(child.properties, schema::parent_group, group_id);
            child.properties.insert_or_assign(
                std::string{schema::group_order},
                static_cast<core::i64>(index));
            plan.puts.push_back(std::move(child));
        }
        std::sort(plan.puts.begin(), plan.puts.end(),
                  [](const doc::Node& left, const doc::Node& right) noexcept {
                      return left.id < right.id;
                  });
        plan.after = apply_group_plan(root, plan.puts, plan.erases);
        return Result::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return Result::failure(GroupEditError::AllocationFailed);
    }
}

core::Result<GroupEditPlan, GroupEditError> plan_ungroup_nodes(
    const doc::Root& root, SceneId scene, doc::NodeId group_id) {
    using Result = core::Result<GroupEditPlan, GroupEditError>;
    try {
        const doc::Node* const group_node = root.get(group_id);
        if (group_node == nullptr) {
            return Result::failure(GroupEditError::GroupNotFound);
        }
        if (group_node->type != schema::group_type) {
            return Result::failure(GroupEditError::NotAGroup);
        }

        SceneModel model;
        model.reset(root);
        const std::vector<SceneInfo> authored_scenes = model.scenes();
        const auto authored_scene_entry = std::lower_bound(
            authored_scenes.begin(), authored_scenes.end(), scene,
            [](const SceneInfo& info, SceneId id) noexcept {
                return info.id < id;
            });
        const bool valid_scene = authored_scene_entry != authored_scenes.end()
            && authored_scene_entry->id == scene;
        if (!valid_scene) {
            return Result::failure(GroupEditError::InvalidScene);
        }
        const auto group_scene = authored_scene(*group_node);
        if (!group_scene.has_value() || *group_scene != scene) {
            return Result::failure(GroupEditError::CrossSceneSelection);
        }
        const std::vector<SceneGroup> valid_groups = model.groups(scene);
        const auto projected_group = std::find_if(
            valid_groups.begin(), valid_groups.end(),
            [group_id](const SceneGroup& group) noexcept {
                return group.node == group_id;
            });
        if (projected_group == valid_groups.end()) {
            return Result::failure(GroupEditError::InvalidHierarchy);
        }
        if (projected_group->effective_locked) {
            return Result::failure(GroupEditError::Locked);
        }
        if (!projected_group->visible) {
            // Drawables do not yet carry their own visibility bit, so removing
            // this group would make its children visible and cannot preserve
            // the authored result.
            return Result::failure(
                GroupEditError::VisibilityNotRepresentable);
        }

        GroupTransform group_transform{};
        if (!read_group_transform(*group_node, group_transform)) {
            return Result::failure(GroupEditError::TransformNotRepresentable);
        }
        const Mat4 group_local = transform_matrix(group_transform);
        const std::map<doc::NodeId, doc::Node> nodes = document_nodes(root);

        struct Child final {
            doc::Node node;
            core::i64 order{};
            GroupTransform transform{};
        };
        std::vector<Child> children;
        for (const auto& [id, node] : nodes) {
            if (!is_groupable_type(node.type)) {
                continue;
            }
            std::optional<doc::NodeId> parent;
            if (!read_optional_reference(node, schema::parent_group, parent)) {
                continue;
            }
            if (!parent.has_value() || *parent != group_id) {
                continue;
            }
            const auto child_scene = authored_scene(node);
            if (!child_scene.has_value() || *child_scene != scene) {
                return Result::failure(GroupEditError::InvalidHierarchy);
            }
            bool locked{};
            if (!read_boolean_property(node, schema::locked, false, locked)) {
                return Result::failure(GroupEditError::UnsupportedNode);
            }
            if (locked) {
                return Result::failure(GroupEditError::Locked);
            }
            core::i64 order{};
            GroupTransform transform{};
            if (!read_integer_property(node, schema::group_order, 0, order)
                || !read_group_transform(node, transform)) {
                return Result::failure(
                    GroupEditError::TransformNotRepresentable);
            }
            children.push_back(Child{node, order, transform});
        }
        std::stable_sort(
            children.begin(), children.end(),
            [](const Child& left, const Child& right) noexcept {
                return std::pair{left.order, left.node.id.value()}
                    < std::pair{right.order, right.node.id.value()};
            });

        std::set<doc::NodeId> transform_targets{group_id};
        for (const Child& child : children) {
            transform_targets.insert(child.node.id);
        }
        if (!children.empty()
            && has_transform_animation(nodes, transform_targets)) {
            return Result::failure(GroupEditError::AnimatedTransform);
        }

        std::optional<doc::NodeId> destination_parent;
        if (!read_optional_reference(*group_node, schema::parent_group,
                                     destination_parent)) {
            return Result::failure(GroupEditError::InvalidHierarchy);
        }
        core::i64 base_order{};
        if (!read_integer_property(*group_node, schema::group_order, 0,
                                   base_order)) {
            return Result::failure(GroupEditError::InvalidHierarchy);
        }

        GroupEditPlan plan{};
        plan.scene = scene;
        plan.group = group_id;
        plan.primary_after = destination_parent;
        plan.puts.reserve(children.size());
        for (std::size_t index = 0U; index < children.size(); ++index) {
            if (index > static_cast<std::size_t>(
                            std::numeric_limits<core::i64>::max())
                || base_order
                    > std::numeric_limits<core::i64>::max()
                        - static_cast<core::i64>(index)) {
                return Result::failure(GroupEditError::OrderOverflow);
            }
            const Mat4 local = multiply(
                group_local, transform_matrix(children[index].transform));
            const auto decomposed = decompose_group_transform(local);
            if (!decomposed.has_value()) {
                return Result::failure(
                    GroupEditError::TransformNotRepresentable);
            }
            doc::Node child = std::move(children[index].node);
            write_group_transform(child, *decomposed);
            if (destination_parent.has_value()) {
                write_reference(child.properties, schema::parent_group,
                                *destination_parent);
            } else {
                child.properties.erase(std::string{schema::parent_group});
            }
            child.properties.insert_or_assign(
                std::string{schema::group_order},
                base_order + static_cast<core::i64>(index));
            plan.puts.push_back(std::move(child));
        }
        std::sort(plan.puts.begin(), plan.puts.end(),
                  [](const doc::Node& left, const doc::Node& right) noexcept {
                      return left.id < right.id;
                  });

        for (const auto& [id, node] : nodes) {
            if (node.type != schema::channel_type) {
                continue;
            }
            const auto target = referenced_id(node, schema::channel_target);
            if (target.has_value() && *target == group_id) {
                plan.erases.push_back(id);
            }
        }
        plan.erases.push_back(group_id);
        std::sort(plan.erases.begin(), plan.erases.end());
        plan.after = apply_group_plan(root, plan.puts, plan.erases);
        return Result::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return Result::failure(GroupEditError::AllocationFailed);
    }
}

core::Result<GroupEditPlan, GroupEditError>
plan_delete_group_preserving_children(
    const doc::Root& root, SceneId scene, doc::NodeId group) {
    return plan_ungroup_nodes(root, scene, group);
}

core::Result<GroupEditPlan, GroupEditError> plan_move_into_group(
    const doc::Root& root,
    SceneId scene,
    std::span<const doc::NodeId> selection,
    std::optional<doc::NodeId> group,
    std::optional<doc::NodeId> anchor,
    bool before) {
    using Result = core::Result<GroupEditPlan, GroupEditError>;
    if (selection.empty()) {
        return Result::failure(GroupEditError::EmptySelection);
    }
    try {
        SceneModel model;
        model.reset(root);
        const std::vector<SceneInfo> authored_scenes = model.scenes();
        const auto authored_scene_entry = std::lower_bound(
            authored_scenes.begin(), authored_scenes.end(), scene,
            [](const SceneInfo& info, SceneId id) noexcept {
                return info.id < id;
            });
        const bool valid_scene = authored_scene_entry != authored_scenes.end()
            && authored_scene_entry->id == scene;
        if (!valid_scene) {
            return Result::failure(GroupEditError::InvalidScene);
        }

        const std::map<doc::NodeId, doc::Node> nodes = document_nodes(root);
        const auto in_scene = [&](const doc::Node& node) {
            const auto owner = authored_scene(node);
            return owner.has_value() && *owner == scene;
        };
        const auto parent_of = [&](const doc::Node& node,
                                   std::optional<doc::NodeId>& parent) {
            return read_optional_reference(node, schema::parent_group, parent);
        };
        const auto is_locked = [](const doc::Node& node) {
            bool value{};
            return !read_boolean_property(node, schema::locked, false, value)
                || value;
        };

        // The chain of groups from `parent` up to the top level, nearest
        // first, and the world matrix it puts its children in.
        struct Chain final {
            std::vector<doc::NodeId> groups;
            Mat4 world{identity()};
        };
        const auto chain_of =
            [&](std::optional<doc::NodeId> parent) -> std::optional<Chain> {
            Chain chain{};
            std::vector<Mat4> locals;
            while (parent.has_value()) {
                if (chain.groups.size() > nodes.size()) {
                    return std::nullopt;
                }
                const auto found = nodes.find(*parent);
                if (found == nodes.end()
                    || found->second.type != schema::group_type) {
                    return std::nullopt;
                }
                GroupTransform local{};
                if (!read_group_transform(found->second, local)) {
                    return std::nullopt;
                }
                chain.groups.push_back(*parent);
                locals.push_back(transform_matrix(local));
                if (!parent_of(found->second, parent)) {
                    return std::nullopt;
                }
            }
            for (auto local = locals.rbegin(); local != locals.rend();
                 ++local) {
                chain.world = multiply(chain.world, *local);
            }
            return chain;
        };

        if (group.has_value()) {
            const auto destination = nodes.find(*group);
            if (destination == nodes.end()) {
                return Result::failure(GroupEditError::GroupNotFound);
            }
            if (destination->second.type != schema::group_type
                || is_linked_occurrence(destination->second)) {
                return Result::failure(GroupEditError::NotAGroup);
            }
            if (!in_scene(destination->second)) {
                return Result::failure(GroupEditError::CrossSceneSelection);
            }
            if (is_locked(destination->second)) {
                return Result::failure(GroupEditError::Locked);
            }
        }
        const std::optional<Chain> destination_chain = chain_of(group);
        if (!destination_chain.has_value()) {
            return Result::failure(GroupEditError::InvalidHierarchy);
        }

        std::vector<doc::NodeId> selected{selection.begin(), selection.end()};
        std::sort(selected.begin(), selected.end());
        if (std::adjacent_find(selected.begin(), selected.end())
            != selected.end()) {
            return Result::failure(GroupEditError::DuplicateSelection);
        }

        struct Moved final {
            doc::Node node;
            core::i64 order{};
        };
        std::vector<Moved> moved;
        moved.reserve(selected.size());
        for (const doc::NodeId id : selected) {
            const auto found = nodes.find(id);
            if (found == nodes.end() || !is_groupable_type(found->second.type)
                || is_linked_occurrence(found->second)) {
                return Result::failure(GroupEditError::UnsupportedNode);
            }
            const doc::Node& source = found->second;
            if (source.type == schema::media_type
                && referenced_id(source, schema::lane_id).has_value()) {
                return Result::failure(GroupEditError::UnsupportedNode);
            }
            if (!in_scene(source)) {
                return Result::failure(GroupEditError::CrossSceneSelection);
            }
            if (is_locked(source)) {
                return Result::failure(GroupEditError::Locked);
            }
            // A group cannot be filed under itself or anything inside it.
            if (std::find(destination_chain->groups.begin(),
                          destination_chain->groups.end(), id)
                != destination_chain->groups.end()) {
                return Result::failure(GroupEditError::InvalidHierarchy);
            }
            std::optional<doc::NodeId> old_parent;
            if (!parent_of(source, old_parent)) {
                return Result::failure(GroupEditError::InvalidHierarchy);
            }
            doc::Node node = source;
            if (old_parent != group) {
                const std::optional<Chain> old_chain = chain_of(old_parent);
                if (!old_chain.has_value()) {
                    return Result::failure(GroupEditError::InvalidHierarchy);
                }
                if (!(old_chain->world == destination_chain->world)) {
                    // The parent space really changes, so the local transform
                    // is rewritten - which only holds still if nothing on
                    // either side of the move is animated.
                    std::set<doc::NodeId> animated{id};
                    animated.insert(old_chain->groups.begin(),
                                    old_chain->groups.end());
                    animated.insert(destination_chain->groups.begin(),
                                    destination_chain->groups.end());
                    if (has_transform_animation(nodes, animated)) {
                        return Result::failure(
                            GroupEditError::AnimatedTransform);
                    }
                    GroupTransform local{};
                    if (!read_group_transform(source, local)) {
                        return Result::failure(
                            GroupEditError::TransformNotRepresentable);
                    }
                    const Mat4 world =
                        multiply(old_chain->world, transform_matrix(local));
                    const auto decomposed = decompose_group_transform(
                        multiply(invert(destination_chain->world), world),
                        false);
                    if (!decomposed.has_value()) {
                        return Result::failure(
                            GroupEditError::TransformNotRepresentable);
                    }
                    write_group_transform(node, *decomposed);
                }
                if (group.has_value()) {
                    write_reference(node.properties, schema::parent_group,
                                    *group);
                } else {
                    node.properties.erase(std::string{schema::parent_group});
                }
            }
            core::i64 order{};
            if (!read_integer_property(source, schema::group_order, 0,
                                       order)) {
                return Result::failure(GroupEditError::InvalidHierarchy);
            }
            moved.push_back(Moved{std::move(node), order});
        }

        // The destination's stack, back to front, as the Timeline orders it.
        const auto orderable_sibling = [](const doc::Node& node) noexcept {
            return node.type == schema::object_type
                || node.type == schema::media_type
                || node.type == schema::model_type
                || node.type == schema::particle_effect_type
                || node.type == schema::group_type
                || node.type == schema::light_type;
        };
        std::vector<Moved> stack;
        for (const auto& [id, node] : nodes) {
            if (std::binary_search(selected.begin(), selected.end(), id)
                || !orderable_sibling(node) || !in_scene(node)
                || is_linked_occurrence(node)) {
                continue;
            }
            if (node.type == schema::media_type
                && referenced_id(node, schema::lane_id).has_value()) {
                continue;
            }
            std::optional<doc::NodeId> parent;
            if (!parent_of(node, parent)) {
                return Result::failure(GroupEditError::InvalidHierarchy);
            }
            if (parent != group) {
                continue;
            }
            core::i64 order{};
            if (!read_integer_property(node, schema::group_order, 0, order)) {
                return Result::failure(GroupEditError::InvalidHierarchy);
            }
            stack.push_back(Moved{node, order});
        }
        const auto back_to_front = [](const Moved& left, const Moved& right) {
            return std::pair{left.order, left.node.id.value()}
                < std::pair{right.order, right.node.id.value()};
        };
        std::stable_sort(stack.begin(), stack.end(), back_to_front);
        std::stable_sort(moved.begin(), moved.end(), back_to_front);
        auto insertion = stack.end();
        if (anchor.has_value()) {
            const auto found = std::find_if(
                stack.begin(), stack.end(), [&](const Moved& entry) {
                    return entry.node.id == *anchor;
                });
            if (found != stack.end()) {
                // Back to front: in front of the anchor is after it here.
                insertion = before ? std::next(found) : found;
            }
        }
        const auto first_moved = static_cast<std::size_t>(
            std::distance(stack.begin(), insertion));
        stack.insert(insertion, std::make_move_iterator(moved.begin()),
                     std::make_move_iterator(moved.end()));

        GroupEditPlan plan{};
        plan.scene = scene;
        plan.group = group.value_or(doc::NodeId{});
        plan.primary_after = selection.front();
        for (std::size_t index = 0U; index < stack.size(); ++index) {
            if (index > static_cast<std::size_t>(
                            std::numeric_limits<core::i64>::max())) {
                return Result::failure(GroupEditError::OrderOverflow);
            }
            doc::Node node = std::move(stack[index].node);
            const auto dense = static_cast<core::i64>(index);
            const bool was_moved =
                index >= first_moved && index < first_moved + moved.size();
            if (!was_moved && stack[index].order == dense) {
                continue;
            }
            node.properties.insert_or_assign(std::string{schema::group_order},
                                             dense);
            plan.puts.push_back(std::move(node));
        }
        std::sort(plan.puts.begin(), plan.puts.end(),
                  [](const doc::Node& left, const doc::Node& right) noexcept {
                      return left.id < right.id;
                  });
        plan.after = apply_group_plan(root, plan.puts, plan.erases);
        return Result::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return Result::failure(GroupEditError::AllocationFailed);
    }
}

core::Result<GroupEditPlan, GroupEditError> plan_create_group(
    const doc::Root& root,
    SceneId scene,
    std::optional<doc::NodeId> parent,
    doc::NodeId first_id,
    std::string_view name,
    GroupKind kind) {
    using Result = core::Result<GroupEditPlan, GroupEditError>;
    constexpr core::u64 max_reference_id =
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());
    if (name.empty()) {
        return Result::failure(GroupEditError::InvalidName);
    }
    if (first_id.value() == 0U || first_id.value() > max_reference_id) {
        return Result::failure(GroupEditError::InvalidFirstId);
    }
    try {
        SceneModel model;
        model.reset(root);
        const std::vector<SceneInfo> authored_scenes = model.scenes();
        const auto authored_scene_entry = std::lower_bound(
            authored_scenes.begin(), authored_scenes.end(), scene,
            [](const SceneInfo& info, SceneId id) noexcept {
                return info.id < id;
            });
        const bool valid_scene = authored_scene_entry != authored_scenes.end()
            && authored_scene_entry->id == scene;
        if (!valid_scene) {
            return Result::failure(GroupEditError::InvalidScene);
        }

        const std::map<doc::NodeId, doc::Node> nodes = document_nodes(root);
        const auto in_scene = [&](const doc::Node& node) {
            const auto owner = authored_scene(node);
            return owner.has_value() && *owner == scene;
        };
        if (parent.has_value()) {
            const auto found = nodes.find(*parent);
            if (found == nodes.end()) {
                return Result::failure(GroupEditError::GroupNotFound);
            }
            if (found->second.type != schema::group_type
                || is_linked_occurrence(found->second)) {
                return Result::failure(GroupEditError::NotAGroup);
            }
            if (!in_scene(found->second)) {
                return Result::failure(GroupEditError::CrossSceneSelection);
            }
            bool locked{};
            if (!read_boolean_property(found->second, schema::locked, false,
                                       locked)) {
                return Result::failure(GroupEditError::InvalidHierarchy);
            }
            if (locked) {
                return Result::failure(GroupEditError::Locked);
            }
        }

        // In front of every sibling: one past the highest order among them.
        std::optional<core::i64> front;
        for (const auto& [id, node] : nodes) {
            if (!is_groupable_type(node.type) || !in_scene(node)) {
                continue;
            }
            std::optional<doc::NodeId> sibling_parent;
            std::int64_t order{};
            if (!read_optional_reference(node, schema::parent_group,
                                         sibling_parent)
                || sibling_parent != parent
                || !read_integer_property(node, schema::group_order, 0,
                                          order)) {
                continue;
            }
            front = std::max(front.value_or(order), order);
        }
        if (front == std::numeric_limits<core::i64>::max()) {
            return Result::failure(GroupEditError::OrderOverflow);
        }

        core::u64 candidate = first_id.value();
        while (root.contains(doc::NodeId{candidate})) {
            if (candidate == max_reference_id) {
                return Result::failure(GroupEditError::IdExhausted);
            }
            ++candidate;
        }
        const doc::NodeId group_id{candidate};

        doc::Node group{};
        group.id = group_id;
        group.type = std::string{schema::group_type};
        write_reference(group.properties, schema::scene_id, scene);
        if (parent.has_value()) {
            write_reference(group.properties, schema::parent_group, *parent);
        }
        group.properties.emplace(std::string{schema::group_name},
                                 std::string{name});
        group.properties.emplace(
            std::string{schema::group_order},
            front.has_value() ? *front + 1 : core::i64{0});
        group.properties.emplace(std::string{schema::locked}, false);
        group.properties.emplace(std::string{schema::visible}, true);
        if (kind == GroupKind::VehicleProcess) {
            group.properties.emplace(
                std::string{schema::group_kind},
                std::string{schema::group_kind_vehicle_process});
        }
        write_group_transform(group, GroupTransform{});

        GroupEditPlan plan{};
        plan.scene = scene;
        plan.group = group_id;
        plan.primary_after = group_id;
        plan.puts.push_back(std::move(group));
        plan.after = apply_group_plan(root, plan.puts, plan.erases);
        return Result::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return Result::failure(GroupEditError::AllocationFailed);
    }
}

core::Result<LayerOrderPlan, LayerOrderError> plan_reorder_layer(
    const doc::Root& root, SceneId scene, doc::NodeId source_id,
    doc::NodeId destination_id, LayerOrderPlacement placement) {
    using Result = core::Result<LayerOrderPlan, LayerOrderError>;
    try {
        SceneModel model;
        model.reset(root);
        const std::vector<SceneInfo> authored_scenes = model.scenes();
        const auto authored_scene_entry = std::lower_bound(
            authored_scenes.begin(), authored_scenes.end(), scene,
            [](const SceneInfo& info, SceneId id) noexcept {
                return info.id < id;
            });
        const bool valid_scene = authored_scene_entry != authored_scenes.end()
            && authored_scene_entry->id == scene;
        if (!valid_scene) {
            return Result::failure(LayerOrderError::InvalidScene);
        }

        const std::map<doc::NodeId, doc::Node> nodes = document_nodes(root);
        const auto source_found = nodes.find(source_id);
        if (source_found == nodes.end()) {
            return Result::failure(LayerOrderError::SourceNotFound);
        }
        const auto destination_found = nodes.find(destination_id);
        if (destination_found == nodes.end()) {
            return Result::failure(LayerOrderError::DestinationNotFound);
        }

        const auto orderable_sibling = [](const doc::Node& node) noexcept {
            return node.type == schema::object_type
                || node.type == schema::media_type
                || node.type == schema::model_type
                || node.type == schema::particle_effect_type
                || node.type == schema::group_type
                || node.type == schema::light_type;
        };
        if (!orderable_sibling(source_found->second)
            || !orderable_sibling(destination_found->second)) {
            return Result::failure(LayerOrderError::UnsupportedNode);
        }
        const auto source_scene = authored_scene(source_found->second);
        const auto destination_scene = authored_scene(
            destination_found->second);
        if (!source_scene.has_value() || !destination_scene.has_value()
            || *source_scene != scene || *destination_scene != scene) {
            return Result::failure(LayerOrderError::CrossScene);
        }

        const bool source_is_clip =
            source_found->second.type == schema::media_type;
        const bool destination_is_clip =
            destination_found->second.type == schema::media_type;
        const std::optional<doc::NodeId> source_lane = source_is_clip
            ? referenced_id(source_found->second, schema::lane_id)
            : std::nullopt;
        const std::optional<doc::NodeId> destination_lane = destination_is_clip
            ? referenced_id(destination_found->second, schema::lane_id)
            : std::nullopt;
        const bool track_domain = source_lane.has_value();
        if (track_domain != destination_lane.has_value()
            || (track_domain
                && (!destination_is_clip
                    || source_lane != destination_lane))) {
            return Result::failure(LayerOrderError::CrossOrderDomain);
        }

        std::optional<doc::NodeId> source_parent;
        std::optional<doc::NodeId> destination_parent;
        if (!track_domain
            && (!read_optional_reference(source_found->second,
                                         schema::parent_group, source_parent)
                || !read_optional_reference(destination_found->second,
                                            schema::parent_group,
                                            destination_parent))) {
            return Result::failure(LayerOrderError::InvalidHierarchy);
        }
        if (!track_domain && source_parent != destination_parent) {
            return Result::failure(LayerOrderError::CrossOrderDomain);
        }

        const auto locked = [&](const doc::Node& node) noexcept {
            bool value{};
            return !read_boolean_property(node, schema::locked, false, value)
                || value;
        };
        if (locked(source_found->second) || locked(destination_found->second)) {
            return Result::failure(LayerOrderError::Locked);
        }
        if (track_domain) {
            const auto lane = nodes.find(*source_lane);
            bool lane_locked{};
            if (lane == nodes.end()
                || lane->second.type != schema::timeline_lane_type
                || !read_boolean_property(lane->second, schema::lane_locked,
                                          false, lane_locked)) {
                return Result::failure(LayerOrderError::CrossOrderDomain);
            }
            if (lane_locked) {
                return Result::failure(LayerOrderError::Locked);
            }
        } else if (source_parent.has_value()) {
            const std::vector<SceneGroup> groups = model.groups(scene);
            const auto parent = std::find_if(
                groups.begin(), groups.end(), [&](const SceneGroup& group) {
                    return group.node == *source_parent;
                });
            if (parent == groups.end()) {
                return Result::failure(LayerOrderError::InvalidHierarchy);
            }
            if (parent->effective_locked) {
                return Result::failure(LayerOrderError::Locked);
            }
        }

        struct Peer final {
            doc::Node node;
            core::i64 order{};
        };
        std::vector<Peer> peers;
        const std::string_view order_property = track_domain
            ? schema::clip_order
            : schema::group_order;
        for (const auto& [id, node] : nodes) {
            static_cast<void>(id);
            if (!orderable_sibling(node)) {
                continue;
            }
            const auto node_scene = authored_scene(node);
            if (!node_scene.has_value() || *node_scene != scene) {
                continue;
            }
            if (track_domain) {
                if (node.type != schema::media_type
                    || referenced_id(node, schema::lane_id) != source_lane) {
                    continue;
                }
            } else {
                if (node.type == schema::media_type
                    && referenced_id(node, schema::lane_id).has_value()) {
                    continue;
                }
                std::optional<doc::NodeId> parent;
                if (!read_optional_reference(node, schema::parent_group,
                                             parent)) {
                    return Result::failure(
                        LayerOrderError::InvalidHierarchy);
                }
                if (parent != source_parent) {
                    continue;
                }
            }
            core::i64 order{};
            if (!read_integer_property(node, order_property, 0, order)) {
                return Result::failure(LayerOrderError::InvalidHierarchy);
            }
            peers.push_back(Peer{node, order});
        }
        std::stable_sort(
            peers.begin(), peers.end(),
            [](const Peer& left, const Peer& right) noexcept {
                return std::pair{left.order, left.node.id.value()}
                    < std::pair{right.order, right.node.id.value()};
            });
        const auto source = std::find_if(
            peers.begin(), peers.end(),
            [source_id](const Peer& peer) { return peer.node.id == source_id; });
        const auto destination = std::find_if(
            peers.begin(), peers.end(), [destination_id](const Peer& peer) {
                return peer.node.id == destination_id;
            });
        if (source == peers.end() || destination == peers.end()) {
            return Result::failure(LayerOrderError::CrossOrderDomain);
        }
        if (source_id != destination_id) {
            Peer moved = std::move(*source);
            peers.erase(source);
            const auto landing = std::find_if(
                peers.begin(), peers.end(),
                [destination_id](const Peer& peer) {
                    return peer.node.id == destination_id;
                });
            auto insertion = landing;
            // `peers` is back-to-front. Visible Before means above/front, so
            // it lands after the destination in authored ascending order.
            if (placement == LayerOrderPlacement::Before) {
                ++insertion;
            }
            peers.insert(insertion, std::move(moved));
        }

        LayerOrderPlan plan{};
        plan.scene = scene;
        plan.source = source_id;
        plan.destination = destination_id;
        plan.after = root;
        for (std::size_t index = 0U; index < peers.size(); ++index) {
            if (index > static_cast<std::size_t>(
                            std::numeric_limits<core::i64>::max())) {
                return Result::failure(LayerOrderError::OrderOverflow);
            }
            const core::i64 dense = static_cast<core::i64>(index);
            if (peers[index].order == dense) {
                continue;
            }
            peers[index].node.properties.insert_or_assign(
                std::string{order_property}, dense);
            plan.after = plan.after.set(peers[index].node);
            plan.puts.push_back(std::move(peers[index].node));
        }
        std::sort(plan.puts.begin(), plan.puts.end(),
                  [](const doc::Node& left, const doc::Node& right) noexcept {
                      return left.id < right.id;
                  });
        return Result::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return Result::failure(LayerOrderError::AllocationFailed);
    }
}

} // namespace mediaserver::scene
