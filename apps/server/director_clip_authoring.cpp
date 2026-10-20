#include "director_clip_authoring.hpp"

#include "clip_timing_authoring.hpp"

#include <mediaserver/doc/node.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace mediaserver::app {

// Writes a clip placement into a media node. One PutNode carries the whole
// ClipTiming because it is one indivisible edit: a move that landed its start
// without its duration would be a clip of a different length for a frame.
void write_clip_timing(doc::Node &node, const scene::ClipTiming &timing) {
    const auto whole = [](std::int64_t value) {
        return doc::PropertyValue{value};
    };
    node.properties.insert_or_assign(
        std::string{scene::schema::clip_start},
        whole(static_cast<std::int64_t>(timing.start)));
    node.properties.insert_or_assign(
        std::string{scene::schema::clip_duration},
        whole(static_cast<std::int64_t>(timing.duration)));
    node.properties.insert_or_assign(
        std::string{scene::schema::source_in},
        whole(static_cast<std::int64_t>(timing.source_in)));
    node.properties.insert_or_assign(
        std::string{scene::schema::fade_in},
        whole(static_cast<std::int64_t>(timing.fade_in)));
    node.properties.insert_or_assign(
        std::string{scene::schema::fade_out},
        whole(static_cast<std::int64_t>(timing.fade_out)));
}

[[nodiscard]] bool schedule_clip_timing(cmd::DirectorScheduler &scheduler,
                                        const editor::ClipTimingEdit &edit,
                                        cmd::FrameNumber apply_at) {
    const auto *const existing = scheduler.projected_document().get(edit.clip);
    if (existing == nullptr || existing->type != scene::schema::media_type) {
        // A stale request from a clip that has since been deleted. Harmless.
        return true;
    }
    doc::Node updated = *existing;
    core::u32 sequence_count{1U};
    if (const auto found = existing->properties.find(scene::schema::sequence_count);
        found != existing->properties.end()) {
        const auto* count = std::get_if<std::int64_t>(&found->second);
        if (!count || *count <= 0 ||
            static_cast<core::u64>(*count) > std::numeric_limits<core::u32>::max()) return true;
        sequence_count = static_cast<core::u32>(*count);
    }
    const auto timing = normalise_clip_timing(edit.timing, sequence_count);
    if (!timing) return true;
    write_clip_timing(updated, *timing);
    if (edit.lane.has_value()) {
        // The implicit lane is the absence of the property, not a zero value:
        // the scene model rejects a lane_id of zero as malformed.
        if (*edit.lane == doc::NodeId{}) {
            updated.properties.erase(std::string{scene::schema::lane_id});
        } else {
            updated.properties.insert_or_assign(
                std::string{scene::schema::lane_id},
                doc::PropertyValue{
                    static_cast<std::int64_t>(edit.lane->value())});
        }
    }
    if (updated == *existing) {
        return true;
    }
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{2U}, cmd::TransactionId{edit.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

// Cuts one placement in two at `frame`. The left half keeps the node and loses
// its tail; the right half is a new node with the same source, the same
// transform, and its trim advanced at the native source cadence to the frame
// visible at the cut. The integer source phase restarts on that frame; a
// fractional-rate cut does not retain a subframe cadence remainder. The tail ramp travels to
// the right half and the head ramp stays with the left, because those are the
// ends that still exist.
[[nodiscard]] bool schedule_clip_split(cmd::DirectorScheduler &scheduler,
                                       const editor::ClipSplitIntent &request,
                                       doc::NodeId id,
                                       cmd::FrameNumber apply_at,
                                       scene::MediaFrameRate show_rate) {
    const auto *const existing =
        scheduler.projected_document().get(request.clip);
    if (existing == nullptr || existing->type != scene::schema::media_type) {
        return true;
    }
    const auto read = [existing](std::string_view name, std::int64_t fallback = 0) -> std::int64_t {
        const auto found = existing->properties.find(name);
        if (found == existing->properties.end()) {
            return fallback;
        }
        const auto *const value = std::get_if<std::int64_t>(&found->second);
        return value != nullptr ? *value : 0;
    };
    scene::ClipTiming timing{};
    timing.start = read(scene::schema::clip_start);
    timing.duration = read(scene::schema::clip_duration);
    timing.source_in = read(scene::schema::source_in);
    timing.fade_in = read(scene::schema::fade_in);
    timing.fade_out = read(scene::schema::fade_out);
    const auto count = read(scene::schema::sequence_count, 1);
    if (count <= 0 || static_cast<core::u64>(count) > std::numeric_limits<core::u32>::max()) return true;
    std::optional<scene::MediaFrameRate> source_rate;
    const auto numerator = read(scene::schema::source_rate_numerator);
    const auto denominator = read(scene::schema::source_rate_denominator);
    if (numerator != 0 || denominator != 0) {
        if (numerator <= 0 || denominator <= 0 ||
            static_cast<core::u64>(numerator) > std::numeric_limits<core::u32>::max() ||
            static_cast<core::u64>(denominator) > std::numeric_limits<core::u32>::max()) return true;
        source_rate = scene::MediaFrameRate{static_cast<core::u32>(numerator),
                                           static_cast<core::u32>(denominator)};
    }
    const auto split = split_clip_timing(timing, request.frame,
        static_cast<core::u32>(count), show_rate, source_rate);
    if (!split) {
        // Nothing to cut, or a cut that would produce an empty half. A split
        // is never allowed to become a delete.
        return true;
    }

    doc::Node head = *existing;
    write_clip_timing(head, split->left);
    doc::Node tail = *existing;
    tail.id = id;
    write_clip_timing(tail, split->right);

    const cmd::UndoContextId context{2U};
    const cmd::TransactionId transaction{request.gesture};
    auto shortened = scheduler.schedule_edit(cmd::PutNode{std::move(head)},
                                             cmd::PutNode{*existing}, context,
                                             transaction, apply_at);
    if (!shortened) {
        return false;
    }
    auto added = scheduler.schedule_edit(cmd::PutNode{std::move(tail)},
                                         cmd::EraseNode{id}, context,
                                         transaction, apply_at);
    return static_cast<bool>(added);
}

} // namespace mediaserver::app
