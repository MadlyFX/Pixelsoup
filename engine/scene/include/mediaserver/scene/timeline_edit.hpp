#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/scene/scene_model.hpp>

#include <span>
#include <vector>

// Pure, deterministic timeline-edit planning.
//
// This layer deliberately stops before document authoring. It consumes the
// editor-facing ClipPlacement projection and returns complete replacement,
// erase, and create sets that a caller may put into one undoable transaction.
// No request mutates its input, consults local media metadata, or depends on
// input iteration order.
namespace mediaserver::scene {

enum class TimelineEditError : core::u8 {
    InvalidRate,
    InvalidSpan,
    ClipNotFound,
    LockedClip,
    IncompatibleClip,
    MultipleLanes,
    AmbiguousSelection,
    GappedSelection,
    OverlappingClips,
    InvalidFirstId,
    IdExhausted,
    IntegerOverflow,
    AllocationFailed,
};

enum class TimelineTrimEdge : core::u8 {
    Head,
    Tail,
};

// Scene is part of a lane address because lane zero is the implicit lane in
// every scene. Authored lane ids are globally stable, but carrying both values
// makes insert/overwrite requests just as unambiguous as clip-selected edits.
struct TimelineLaneRef final {
    SceneId scene{};
    doc::NodeId lane{};

    bool operator==(const TimelineLaneRef&) const = default;
};

struct TimelineEditPlan final {
    // Existing placements replaced in full, sorted by node id.
    std::vector<ClipPlacement> updates;
    // Existing placement ids removed, sorted ascending.
    std::vector<doc::NodeId> erases;
    // New split remainders, sorted by their newly allocated node id.
    std::vector<ClipPlacement> creates;

    // The signed show-frame gesture actually admitted after every constraint
    // was applied. Ripple delete and insert report the signed timeline-size
    // change; overwrite reports zero because later clips remain fixed.
    core::i64 applied_delta{};
    // Primary edit/preview span in the pre-edit timeline. For a roll or slide
    // this is the fixed outer span; for insert/overwrite it is the requested
    // paste span.
    core::i64 span_start{};
    core::i64 span_duration{};

    bool operator==(const TimelineEditPlan&) const = default;
};

using TimelineEditResult =
    core::Result<TimelineEditPlan, TimelineEditError>;
using SourceOrdinalResult =
    core::Result<core::i64, TimelineEditError>;

// Maps a signed show-frame delta to a signed source-ordinal delta using exact
// rational arithmetic:
//
//   show_delta * source_rate / show_rate
//
// The result is rounded to nearest, with exact half ties away from zero. The
// product is evaluated without overflowing a 64-bit intermediate and fails
// rather than saturating if the rounded signed result is not representable.
// An absent source rate is the image-sequence compatibility mapping (1:1).
[[nodiscard]] SourceOrdinalResult source_ordinal_delta(
    core::i64 show_delta,
    MediaFrameRate show_rate,
    const std::optional<MediaFrameRate>& source_rate) noexcept;

// How many show frames `source_frames` consecutive source frames occupy: the
// fewest show frames whose playback (the Floor mapping a cut uses) reaches
// the last of them,
//
//   ceil(source_frames * show_rate / source_rate)
//
// so a source range cut into the timeline plays every frame it marked and no
// frame after it. An absent source rate is the image-sequence 1:1 mapping.
// Zero frames is an InvalidSpan; a result past i64 is an IntegerOverflow.
[[nodiscard]] SourceOrdinalResult source_span_show_frames(
    core::u64 source_frames,
    MediaFrameRate show_rate,
    const std::optional<MediaFrameRate>& source_rate) noexcept;

enum class SourcePhaseRounding : core::u8 { Nearest, Floor };

// Advances a looping source phase by the same exact rational, nearest-frame
// mapping as source_ordinal_delta, then wraps into [0, sequence_count).
// Modular wide division also handles offsets whose unwrapped ordinal would
// exceed i64. Floor selects playback's frame at a cut; Nearest is the editing
// gesture policy. The source offset must be nonnegative and the count nonzero.
[[nodiscard]] SourceOrdinalResult advance_source_phase(
    core::i64 source_in, core::i64 show_delta, MediaFrameRate show_rate,
    const std::optional<MediaFrameRate>& source_rate,
    core::u32 sequence_count,
    SourcePhaseRounding rounding = SourcePhaseRounding::Nearest) noexcept;

// Sync lock. Each ripple edit below also takes the tracks that keep time with
// it: on each of those lanes (the edited lane itself is skipped), every
// unlocked clip that starts at or after the edit's boundary moves by the same
// amount as the edited lane's. A clip straddling the boundary, and a locked
// clip, stay where they are. An edit that would push a moving clip into one
// that stays is clamped where the edit clamps (ripple trim) and refused with
// OverlappingClips where it cannot (ripple delete, insert). Overlaps a synced
// lane already had are not counted against the edit. Every moved clip is an
// update in the plan.

// Tail: duration += d, then later unlocked clips move by d.
// Head: source-in += rate(d), duration -= d, then later unlocked clips move
// by -d while the selected start remains fixed.
[[nodiscard]] TimelineEditResult plan_ripple_trim(
    std::span<const ClipPlacement> clips,
    doc::NodeId selected,
    TimelineTrimEdge edge,
    core::i64 requested_delta,
    MediaFrameRate show_rate,
    std::span<const doc::NodeId> synced_lanes = {});

// Selected whole clips must form one contiguous union on one lane. Clips at
// or after the old union end move left by its exact duration.
[[nodiscard]] TimelineEditResult plan_ripple_delete(
    std::span<const ClipPlacement> clips,
    std::span<const doc::NodeId> selected,
    std::span<const doc::NodeId> synced_lanes = {});

// The two supplied clips may be in either input order; their timeline order
// determines left and right. They must touch at exactly one unambiguous cut.
[[nodiscard]] TimelineEditResult plan_roll(
    std::span<const ClipPlacement> clips,
    doc::NodeId first,
    doc::NodeId second,
    core::i64 requested_delta,
    MediaFrameRate show_rate);

// Slips the looping source in either direction without changing placement or
// fades. Full-source and longer-than-source placements have the same policy.
[[nodiscard]] TimelineEditResult plan_slip(
    std::span<const ClipPlacement> clips,
    doc::NodeId selected,
    core::i64 requested_delta,
    MediaFrameRate show_rate);

// Requires one touching predecessor and successor on the selected clip's lane.
[[nodiscard]] TimelineEditResult plan_slide(
    std::span<const ClipPlacement> clips,
    doc::NodeId selected,
    core::i64 requested_delta,
    MediaFrameRate show_rate);

// Plans the displacement of existing clips only; the pasted placements are
// authored by the caller in the same transaction.
[[nodiscard]] TimelineEditResult plan_insert(
    std::span<const ClipPlacement> clips,
    TimelineLaneRef target,
    core::i64 start,
    core::i64 duration,
    std::span<const doc::NodeId> synced_lanes = {});

// Plans the effect of a pasted span on existing clips. A clip strictly
// containing the span is updated to its left remainder and copied to a newly
// allocated right remainder. New ids ascend from first_create_id, skipping
// ids present in clips, and remain in the signed document-reference domain.
[[nodiscard]] TimelineEditResult plan_overwrite(
    std::span<const ClipPlacement> clips,
    TimelineLaneRef target,
    core::i64 start,
    core::i64 duration,
    MediaFrameRate show_rate,
    doc::NodeId first_create_id);

} // namespace mediaserver::scene
