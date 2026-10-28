#include "timeline_edit_integration.hpp"

#include "timeline_edit_authoring.hpp"

#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
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
    core::u64 next_node{};

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

    [[nodiscard]] scene::TimelineEditResult operator()(
        const editor::TimelineOverwriteRequest& request) const {
        const std::optional<core::i64> span = clipboard.span_duration();
        if (!span.has_value() || *span <= 0 ||
            intent.playhead > static_cast<core::u64>(
                                  std::numeric_limits<core::i64>::max())) {
            return scene::TimelineEditResult::failure(
                scene::TimelineEditError::InvalidSpan);
        }
        return scene::plan_overwrite(
            clips, scene::TimelineLaneRef{intent.scene, request.lane},
            static_cast<core::i64>(intent.playhead), *span,
            intent.show_rate, doc::NodeId{next_node});
    }
};

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
            RequestPlanner{clips, synced, request, clipboard, next_node},
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
