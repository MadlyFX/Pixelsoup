#include "timeline_edit_integration.hpp"

#include "timeline_edit_authoring.hpp"

#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace mediaserver::app {
namespace {

using Result = core::Result<
    TimelineEditIntegrationPlan,
    TimelineEditIntegrationError>;

[[nodiscard]] const std::int64_t* integer(
    const doc::Node& node,
    std::string_view property) noexcept {
    const auto found = node.properties.find(property);
    return found == node.properties.end()
        ? nullptr
        : std::get_if<std::int64_t>(&found->second);
}

[[nodiscard]] bool boolean(
    const doc::Node& node,
    std::string_view property) noexcept {
    const auto found = node.properties.find(property);
    if (found == node.properties.end()) {
        return false;
    }
    const bool* const held = std::get_if<bool>(&found->second);
    return held != nullptr && *held;
}

[[nodiscard]] bool target_lane_available(
    const doc::Root& document,
    scene::SceneId scene,
    doc::NodeId lane) noexcept {
    if (lane == doc::NodeId{}) {
        return true;
    }
    const doc::Node* const node = document.get(lane);
    if (node == nullptr || node->type != scene::schema::timeline_lane_type ||
        boolean(*node, scene::schema::lane_locked)) {
        return false;
    }
    const std::int64_t* const owner = integer(*node, scene::schema::scene_id);
    return owner != nullptr && *owner >= 0 &&
        static_cast<core::u64>(*owner) == scene.value();
}

// The scene's tracks that keep time with ripple edits: sync-locked, and not
// locked, since a locked track's clips stay where they are whatever else
// moves.
[[nodiscard]] std::vector<doc::NodeId> synced_lanes(
    const doc::Root& document, scene::SceneId scene) {
    std::vector<doc::NodeId> lanes;
    for (const doc::NodeChange& change :
         doc::diff(doc::Root{}, document).changes) {
        if (!change.after) {
            continue;
        }
        const doc::Node& node = *change.after;
        const doc::NodeId id = change.id;
        if (node.type != scene::schema::timeline_lane_type ||
            !boolean(node, scene::schema::lane_sync_locked) ||
            boolean(node, scene::schema::lane_locked)) {
            continue;
        }
        const std::int64_t* const owner =
            integer(node, scene::schema::scene_id);
        if (owner != nullptr && *owner >= 0 &&
            static_cast<core::u64>(*owner) == scene.value()) {
            lanes.push_back(id);
        }
    }
    return lanes;
}

struct RequestPlanner final {
    std::span<const scene::ClipPlacement> clips;
    std::span<const doc::NodeId> synced;
    const editor::TimelineEditIntent& intent;
    const ClipClipboard& clipboard;

    [[nodiscard]] scene::TimelineEditResult operator()(
        const editor::TimelineRippleTrimRequest& request) const {
        return scene::plan_ripple_trim(
            clips, request.clip, request.edge, request.delta,
            intent.show_rate, synced);
    }

    [[nodiscard]] scene::TimelineEditResult operator()(
        const editor::TimelineRippleDeleteRequest& request) const {
        return scene::plan_ripple_delete(clips, request.clips, synced);
    }

    [[nodiscard]] scene::TimelineEditResult operator()(
        const editor::TimelineRollRequest& request) const {
        return scene::plan_roll(
            clips, request.first, request.second, request.delta,
            intent.show_rate);
    }

    [[nodiscard]] scene::TimelineEditResult operator()(
        const editor::TimelineSlipRequest& request) const {
        return scene::plan_slip(
            clips, request.clip, request.delta, intent.show_rate);
    }

    [[nodiscard]] scene::TimelineEditResult operator()(
        const editor::TimelineSlideRequest& request) const {
        return scene::plan_slide(
            clips, request.clip, request.delta, intent.show_rate);
    }

    [[nodiscard]] scene::TimelineEditResult operator()(
        const editor::TimelineInsertRequest& request) const {
        const std::optional<core::i64> span = clipboard.span_duration();
        if (!span.has_value() || *span <= 0 ||
            intent.playhead > static_cast<core::u64>(
                                  std::numeric_limits<core::i64>::max())) {
            return scene::TimelineEditResult::failure(
                scene::TimelineEditError::InvalidSpan);
        }
        return scene::plan_insert(
            clips, scene::TimelineLaneRef{intent.scene, request.lane},
            static_cast<core::i64>(intent.playhead), *span, synced);
    }

    // Overwrite lays the clipboard over the track: no clip already there is
    // cut or moved, so the plan only names the span the paste covers.
    [[nodiscard]] scene::TimelineEditResult operator()(
        const editor::TimelineOverwriteRequest&) const {
        const std::optional<core::i64> span = clipboard.span_duration();
        constexpr core::i64 last = std::numeric_limits<core::i64>::max();
        if (!span.has_value() || *span <= 0 ||
            intent.playhead > static_cast<core::u64>(last - *span)) {
            return scene::TimelineEditResult::failure(
                scene::TimelineEditError::InvalidSpan);
        }
        scene::TimelineEditPlan plan{};
        plan.span_start = static_cast<core::i64>(intent.playhead);
        plan.span_duration = *span;
        return scene::TimelineEditResult::success(std::move(plan));
    }
};

// Stacks the pasted clips in front of every clip already on `lane`, keeping
// their own order among themselves. A track stacks its clips by clip_order;
// clips filed under no track stack by group_order. False when the orders
// would run past i64.
[[nodiscard]] bool stack_in_front(
    doc::Root& document,
    std::vector<doc::Node>& created,
    std::span<const scene::ClipPlacement> clips,
    scene::SceneId scene,
    doc::NodeId lane) {
    constexpr core::i64 last = std::numeric_limits<core::i64>::max();
    const bool untracked = lane == doc::NodeId{};
    core::i64 front = 0;
    for (const scene::ClipPlacement& clip : clips) {
        if (clip.scene != scene || clip.lane != lane) {
            continue;
        }
        const core::i64 order = untracked ? clip.group_order : clip.order;
        if (order == last) {
            return false;
        }
        front = std::max(front, order + 1);
    }

    // The pasted clips in their own compositing order: order, start, id.
    const std::string_view property =
        untracked ? scene::schema::group_order : scene::schema::clip_order;
    const auto stacking = [property](const doc::Node& node) noexcept {
        const std::int64_t* const order = integer(node, property);
        const std::int64_t* const start =
            integer(node, scene::schema::clip_start);
        return std::tuple{order != nullptr ? *order : 0,
                          start != nullptr ? *start : 0, node.id.value()};
    };
    std::vector<doc::Node*> pasted;
    for (doc::Node& node : created) {
        if (node.type == scene::schema::media_type) {
            pasted.push_back(&node);
        }
    }
    std::sort(pasted.begin(), pasted.end(),
              [&](const doc::Node* left, const doc::Node* right) noexcept {
                  return stacking(*left) < stacking(*right);
              });
    if (static_cast<core::u64>(last - front) < pasted.size()) {
        return false;
    }
    for (doc::Node* const node : pasted) {
        node->properties.insert_or_assign(std::string{property},
                                          doc::PropertyValue{front++});
        document = document.set(*node);
    }
    return true;
}

[[nodiscard]] std::optional<doc::NodeId> paste_lane(
    const editor::TimelineEditRequest& request) noexcept {
    if (const auto* const insert =
            std::get_if<editor::TimelineInsertRequest>(&request)) {
        return insert->lane;
    }
    if (const auto* const overwrite =
            std::get_if<editor::TimelineOverwriteRequest>(&request)) {
        return overwrite->lane;
    }
    return std::nullopt;
}

} // namespace

Result plan_timeline_edit_integration(
    const doc::Root& document,
    std::span<const scene::ClipPlacement> clips,
    const editor::TimelineEditIntent& request,
    const ClipClipboard& clipboard,
    core::u64 next_node) {
    try {
        const std::optional<doc::NodeId> target = paste_lane(request.request);
        if (target.has_value() && !clipboard.single_source_lane()) {
            return Result::failure(
                TimelineEditIntegrationError::ClipboardIncompatible);
        }
        if (target.has_value() &&
            !target_lane_available(document, request.scene, *target)) {
            return Result::failure(
                TimelineEditIntegrationError::TargetLaneUnavailable);
        }

        const std::vector<doc::NodeId> synced =
            synced_lanes(document, request.scene);
        scene::TimelineEditResult planned = std::visit(
            RequestPlanner{clips, synced, request, clipboard},
            request.request);
        if (!planned) {
            return Result::failure(
                TimelineEditIntegrationError::TimelineRejected);
        }

        auto authored = plan_timeline_edit_authoring(
            document, planned.value(), next_node);
        if (!authored) {
            return Result::failure(
                TimelineEditIntegrationError::AuthoringRejected);
        }

        TimelineEditIntegrationPlan result{};
        result.document = authored.value().document;
        result.timeline = std::move(planned.value());
        result.changed = authored.value().changed;
        result.erased = authored.value().erased;
        result.created = authored.value().created;
        result.next_node = authored.value().next_node;

        if (target.has_value()) {
            auto pasted = clipboard.paste(
                result.document, request.scene, request.playhead,
                result.next_node, *target);
            if (!pasted) {
                return Result::failure(
                    TimelineEditIntegrationError::PasteRejected);
            }
            result.document = pasted.value().document;
            result.next_node = pasted.value().next_node;
            if (std::holds_alternative<editor::TimelineOverwriteRequest>(
                    request.request) &&
                !stack_in_front(result.document, pasted.value().created,
                                clips, request.scene, *target)) {
                return Result::failure(
                    TimelineEditIntegrationError::PasteRejected);
            }
            result.created.insert(
                result.created.end(),
                pasted.value().created.begin(), pasted.value().created.end());
        }

        std::sort(result.changed.begin(), result.changed.end());
        result.changed.erase(
            std::unique(result.changed.begin(), result.changed.end()),
            result.changed.end());
        return Result::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return Result::failure(
            TimelineEditIntegrationError::AllocationFailed);
    }
}

} // namespace mediaserver::app
