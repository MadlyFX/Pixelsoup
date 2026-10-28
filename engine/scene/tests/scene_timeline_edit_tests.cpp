#include <mediaserver/scene/timeline_edit.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define EDIT_CHECK(...)                                                        \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr scene::MediaFrameRate show_60{60U, 1U};
constexpr scene::SceneId default_scene{10U};
constexpr doc::NodeId default_lane{20U};

[[nodiscard]] scene::ClipPlacement clip(
    core::u64 id, core::i64 start, core::i64 duration,
    core::i64 source_in = 0, core::u32 sequence_count = 1'000U,
    std::optional<scene::MediaFrameRate> frame_rate = std::nullopt,
    doc::NodeId lane = default_lane, bool locked = false,
    scene::SceneId scene_id = default_scene) {
    scene::ClipPlacement result{};
    result.node = doc::NodeId{id};
    result.scene = scene_id;
    result.source = "clip_%04u.png";
    result.sequence_count = sequence_count;
    result.timing = scene::ClipTiming{start, duration, source_in, 0, 0};
    result.lane = lane;
    result.order = static_cast<core::i64>(id);
    result.locked = locked;
    result.frame_rate = frame_rate;
    return result;
}

[[nodiscard]] const scene::ClipPlacement* update_for(
    const scene::TimelineEditPlan& plan, doc::NodeId node) noexcept {
    const auto found = std::lower_bound(
        plan.updates.begin(), plan.updates.end(), node,
        [](const scene::ClipPlacement& placement,
           doc::NodeId id) noexcept { return placement.node < id; });
    return found == plan.updates.end() || found->node != node ? nullptr
                                                              : &*found;
}

[[nodiscard]] const scene::ClipPlacement* create_for(
    const scene::TimelineEditPlan& plan, doc::NodeId node) noexcept {
    const auto found = std::lower_bound(
        plan.creates.begin(), plan.creates.end(), node,
        [](const scene::ClipPlacement& placement,
           doc::NodeId id) noexcept { return placement.node < id; });
    return found == plan.creates.end() || found->node != node ? nullptr
                                                              : &*found;
}

void exact_rate_mapping_is_signed_rounded_and_overflow_safe() {
    const auto source_rate = scene::MediaFrameRate{30'000U, 1'001U};
    const auto mapped_one =
        scene::source_ordinal_delta(1, show_60, source_rate);
    const auto mapped_two =
        scene::source_ordinal_delta(2, show_60, source_rate);
    const auto mapped_three =
        scene::source_ordinal_delta(3, show_60, source_rate);
    const auto mapped_thousand_one =
        scene::source_ordinal_delta(1'001, show_60, source_rate);
    EDIT_CHECK(mapped_one.has_value() && mapped_one.value() == 0);
    EDIT_CHECK(mapped_two.has_value() && mapped_two.value() == 1);
    EDIT_CHECK(mapped_three.has_value() && mapped_three.value() == 1);
    EDIT_CHECK(mapped_thousand_one.has_value() &&
               mapped_thousand_one.value() == 500);

    const auto negative =
        scene::source_ordinal_delta(-3, show_60, source_rate);
    EDIT_CHECK(negative.has_value() && negative.value() == -1);
    const auto positive_tie = scene::source_ordinal_delta(
        1, show_60, scene::MediaFrameRate{30U, 1U});
    const auto negative_tie = scene::source_ordinal_delta(
        -1, show_60, scene::MediaFrameRate{30U, 1U});
    EDIT_CHECK(positive_tie.has_value() && positive_tie.value() == 1);
    EDIT_CHECK(negative_tie.has_value() && negative_tie.value() == -1);

    const auto identity = scene::source_ordinal_delta(
        std::numeric_limits<core::i64>::min(), show_60, std::nullopt);
    EDIT_CHECK(identity.has_value());
    EDIT_CHECK(identity.value() == std::numeric_limits<core::i64>::min());

    // Both products occupy the high 64-bit limb, but their equal rational
    // factors cancel exactly. This pins the portable wide divide on a large
    // representable result, not only on its overflow path.
    constexpr core::u32 maximum = std::numeric_limits<core::u32>::max();
    const auto wide_identity = scene::source_ordinal_delta(
        std::numeric_limits<core::i64>::max(),
        scene::MediaFrameRate{maximum, maximum},
        scene::MediaFrameRate{maximum, maximum});
    EDIT_CHECK(wide_identity.has_value());
    EDIT_CHECK(wide_identity.value() ==
               std::numeric_limits<core::i64>::max());

    const auto invalid = scene::source_ordinal_delta(
        1, scene::MediaFrameRate{0U, 1U}, source_rate);
    EDIT_CHECK(!invalid.has_value());
    EDIT_CHECK(invalid.error() == scene::TimelineEditError::InvalidRate);

    const auto overflow = scene::source_ordinal_delta(
        std::numeric_limits<core::i64>::max(),
        scene::MediaFrameRate{1U, maximum},
        scene::MediaFrameRate{maximum, 1U});
    EDIT_CHECK(!overflow.has_value());
    EDIT_CHECK(overflow.error() ==
               scene::TimelineEditError::IntegerOverflow);
}

void ripple_tail_moves_the_whole_suffix_and_clamps_handles() {
    std::vector<scene::ClipPlacement> clips{
        clip(1U, 10, 10, 0, 100), clip(2U, 20, 5, 0, 100),
        clip(3U, 20, 5, 0, 100, std::nullopt, doc::NodeId{99U})};
    const auto grown = scene::plan_ripple_trim(
        clips, doc::NodeId{1U}, scene::TimelineTrimEdge::Tail, 4, show_60);
    EDIT_CHECK(grown.has_value());
    EDIT_CHECK(grown.value().applied_delta == 4);
    EDIT_CHECK(grown.value().updates.size() == 2U);
    EDIT_CHECK(update_for(grown.value(), doc::NodeId{1U})->timing.duration ==
               14);
    EDIT_CHECK(update_for(grown.value(), doc::NodeId{2U})->timing.start == 24);
    EDIT_CHECK(update_for(grown.value(), doc::NodeId{3U}) == nullptr);

    clips[0] = clip(1U, 10, 10, 0, 12);
    const auto handle_limited = scene::plan_ripple_trim(
        clips, doc::NodeId{1U}, scene::TimelineTrimEdge::Tail, 100, show_60);
    EDIT_CHECK(handle_limited.has_value());
    EDIT_CHECK(handle_limited.value().applied_delta == 2);
    EDIT_CHECK(update_for(handle_limited.value(), doc::NodeId{1U})
                   ->timing.duration == 12);
    EDIT_CHECK(update_for(handle_limited.value(), doc::NodeId{2U})
                   ->timing.start == 22);

    clips[0] = clip(1U, 10, 10, 0, 100);
    const auto shrunk = scene::plan_ripple_trim(
        clips, doc::NodeId{1U}, scene::TimelineTrimEdge::Tail, -100, show_60);
    EDIT_CHECK(shrunk.has_value());
    EDIT_CHECK(shrunk.value().applied_delta == -9);
    EDIT_CHECK(update_for(shrunk.value(), doc::NodeId{1U})->timing.duration ==
               1);
    EDIT_CHECK(update_for(shrunk.value(), doc::NodeId{2U})->timing.start == 11);
}

void ripple_head_uses_native_rate_and_clamps_at_source_zero() {
    const auto native = scene::MediaFrameRate{30'000U, 1'001U};
    std::vector<scene::ClipPlacement> clips{
        clip(1U, 0, 20, 5, 100, native), clip(2U, 20, 5, 0, 100)};
    const auto trimmed = scene::plan_ripple_trim(
        clips, doc::NodeId{1U}, scene::TimelineTrimEdge::Head, 3, show_60);
    EDIT_CHECK(trimmed.has_value());
    const auto* selected = update_for(trimmed.value(), doc::NodeId{1U});
    EDIT_CHECK(selected != nullptr);
    EDIT_CHECK(selected->timing.start == 0);
    EDIT_CHECK(selected->timing.duration == 17);
    EDIT_CHECK(selected->timing.source_in == 6);
    EDIT_CHECK(update_for(trimmed.value(), doc::NodeId{2U})->timing.start ==
               17);

    clips = {clip(1U, 0, 10, 5, 100), clip(2U, 10, 5, 0, 100)};
    const auto extended = scene::plan_ripple_trim(
        clips, doc::NodeId{1U}, scene::TimelineTrimEdge::Head, -20, show_60);
    EDIT_CHECK(extended.has_value());
    EDIT_CHECK(extended.value().applied_delta == -5);
    EDIT_CHECK(update_for(extended.value(), doc::NodeId{1U})
                   ->timing.source_in == 0);
    EDIT_CHECK(update_for(extended.value(), doc::NodeId{1U})
                   ->timing.duration == 15);
    EDIT_CHECK(update_for(extended.value(), doc::NodeId{2U})->timing.start ==
               15);
}

void ripple_gestures_clamp_before_timeline_overflow() {
    constexpr core::u32 maximum = std::numeric_limits<core::u32>::max();
    const auto very_slow = scene::MediaFrameRate{1U, maximum};
    const auto show = scene::MediaFrameRate{maximum, 1U};
    const core::i64 near_end = std::numeric_limits<core::i64>::max() - 5;
    const std::vector<scene::ClipPlacement> clips{
        clip(1U, 0, 1, 0, 2, very_slow),
        clip(2U, near_end, 5, 0, 2, very_slow)};
    const auto plan = scene::plan_ripple_trim(
        clips, doc::NodeId{1U}, scene::TimelineTrimEdge::Tail, 10, show);
    EDIT_CHECK(plan.has_value());
    EDIT_CHECK(plan.value().applied_delta == 0);
    EDIT_CHECK(plan.value().updates.empty());
}

void ripple_delete_requires_one_unambiguous_contiguous_union() {
    const std::vector<scene::ClipPlacement> clips{
        clip(3U, 10, 4), clip(2U, 5, 5), clip(1U, 0, 5),
        clip(9U, 0, 30, 0, 1'000U, std::nullopt, doc::NodeId{99U})};
    const std::array selected{doc::NodeId{2U}, doc::NodeId{1U}};
    const auto plan = scene::plan_ripple_delete(clips, selected);
    EDIT_CHECK(plan.has_value());
    EDIT_CHECK(plan.value().span_start == 0);
    EDIT_CHECK(plan.value().span_duration == 10);
    EDIT_CHECK(plan.value().applied_delta == -10);
    EDIT_CHECK((plan.value().erases ==
                std::vector<doc::NodeId>{doc::NodeId{1U}, doc::NodeId{2U}}));
    EDIT_CHECK(plan.value().updates.size() == 1U);
    EDIT_CHECK(plan.value().updates.front().node == doc::NodeId{3U});
    EDIT_CHECK(plan.value().updates.front().timing.start == 0);

    const std::vector<scene::ClipPlacement> gapped{
        clip(1U, 0, 4), clip(2U, 5, 4)};
    const auto gap = scene::plan_ripple_delete(gapped, selected);
    EDIT_CHECK(!gap.has_value());
    EDIT_CHECK(gap.error() == scene::TimelineEditError::GappedSelection);

    const std::vector<scene::ClipPlacement> intersected{
        clip(1U, 0, 5), clip(2U, 5, 5), clip(3U, 4, 2)};
    const auto ambiguous = scene::plan_ripple_delete(intersected, selected);
    EDIT_CHECK(!ambiguous.has_value());
    EDIT_CHECK(ambiguous.error() ==
               scene::TimelineEditError::AmbiguousSelection);
}

void ripple_delete_rejects_locked_and_multi_lane_selections() {
    const std::array selected{doc::NodeId{1U}, doc::NodeId{2U}};
    const std::vector<scene::ClipPlacement> locked{
        clip(1U, 0, 5), clip(2U, 5, 5, 0, 100, std::nullopt,
                             default_lane, true)};
    const auto locked_plan = scene::plan_ripple_delete(locked, selected);
    EDIT_CHECK(!locked_plan.has_value());
    EDIT_CHECK(locked_plan.error() == scene::TimelineEditError::LockedClip);

    const std::vector<scene::ClipPlacement> lanes{
        clip(1U, 0, 5),
        clip(2U, 5, 5, 0, 100, std::nullopt, doc::NodeId{21U})};
    const auto lane_plan = scene::plan_ripple_delete(lanes, selected);
    EDIT_CHECK(!lane_plan.has_value());
    EDIT_CHECK(lane_plan.error() == scene::TimelineEditError::MultipleLanes);
}

void roll_moves_only_one_touching_cut_and_clamps_both_assets() {
    std::vector<scene::ClipPlacement> clips{
        clip(1U, 0, 10, 0, 100), clip(2U, 10, 10, 5, 100),
        clip(3U, 20, 5)};
    const auto moved = scene::plan_roll(
        clips, doc::NodeId{2U}, doc::NodeId{1U}, 3, show_60);
    EDIT_CHECK(moved.has_value());
    EDIT_CHECK(moved.value().applied_delta == 3);
    EDIT_CHECK(moved.value().span_start == 0);
    EDIT_CHECK(moved.value().span_duration == 20);
    EDIT_CHECK(update_for(moved.value(), doc::NodeId{1U})->timing.duration ==
               13);
    const auto* right = update_for(moved.value(), doc::NodeId{2U});
    EDIT_CHECK(right->timing.start == 13);
    EDIT_CHECK(right->timing.duration == 7);
    EDIT_CHECK(right->timing.source_in == 8);
    EDIT_CHECK(update_for(moved.value(), doc::NodeId{3U}) == nullptr);

    clips[0] = clip(1U, 0, 10, 0, 12);
    const auto tail_limited = scene::plan_roll(
        clips, doc::NodeId{1U}, doc::NodeId{2U}, 20, show_60);
    EDIT_CHECK(tail_limited.has_value());
    EDIT_CHECK(tail_limited.value().applied_delta == 2);

    clips[0] = clip(1U, 0, 10, 0, 100);
    clips[1] = clip(2U, 10, 10, 2, 100);
    const auto head_limited = scene::plan_roll(
        clips, doc::NodeId{1U}, doc::NodeId{2U}, -20, show_60);
    EDIT_CHECK(head_limited.has_value());
    EDIT_CHECK(head_limited.value().applied_delta == -2);
}

void roll_uses_native_rate_and_rejects_gaps_overlaps_and_locks() {
    const auto native = scene::MediaFrameRate{30'000U, 1'001U};
    std::vector<scene::ClipPlacement> clips{
        clip(1U, 0, 10, 0, 100, native),
        clip(2U, 10, 10, 10, 100, native)};
    const auto mapped = scene::plan_roll(
        clips, doc::NodeId{1U}, doc::NodeId{2U}, 3, show_60);
    EDIT_CHECK(mapped.has_value());
    EDIT_CHECK(update_for(mapped.value(), doc::NodeId{2U})
                   ->timing.source_in == 11);

    clips[1].timing.start = 11;
    const auto gap = scene::plan_roll(
        clips, doc::NodeId{1U}, doc::NodeId{2U}, 1, show_60);
    EDIT_CHECK(!gap.has_value());
    EDIT_CHECK(gap.error() == scene::TimelineEditError::GappedSelection);

    clips[1].timing.start = 9;
    const auto overlap = scene::plan_roll(
        clips, doc::NodeId{1U}, doc::NodeId{2U}, 1, show_60);
    EDIT_CHECK(!overlap.has_value());
    EDIT_CHECK(overlap.error() == scene::TimelineEditError::OverlappingClips);

    clips[1].timing.start = 10;
    clips[1].locked = true;
    const auto locked = scene::plan_roll(
        clips, doc::NodeId{1U}, doc::NodeId{2U}, 1, show_60);
    EDIT_CHECK(!locked.has_value());
    EDIT_CHECK(locked.error() == scene::TimelineEditError::LockedClip);
}

void slip_keeps_placement_fixed_and_wraps_source_phase() {
    std::vector<scene::ClipPlacement> clips{
        clip(1U, 10, 10, 5, 20), clip(2U, 12, 3, 0, 100)};
    clips.front().timing.fade_in = 3;
    clips.front().timing.fade_out = 4;
    const auto forward =
        scene::plan_slip(clips, doc::NodeId{1U}, 103, show_60);
    EDIT_CHECK(forward.has_value());
    EDIT_CHECK(forward.value().applied_delta == 103);
    const auto* changed = update_for(forward.value(), doc::NodeId{1U});
    EDIT_CHECK(changed != nullptr);
    EDIT_CHECK(changed->timing == (scene::ClipTiming{10, 10, 8, 3, 4}));

    const auto backward =
        scene::plan_slip(clips, doc::NodeId{1U}, -107, show_60);
    EDIT_CHECK(backward.has_value());
    EDIT_CHECK(backward.value().applied_delta == -107);
    EDIT_CHECK(update_for(backward.value(), doc::NodeId{1U})
                   ->timing == (scene::ClipTiming{10, 10, 18, 3, 4}));

    const std::vector<scene::ClipPlacement> no_handles{
        clip(1U, 0, 10, 0, 10)};
    const auto full =
        scene::plan_slip(no_handles, doc::NodeId{1U}, 1, show_60);
    EDIT_CHECK(full.has_value());
    EDIT_CHECK(full.value().applied_delta == 1);
    EDIT_CHECK(update_for(full.value(), doc::NodeId{1U})->timing.source_in == 1);

    // A five-minute placement of a 232-second source still loops and slips.
    const std::vector<scene::ClipPlacement> overlong{
        clip(1U, -30, 18'000, 0, 5'569, scene::MediaFrameRate{24'000U, 1'001U})};
    for (const core::i64 delta : {60, -60}) {
        const auto slipped = scene::plan_slip(overlong, doc::NodeId{1U}, delta, show_60);
        EDIT_CHECK(slipped && slipped.value().applied_delta == delta);
        EDIT_CHECK(update_for(slipped.value(), doc::NodeId{1U})->timing ==
            (scene::ClipTiming{-30, 18'000, delta > 0 ? 24 : 5'545, 0, 0}));
    }
}

void slip_maps_fractional_rates_and_remains_unambiguous_in_an_overlap() {
    const auto native = scene::MediaFrameRate{30'000U, 1'001U};
    const std::vector<scene::ClipPlacement> clips{
        clip(1U, 0, 10, 5, 100, native), clip(2U, 2, 4, 0, 100)};
    const auto plan =
        scene::plan_slip(clips, doc::NodeId{1U}, 3, show_60);
    EDIT_CHECK(plan.has_value());
    EDIT_CHECK(plan.value().applied_delta == 3);
    EDIT_CHECK(update_for(plan.value(), doc::NodeId{1U})
                   ->timing.source_in == 6);
}

void slide_rolls_two_touching_neighbours_and_keeps_outer_span() {
    std::vector<scene::ClipPlacement> clips{
        clip(1U, 0, 10, 0, 100), clip(2U, 10, 5, 20, 100),
        clip(3U, 15, 10, 10, 100)};
    const auto moved =
        scene::plan_slide(clips, doc::NodeId{2U}, 3, show_60);
    EDIT_CHECK(moved.has_value());
    EDIT_CHECK(moved.value().applied_delta == 3);
    EDIT_CHECK(moved.value().span_start == 0);
    EDIT_CHECK(moved.value().span_duration == 25);
    EDIT_CHECK(update_for(moved.value(), doc::NodeId{1U})->timing.duration ==
               13);
    EDIT_CHECK(update_for(moved.value(), doc::NodeId{2U})->timing.start == 13);
    const auto* right = update_for(moved.value(), doc::NodeId{3U});
    EDIT_CHECK(right->timing.start == 18);
    EDIT_CHECK(right->timing.duration == 7);
    EDIT_CHECK(right->timing.source_in == 13);

    clips[2] = clip(3U, 15, 10, 2, 100);
    const auto limited =
        scene::plan_slide(clips, doc::NodeId{2U}, -20, show_60);
    EDIT_CHECK(limited.has_value());
    EDIT_CHECK(limited.value().applied_delta == -2);
}

void slide_rejects_missing_gapped_overlapping_and_locked_neighbours() {
    std::vector<scene::ClipPlacement> clips{
        clip(1U, 0, 10), clip(2U, 10, 5), clip(3U, 15, 10)};
    const auto missing = scene::plan_slide(
        std::span<const scene::ClipPlacement>{clips.data(), 2U},
        doc::NodeId{2U}, 1, show_60);
    EDIT_CHECK(!missing.has_value());
    EDIT_CHECK(missing.error() ==
               scene::TimelineEditError::AmbiguousSelection);

    clips[1].timing.start = 11;
    clips[2].timing.start = 16;
    const auto gapped =
        scene::plan_slide(clips, doc::NodeId{2U}, 1, show_60);
    EDIT_CHECK(!gapped.has_value());
    EDIT_CHECK(gapped.error() == scene::TimelineEditError::GappedSelection);

    clips[1].timing.start = 10;
    clips[2].timing.start = 14;
    const auto overlap =
        scene::plan_slide(clips, doc::NodeId{2U}, 1, show_60);
    EDIT_CHECK(!overlap.has_value());
    EDIT_CHECK(overlap.error() == scene::TimelineEditError::OverlappingClips);

    clips[2].timing.start = 15;
    clips[0].locked = true;
    const auto locked =
        scene::plan_slide(clips, doc::NodeId{2U}, 1, show_60);
    EDIT_CHECK(!locked.has_value());
    EDIT_CHECK(locked.error() == scene::TimelineEditError::LockedClip);
}

void insert_shifts_only_the_target_suffix_and_rejects_collisions() {
    const scene::TimelineLaneRef lane{default_scene, default_lane};
    std::vector<scene::ClipPlacement> clips{
        clip(1U, 0, 5), clip(2U, 5, 5),
        clip(3U, 5, 5, 0, 100, std::nullopt, doc::NodeId{99U})};
    const auto plan = scene::plan_insert(clips, lane, 5, 3);
    EDIT_CHECK(plan.has_value());
    EDIT_CHECK(plan.value().applied_delta == 3);
    EDIT_CHECK(plan.value().span_start == 5);
    EDIT_CHECK(plan.value().span_duration == 3);
    EDIT_CHECK(plan.value().updates.size() == 1U);
    EDIT_CHECK(plan.value().updates.front().node == doc::NodeId{2U});
    EDIT_CHECK(plan.value().updates.front().timing.start == 8);

    clips[0].timing.duration = 6;
    const auto crossing = scene::plan_insert(clips, lane, 5, 3);
    EDIT_CHECK(!crossing.has_value());
    EDIT_CHECK(crossing.error() ==
               scene::TimelineEditError::AmbiguousSelection);

    clips[0].timing.duration = 5;
    clips[1].locked = true;
    const auto locked = scene::plan_insert(clips, lane, 5, 3);
    EDIT_CHECK(!locked.has_value());
    EDIT_CHECK(locked.error() == scene::TimelineEditError::LockedClip);
}

void insert_detects_integer_overflow_without_partial_updates() {
    const core::i64 near_end = std::numeric_limits<core::i64>::max() - 3;
    const std::vector<scene::ClipPlacement> clips{clip(1U, near_end, 2)};
    const auto plan = scene::plan_insert(
        clips, scene::TimelineLaneRef{default_scene, default_lane}, 0, 10);
    EDIT_CHECK(!plan.has_value());
    EDIT_CHECK(plan.error() == scene::TimelineEditError::IntegerOverflow);
}

void negative_placements_are_valid_but_unrepresentable_ends_are_refused() {
    const std::vector<scene::ClipPlacement> negative{clip(1U, -1, 5)};
    const auto slip =
        scene::plan_slip(negative, doc::NodeId{1U}, 1, show_60);
    EDIT_CHECK(slip.has_value());
    EDIT_CHECK(update_for(slip.value(), doc::NodeId{1U})->timing ==
        (scene::ClipTiming{-1, 5, 1, 0, 0}));

    const scene::TimelineLaneRef lane{default_scene, default_lane};
    const auto insert = scene::plan_insert({}, lane, -1, 5);
    EDIT_CHECK(insert && insert.value().span_start == -1);
    const auto overwrite = scene::plan_overwrite(
        {}, lane, -1, 5, show_60, doc::NodeId{100U});
    EDIT_CHECK(overwrite && overwrite.value().span_start == -1);
    const auto maximum = std::numeric_limits<core::i64>::max();
    const std::vector<scene::ClipPlacement> invalid{clip(1U, maximum, 1)};
    const auto invalid_slip = scene::plan_slip(invalid, doc::NodeId{1U}, 1, show_60);
    EDIT_CHECK(!invalid_slip && invalid_slip.error() == scene::TimelineEditError::IncompatibleClip);
    EDIT_CHECK(!scene::plan_insert({}, lane, maximum, 1));
    EDIT_CHECK(!scene::plan_overwrite({}, lane, maximum, 1, show_60, doc::NodeId{100U}));
}

void looping_phase_mapping_is_exact_at_signed_and_wide_boundaries() {
    const auto phase = [](core::i64 start, core::i64 delta,
                          scene::SourcePhaseRounding rounding) {
        return scene::advance_source_phase(start, delta, show_60,
            scene::MediaFrameRate{30U, 1U}, 7U, rounding);
    };
    EDIT_CHECK(phase(0, 1, scene::SourcePhaseRounding::Nearest).value() == 1);
    EDIT_CHECK(phase(0, -1, scene::SourcePhaseRounding::Nearest).value() == 6);
    EDIT_CHECK(phase(0, 1, scene::SourcePhaseRounding::Floor).value() == 0);
    EDIT_CHECK(phase(0, -1, scene::SourcePhaseRounding::Floor).value() == 6);
    EDIT_CHECK(phase(20, 3, scene::SourcePhaseRounding::Floor).value() == 0);
    constexpr auto maximum = std::numeric_limits<core::u32>::max();
    constexpr auto highest = std::numeric_limits<core::i64>::max();
    constexpr auto lowest = std::numeric_limits<core::i64>::min();
    // Independent modular expectations for (7 + delta*(2^32-1)^2) mod97.
    const auto large = [&](core::i64 delta) {
        return scene::advance_source_phase(7, delta,
            scene::MediaFrameRate{1U, maximum},
            scene::MediaFrameRate{maximum, 1U}, 97U);
    };
    EDIT_CHECK(large(highest).value() == 62);
    EDIT_CHECK(large(lowest).value() == 57);
    EDIT_CHECK(scene::advance_source_phase(0, lowest, show_60, std::nullopt, 97U).value() == 18);
    EDIT_CHECK(!scene::advance_source_phase(0, 1, show_60, std::nullopt, 0U));
    EDIT_CHECK(!scene::advance_source_phase(-1, 1, show_60, std::nullopt, 97U));
    EDIT_CHECK(!scene::advance_source_phase(0, 1, {0U, 1U}, std::nullopt, 97U));

    const std::vector<scene::ClipPlacement> clips{
        clip(1U, -5, 10, 7, 97U, scene::MediaFrameRate{maximum, 1U})};
    const auto slipped = scene::plan_slip(clips, doc::NodeId{1U}, lowest,
        scene::MediaFrameRate{1U, maximum});
    EDIT_CHECK(slipped && slipped.value().applied_delta == lowest);
    EDIT_CHECK(update_for(slipped.value(), doc::NodeId{1U})->timing.source_in == 57);
}

void all_edit_planners_accept_negative_placement_starts() {
    const std::vector<scene::ClipPlacement> clips{
        clip(1U, -20, 10, 20), clip(2U, -10, 10, 20)};
    const auto tail = scene::plan_ripple_trim(clips, doc::NodeId{1U},
        scene::TimelineTrimEdge::Tail, 2, show_60);
    EDIT_CHECK(tail && tail.value().applied_delta == 2);
    EDIT_CHECK(update_for(tail.value(), doc::NodeId{2U})->timing.start == -8);
    const auto head = scene::plan_ripple_trim(clips, doc::NodeId{1U},
        scene::TimelineTrimEdge::Head, 2, show_60);
    EDIT_CHECK(head && head.value().applied_delta == 2);
    EDIT_CHECK(update_for(head.value(), doc::NodeId{2U})->timing.start == -12);
    const std::array selected{doc::NodeId{1U}};
    const auto erased = scene::plan_ripple_delete(clips, selected);
    EDIT_CHECK(erased && erased.value().applied_delta == -10);
    EDIT_CHECK(update_for(erased.value(), doc::NodeId{2U})->timing.start == -20);
    const auto rolled = scene::plan_roll(clips, doc::NodeId{1U}, doc::NodeId{2U}, 2, show_60);
    EDIT_CHECK(rolled && rolled.value().span_duration == 20);
    EDIT_CHECK(update_for(rolled.value(), doc::NodeId{2U})->timing ==
        (scene::ClipTiming{-8, 8, 22, 0, 0}));
    const std::vector<scene::ClipPlacement> neighbours{
        clip(1U, -20, 10, 20), clip(2U, -10, 5, 20), clip(3U, -5, 10, 20)};
    const auto slid = scene::plan_slide(neighbours, doc::NodeId{2U}, 2, show_60);
    EDIT_CHECK(slid && slid.value().span_duration == 25);
    EDIT_CHECK(update_for(slid.value(), doc::NodeId{2U})->timing.start == -8);
    const scene::TimelineLaneRef lane{default_scene, default_lane};
    const auto inserted = scene::plan_insert(clips, lane, -20, 3);
    EDIT_CHECK(inserted && update_for(inserted.value(), doc::NodeId{1U})->timing.start == -17);
    const auto overwritten = scene::plan_overwrite(clips, lane, -17, 3,
        show_60, doc::NodeId{100U});
    EDIT_CHECK(overwritten);
    EDIT_CHECK(update_for(overwritten.value(), doc::NodeId{1U})->timing.duration == 3);
    EDIT_CHECK(create_for(overwritten.value(), doc::NodeId{100U})->timing ==
        (scene::ClipTiming{-14, 4, 26, 0, 0}));
}

void signed_placement_union_and_shift_overflow_are_refused() {
    constexpr auto highest = std::numeric_limits<core::i64>::max();
    constexpr auto lowest = std::numeric_limits<core::i64>::min();
    constexpr auto rate_max = std::numeric_limits<core::u32>::max();
    const scene::MediaFrameRate show{rate_max, 1U};
    const scene::MediaFrameRate slow{1U, rate_max};
    const std::vector<scene::ClipPlacement> pair{
        clip(1U, lowest, highest, 0, 100, slow), clip(2U, -1, highest, 0, 100, slow)};
    const auto rolled = scene::plan_roll(pair, doc::NodeId{1U}, doc::NodeId{2U}, 1, show);
    EDIT_CHECK(!rolled && rolled.error() == scene::TimelineEditError::IntegerOverflow);
    const std::array both{doc::NodeId{1U}, doc::NodeId{2U}};
    const auto erased = scene::plan_ripple_delete(pair, both);
    EDIT_CHECK(!erased && erased.error() == scene::TimelineEditError::IntegerOverflow);
    const std::vector<scene::ClipPlacement> triple{
        clip(1U, lowest, highest - 1, 0, 100, slow),
        clip(2U, -2, 1, 0, 100, slow), clip(3U, -1, highest, 0, 100, slow)};
    const auto slid = scene::plan_slide(triple, doc::NodeId{2U}, 1, show);
    EDIT_CHECK(!slid && slid.error() == scene::TimelineEditError::IntegerOverflow);
    // The shifted start fits; its exclusive end does not.
    const std::vector<scene::ClipPlacement> late{clip(1U, highest - 5, 4)};
    const auto inserted = scene::plan_insert(late,
        {default_scene, default_lane}, -1, 2);
    EDIT_CHECK(!inserted && inserted.error() == scene::TimelineEditError::IntegerOverflow);
}

void overwrite_preserves_left_and_right_remainders_and_later_clips() {
    const scene::TimelineLaneRef lane{default_scene, default_lane};
    const std::vector<scene::ClipPlacement> clips{
        clip(1U, 0, 4), clip(2U, 3, 4, 0, 100),
        clip(3U, 6, 2, 0, 100), clip(4U, 8, 5, 10, 100),
        clip(5U, 2, 12, 20, 100), clip(6U, 15, 5, 0, 100)};
    const auto result = scene::plan_overwrite(
        clips, lane, 5, 5, show_60, doc::NodeId{5U});
    EDIT_CHECK(result.has_value());
    const auto& plan = result.value();
    EDIT_CHECK(plan.applied_delta == 0);
    EDIT_CHECK(plan.span_start == 5);
    EDIT_CHECK(plan.span_duration == 5);
    EDIT_CHECK(plan.updates.size() == 3U);
    EDIT_CHECK(update_for(plan, doc::NodeId{2U})->timing ==
               (scene::ClipTiming{3, 2, 0, 0, 0}));
    EDIT_CHECK(update_for(plan, doc::NodeId{4U})->timing ==
               (scene::ClipTiming{10, 3, 12, 0, 0}));
    EDIT_CHECK(update_for(plan, doc::NodeId{5U})->timing ==
               (scene::ClipTiming{2, 3, 20, 0, 0}));
    EDIT_CHECK((plan.erases ==
                std::vector<doc::NodeId>{doc::NodeId{3U}}));
    EDIT_CHECK(plan.creates.size() == 1U);
    // 5 and 6 are occupied, so deterministic ascending allocation chooses 7.
    const auto* right = create_for(plan, doc::NodeId{7U});
    EDIT_CHECK(right != nullptr);
    EDIT_CHECK(right->timing == (scene::ClipTiming{10, 4, 28, 0, 0}));
    EDIT_CHECK(update_for(plan, doc::NodeId{1U}) == nullptr);
    EDIT_CHECK(update_for(plan, doc::NodeId{6U}) == nullptr);
}

void overwrite_uses_native_rate_and_rejects_locked_or_exhausted_splits() {
    const scene::TimelineLaneRef lane{default_scene, default_lane};
    const auto native = scene::MediaFrameRate{30'000U, 1'001U};
    std::vector<scene::ClipPlacement> clips{
        clip(1U, 0, 20, 0, 100, native)};
    const auto mapped = scene::plan_overwrite(
        clips, lane, 2, 4, show_60, doc::NodeId{50U});
    EDIT_CHECK(mapped.has_value());
    EDIT_CHECK(mapped.value().creates.size() == 1U);
    EDIT_CHECK(mapped.value().creates.front().timing ==
               (scene::ClipTiming{6, 14, 3, 0, 0}));

    clips[0].locked = true;
    const auto locked = scene::plan_overwrite(
        clips, lane, 2, 4, show_60, doc::NodeId{50U});
    EDIT_CHECK(!locked.has_value());
    EDIT_CHECK(locked.error() == scene::TimelineEditError::LockedClip);

    constexpr core::u64 max_id =
        static_cast<core::u64>(std::numeric_limits<core::i64>::max());
    clips[0] = clip(max_id, 0, 20, 0, 100);
    const auto exhausted = scene::plan_overwrite(
        clips, lane, 2, 4, show_60, doc::NodeId{max_id});
    EDIT_CHECK(!exhausted.has_value());
    EDIT_CHECK(exhausted.error() == scene::TimelineEditError::IdExhausted);

    const auto invalid_id = scene::plan_overwrite(
        clips, lane, 30, 2, show_60,
        doc::NodeId{std::numeric_limits<core::u64>::max()});
    EDIT_CHECK(!invalid_id.has_value());
    EDIT_CHECK(invalid_id.error() ==
               scene::TimelineEditError::InvalidFirstId);
}

void every_plan_is_independent_of_input_and_selection_order() {
    std::vector<scene::ClipPlacement> clips{
        clip(1U, 0, 5), clip(2U, 5, 5), clip(3U, 10, 5),
        clip(4U, 15, 5)};
    auto reversed = clips;
    std::reverse(reversed.begin(), reversed.end());

    const auto trim_a = scene::plan_ripple_trim(
        clips, doc::NodeId{2U}, scene::TimelineTrimEdge::Tail, 2, show_60);
    const auto trim_b = scene::plan_ripple_trim(
        reversed, doc::NodeId{2U}, scene::TimelineTrimEdge::Tail, 2,
        show_60);
    EDIT_CHECK(trim_a.has_value() && trim_b.has_value());
    EDIT_CHECK(trim_a.value() == trim_b.value());

    const std::array selection_a{doc::NodeId{1U}, doc::NodeId{2U}};
    const std::array selection_b{doc::NodeId{2U}, doc::NodeId{1U}};
    const auto delete_a = scene::plan_ripple_delete(clips, selection_a);
    const auto delete_b = scene::plan_ripple_delete(reversed, selection_b);
    EDIT_CHECK(delete_a.has_value() && delete_b.has_value());
    EDIT_CHECK(delete_a.value() == delete_b.value());

    const std::vector<scene::ClipPlacement> overwrite_clips{
        clip(8U, 0, 20, 4, 100), clip(2U, 30, 5)};
    auto overwrite_reversed = overwrite_clips;
    std::reverse(overwrite_reversed.begin(), overwrite_reversed.end());
    const scene::TimelineLaneRef lane{default_scene, default_lane};
    const auto overwrite_a = scene::plan_overwrite(
        overwrite_clips, lane, 5, 5, show_60, doc::NodeId{20U});
    const auto overwrite_b = scene::plan_overwrite(
        overwrite_reversed, lane, 5, 5, show_60, doc::NodeId{20U});
    EDIT_CHECK(overwrite_a.has_value() && overwrite_b.has_value());
    EDIT_CHECK(overwrite_a.value() == overwrite_b.value());
}

constexpr doc::NodeId synced_lane{21U};
constexpr doc::NodeId free_lane{22U};

// On the edited lane A is 0..100 and B 100..150. On the synced lane S1
// (50..110) straddles A's tail, S2 is 130..140 and S3 a locked 200..220; an
// unsynced lane holds U at 130..140.
[[nodiscard]] std::vector<scene::ClipPlacement> synced_fixture() {
    return {clip(1U, 0, 100), clip(2U, 100, 50),
            clip(11U, 50, 60, 0, 1'000U, std::nullopt, synced_lane),
            clip(12U, 130, 10, 0, 1'000U, std::nullopt, synced_lane),
            clip(13U, 200, 20, 0, 1'000U, std::nullopt, synced_lane, true),
            clip(21U, 130, 10, 0, 1'000U, std::nullopt, free_lane)};
}

void a_ripple_trim_moves_sync_locked_lanes_and_clamps_where_they_collide() {
    const std::vector<scene::ClipPlacement> clips = synced_fixture();
    const std::array<doc::NodeId, 1U> synced{synced_lane};

    // Ten frames longer: B and S2 move on, the straddler, the locked clip and
    // the unsynced lane stay.
    const auto grown = scene::plan_ripple_trim(
        clips, doc::NodeId{1U}, scene::TimelineTrimEdge::Tail, 10, show_60,
        synced);
    EDIT_CHECK(grown.has_value());
    EDIT_CHECK(grown.value().applied_delta == 10);
    EDIT_CHECK(update_for(grown.value(), doc::NodeId{2U})->timing.start == 110);
    EDIT_CHECK(update_for(grown.value(), doc::NodeId{12U})->timing.start ==
               140);
    EDIT_CHECK(update_for(grown.value(), doc::NodeId{11U}) == nullptr);
    EDIT_CHECK(update_for(grown.value(), doc::NodeId{13U}) == nullptr);
    EDIT_CHECK(update_for(grown.value(), doc::NodeId{21U}) == nullptr);
    EDIT_CHECK(std::is_sorted(
        grown.value().updates.begin(), grown.value().updates.end(),
        [](const scene::ClipPlacement& left,
           const scene::ClipPlacement& right) {
            return left.node < right.node;
        }));

    // Without the lane synced, nothing on it moves.
    const auto alone = scene::plan_ripple_trim(
        clips, doc::NodeId{1U}, scene::TimelineTrimEdge::Tail, 10, show_60);
    EDIT_CHECK(alone.has_value());
    EDIT_CHECK(update_for(alone.value(), doc::NodeId{12U}) == nullptr);

    // Thirty shorter would put S2 inside the straddler; it stops at twenty,
    // where S2 butts S1's end.
    const auto shrunk = scene::plan_ripple_trim(
        clips, doc::NodeId{1U}, scene::TimelineTrimEdge::Tail, -30, show_60,
        synced);
    EDIT_CHECK(shrunk.has_value());
    EDIT_CHECK(shrunk.value().applied_delta == -20);
    EDIT_CHECK(update_for(shrunk.value(), doc::NodeId{12U})->timing.start ==
               110);
    EDIT_CHECK(update_for(shrunk.value(), doc::NodeId{2U})->timing.start == 80);
}

void a_synced_lane_closes_up_or_refuses_a_ripple_delete_and_insert() {
    const std::array<doc::NodeId, 1U> synced{synced_lane};
    // Deleting B (100..150) closes the synced lane up by fifty as well.
    std::vector<scene::ClipPlacement> clips{
        clip(1U, 0, 100), clip(2U, 100, 50), clip(3U, 150, 20),
        clip(12U, 160, 20, 0, 1'000U, std::nullopt, synced_lane)};
    const std::array<doc::NodeId, 1U> doomed{doc::NodeId{2U}};
    const auto closed = scene::plan_ripple_delete(clips, doomed, synced);
    EDIT_CHECK(closed.has_value());
    EDIT_CHECK(update_for(closed.value(), doc::NodeId{3U})->timing.start == 100);
    EDIT_CHECK(update_for(closed.value(), doc::NodeId{12U})->timing.start ==
               110);

    // Something on the synced lane inside the deleted span would be covered:
    // refused rather than overlapped.
    clips.push_back(clip(11U, 120, 10, 0, 1'000U, std::nullopt, synced_lane));
    const auto blocked = scene::plan_ripple_delete(clips, doomed, synced);
    EDIT_CHECK(!blocked.has_value());
    EDIT_CHECK(blocked.error() == scene::TimelineEditError::OverlappingClips);

    // An insert opens the same gap on the synced lane: the straddler stays,
    // S2 moves thirty on.
    const scene::TimelineLaneRef lane{default_scene, default_lane};
    std::vector<scene::ClipPlacement> pasted = synced_fixture();
    const auto opened = scene::plan_insert(pasted, lane, 100, 30, synced);
    EDIT_CHECK(opened.has_value());
    EDIT_CHECK(update_for(opened.value(), doc::NodeId{2U})->timing.start == 130);
    EDIT_CHECK(update_for(opened.value(), doc::NodeId{12U})->timing.start ==
               160);
    EDIT_CHECK(update_for(opened.value(), doc::NodeId{11U}) == nullptr);
    // A locked clip where S2 would land refuses it.
    pasted[4].timing.start = 165;
    const auto walled = scene::plan_insert(pasted, lane, 100, 30, synced);
    EDIT_CHECK(!walled.has_value());
    EDIT_CHECK(walled.error() == scene::TimelineEditError::OverlappingClips);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 23U> cases{{
    {"a ripple trim moves sync-locked lanes and clamps where they collide",
     &a_ripple_trim_moves_sync_locked_lanes_and_clamps_where_they_collide},
    {"a synced lane closes up or refuses a ripple delete and insert",
     &a_synced_lane_closes_up_or_refuses_a_ripple_delete_and_insert},
    {"looping phase mapping is exact at signed and wide boundaries",
     &looping_phase_mapping_is_exact_at_signed_and_wide_boundaries},
    {"all edit planners accept negative placement starts",
     &all_edit_planners_accept_negative_placement_starts},
    {"signed placement union and shift overflow are refused",
     &signed_placement_union_and_shift_overflow_are_refused},
    {"exact rate mapping is signed rounded and overflow safe",
     &exact_rate_mapping_is_signed_rounded_and_overflow_safe},
    {"ripple tail moves the whole suffix and clamps handles",
     &ripple_tail_moves_the_whole_suffix_and_clamps_handles},
    {"ripple head uses native rate and clamps at source zero",
     &ripple_head_uses_native_rate_and_clamps_at_source_zero},
    {"ripple gestures clamp before timeline overflow",
     &ripple_gestures_clamp_before_timeline_overflow},
    {"ripple delete requires one unambiguous contiguous union",
     &ripple_delete_requires_one_unambiguous_contiguous_union},
    {"ripple delete rejects locked and multi lane selections",
     &ripple_delete_rejects_locked_and_multi_lane_selections},
    {"roll moves only one touching cut and clamps both assets",
     &roll_moves_only_one_touching_cut_and_clamps_both_assets},
    {"roll uses native rate and rejects gaps overlaps and locks",
     &roll_uses_native_rate_and_rejects_gaps_overlaps_and_locks},
    {"slip keeps placement fixed and wraps source phase",
     &slip_keeps_placement_fixed_and_wraps_source_phase},
    {"slip maps fractional rates and remains unambiguous in an overlap",
     &slip_maps_fractional_rates_and_remains_unambiguous_in_an_overlap},
    {"slide rolls two touching neighbours and keeps outer span",
     &slide_rolls_two_touching_neighbours_and_keeps_outer_span},
    {"slide rejects missing gapped overlapping and locked neighbours",
     &slide_rejects_missing_gapped_overlapping_and_locked_neighbours},
    {"insert shifts only the target suffix and rejects collisions",
     &insert_shifts_only_the_target_suffix_and_rejects_collisions},
    {"insert detects integer overflow without partial updates",
     &insert_detects_integer_overflow_without_partial_updates},
    {"negative placements are valid but unrepresentable ends are refused",
     &negative_placements_are_valid_but_unrepresentable_ends_are_refused},
    {"overwrite preserves left and right remainders and later clips",
     &overwrite_preserves_left_and_right_remainders_and_later_clips},
    {"overwrite uses native rate and rejects locked or exhausted splits",
     &overwrite_uses_native_rate_and_rejects_locked_or_exhausted_splits},
    {"every plan is independent of input and selection order",
     &every_plan_is_independent_of_input_and_selection_order},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            std::cout << "[run ] " << entry.name << std::endl;
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
