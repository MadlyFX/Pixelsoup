#pragma once

#include <mediaserver/scene/timeline_edit.hpp>

#include <algorithm>
#include <optional>

namespace mediaserver::app {

// Ordinary move/trim/fade edits keep signed placement time separate from the
// nonnegative transport. Invalid exclusive ends are refused at authoring.
[[nodiscard]] inline std::optional<scene::ClipTiming> normalise_clip_timing(
    scene::ClipTiming timing, core::u32 sequence_count) noexcept {
    if (sequence_count == 0U) return std::nullopt;
    timing.duration = std::max(timing.duration, core::i64{1});
    timing.source_in = std::max(timing.source_in, core::i64{0}) % sequence_count;
    timing.fade_in = std::clamp(timing.fade_in, core::i64{0}, timing.duration);
    timing.fade_out = std::clamp(timing.fade_out, core::i64{0}, timing.duration);
    return scene::valid_clip_timing(timing)
        ? std::optional{timing} : std::nullopt;
}

struct ClipSplitTiming final {
    scene::ClipTiming left;
    scene::ClipTiming right;
};

// Razor samples the source frame already visible at the cut, including wraps
// and the persisted native cadence. The source remains an integer ordinal.
[[nodiscard]] inline std::optional<ClipSplitTiming> split_clip_timing(
    const scene::ClipTiming& timing, core::i64 cut,
    core::u32 sequence_count, scene::MediaFrameRate show_rate,
    const std::optional<scene::MediaFrameRate>& source_rate) noexcept {
    if (!timing.placed() || !scene::valid_clip_timing(timing) ||
        cut <= timing.start || cut >= timing.start + timing.duration) {
        return std::nullopt;
    }
    const core::i64 left_length = cut - timing.start;
    const auto offset = scene::advance_source_phase(timing.source_in, left_length,
        show_rate, source_rate, sequence_count, scene::SourcePhaseRounding::Floor);
    if (!offset) return std::nullopt;
    ClipSplitTiming result{timing, timing};
    result.left.source_in %= sequence_count;
    result.left.duration = left_length;
    result.left.fade_in = std::min(timing.fade_in, left_length);
    result.left.fade_out = 0;
    result.right.start = cut;
    result.right.duration = timing.duration - left_length;
    result.right.source_in = offset.value();
    result.right.fade_in = 0;
    result.right.fade_out = std::min(timing.fade_out, result.right.duration);
    return result;
}

} // namespace mediaserver::app
