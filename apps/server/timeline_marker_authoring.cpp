#include "timeline_marker_authoring.hpp"

#include <mediaserver/scene/scene_schema.hpp>

#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>

namespace mediaserver::app {
namespace {

[[nodiscard]] std::optional<scene::SceneId> owner_of(
    const doc::Node& node) noexcept {
    const auto found = node.properties.find(scene::schema::scene_id);
    if (found == node.properties.end()) {
        return std::nullopt;
    }
    const auto* const value = std::get_if<std::int64_t>(&found->second);
    return value != nullptr && *value >= 0
        ? std::optional<scene::SceneId>{
              scene::SceneId{static_cast<core::u64>(*value)}}
        : std::nullopt;
}

[[nodiscard]] std::string_view cue_word(scene::CueAction cue) noexcept {
    switch (cue) {
    case scene::CueAction::Hold:
        return scene::schema::marker_cue_hold;
    case scene::CueAction::Loop:
        return scene::schema::marker_cue_loop;
    case scene::CueAction::None:
        break;
    }
    return scene::schema::marker_cue_none;
}

[[nodiscard]] std::optional<MarkerAuthorError> validate(
    const scene::TimelineMarker& marker) noexcept {
    if (marker.frame < 0) {
        return MarkerAuthorError::InvalidFrame;
    }
    if (marker.name.size() > scene::max_timeline_marker_name_bytes) {
        return MarkerAuthorError::InvalidName;
    }
    if (marker.note.size() > scene::max_timeline_marker_note_bytes) {
        return MarkerAuthorError::InvalidNote;
    }
    if (marker.color >= scene::timeline_marker_colors) {
        return MarkerAuthorError::InvalidColor;
    }
    if (marker.loop_wait_ms > scene::max_loop_wait_ms
        || marker.loop_fade_ms > scene::max_loop_fade_ms) {
        return MarkerAuthorError::InvalidLoopTiming;
    }
    return std::nullopt;
}

// Writes every authored field, replacing what was there. Unknown properties a
// newer build may have written are kept, so an older Director editing a cue
// does not strip what it does not understand.
void write_marker(doc::Node& node, scene::SceneId owner,
                  const scene::TimelineMarker& marker) {
    const auto put = [&node](std::string_view key, doc::PropertyValue value) {
        node.properties.insert_or_assign(std::string{key}, std::move(value));
    };
    put(scene::schema::scene_id,
        doc::PropertyValue{static_cast<std::int64_t>(owner.value())});
    put(scene::schema::marker_frame, doc::PropertyValue{marker.frame});
    put(scene::schema::marker_name, doc::PropertyValue{marker.name});
    put(scene::schema::marker_note, doc::PropertyValue{marker.note});
    put(scene::schema::marker_color,
        doc::PropertyValue{static_cast<std::int64_t>(marker.color)});
    put(scene::schema::marker_cue,
        doc::PropertyValue{std::string{cue_word(marker.cue)}});
    put(scene::schema::marker_loop_wait,
        doc::PropertyValue{static_cast<std::int64_t>(marker.loop_wait_ms)});
    put(scene::schema::marker_loop_fade,
        doc::PropertyValue{static_cast<std::int64_t>(marker.loop_fade_ms)});
}

} // namespace

core::Result<MarkerAuthorPlan, MarkerAuthorError> plan_marker_authoring(
    const doc::Root& document, const MarkerAuthorRequest& request,
    core::u64 next_node) {
    using Result = core::Result<MarkerAuthorPlan, MarkerAuthorError>;
    try {
        // One pass proves the requested scene exists, and a second counts its
        // markers for the bound.
        bool scene_found{};
        std::size_t markers{};
        const doc::Delta all = doc::diff(doc::Root{}, document);
        for (const doc::NodeChange& change : all.changes) {
            if (!change.after) {
                continue;
            }
            if (change.after->type == scene::schema::stage_type) {
                scene_found = scene_found || change.id == request.scene;
            }
        }
        if (!scene_found) {
            return Result::failure(MarkerAuthorError::InvalidScene);
        }
        for (const doc::NodeChange& change : all.changes) {
            if (change.after
                && change.after->type == scene::schema::timeline_marker_type
                && owner_of(*change.after) == request.scene) {
                ++markers;
            }
        }

        MarkerAuthorPlan plan{};
        plan.document = document;
        plan.next_node = next_node;

        if (request.action == MarkerAuthorAction::Create) {
            if (const auto invalid = validate(request.marker)) {
                return Result::failure(*invalid);
            }
            if (markers >= max_scene_markers) {
                return Result::failure(MarkerAuthorError::MarkerLimit);
            }
            constexpr core::u64 maximum = static_cast<core::u64>(
                std::numeric_limits<std::int64_t>::max());
            while (next_node <= maximum
                   && document.contains(doc::NodeId{next_node})) {
                ++next_node;
            }
            if (next_node == 0U || next_node > maximum) {
                return Result::failure(MarkerAuthorError::IdExhausted);
            }
            doc::Node node{};
            node.id = doc::NodeId{next_node};
            node.type = std::string{scene::schema::timeline_marker_type};
            write_marker(node, request.scene, request.marker);
            plan.document = plan.document.set(std::move(node));
            plan.marker = doc::NodeId{next_node};
            plan.next_node = next_node == maximum ? maximum : next_node + 1U;
            return Result::success(std::move(plan));
        }

        const doc::Node* const existing = document.get(request.marker.id);
        if (existing == nullptr
            || existing->type != scene::schema::timeline_marker_type) {
            return Result::failure(MarkerAuthorError::MarkerNotFound);
        }
        if (owner_of(*existing) != request.scene) {
            return Result::failure(MarkerAuthorError::CrossSceneMarker);
        }
        plan.marker = existing->id;

        if (request.action == MarkerAuthorAction::Delete) {
            auto erased = document.erase(existing->id);
            if (!erased) {
                return Result::failure(MarkerAuthorError::MarkerNotFound);
            }
            plan.document = std::move(*erased);
            return Result::success(std::move(plan));
        }

        if (const auto invalid = validate(request.marker)) {
            return Result::failure(*invalid);
        }
        doc::Node updated = *existing;
        write_marker(updated, request.scene, request.marker);
        if (updated != *existing) {
            plan.document = plan.document.set(std::move(updated));
        }
        return Result::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return Result::failure(MarkerAuthorError::AllocationFailed);
    }
}

} // namespace mediaserver::app
