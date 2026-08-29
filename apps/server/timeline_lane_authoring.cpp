#include "timeline_lane_authoring.hpp"

#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mediaserver::app {
namespace {

[[nodiscard]] const std::int64_t* integer(
    const doc::Node& node, std::string_view property) noexcept {
    const auto found = node.properties.find(property);
    return found == node.properties.end()
        ? nullptr
        : std::get_if<std::int64_t>(&found->second);
}

[[nodiscard]] std::optional<doc::NodeId> reference(
    const doc::Node& node, std::string_view property) noexcept {
    const auto* const value = integer(node, property);
    return value != nullptr && *value >= 0
        ? std::optional<doc::NodeId>{
              doc::NodeId{static_cast<core::u64>(*value)}}
        : std::nullopt;
}

[[nodiscard]] bool belongs_to(
    const doc::Node& node, scene::SceneId wanted,
    scene::SceneId default_scene) noexcept {
    const auto owned = reference(node, scene::schema::scene_id);
    return owned.value_or(default_scene) == wanted;
}

[[nodiscard]] bool valid_scene(
    const std::map<doc::NodeId, doc::Node>& nodes,
    scene::SceneId scene) noexcept {
    bool any_stage{};
    for (const auto& [id, node] : nodes) {
        if (node.type != scene::schema::stage_type) {
            continue;
        }
        any_stage = true;
        if (id == scene) {
            return true;
        }
    }
    return !any_stage && scene == scene::legacy_scene_id;
}

[[nodiscard]] scene::SceneId default_scene(
    const std::map<doc::NodeId, doc::Node>& nodes) noexcept {
    for (const auto& [id, node] : nodes) {
        if (node.type == scene::schema::stage_type) {
            return id;
        }
    }
    return scene::legacy_scene_id;
}

void put_integer(doc::Node& node, std::string_view property,
                 std::int64_t value) {
    node.properties.insert_or_assign(
        std::string{property}, doc::PropertyValue{value});
}

void put_bool(doc::Node& node, std::string_view property, bool value) {
    node.properties.insert_or_assign(
        std::string{property}, doc::PropertyValue{value});
}

[[nodiscard]] bool read_bool(
    const doc::Node& node, std::string_view property) noexcept {
    const auto found = node.properties.find(property);
    if (found == node.properties.end()) {
        return false;
    }
    if (const auto* const value = std::get_if<bool>(&found->second)) {
        return *value;
    }
    return false;
}

[[nodiscard]] std::map<doc::NodeId, doc::Node>
nodes_of(const doc::Root& root) {
    std::map<doc::NodeId, doc::Node> result;
    for (const doc::NodeChange& change : doc::diff(doc::Root{}, root).changes) {
        if (change.after) {
            result.emplace(change.id, *change.after);
        }
    }
    return result;
}

[[nodiscard]] std::vector<doc::NodeId> lane_ids(
    const std::map<doc::NodeId, doc::Node>& nodes,
    scene::SceneId scene_id,
    scene::SceneId fallback) {
    std::vector<doc::NodeId> result;
    for (const auto& [id, node] : nodes) {
        if (node.type == scene::schema::timeline_lane_type
            && belongs_to(node, scene_id, fallback)) {
            result.push_back(id);
        }
    }
    std::stable_sort(
        result.begin(), result.end(), [&](doc::NodeId left, doc::NodeId right) {
            const auto order = [&](doc::NodeId id) {
                const auto* const value = integer(
                    nodes.at(id), scene::schema::lane_order);
                return value != nullptr ? *value : std::int64_t{};
            };
            return std::pair{order(left), left.value()}
                < std::pair{order(right), right.value()};
        });
    return result;
}

} // namespace

core::Result<LaneAuthorPlan, LaneAuthorError> plan_lane_authoring(
    const doc::Root& document, const LaneAuthorRequest& request,
    core::u64 next_node) {
    using Result = core::Result<LaneAuthorPlan, LaneAuthorError>;
    try {
        auto nodes = nodes_of(document);
        if (!valid_scene(nodes, request.scene)) {
            return Result::failure(LaneAuthorError::InvalidScene);
        }
        const scene::SceneId fallback = default_scene(nodes);
        auto lanes = lane_ids(nodes, request.scene, fallback);
        LaneAuthorPlan plan{};
        plan.document = document;

        if (request.action == LaneAuthorAction::Create) {
            if (lanes.size() >= scene::max_scene_lanes) {
                return Result::failure(LaneAuthorError::LaneLimit);
            }
            constexpr core::u64 maximum = static_cast<core::u64>(
                std::numeric_limits<std::int64_t>::max());
            while (next_node <= maximum
                   && document.contains(doc::NodeId{next_node})) {
                ++next_node;
            }
            if (next_node == 0U || next_node > maximum) {
                return Result::failure(LaneAuthorError::IdExhausted);
            }
            doc::Node lane{};
            lane.id = doc::NodeId{next_node};
            lane.type = std::string{scene::schema::timeline_lane_type};
            put_integer(lane, scene::schema::scene_id,
                        static_cast<std::int64_t>(request.scene.value()));
            // Normalize first so arbitrary imported order values cannot place
            // a newly created lane between existing lanes or collide with one.
            for (std::size_t order = 0U; order < lanes.size(); ++order) {
                doc::Node existing = nodes.at(lanes[order]);
                const auto normalized = static_cast<std::int64_t>(order) * 10;
                const auto* const current = integer(
                    existing, scene::schema::lane_order);
                if (current == nullptr || *current != normalized) {
                    put_integer(existing, scene::schema::lane_order, normalized);
                    plan.document = plan.document.set(existing);
                    plan.changed.push_back(existing.id);
                }
            }
            put_integer(lane, scene::schema::lane_order,
                        static_cast<std::int64_t>(lanes.size()) * 10);
            const std::string name = request.name.empty()
                ? "Track " + std::to_string(lanes.size() + 1U)
                : request.name;
            if (name.empty() || name.size() > 128U) {
                return Result::failure(LaneAuthorError::InvalidName);
            }
            lane.properties.emplace(std::string{scene::schema::lane_name},
                                    doc::PropertyValue{name});
            lane.properties.emplace(
                std::string{scene::schema::lane_compositing_mode},
                doc::PropertyValue{
                    std::string{scene::schema::lane_normal_mode}});
            plan.document = plan.document.set(lane);
            plan.created = lane.id;
            plan.changed.push_back(lane.id);
            std::sort(plan.changed.begin(), plan.changed.end());
            plan.next_node = next_node == maximum ? maximum : next_node + 1U;
            return Result::success(std::move(plan));
        }

        const auto held = nodes.find(request.lane);
        if (held == nodes.end()
            || held->second.type != scene::schema::timeline_lane_type
            || !belongs_to(held->second, request.scene, fallback)) {
            return Result::failure(LaneAuthorError::LaneNotFound);
        }
        doc::Node lane = held->second;

        if (request.action == LaneAuthorAction::Rename) {
            if (request.name.empty() || request.name.size() > 128U) {
                return Result::failure(LaneAuthorError::InvalidName);
            }
            lane.properties.insert_or_assign(
                std::string{scene::schema::lane_name},
                doc::PropertyValue{request.name});
            plan.document = plan.document.set(lane);
            plan.changed.push_back(lane.id);
            plan.next_node = next_node;
            return Result::success(std::move(plan));
        }
        if (request.action == LaneAuthorAction::ToggleMute
            || request.action == LaneAuthorAction::ToggleLock) {
            const std::string_view property =
                request.action == LaneAuthorAction::ToggleMute
                ? scene::schema::lane_muted
                : scene::schema::lane_locked;
            put_bool(lane, property, !read_bool(lane, property));
            plan.document = plan.document.set(lane);
            plan.changed.push_back(lane.id);
            plan.next_node = next_node;
            return Result::success(std::move(plan));
        }
        if (request.action == LaneAuthorAction::MoveEarlier
            || request.action == LaneAuthorAction::MoveLater) {
            const auto position = std::find(lanes.begin(), lanes.end(), lane.id);
            if (position == lanes.end()) {
                return Result::failure(LaneAuthorError::LaneNotFound);
            }
            const std::size_t index = static_cast<std::size_t>(
                std::distance(lanes.begin(), position));
            if (request.action == LaneAuthorAction::MoveEarlier && index > 0U) {
                std::swap(lanes[index], lanes[index - 1U]);
            } else if (request.action == LaneAuthorAction::MoveLater
                       && index + 1U < lanes.size()) {
                std::swap(lanes[index], lanes[index + 1U]);
            } else {
                plan.next_node = next_node;
                return Result::success(std::move(plan));
            }
            for (std::size_t order = 0U; order < lanes.size(); ++order) {
                doc::Node updated = nodes.at(lanes[order]);
                put_integer(updated, scene::schema::lane_order,
                            static_cast<std::int64_t>(order) * 10);
                plan.document = plan.document.set(updated);
                plan.changed.push_back(updated.id);
            }
            std::sort(plan.changed.begin(), plan.changed.end());
            plan.next_node = next_node;
            return Result::success(std::move(plan));
        }

        std::vector<doc::NodeId> clips;
        for (const auto& [id, node] : nodes) {
            if (node.type == scene::schema::media_type
                && reference(node, scene::schema::lane_id) == lane.id) {
                clips.push_back(id);
            }
        }
        if (request.action == LaneAuthorAction::DeleteEmpty
            && !clips.empty()) {
            return Result::failure(LaneAuthorError::NonEmptyLane);
        }
        if (request.action == LaneAuthorAction::DeleteMoveClips) {
            const auto destination = nodes.find(request.destination);
            if (destination == nodes.end()
                || destination->second.type
                    != scene::schema::timeline_lane_type) {
                return Result::failure(LaneAuthorError::DestinationNotFound);
            }
            if (request.destination == lane.id
                || !belongs_to(destination->second, request.scene, fallback)) {
                return Result::failure(LaneAuthorError::CrossSceneDestination);
            }
            for (const doc::NodeId clip_id : clips) {
                doc::Node clip = nodes.at(clip_id);
                put_integer(
                    clip, scene::schema::lane_id,
                    static_cast<std::int64_t>(request.destination.value()));
                plan.document = plan.document.set(clip);
                plan.changed.push_back(clip.id);
            }
        } else if (request.action == LaneAuthorAction::DeleteClips) {
            std::set<doc::NodeId> erased{clips.begin(), clips.end()};
            for (const auto& [id, node] : nodes) {
                if (node.type == scene::schema::effect_type) {
                    const auto target = reference(
                        node, scene::schema::effect_target);
                    if (target.has_value() && erased.contains(*target)) {
                        erased.insert(id);
                    }
                }
            }
            for (const auto& [id, node] : nodes) {
                if (node.type == scene::schema::channel_type) {
                    const auto target = reference(
                        node, scene::schema::channel_target);
                    if (target.has_value() && erased.contains(*target)) {
                        erased.insert(id);
                    }
                }
            }
            for (const doc::NodeId id : erased) {
                const auto removed = plan.document.erase(id);
                if (!removed.has_value()) {
                    return Result::failure(LaneAuthorError::InvalidDocument);
                }
                plan.document = *removed;
                plan.erased.push_back(id);
            }
        } else if (request.action != LaneAuthorAction::DeleteEmpty) {
            return Result::failure(LaneAuthorError::InvalidDocument);
        }

        const auto removed_lane = plan.document.erase(lane.id);
        if (!removed_lane.has_value()) {
            return Result::failure(LaneAuthorError::InvalidDocument);
        }
        plan.document = *removed_lane;
        plan.erased.push_back(lane.id);
        lanes.erase(std::remove(lanes.begin(), lanes.end(), lane.id), lanes.end());
        for (std::size_t order = 0U; order < lanes.size(); ++order) {
            doc::Node updated = *plan.document.get(lanes[order]);
            const auto normalized = static_cast<std::int64_t>(order) * 10;
            const auto* const current = integer(
                updated, scene::schema::lane_order);
            if (current == nullptr || *current != normalized) {
                put_integer(updated, scene::schema::lane_order, normalized);
                plan.document = plan.document.set(updated);
                plan.changed.push_back(updated.id);
            }
        }
        std::sort(plan.changed.begin(), plan.changed.end());
        plan.changed.erase(
            std::unique(plan.changed.begin(), plan.changed.end()),
            plan.changed.end());
        std::sort(plan.erased.begin(), plan.erased.end());
        plan.next_node = next_node;
        return Result::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return Result::failure(LaneAuthorError::AllocationFailed);
    }
}

} // namespace mediaserver::app
