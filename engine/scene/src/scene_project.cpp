#include <mediaserver/scene/scene_project.hpp>

#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <string>
#include <string_view>

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
        || type == schema::media_type || type == schema::model_type
        || type == schema::light_type;
}

[[nodiscard]] bool is_drawable_type(std::string_view type) noexcept {
    return type == schema::object_type || type == schema::media_type
        || type == schema::model_type;
}

[[nodiscard]] bool belongs_to(
    const doc::Node& node,
    SceneId source,
    SceneId default_scene) noexcept {
    const auto* const value = find(node.properties, schema::scene_id);
    if (value == nullptr) {
        return source == default_scene;
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
        const SceneId default_scene = authored_scenes.front().id;

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
                || !belongs_to(node, source, default_scene)) {
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
            if (is_direct_scoped_type(node.type)) {
                write_reference(node.properties, schema::scene_id, plan.destination);
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
            }
            plan.puts.push_back(std::move(node));
        }
        return Result::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return Result::failure(SceneCloneError::AllocationFailed);
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

} // namespace mediaserver::scene
