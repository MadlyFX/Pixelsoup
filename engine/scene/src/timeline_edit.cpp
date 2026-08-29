#include <mediaserver/scene/timeline_edit.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <utility>

namespace mediaserver::scene {
namespace {

using EditResult = core::Result<TimelineEditPlan, TimelineEditError>;

struct WideUnsigned final {
    core::u64 high{};
    core::u64 low{};
};

struct WideDivision final {
    core::u64 quotient{};
    core::u64 remainder{};
    bool quotient_overflow{};
};

[[nodiscard]] bool valid_rate(MediaFrameRate rate) noexcept {
    return rate.numerator != 0U && rate.denominator != 0U;
}

[[nodiscard]] core::u64 magnitude(core::i64 value) noexcept {
    if (value >= 0) {
        return static_cast<core::u64>(value);
    }
    // Written this way so INT64_MIN never has to be negated as a signed value.
    return static_cast<core::u64>(-(value + 1)) + 1U;
}

[[nodiscard]] bool signed_magnitude(core::u64 value, bool negative,
                                    core::i64& out) noexcept {
    constexpr core::u64 positive_limit =
        static_cast<core::u64>(std::numeric_limits<core::i64>::max());
    constexpr core::u64 negative_limit = positive_limit + 1U;
    if (!negative) {
        if (value > positive_limit) {
            return false;
        }
        out = static_cast<core::i64>(value);
        return true;
    }
    if (value > negative_limit) {
        return false;
    }
    if (value == negative_limit) {
        out = std::numeric_limits<core::i64>::min();
        return true;
    }
    out = -static_cast<core::i64>(value);
    return true;
}

// Exact 64 x 64 -> 128 multiplication using 32-bit limbs. This is portable to
// MSVC, where an integer __int128 is not part of the C++ implementation.
[[nodiscard]] WideUnsigned multiply_wide(core::u64 left,
                                         core::u64 right) noexcept {
    constexpr core::u64 mask = 0xffff'ffffULL;
    const core::u64 left_low = left & mask;
    const core::u64 left_high = left >> 32U;
    const core::u64 right_low = right & mask;
    const core::u64 right_high = right >> 32U;

    const core::u64 low_product = left_low * right_low;
    const core::u64 cross_left = left_low * right_high;
    const core::u64 cross_right = left_high * right_low;
    const core::u64 high_product = left_high * right_high;
    const core::u64 middle = (low_product >> 32U) + (cross_left & mask) +
                             (cross_right & mask);
    return WideUnsigned{
        high_product + (cross_left >> 32U) + (cross_right >> 32U) +
            (middle >> 32U),
        (middle << 32U) | (low_product & mask)};
}

// Binary long division. The partial remainder is less than denominator before
// each step, so one subtraction is sufficient. `carry` represents the 65th
// bit when doubling a remainder close to UINT64_MAX.
[[nodiscard]] WideDivision divide_wide(WideUnsigned numerator,
                                       core::u64 denominator) noexcept {
    WideDivision result{};
    core::u64 remainder{};
    for (int bit_index = 127; bit_index >= 0; --bit_index) {
        const core::u64 input_bit = bit_index >= 64
            ? (numerator.high >> (bit_index - 64)) & 1U
            : (numerator.low >> bit_index) & 1U;
        const bool carry = (remainder >> 63U) != 0U;
        remainder = (remainder << 1U) | input_bit;
        const bool subtract = carry || remainder >= denominator;
        if (subtract) {
            remainder -= denominator;
            if (bit_index >= 64) {
                result.quotient_overflow = true;
            } else {
                result.quotient |= core::u64{1U} << bit_index;
            }
        }
    }
    result.remainder = remainder;
    return result;
}

[[nodiscard]] bool checked_add(core::i64 left, core::i64 right,
                               core::i64& out) noexcept {
    if ((right > 0 &&
         left > std::numeric_limits<core::i64>::max() - right) ||
        (right < 0 &&
         left < std::numeric_limits<core::i64>::min() - right)) {
        return false;
    }
    out = left + right;
    return true;
}

[[nodiscard]] bool checked_subtract(core::i64 left, core::i64 right,
                                    core::i64& out) noexcept {
    if ((right > 0 &&
         left < std::numeric_limits<core::i64>::min() + right) ||
        (right < 0 &&
         left > std::numeric_limits<core::i64>::max() + right)) {
        return false;
    }
    out = left - right;
    return true;
}

[[nodiscard]] bool clip_end(const ClipPlacement& clip,
                            core::i64& out) noexcept {
    return clip.timing.duration > 0 && clip.timing.start >= 0 &&
           checked_add(clip.timing.start, clip.timing.duration, out);
}

[[nodiscard]] bool timeline_valid(const ClipPlacement& clip) noexcept {
    core::i64 end{};
    return clip_end(clip, end) && end > clip.timing.start;
}

[[nodiscard]] TimelineEditError source_valid(
    const ClipPlacement& clip, MediaFrameRate show_rate) noexcept {
    if (!timeline_valid(clip) || clip.sequence_count == 0U ||
        clip.timing.source_in < 0 ||
        static_cast<core::u64>(clip.timing.source_in) >=
            static_cast<core::u64>(clip.sequence_count) ||
        (clip.frame_rate.has_value() && !valid_rate(*clip.frame_rate))) {
        return TimelineEditError::IncompatibleClip;
    }
    const SourceOrdinalResult last = source_ordinal_delta(
        clip.timing.duration - 1, show_rate, clip.frame_rate);
    if (!last.has_value()) {
        return last.error() == TimelineEditError::InvalidRate
            ? TimelineEditError::IncompatibleClip
            : last.error();
    }
    core::i64 last_ordinal{};
    if (last.value() < 0 ||
        !checked_add(clip.timing.source_in, last.value(), last_ordinal)) {
        return TimelineEditError::IntegerOverflow;
    }
    return last_ordinal < static_cast<core::i64>(clip.sequence_count)
        ? static_cast<TimelineEditError>(0xffU)
        : TimelineEditError::IncompatibleClip;
}

[[nodiscard]] bool source_is_valid(const ClipPlacement& clip,
                                   MediaFrameRate show_rate) noexcept {
    return static_cast<core::u8>(source_valid(clip, show_rate)) == 0xffU;
}

void clamp_fades(ClipTiming& timing) noexcept {
    if (timing.duration <= 0) {
        timing.fade_in = 0;
        timing.fade_out = 0;
        return;
    }
    timing.fade_in = std::clamp(timing.fade_in, core::i64{0}, timing.duration);
    timing.fade_out =
        std::clamp(timing.fade_out, core::i64{0}, timing.duration);
}

class ClipIndex final {
public:
    explicit ClipIndex(std::span<const ClipPlacement> clips) {
        for (const ClipPlacement& clip : clips) {
            if (!by_id_.emplace(clip.node, &clip).second) {
                duplicate_ = true;
            }
        }
    }

    [[nodiscard]] bool duplicate() const noexcept { return duplicate_; }

    [[nodiscard]] const ClipPlacement* find(doc::NodeId node) const noexcept {
        const auto found = by_id_.find(node);
        return found == by_id_.end() ? nullptr : found->second;
    }

private:
    std::map<doc::NodeId, const ClipPlacement*> by_id_;
    bool duplicate_{};
};

[[nodiscard]] bool same_lane(const ClipPlacement& clip,
                             TimelineLaneRef lane) noexcept {
    return clip.scene == lane.scene && clip.lane == lane.lane;
}

[[nodiscard]] std::vector<const ClipPlacement*> lane_clips(
    std::span<const ClipPlacement> clips, TimelineLaneRef lane) {
    std::vector<const ClipPlacement*> result;
    for (const ClipPlacement& clip : clips) {
        if (same_lane(clip, lane)) {
            result.push_back(&clip);
        }
    }
    std::stable_sort(
        result.begin(), result.end(),
        [](const ClipPlacement* left,
           const ClipPlacement* right) noexcept {
            if (left->timing.start != right->timing.start) {
                return left->timing.start < right->timing.start;
            }
            if (left->order != right->order) {
                return left->order < right->order;
            }
            return left->node < right->node;
        });
    return result;
}

[[nodiscard]] TimelineEditError strict_lane_error(
    const std::vector<const ClipPlacement*>& clips) noexcept {
    core::i64 previous_end{};
    bool first = true;
    for (const ClipPlacement* clip : clips) {
        core::i64 end{};
        if (!clip_end(*clip, end)) {
            return TimelineEditError::IncompatibleClip;
        }
        if (!first && clip->timing.start < previous_end) {
            return TimelineEditError::OverlappingClips;
        }
        previous_end = end;
        first = false;
    }
    return static_cast<TimelineEditError>(0xffU);
}

[[nodiscard]] bool strict_candidate_valid(
    const std::vector<ClipPlacement>& clips) noexcept {
    core::i64 previous_end{};
    bool first = true;
    for (const ClipPlacement& clip : clips) {
        core::i64 end{};
        if (!clip_end(clip, end)) {
            return false;
        }
        if (!first && clip.timing.start < previous_end) {
            return false;
        }
        previous_end = end;
        first = false;
    }
    return true;
}

[[nodiscard]] bool sentinel_error(TimelineEditError error) noexcept {
    return static_cast<core::u8>(error) != 0xffU;
}

template <typename Feasible>
[[nodiscard]] core::i64 clamp_delta(core::i64 requested,
                                    Feasible&& feasible) {
    if (requested == 0) {
        return 0;
    }
    const bool negative = requested < 0;
    core::u64 low{};
    core::u64 high = magnitude(requested);
    while (low < high) {
        const core::u64 middle = low + (high - low) / 2U + 1U;
        core::i64 candidate{};
        if (!signed_magnitude(middle, negative, candidate) ||
            !feasible(candidate)) {
            high = middle - 1U;
        } else {
            low = middle;
        }
    }
    core::i64 result{};
    const bool representable = signed_magnitude(low, negative, result);
    static_cast<void>(representable);
    return result;
}

void append_changed_updates(
    const std::vector<const ClipPlacement*>& originals,
    const std::vector<ClipPlacement>& candidates,
    TimelineEditPlan& plan) {
    for (std::size_t index = 0U; index < originals.size(); ++index) {
        if (*originals[index] != candidates[index]) {
            plan.updates.push_back(candidates[index]);
        }
    }
    std::sort(plan.updates.begin(), plan.updates.end(),
              [](const ClipPlacement& left,
                 const ClipPlacement& right) noexcept {
                  return left.node < right.node;
              });
}

[[nodiscard]] std::vector<ClipPlacement> copy_lane(
    const std::vector<const ClipPlacement*>& clips) {
    std::vector<ClipPlacement> result;
    result.reserve(clips.size());
    for (const ClipPlacement* clip : clips) {
        result.push_back(*clip);
    }
    return result;
}

[[nodiscard]] EditResult allocation_failure() {
    return EditResult::failure(TimelineEditError::AllocationFailed);
}

} // namespace

SourceOrdinalResult source_ordinal_delta(
    core::i64 show_delta, MediaFrameRate show_rate,
    const std::optional<MediaFrameRate>& source_rate) noexcept {
    using Result = SourceOrdinalResult;
    if (!valid_rate(show_rate) ||
        (source_rate.has_value() && !valid_rate(*source_rate))) {
        return Result::failure(TimelineEditError::InvalidRate);
    }
    if (!source_rate.has_value() || show_delta == 0) {
        return Result::success(show_delta);
    }

    const core::u64 numerator_factor =
        static_cast<core::u64>(source_rate->numerator) *
        static_cast<core::u64>(show_rate.denominator);
    const core::u64 denominator =
        static_cast<core::u64>(source_rate->denominator) *
        static_cast<core::u64>(show_rate.numerator);
    const WideUnsigned numerator =
        multiply_wide(magnitude(show_delta), numerator_factor);
    WideDivision division = divide_wide(numerator, denominator);
    if (division.quotient_overflow) {
        return Result::failure(TimelineEditError::IntegerOverflow);
    }

    // r >= denominator / 2, with equality included only for an exact half.
    const bool round_up =
        division.remainder > denominator / 2U ||
        ((denominator & 1U) == 0U &&
         division.remainder == denominator / 2U);
    if (round_up) {
        if (division.quotient == std::numeric_limits<core::u64>::max()) {
            return Result::failure(TimelineEditError::IntegerOverflow);
        }
        ++division.quotient;
    }

    core::i64 signed_result{};
    if (!signed_magnitude(division.quotient, show_delta < 0,
                          signed_result)) {
        return Result::failure(TimelineEditError::IntegerOverflow);
    }
    return Result::success(signed_result);
}

TimelineEditResult plan_ripple_trim(
    std::span<const ClipPlacement> clips, doc::NodeId selected,
    TimelineTrimEdge edge, core::i64 requested_delta,
    MediaFrameRate show_rate) {
    try {
        if (!valid_rate(show_rate)) {
            return EditResult::failure(TimelineEditError::InvalidRate);
        }
        const ClipIndex index{clips};
        if (index.duplicate()) {
            return EditResult::failure(
                TimelineEditError::AmbiguousSelection);
        }
        const ClipPlacement* const chosen = index.find(selected);
        if (chosen == nullptr) {
            return EditResult::failure(TimelineEditError::ClipNotFound);
        }
        if (chosen->locked) {
            return EditResult::failure(TimelineEditError::LockedClip);
        }
        const TimelineEditError chosen_error = source_valid(*chosen, show_rate);
        if (sentinel_error(chosen_error)) {
            return EditResult::failure(chosen_error);
        }
        const TimelineLaneRef lane{chosen->scene, chosen->lane};
        const auto scoped = lane_clips(clips, lane);
        const TimelineEditError layout_error = strict_lane_error(scoped);
        if (sentinel_error(layout_error)) {
            return EditResult::failure(layout_error);
        }
        core::i64 old_tail{};
        if (!clip_end(*chosen, old_tail)) {
            return EditResult::failure(
                TimelineEditError::IncompatibleClip);
        }

        const auto build = [&](core::i64 delta,
                               std::vector<ClipPlacement>& candidate) {
            candidate = copy_lane(scoped);
            for (ClipPlacement& clip : candidate) {
                if (clip.node == selected) {
                    core::i64 duration{};
                    if (edge == TimelineTrimEdge::Tail) {
                        if (!checked_add(clip.timing.duration, delta,
                                         duration)) {
                            return false;
                        }
                    } else {
                        if (!checked_subtract(clip.timing.duration, delta,
                                              duration)) {
                            return false;
                        }
                        const SourceOrdinalResult source_delta =
                            source_ordinal_delta(delta, show_rate,
                                                 clip.frame_rate);
                        if (!source_delta.has_value() ||
                            !checked_add(clip.timing.source_in,
                                         source_delta.value(),
                                         clip.timing.source_in)) {
                            return false;
                        }
                    }
                    clip.timing.duration = duration;
                    clamp_fades(clip.timing);
                } else if (!clip.locked && clip.timing.start >= old_tail) {
                    const bool shifted = edge == TimelineTrimEdge::Tail
                        ? checked_add(clip.timing.start, delta,
                                      clip.timing.start)
                        : checked_subtract(clip.timing.start, delta,
                                           clip.timing.start);
                    if (!shifted) {
                        return false;
                    }
                }
            }
            const auto edited = std::find_if(
                candidate.begin(), candidate.end(),
                [selected](const ClipPlacement& clip) noexcept {
                    return clip.node == selected;
                });
            return strict_candidate_valid(candidate) &&
                   edited != candidate.end() &&
                   source_is_valid(*edited, show_rate);
        };

        std::vector<ClipPlacement> scratch;
        const core::i64 applied = clamp_delta(
            requested_delta,
            [&](core::i64 delta) { return build(delta, scratch); });
        std::vector<ClipPlacement> candidate;
        const bool built = build(applied, candidate);
        static_cast<void>(built);

        TimelineEditPlan plan{};
        plan.applied_delta = applied;
        plan.span_start = chosen->timing.start;
        plan.span_duration = chosen->timing.duration;
        append_changed_updates(scoped, candidate, plan);
        return EditResult::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    }
}

TimelineEditResult plan_ripple_delete(
    std::span<const ClipPlacement> clips,
    std::span<const doc::NodeId> selected) {
    try {
        if (selected.empty()) {
            return EditResult::failure(
                TimelineEditError::AmbiguousSelection);
        }
        const ClipIndex index{clips};
        if (index.duplicate()) {
            return EditResult::failure(
                TimelineEditError::AmbiguousSelection);
        }
        std::set<doc::NodeId> selected_ids;
        std::vector<const ClipPlacement*> chosen;
        chosen.reserve(selected.size());
        for (doc::NodeId node : selected) {
            if (!selected_ids.insert(node).second) {
                return EditResult::failure(
                    TimelineEditError::AmbiguousSelection);
            }
            const ClipPlacement* const clip = index.find(node);
            if (clip == nullptr) {
                return EditResult::failure(
                    TimelineEditError::ClipNotFound);
            }
            if (clip->locked) {
                return EditResult::failure(TimelineEditError::LockedClip);
            }
            if (!timeline_valid(*clip)) {
                return EditResult::failure(
                    TimelineEditError::IncompatibleClip);
            }
            chosen.push_back(clip);
        }
        const TimelineLaneRef lane{chosen.front()->scene,
                                   chosen.front()->lane};
        if (std::any_of(chosen.begin(), chosen.end(),
                        [lane](const ClipPlacement* clip) noexcept {
                            return !same_lane(*clip, lane);
                        })) {
            return EditResult::failure(TimelineEditError::MultipleLanes);
        }
        std::stable_sort(
            chosen.begin(), chosen.end(),
            [](const ClipPlacement* left,
               const ClipPlacement* right) noexcept {
                if (left->timing.start != right->timing.start) {
                    return left->timing.start < right->timing.start;
                }
                return left->node < right->node;
            });

        core::i64 span_start = chosen.front()->timing.start;
        core::i64 span_end{};
        if (!clip_end(*chosen.front(), span_end)) {
            return EditResult::failure(
                TimelineEditError::IncompatibleClip);
        }
        for (std::size_t position = 1U; position < chosen.size(); ++position) {
            core::i64 end{};
            if (!clip_end(*chosen[position], end)) {
                return EditResult::failure(
                    TimelineEditError::IncompatibleClip);
            }
            if (chosen[position]->timing.start > span_end) {
                return EditResult::failure(
                    TimelineEditError::GappedSelection);
            }
            span_end = std::max(span_end, end);
        }
        core::i64 span_duration{};
        if (!checked_subtract(span_end, span_start, span_duration) ||
            span_duration <= 0) {
            return EditResult::failure(TimelineEditError::IntegerOverflow);
        }

        const auto scoped = lane_clips(clips, lane);
        for (const ClipPlacement* clip : scoped) {
            core::i64 end{};
            if (!clip_end(*clip, end)) {
                return EditResult::failure(
                    TimelineEditError::IncompatibleClip);
            }
            const bool intersects =
                clip->timing.start < span_end && end > span_start;
            if (!selected_ids.contains(clip->node) && intersects) {
                return EditResult::failure(
                    TimelineEditError::AmbiguousSelection);
            }
            if (!selected_ids.contains(clip->node) &&
                clip->timing.start >= span_end && clip->locked) {
                return EditResult::failure(TimelineEditError::LockedClip);
            }
        }

        TimelineEditPlan plan{};
        plan.span_start = span_start;
        plan.span_duration = span_duration;
        plan.applied_delta = -span_duration;
        plan.erases.assign(selected_ids.begin(), selected_ids.end());
        for (const ClipPlacement* clip : scoped) {
            if (selected_ids.contains(clip->node) ||
                clip->timing.start < span_end) {
                continue;
            }
            ClipPlacement shifted = *clip;
            if (!checked_subtract(shifted.timing.start, span_duration,
                                  shifted.timing.start) ||
                shifted.timing.start < 0) {
                return EditResult::failure(
                    TimelineEditError::IntegerOverflow);
            }
            plan.updates.push_back(std::move(shifted));
        }
        std::sort(plan.updates.begin(), plan.updates.end(),
                  [](const ClipPlacement& left,
                     const ClipPlacement& right) noexcept {
                      return left.node < right.node;
                  });
        return EditResult::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    }
}

TimelineEditResult plan_roll(
    std::span<const ClipPlacement> clips, doc::NodeId first,
    doc::NodeId second, core::i64 requested_delta,
    MediaFrameRate show_rate) {
    try {
        if (!valid_rate(show_rate)) {
            return EditResult::failure(TimelineEditError::InvalidRate);
        }
        if (first == second) {
            return EditResult::failure(
                TimelineEditError::AmbiguousSelection);
        }
        const ClipIndex index{clips};
        if (index.duplicate()) {
            return EditResult::failure(
                TimelineEditError::AmbiguousSelection);
        }
        const ClipPlacement* left = index.find(first);
        const ClipPlacement* right = index.find(second);
        if (left == nullptr || right == nullptr) {
            return EditResult::failure(TimelineEditError::ClipNotFound);
        }
        if (left->scene != right->scene || left->lane != right->lane) {
            return EditResult::failure(TimelineEditError::MultipleLanes);
        }
        if (left->timing.start > right->timing.start) {
            std::swap(left, right);
        }
        if (left->locked || right->locked) {
            return EditResult::failure(TimelineEditError::LockedClip);
        }
        const TimelineEditError left_error = source_valid(*left, show_rate);
        const TimelineEditError right_error = source_valid(*right, show_rate);
        if (sentinel_error(left_error)) {
            return EditResult::failure(left_error);
        }
        if (sentinel_error(right_error)) {
            return EditResult::failure(right_error);
        }
        const TimelineLaneRef lane{left->scene, left->lane};
        const auto scoped = lane_clips(clips, lane);
        const TimelineEditError layout_error = strict_lane_error(scoped);
        if (sentinel_error(layout_error)) {
            return EditResult::failure(layout_error);
        }
        const auto left_position = std::find(scoped.begin(), scoped.end(), left);
        const auto right_position =
            std::find(scoped.begin(), scoped.end(), right);
        if (left_position == scoped.end() || right_position == scoped.end() ||
            left_position + 1 != right_position) {
            return EditResult::failure(
                TimelineEditError::AmbiguousSelection);
        }
        core::i64 cut{};
        if (!clip_end(*left, cut)) {
            return EditResult::failure(
                TimelineEditError::IncompatibleClip);
        }
        if (cut != right->timing.start) {
            return EditResult::failure(TimelineEditError::GappedSelection);
        }
        core::i64 outer_end{};
        if (!clip_end(*right, outer_end)) {
            return EditResult::failure(
                TimelineEditError::IncompatibleClip);
        }

        const auto build = [&](core::i64 delta,
                               std::vector<ClipPlacement>& candidate) {
            candidate = copy_lane(scoped);
            ClipPlacement* candidate_left = nullptr;
            ClipPlacement* candidate_right = nullptr;
            for (ClipPlacement& clip : candidate) {
                if (clip.node == left->node) {
                    candidate_left = &clip;
                } else if (clip.node == right->node) {
                    candidate_right = &clip;
                }
            }
            if (candidate_left == nullptr || candidate_right == nullptr ||
                !checked_add(candidate_left->timing.duration, delta,
                             candidate_left->timing.duration) ||
                !checked_add(candidate_right->timing.start, delta,
                             candidate_right->timing.start) ||
                !checked_subtract(candidate_right->timing.duration, delta,
                                  candidate_right->timing.duration)) {
                return false;
            }
            const SourceOrdinalResult right_delta = source_ordinal_delta(
                delta, show_rate, candidate_right->frame_rate);
            if (!right_delta.has_value() ||
                !checked_add(candidate_right->timing.source_in,
                             right_delta.value(),
                             candidate_right->timing.source_in)) {
                return false;
            }
            clamp_fades(candidate_left->timing);
            clamp_fades(candidate_right->timing);
            return strict_candidate_valid(candidate) &&
                   source_is_valid(*candidate_left, show_rate) &&
                   source_is_valid(*candidate_right, show_rate);
        };

        std::vector<ClipPlacement> scratch;
        const core::i64 applied = clamp_delta(
            requested_delta,
            [&](core::i64 delta) { return build(delta, scratch); });
        std::vector<ClipPlacement> candidate;
        const bool built = build(applied, candidate);
        static_cast<void>(built);

        TimelineEditPlan plan{};
        plan.applied_delta = applied;
        plan.span_start = left->timing.start;
        plan.span_duration = outer_end - left->timing.start;
        append_changed_updates(scoped, candidate, plan);
        return EditResult::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    }
}

TimelineEditResult plan_slip(
    std::span<const ClipPlacement> clips, doc::NodeId selected,
    core::i64 requested_delta, MediaFrameRate show_rate) {
    try {
        if (!valid_rate(show_rate)) {
            return EditResult::failure(TimelineEditError::InvalidRate);
        }
        const ClipIndex index{clips};
        if (index.duplicate()) {
            return EditResult::failure(
                TimelineEditError::AmbiguousSelection);
        }
        const ClipPlacement* const chosen = index.find(selected);
        if (chosen == nullptr) {
            return EditResult::failure(TimelineEditError::ClipNotFound);
        }
        if (chosen->locked) {
            return EditResult::failure(TimelineEditError::LockedClip);
        }
        const TimelineEditError chosen_error = source_valid(*chosen, show_rate);
        if (sentinel_error(chosen_error)) {
            return EditResult::failure(chosen_error);
        }

        const auto build = [&](core::i64 delta, ClipPlacement& candidate) {
            candidate = *chosen;
            const SourceOrdinalResult mapped = source_ordinal_delta(
                delta, show_rate, candidate.frame_rate);
            return mapped.has_value() &&
                   checked_add(candidate.timing.source_in, mapped.value(),
                               candidate.timing.source_in) &&
                   source_is_valid(candidate, show_rate);
        };
        ClipPlacement scratch;
        const core::i64 applied = clamp_delta(
            requested_delta,
            [&](core::i64 delta) { return build(delta, scratch); });
        ClipPlacement candidate;
        const bool built = build(applied, candidate);
        static_cast<void>(built);

        TimelineEditPlan plan{};
        plan.applied_delta = applied;
        plan.span_start = chosen->timing.start;
        plan.span_duration = chosen->timing.duration;
        if (candidate != *chosen) {
            plan.updates.push_back(std::move(candidate));
        }
        return EditResult::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    }
}

TimelineEditResult plan_slide(
    std::span<const ClipPlacement> clips, doc::NodeId selected,
    core::i64 requested_delta, MediaFrameRate show_rate) {
    try {
        if (!valid_rate(show_rate)) {
            return EditResult::failure(TimelineEditError::InvalidRate);
        }
        const ClipIndex index{clips};
        if (index.duplicate()) {
            return EditResult::failure(
                TimelineEditError::AmbiguousSelection);
        }
        const ClipPlacement* const chosen = index.find(selected);
        if (chosen == nullptr) {
            return EditResult::failure(TimelineEditError::ClipNotFound);
        }
        if (chosen->locked) {
            return EditResult::failure(TimelineEditError::LockedClip);
        }
        const TimelineLaneRef lane{chosen->scene, chosen->lane};
        const auto scoped = lane_clips(clips, lane);
        const TimelineEditError layout_error = strict_lane_error(scoped);
        if (sentinel_error(layout_error)) {
            return EditResult::failure(layout_error);
        }
        const auto position = std::find(scoped.begin(), scoped.end(), chosen);
        if (position == scoped.end() || position == scoped.begin() ||
            position + 1 == scoped.end()) {
            return EditResult::failure(
                TimelineEditError::AmbiguousSelection);
        }
        const ClipPlacement* const left = *(position - 1);
        const ClipPlacement* const right = *(position + 1);
        if (left->locked || right->locked) {
            return EditResult::failure(TimelineEditError::LockedClip);
        }
        core::i64 left_end{};
        core::i64 selected_end{};
        core::i64 outer_end{};
        if (!clip_end(*left, left_end) || !clip_end(*chosen, selected_end) ||
            !clip_end(*right, outer_end)) {
            return EditResult::failure(
                TimelineEditError::IncompatibleClip);
        }
        if (left_end != chosen->timing.start ||
            selected_end != right->timing.start) {
            return EditResult::failure(TimelineEditError::GappedSelection);
        }
        for (const ClipPlacement* clip : {left, chosen, right}) {
            const TimelineEditError error = source_valid(*clip, show_rate);
            if (sentinel_error(error)) {
                return EditResult::failure(error);
            }
        }

        const auto build = [&](core::i64 delta,
                               std::vector<ClipPlacement>& candidate) {
            candidate = copy_lane(scoped);
            ClipPlacement* candidate_left = nullptr;
            ClipPlacement* candidate_selected = nullptr;
            ClipPlacement* candidate_right = nullptr;
            for (ClipPlacement& clip : candidate) {
                if (clip.node == left->node) {
                    candidate_left = &clip;
                } else if (clip.node == chosen->node) {
                    candidate_selected = &clip;
                } else if (clip.node == right->node) {
                    candidate_right = &clip;
                }
            }
            if (candidate_left == nullptr || candidate_selected == nullptr ||
                candidate_right == nullptr ||
                !checked_add(candidate_left->timing.duration, delta,
                             candidate_left->timing.duration) ||
                !checked_add(candidate_selected->timing.start, delta,
                             candidate_selected->timing.start) ||
                !checked_add(candidate_right->timing.start, delta,
                             candidate_right->timing.start) ||
                !checked_subtract(candidate_right->timing.duration, delta,
                                  candidate_right->timing.duration)) {
                return false;
            }
            const SourceOrdinalResult right_delta = source_ordinal_delta(
                delta, show_rate, candidate_right->frame_rate);
            if (!right_delta.has_value() ||
                !checked_add(candidate_right->timing.source_in,
                             right_delta.value(),
                             candidate_right->timing.source_in)) {
                return false;
            }
            clamp_fades(candidate_left->timing);
            clamp_fades(candidate_right->timing);
            return strict_candidate_valid(candidate) &&
                   source_is_valid(*candidate_left, show_rate) &&
                   source_is_valid(*candidate_selected, show_rate) &&
                   source_is_valid(*candidate_right, show_rate);
        };

        std::vector<ClipPlacement> scratch;
        const core::i64 applied = clamp_delta(
            requested_delta,
            [&](core::i64 delta) { return build(delta, scratch); });
        std::vector<ClipPlacement> candidate;
        const bool built = build(applied, candidate);
        static_cast<void>(built);

        TimelineEditPlan plan{};
        plan.applied_delta = applied;
        plan.span_start = left->timing.start;
        plan.span_duration = outer_end - left->timing.start;
        append_changed_updates(scoped, candidate, plan);
        return EditResult::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    }
}

TimelineEditResult plan_insert(
    std::span<const ClipPlacement> clips, TimelineLaneRef target,
    core::i64 start, core::i64 duration) {
    try {
        core::i64 paste_end{};
        if (start < 0 || duration <= 0 ||
            !checked_add(start, duration, paste_end)) {
            return EditResult::failure(TimelineEditError::InvalidSpan);
        }
        static_cast<void>(paste_end);
        const ClipIndex index{clips};
        if (index.duplicate()) {
            return EditResult::failure(
                TimelineEditError::AmbiguousSelection);
        }
        const auto scoped = lane_clips(clips, target);
        for (const ClipPlacement* clip : scoped) {
            core::i64 end{};
            if (!clip_end(*clip, end)) {
                return EditResult::failure(
                    TimelineEditError::IncompatibleClip);
            }
            if (clip->timing.start < start && end > start) {
                return EditResult::failure(
                    TimelineEditError::AmbiguousSelection);
            }
            if (clip->timing.start >= start && clip->locked) {
                return EditResult::failure(TimelineEditError::LockedClip);
            }
        }

        TimelineEditPlan plan{};
        plan.applied_delta = duration;
        plan.span_start = start;
        plan.span_duration = duration;
        for (const ClipPlacement* clip : scoped) {
            if (clip->timing.start < start) {
                continue;
            }
            ClipPlacement shifted = *clip;
            if (!checked_add(shifted.timing.start, duration,
                             shifted.timing.start)) {
                return EditResult::failure(
                    TimelineEditError::IntegerOverflow);
            }
            plan.updates.push_back(std::move(shifted));
        }
        std::sort(plan.updates.begin(), plan.updates.end(),
                  [](const ClipPlacement& left,
                     const ClipPlacement& right) noexcept {
                      return left.node < right.node;
                  });
        return EditResult::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    }
}

TimelineEditResult plan_overwrite(
    std::span<const ClipPlacement> clips, TimelineLaneRef target,
    core::i64 start, core::i64 duration, MediaFrameRate show_rate,
    doc::NodeId first_create_id) {
    try {
        if (!valid_rate(show_rate)) {
            return EditResult::failure(TimelineEditError::InvalidRate);
        }
        constexpr core::u64 max_reference_id =
            static_cast<core::u64>(std::numeric_limits<core::i64>::max());
        if (first_create_id.value() > max_reference_id) {
            return EditResult::failure(TimelineEditError::InvalidFirstId);
        }
        core::i64 paste_end{};
        if (start < 0 || duration <= 0 ||
            !checked_add(start, duration, paste_end)) {
            return EditResult::failure(TimelineEditError::InvalidSpan);
        }
        const ClipIndex index{clips};
        if (index.duplicate()) {
            return EditResult::failure(
                TimelineEditError::AmbiguousSelection);
        }
        const auto scoped = lane_clips(clips, target);
        std::map<doc::NodeId, ClipPlacement> updates;
        std::set<doc::NodeId> erases;
        std::vector<ClipPlacement> right_remainders;

        for (const ClipPlacement* clip : scoped) {
            core::i64 end{};
            if (!clip_end(*clip, end)) {
                return EditResult::failure(
                    TimelineEditError::IncompatibleClip);
            }
            if (clip->timing.start >= paste_end || end <= start) {
                continue;
            }
            if (clip->locked) {
                return EditResult::failure(TimelineEditError::LockedClip);
            }
            const TimelineEditError existing_error =
                source_valid(*clip, show_rate);
            if (sentinel_error(existing_error)) {
                return EditResult::failure(existing_error);
            }

            const bool keep_left = clip->timing.start < start;
            const bool keep_right = end > paste_end;
            if (!keep_left && !keep_right) {
                erases.insert(clip->node);
                continue;
            }

            if (keep_left) {
                ClipPlacement left = *clip;
                left.timing.duration = start - clip->timing.start;
                clamp_fades(left.timing);
                if (!source_is_valid(left, show_rate)) {
                    return EditResult::failure(
                        TimelineEditError::IncompatibleClip);
                }
                updates.insert_or_assign(left.node, std::move(left));
            }
            if (keep_right) {
                ClipPlacement right = *clip;
                const core::i64 skipped = paste_end - clip->timing.start;
                right.timing.start = paste_end;
                right.timing.duration = end - paste_end;
                const SourceOrdinalResult source_delta = source_ordinal_delta(
                    skipped, show_rate, right.frame_rate);
                if (!source_delta.has_value() ||
                    !checked_add(right.timing.source_in,
                                 source_delta.value(),
                                 right.timing.source_in)) {
                    return EditResult::failure(
                        TimelineEditError::IntegerOverflow);
                }
                clamp_fades(right.timing);
                if (!source_is_valid(right, show_rate)) {
                    return EditResult::failure(
                        TimelineEditError::IncompatibleClip);
                }
                if (keep_left) {
                    right_remainders.push_back(std::move(right));
                } else {
                    updates.insert_or_assign(right.node, std::move(right));
                }
            }
        }

        std::sort(right_remainders.begin(), right_remainders.end(),
                  [](const ClipPlacement& left,
                     const ClipPlacement& right) noexcept {
                      return left.node < right.node;
                  });
        std::set<doc::NodeId> occupied;
        for (const ClipPlacement& clip : clips) {
            occupied.insert(clip.node);
        }
        core::u64 candidate_id = first_create_id.value();
        for (ClipPlacement& remainder : right_remainders) {
            while (occupied.contains(doc::NodeId{candidate_id})) {
                if (candidate_id == max_reference_id) {
                    return EditResult::failure(
                        TimelineEditError::IdExhausted);
                }
                ++candidate_id;
            }
            remainder.node = doc::NodeId{candidate_id};
            occupied.insert(remainder.node);
            if (candidate_id != max_reference_id) {
                ++candidate_id;
            } else if (&remainder != &right_remainders.back()) {
                return EditResult::failure(TimelineEditError::IdExhausted);
            }
        }

        TimelineEditPlan plan{};
        plan.span_start = start;
        plan.span_duration = duration;
        for (auto& [node, clip] : updates) {
            static_cast<void>(node);
            plan.updates.push_back(std::move(clip));
        }
        plan.erases.assign(erases.begin(), erases.end());
        plan.creates = std::move(right_remainders);
        std::sort(plan.creates.begin(), plan.creates.end(),
                  [](const ClipPlacement& left,
                     const ClipPlacement& right) noexcept {
                      return left.node < right.node;
                  });
        return EditResult::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    }
}

} // namespace mediaserver::scene
