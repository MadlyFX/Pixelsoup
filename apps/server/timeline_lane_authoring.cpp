#include "timeline_lane_authoring.hpp"

#include "channel_node_id.hpp"
#include "clip_timing_authoring.hpp"

#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
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

// A placement's timing as the document holds it; a clip missing either
// placement property is unplaced, which a cut neither moves nor splits.
[[nodiscard]] std::optional<scene::ClipTiming> clip_timing(
    const doc::Node& clip) noexcept {
    const auto* const start = integer(clip, scene::schema::clip_start);
    const auto* const duration = integer(clip, scene::schema::clip_duration);
    if (start == nullptr || duration == nullptr) {
        return std::nullopt;
    }
    const auto read = [&](std::string_view name) {
        const auto* const value = integer(clip, name);
        return value != nullptr ? *value : std::int64_t{};
    };
    scene::ClipTiming timing{};
    timing.start = *start;
    timing.duration = *duration;
    timing.source_in = read(scene::schema::source_in);
    timing.fade_in = read(scene::schema::fade_in);
    timing.fade_out = read(scene::schema::fade_out);
    if (!timing.placed()) {
        return std::nullopt;
    }
    return timing;
}

void put_clip_timing(doc::Node& clip, const scene::ClipTiming& timing) {
    put_integer(clip, scene::schema::clip_start, timing.start);
    put_integer(clip, scene::schema::clip_duration, timing.duration);
    put_integer(clip, scene::schema::source_in, timing.source_in);
    put_integer(clip, scene::schema::fade_in, timing.fade_in);
    put_integer(clip, scene::schema::fade_out, timing.fade_out);
}

[[nodiscard]] const std::string* text(
    const doc::Node& node, std::string_view property) noexcept {
    const auto found = node.properties.find(property);
    return found == node.properties.end()
        ? nullptr
        : std::get_if<std::string>(&found->second);
}

// The second half of a split track is named for the first: "Plates" becomes
// "Plates (2)", and splitting either again counts on to a number not taken.
[[nodiscard]] std::string split_name(
    std::string_view name, const std::vector<std::string>& taken) {
    std::string base{name};
    if (const auto open = base.rfind(" (");
        open != std::string::npos && base.size() > open + 3U
        && base.back() == ')'
        && std::all_of(base.begin() + static_cast<std::ptrdiff_t>(open + 2U),
                       base.end() - 1,
                       [](char digit) { return digit >= '0' && digit <= '9'; })) {
        base.resize(open);
    }
    for (std::size_t number = 2U;; ++number) {
        std::string candidate = base + " (" + std::to_string(number) + ")";
        if (std::find(taken.begin(), taken.end(), candidate) == taken.end()) {
            return candidate;
        }
    }
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
            || request.action == LaneAuthorAction::ToggleLock
            || request.action == LaneAuthorAction::ToggleSyncLock) {
            const std::string_view property =
                request.action == LaneAuthorAction::ToggleMute
                ? scene::schema::lane_muted
                : (request.action == LaneAuthorAction::ToggleLock
                       ? scene::schema::lane_locked
                       : scene::schema::lane_sync_locked);
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
        if (request.action == LaneAuthorAction::MoveTo) {
            const auto destination_node = nodes.find(request.destination);
            if (destination_node == nodes.end()
                || destination_node->second.type
                    != scene::schema::timeline_lane_type) {
                return Result::failure(LaneAuthorError::DestinationNotFound);
            }
            if (!belongs_to(destination_node->second, request.scene,
                            fallback)) {
                return Result::failure(
                    LaneAuthorError::CrossSceneDestination);
            }
            if (request.destination == lane.id) {
                plan.next_node = next_node;
                return Result::success(std::move(plan));
            }
            const auto position = std::find(lanes.begin(), lanes.end(), lane.id);
            const auto destination =
                std::find(lanes.begin(), lanes.end(), request.destination);
            if (position == lanes.end()) {
                return Result::failure(LaneAuthorError::LaneNotFound);
            }
            if (destination == lanes.end()) {
                return Result::failure(LaneAuthorError::DestinationNotFound);
            }
            const bool moving_later = position < destination;
            lanes.erase(position);
            const auto remaining_destination =
                std::find(lanes.begin(), lanes.end(), request.destination);
            lanes.insert(moving_later ? std::next(remaining_destination)
                                      : remaining_destination,
                         lane.id);
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
        if (request.action == LaneAuthorAction::Split) {
            if (read_bool(lane, scene::schema::lane_locked)) {
                return Result::failure(LaneAuthorError::Locked);
            }
            if (lanes.size() >= scene::max_scene_lanes) {
                return Result::failure(LaneAuthorError::LaneLimit);
            }
            const core::i64 cut = request.frame;
            // Every placed clip stays (it ends by the cut), moves whole (it
            // starts at or after it), or is cut in two.
            struct Cut final {
                doc::NodeId clip{};
                ClipSplitTiming halves;
            };
            std::vector<doc::NodeId> moving;
            std::vector<Cut> cutting;
            bool staying{};
            for (const doc::NodeId clip_id : clips) {
                const doc::Node& clip = nodes.at(clip_id);
                const auto timing = clip_timing(clip);
                if (!timing.has_value()) {
                    continue;
                }
                if (timing->start + timing->duration <= cut) {
                    staying = true;
                    continue;
                }
                if (read_bool(clip, scene::schema::locked)) {
                    return Result::failure(LaneAuthorError::Locked);
                }
                if (timing->start >= cut) {
                    moving.push_back(clip_id);
                    continue;
                }
                const auto* const count =
                    integer(clip, scene::schema::sequence_count);
                const std::int64_t frames = count != nullptr ? *count : 1;
                if (frames <= 0
                    || static_cast<core::u64>(frames)
                        > std::numeric_limits<core::u32>::max()) {
                    return Result::failure(LaneAuthorError::InvalidDocument);
                }
                std::optional<scene::MediaFrameRate> source_rate;
                const auto* const numerator =
                    integer(clip, scene::schema::source_rate_numerator);
                const auto* const denominator =
                    integer(clip, scene::schema::source_rate_denominator);
                const std::int64_t num = numerator != nullptr ? *numerator : 0;
                const std::int64_t den =
                    denominator != nullptr ? *denominator : 0;
                if (num != 0 || den != 0) {
                    if (num <= 0 || den <= 0
                        || static_cast<core::u64>(num)
                            > std::numeric_limits<core::u32>::max()
                        || static_cast<core::u64>(den)
                            > std::numeric_limits<core::u32>::max()) {
                        return Result::failure(
                            LaneAuthorError::InvalidDocument);
                    }
                    source_rate = scene::MediaFrameRate{
                        static_cast<core::u32>(num),
                        static_cast<core::u32>(den)};
                }
                const auto halves = split_clip_timing(
                    *timing, cut, static_cast<core::u32>(frames),
                    request.show_rate, source_rate);
                if (!halves.has_value()) {
                    return Result::failure(LaneAuthorError::InvalidDocument);
                }
                staying = true;
                cutting.push_back(Cut{clip_id, *halves});
            }
            if (!staying || (moving.empty() && cutting.empty())) {
                return Result::failure(LaneAuthorError::NothingToSplit);
            }

            constexpr core::u64 maximum = static_cast<core::u64>(
                std::numeric_limits<std::int64_t>::max());
            const auto allocate = [&]() -> std::optional<doc::NodeId> {
                while (next_node <= maximum
                       && plan.document.contains(doc::NodeId{next_node})) {
                    ++next_node;
                }
                if (next_node == 0U || next_node > maximum) {
                    return std::nullopt;
                }
                return doc::NodeId{next_node++};
            };
            const auto reference_to = [](doc::NodeId id) {
                return static_cast<std::int64_t>(id.value());
            };
            const auto second_id = allocate();
            if (!second_id.has_value()) {
                return Result::failure(LaneAuthorError::IdExhausted);
            }

            // The new track is the same screen - every property the track
            // has - under a name of its own, directly in front of it.
            std::vector<std::string> names;
            for (const doc::NodeId id : lanes) {
                if (const auto* const taken =
                        text(nodes.at(id), scene::schema::lane_name)) {
                    names.push_back(*taken);
                }
            }
            const auto* const own_name = text(lane, scene::schema::lane_name);
            const std::string name =
                split_name(own_name != nullptr ? *own_name : "Track", names);
            if (name.size() > 128U) {
                return Result::failure(LaneAuthorError::InvalidName);
            }
            doc::Node second = lane;
            second.id = *second_id;
            second.properties.insert_or_assign(
                std::string{scene::schema::lane_name}, doc::PropertyValue{name});
            plan.document = plan.document.set(second);
            plan.created = second.id;
            plan.changed.push_back(second.id);
            lanes.insert(std::next(std::find(lanes.begin(), lanes.end(),
                                             lane.id)),
                         second.id);
            for (std::size_t order = 0U; order < lanes.size(); ++order) {
                doc::Node updated = *plan.document.get(lanes[order]);
                const auto normalized = static_cast<std::int64_t>(order) * 10;
                const auto* const current =
                    integer(updated, scene::schema::lane_order);
                if (current == nullptr || *current != normalized) {
                    put_integer(updated, scene::schema::lane_order, normalized);
                    plan.document = plan.document.set(updated);
                    plan.changed.push_back(updated.id);
                }
            }

            // Its keys and its look come with it, so the screen neither jumps
            // nor changes grade at the cut. Each channel is copied to the node
            // the new owner's own key edits address.
            const auto copy_channels = [&](doc::NodeId from, doc::NodeId to) {
                for (core::u64 component = 0U;
                     component
                     < static_cast<core::u64>(scene::PropertyChannel::Count);
                     ++component) {
                    const doc::Node* const channel =
                        plan.document.get(channel_node_id(from, component));
                    if (channel == nullptr
                        || channel->type != scene::schema::channel_type) {
                        continue;
                    }
                    doc::Node copy = *channel;
                    copy.id = channel_node_id(to, component);
                    if (plan.document.contains(copy.id)) {
                        return false;
                    }
                    put_integer(copy, scene::schema::channel_target,
                                reference_to(to));
                    plan.document = plan.document.set(copy);
                    plan.changed.push_back(copy.id);
                }
                return true;
            };
            if (!copy_channels(lane.id, second.id)) {
                return Result::failure(LaneAuthorError::InvalidDocument);
            }
            for (const auto& [id, node] : nodes) {
                if (node.type != scene::schema::effect_type
                    || reference(node, scene::schema::effect_target)
                        != lane.id) {
                    continue;
                }
                const auto effect_id = allocate();
                if (!effect_id.has_value()) {
                    return Result::failure(LaneAuthorError::IdExhausted);
                }
                doc::Node effect = node;
                effect.id = *effect_id;
                put_integer(effect, scene::schema::effect_target,
                            reference_to(second.id));
                plan.document = plan.document.set(effect);
                plan.changed.push_back(effect.id);
                if (!copy_channels(id, effect.id)) {
                    return Result::failure(LaneAuthorError::InvalidDocument);
                }
            }

            for (const doc::NodeId clip_id : moving) {
                doc::Node clip = nodes.at(clip_id);
                put_integer(clip, scene::schema::lane_id,
                            reference_to(second.id));
                plan.document = plan.document.set(clip);
                plan.changed.push_back(clip.id);
            }
            // As the clip razor cuts: the head keeps its node and the tail is
            // a new placement of the same source, on the new track.
            for (const Cut& piece : cutting) {
                const auto tail_id = allocate();
                if (!tail_id.has_value()) {
                    return Result::failure(LaneAuthorError::IdExhausted);
                }
                doc::Node head = nodes.at(piece.clip);
                doc::Node tail = head;
                put_clip_timing(head, piece.halves.left);
                tail.id = *tail_id;
                put_clip_timing(tail, piece.halves.right);
                put_integer(tail, scene::schema::lane_id,
                            reference_to(second.id));
                plan.document = plan.document.set(head);
                plan.document = plan.document.set(tail);
                plan.changed.push_back(head.id);
                plan.changed.push_back(tail.id);
            }
            std::sort(plan.changed.begin(), plan.changed.end());
            plan.changed.erase(
                std::unique(plan.changed.begin(), plan.changed.end()),
                plan.changed.end());
            plan.next_node = next_node;
            return Result::success(std::move(plan));
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
