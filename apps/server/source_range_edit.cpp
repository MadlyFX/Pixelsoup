#include "source_range_edit.hpp"

#include "clip_timing_authoring.hpp"

#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/scene/timeline_edit.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <new>
#include <string>

namespace mediaserver::app {
namespace {

using Result =
    core::Result<TimelineEditIntegrationPlan, SourceRangeEditError>;

[[nodiscard]] std::optional<scene::MediaFrameRate>
scene_rate(const std::optional<assets::MediaFrameRate>& rate) noexcept {
    if (!rate.has_value()) {
        return std::nullopt;
    }
    return scene::MediaFrameRate{rate->numerator, rate->denominator};
}

void write_timing(doc::Node& node, const scene::ClipTiming& timing) {
    node.properties.insert_or_assign(std::string{scene::schema::clip_start},
                                     doc::PropertyValue{timing.start});
    node.properties.insert_or_assign(std::string{scene::schema::clip_duration},
                                     doc::PropertyValue{timing.duration});
    node.properties.insert_or_assign(std::string{scene::schema::source_in},
                                     doc::PropertyValue{timing.source_in});
    node.properties.insert_or_assign(std::string{scene::schema::fade_in},
                                     doc::PropertyValue{timing.fade_in});
    node.properties.insert_or_assign(std::string{scene::schema::fade_out},
                                     doc::PropertyValue{timing.fade_out});
}

} // namespace

core::u64 source_append_frame(std::span<const scene::ClipPlacement> clips,
                              scene::SceneId scene,
                              doc::NodeId lane) noexcept {
    core::i64 end = 0;
    for (const scene::ClipPlacement& clip : clips) {
        if (clip.scene != scene || clip.lane != lane ||
            !clip.timing.placed() ||
            clip.timing.start >
                std::numeric_limits<core::i64>::max() - clip.timing.duration) {
            continue;
        }
        end = std::max(end, clip.timing.start + clip.timing.duration);
    }
    return static_cast<core::u64>(end);
}

Result plan_source_range_edit(const doc::Root& document,
                              std::span<const scene::ClipPlacement> clips,
                              const assets::MediaAsset& asset,
                              const editor::SourceRangeEditIntent& request,
                              doc::Node placement, core::u64 next_node) {
    try {
        if (!asset.frame_count.has_value() || *asset.frame_count == 0U) {
            return Result::failure(SourceRangeEditError::NotProbed);
        }
        const core::u64 count = *asset.frame_count;
        if (request.source_frames == 0U || request.source_in >= count ||
            request.source_frames > count - request.source_in) {
            return Result::failure(SourceRangeEditError::RangeOutsideSource);
        }
        if (count > 1U) {
            const auto duration = scene::source_span_show_frames(
                request.source_frames, request.show_rate,
                scene_rate(asset.frame_rate));
            if (!duration) {
                return Result::failure(
                    SourceRangeEditError::RangeOutsideSource);
            }
            placement.properties.insert_or_assign(
                std::string{scene::schema::clip_duration},
                doc::PropertyValue{duration.value()});
            placement.properties.insert_or_assign(
                std::string{scene::schema::source_in},
                doc::PropertyValue{
                    static_cast<std::int64_t>(request.source_in)});
        }
        placement.properties.insert_or_assign(
            std::string{scene::schema::clip_start}, doc::PropertyValue{0});

        // A one-clip clipboard holding the trimmed placement, pasted by the
        // same planner a clipboard Insert or Overwrite uses.
        ClipClipboard clipboard;
        const doc::NodeId staged = placement.id;
        const doc::Root source_document = doc::Root{}.set(std::move(placement));
        const std::array<doc::NodeId, 1U> selection{staged};
        if (!clipboard.copy(source_document, selection)) {
            return Result::failure(SourceRangeEditError::RangeOutsideSource);
        }

        editor::TimelineEditIntent intent{};
        intent.scene = request.scene;
        intent.show_rate = request.show_rate;
        intent.transaction = request.transaction;
        if (request.mode == editor::SourceEditMode::Insert) {
            intent.request = editor::TimelineInsertRequest{request.lane};
            intent.playhead = request.start;
        } else {
            intent.request = editor::TimelineOverwriteRequest{request.lane};
            intent.playhead =
                request.mode == editor::SourceEditMode::Append
                    ? source_append_frame(clips, request.scene, request.lane)
                    : request.start;
        }
        // An editor's Insert lands inside a clip by cutting it there, as the
        // Razor does, so what followed the edit point moves along with the
        // rest of the track. The cut, the insert and the shift are one
        // document replacement and so one undo step.
        doc::Root working = document;
        std::vector<scene::ClipPlacement> working_clips(clips.begin(),
                                                        clips.end());
        core::u64 allocator = next_node;
        if (request.mode == editor::SourceEditMode::Insert &&
            request.start <=
                static_cast<core::u64>(std::numeric_limits<core::i64>::max())) {
            const auto cut = static_cast<core::i64>(request.start);
            bool split_any = false;
            for (const scene::ClipPlacement& clip : clips) {
                if (clip.scene != request.scene || clip.lane != request.lane ||
                    !clip.timing.placed() || clip.timing.start >= cut ||
                    clip.timing.start >= std::numeric_limits<core::i64>::max() -
                                             clip.timing.duration ||
                    clip.timing.start + clip.timing.duration <= cut) {
                    continue;
                }
                const doc::Node* const node = document.get(clip.node);
                const auto split = scene::valid_clip_timing(clip.timing)
                                       ? split_clip_timing(
                                             clip.timing, cut,
                                             std::max(clip.sequence_count, 1U),
                                             request.show_rate, clip.frame_rate)
                                       : std::nullopt;
                if (clip.locked || node == nullptr || !split.has_value()) {
                    return Result::failure(
                        SourceRangeEditError::TimelineRejected);
                }
                while (working.contains(doc::NodeId{allocator})) {
                    ++allocator;
                }
                doc::Node head = *node;
                write_timing(head, split->left);
                doc::Node tail = *node;
                tail.id = doc::NodeId{allocator++};
                write_timing(tail, split->right);
                working = working.set(std::move(head)).set(std::move(tail));
                split_any = true;
            }
            if (split_any) {
                scene::SceneModel model;
                model.reset(working);
                working_clips = model.clips(request.scene);
            }
        }
        auto planned = plan_timeline_edit_integration(
            working, working_clips, intent, clipboard, allocator);
        if (!planned) {
            switch (planned.error()) {
            case TimelineEditIntegrationError::TargetLaneUnavailable:
                return Result::failure(
                    SourceRangeEditError::TargetLaneUnavailable);
            case TimelineEditIntegrationError::AllocationFailed:
                return Result::failure(SourceRangeEditError::AllocationFailed);
            default:
                return Result::failure(SourceRangeEditError::TimelineRejected);
            }
        }
        return Result::success(std::move(planned).value());
    } catch (const std::bad_alloc&) {
        return Result::failure(SourceRangeEditError::AllocationFailed);
    }
}

} // namespace mediaserver::app
