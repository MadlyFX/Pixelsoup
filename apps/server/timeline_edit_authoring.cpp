#include "timeline_edit_authoring.hpp"

#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace mediaserver::app {
namespace {

using AuthorResult =
    core::Result<TimelineEditAuthorPlan, TimelineEditAuthorError>;

constexpr core::u64 max_reference_id =
    static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());

[[nodiscard]] const std::int64_t* integer(
    const doc::Node& node, std::string_view property) noexcept {
    const auto found = node.properties.find(property);
    return found == node.properties.end()
        ? nullptr
        : std::get_if<std::int64_t>(&found->second);
}

[[nodiscard]] std::optional<doc::NodeId> reference(
    const doc::Node& node, std::string_view property) noexcept {
    const std::int64_t* const value = integer(node, property);
    return value != nullptr && *value >= 0
        ? std::optional<doc::NodeId>{
              doc::NodeId{static_cast<core::u64>(*value)}}
        : std::nullopt;
}

[[nodiscard]] bool boolean(
    const doc::Node& node, std::string_view property) noexcept {
    const auto found = node.properties.find(property);
    if (found == node.properties.end()) {
        return false;
    }
    const bool* const value = std::get_if<bool>(&found->second);
    return value != nullptr && *value;
}

void put_integer(doc::Node& node, std::string_view property,
                 std::int64_t value) {
    node.properties.insert_or_assign(
        std::string{property}, doc::PropertyValue{value});
}

void put_reference(doc::Node& node, std::string_view property,
                   doc::NodeId value) {
    put_integer(node, property, static_cast<std::int64_t>(value.value()));
}

void put_bool(doc::Node& node, std::string_view property, bool value) {
    node.properties.insert_or_assign(
        std::string{property}, doc::PropertyValue{value});
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

[[nodiscard]] std::map<doc::NodeId, scene::ClipPlacement>
placements_of(const doc::Root& root) {
    scene::SceneModel model;
    model.reset(root);
    std::map<doc::NodeId, scene::ClipPlacement> result;
    for (const scene::SceneInfo& info : model.scenes()) {
        for (scene::ClipPlacement& clip : model.clips(info.id)) {
            result.emplace(clip.node, std::move(clip));
        }
    }
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

[[nodiscard]] bool clip_end(const scene::ClipPlacement& clip,
                            core::i64& out) noexcept {
    return clip.timing.duration > 0 &&
           checked_add(clip.timing.start, clip.timing.duration, out);
}

[[nodiscard]] bool valid_timing(
    const scene::ClipPlacement& clip) noexcept {
    core::i64 end{};
    return clip_end(clip, end) && clip.sequence_count != 0U &&
           clip.timing.source_in >= 0 &&
           static_cast<core::u64>(clip.timing.source_in) <
               static_cast<core::u64>(clip.sequence_count) &&
           clip.timing.fade_in >= 0 && clip.timing.fade_out >= 0 &&
           clip.timing.fade_in <= clip.timing.duration &&
           clip.timing.fade_out <= clip.timing.duration &&
           (!clip.frame_rate.has_value() ||
            (clip.frame_rate->numerator != 0U &&
             clip.frame_rate->denominator != 0U));
}

[[nodiscard]] bool same_split_identity(
    const scene::ClipPlacement& created,
    const scene::ClipPlacement& source) noexcept {
    return created.scene == source.scene && created.source == source.source &&
           created.sequence_start == source.sequence_start &&
           created.sequence_count == source.sequence_count &&
           created.lane == source.lane && created.order == source.order &&
           created.locked == source.locked &&
           created.frame_rate == source.frame_rate &&
           created.parent_group == source.parent_group &&
           created.group_order == source.group_order;
}

[[nodiscard]] bool apply_placement(
    doc::Node& node, const scene::ClipPlacement& clip) {
    if (clip.scene.value() > max_reference_id ||
        clip.lane.value() > max_reference_id ||
        (clip.parent_group.has_value() &&
         clip.parent_group->value() > max_reference_id)) {
        return false;
    }
    put_reference(node, scene::schema::scene_id, clip.scene);
    node.properties.insert_or_assign(
        std::string{scene::schema::source},
        doc::PropertyValue{clip.source});
    put_integer(node, scene::schema::sequence_start,
                static_cast<std::int64_t>(clip.sequence_start));
    put_integer(node, scene::schema::sequence_count,
                static_cast<std::int64_t>(clip.sequence_count));
    put_integer(node, scene::schema::clip_start, clip.timing.start);
    put_integer(node, scene::schema::clip_duration, clip.timing.duration);
    put_integer(node, scene::schema::source_in, clip.timing.source_in);
    put_integer(node, scene::schema::fade_in, clip.timing.fade_in);
    put_integer(node, scene::schema::fade_out, clip.timing.fade_out);
    if (clip.lane) {
        put_reference(node, scene::schema::lane_id, clip.lane);
    } else {
        node.properties.erase(std::string{scene::schema::lane_id});
    }
    put_integer(node, scene::schema::clip_order, clip.order);
    put_bool(node, scene::schema::locked, clip.locked);
    if (clip.frame_rate.has_value()) {
        put_integer(node, scene::schema::source_rate_numerator,
                    static_cast<std::int64_t>(clip.frame_rate->numerator));
        put_integer(node, scene::schema::source_rate_denominator,
                    static_cast<std::int64_t>(clip.frame_rate->denominator));
    } else {
        node.properties.erase(
            std::string{scene::schema::source_rate_numerator});
        node.properties.erase(
            std::string{scene::schema::source_rate_denominator});
    }
    if (clip.parent_group.has_value()) {
        put_reference(node, scene::schema::parent_group,
                      *clip.parent_group);
    } else {
        node.properties.erase(std::string{scene::schema::parent_group});
    }
    put_integer(node, scene::schema::group_order, clip.group_order);
    return true;
}

[[nodiscard]] bool lane_locked(
    const std::map<doc::NodeId, doc::Node>& nodes,
    const scene::ClipPlacement& clip) noexcept {
    if (!clip.lane) {
        return false;
    }
    const auto lane = nodes.find(clip.lane);
    return lane != nodes.end() &&
           lane->second.type == scene::schema::timeline_lane_type &&
           boolean(lane->second, scene::schema::lane_locked);
}

struct SplitSource final {
    doc::NodeId original{};
    doc::NodeId created{};
};

struct SubgraphClone final {
    doc::NodeId split_source{};
    doc::NodeId split_created{};
    doc::Node original;
    doc::NodeId created{};
};

[[nodiscard]] AuthorResult allocation_failure() {
    return AuthorResult::failure(TimelineEditAuthorError::AllocationFailed);
}

} // namespace

core::Result<TimelineEditAuthorPlan, TimelineEditAuthorError>
plan_timeline_edit_authoring(
    const doc::Root& document, const scene::TimelineEditPlan& edit,
    core::u64 next_node) {
    try {
        const auto nodes = nodes_of(document);
        const auto placements = placements_of(document);

        std::set<doc::NodeId> update_ids;
        std::set<doc::NodeId> erase_ids;
        std::set<doc::NodeId> create_ids;
        for (const scene::ClipPlacement& clip : edit.updates) {
            if (!update_ids.insert(clip.node).second) {
                return AuthorResult::failure(
                    TimelineEditAuthorError::InvalidPlan);
            }
        }
        for (doc::NodeId id : edit.erases) {
            if (!erase_ids.insert(id).second || update_ids.contains(id)) {
                return AuthorResult::failure(
                    TimelineEditAuthorError::InvalidPlan);
            }
        }
        for (const scene::ClipPlacement& clip : edit.creates) {
            if (clip.node.value() > max_reference_id ||
                !create_ids.insert(clip.node).second ||
                update_ids.contains(clip.node) || erase_ids.contains(clip.node)) {
                return AuthorResult::failure(
                    TimelineEditAuthorError::InvalidPlan);
            }
        }

        const auto require_media = [&](doc::NodeId id)
            -> std::optional<TimelineEditAuthorError> {
            const auto found = nodes.find(id);
            if (found == nodes.end()) {
                return TimelineEditAuthorError::ClipNotFound;
            }
            if (found->second.type != scene::schema::media_type) {
                return TimelineEditAuthorError::NotMediaClip;
            }
            if (!placements.contains(id)) {
                return TimelineEditAuthorError::InvalidPlan;
            }
            return std::nullopt;
        };
        for (doc::NodeId id : update_ids) {
            const auto error = require_media(id);
            if (error.has_value()) {
                return AuthorResult::failure(*error);
            }
            const scene::ClipPlacement& placement = placements.at(id);
            if (placement.locked || boolean(nodes.at(id), scene::schema::locked)) {
                return AuthorResult::failure(
                    TimelineEditAuthorError::LockedClip);
            }
            if (lane_locked(nodes, placement)) {
                return AuthorResult::failure(
                    TimelineEditAuthorError::LockedLane);
            }
            const scene::ClipPlacement* requested = nullptr;
            for (const scene::ClipPlacement& update : edit.updates) {
                if (update.node == id) {
                    requested = &update;
                    break;
                }
            }
            if (requested != nullptr && lane_locked(nodes, *requested)) {
                return AuthorResult::failure(
                    TimelineEditAuthorError::LockedLane);
            }
            if (requested == nullptr ||
                !same_split_identity(*requested, placement) ||
                !valid_timing(*requested)) {
                return AuthorResult::failure(
                    TimelineEditAuthorError::InvalidPlan);
            }
        }
        for (doc::NodeId id : erase_ids) {
            const auto error = require_media(id);
            if (error.has_value()) {
                return AuthorResult::failure(*error);
            }
            const scene::ClipPlacement& placement = placements.at(id);
            if (placement.locked || boolean(nodes.at(id), scene::schema::locked)) {
                return AuthorResult::failure(
                    TimelineEditAuthorError::LockedClip);
            }
            if (lane_locked(nodes, placement)) {
                return AuthorResult::failure(
                    TimelineEditAuthorError::LockedLane);
            }
        }

        core::i64 span_end{};
        if (!edit.creates.empty() &&
            (edit.span_duration <= 0 ||
             !checked_add(edit.span_start, edit.span_duration, span_end))) {
            return AuthorResult::failure(
                TimelineEditAuthorError::InvalidPlan);
        }

        // A pure overwrite split updates the original to the left remainder
        // and creates one right remainder. Recover that lineage from the old
        // placement and the plan's fixed overwrite span. Both lists are then
        // paired in original-node order, matching the planner's documented ID
        // allocation order even when input clips were shuffled.
        std::vector<doc::NodeId> split_originals;
        if (!edit.creates.empty()) {
            for (const scene::ClipPlacement& updated : edit.updates) {
                const auto original = placements.find(updated.node);
                if (original == placements.end()) {
                    continue;
                }
                core::i64 old_end{};
                core::i64 updated_end{};
                if (!clip_end(original->second, old_end) ||
                    !clip_end(updated, updated_end)) {
                    return AuthorResult::failure(
                        TimelineEditAuthorError::InvalidPlan);
                }
                if (original->second.timing.start < edit.span_start &&
                    old_end > span_end &&
                    updated.timing.start == original->second.timing.start &&
                    updated_end == edit.span_start) {
                    split_originals.push_back(updated.node);
                }
            }
        }
        std::sort(split_originals.begin(), split_originals.end());
        std::vector<const scene::ClipPlacement*> split_creates;
        split_creates.reserve(edit.creates.size());
        for (const scene::ClipPlacement& created : edit.creates) {
            split_creates.push_back(&created);
        }
        std::sort(
            split_creates.begin(), split_creates.end(),
            [](const scene::ClipPlacement* left,
               const scene::ClipPlacement* right) noexcept {
                return left->node < right->node;
            });
        if (split_originals.size() != split_creates.size()) {
            return AuthorResult::failure(
                TimelineEditAuthorError::InvalidPlan);
        }

        std::set<doc::NodeId> occupied;
        for (const auto& [id, node] : nodes) {
            static_cast<void>(node);
            occupied.insert(id);
        }
        core::u64 cursor = next_node;
        auto allocate = [&](core::u64 preferred)
            -> std::optional<doc::NodeId> {
            cursor = std::max(cursor, preferred);
            while (cursor <= max_reference_id &&
                   occupied.contains(doc::NodeId{cursor})) {
                ++cursor;
            }
            if (cursor > max_reference_id) {
                return std::nullopt;
            }
            const doc::NodeId allocated{cursor};
            occupied.insert(allocated);
            ++cursor;
            return allocated;
        };

        std::vector<SplitSource> splits;
        splits.reserve(split_originals.size());
        for (std::size_t index = 0U; index < split_originals.size(); ++index) {
            const scene::ClipPlacement& source =
                placements.at(split_originals[index]);
            const scene::ClipPlacement& created = *split_creates[index];
            core::i64 source_end{};
            if (!clip_end(source, source_end) ||
                !same_split_identity(created, source) ||
                created.timing.start != span_end ||
                created.timing.duration != source_end - span_end ||
                !valid_timing(created) || source.locked ||
                lane_locked(nodes, source)) {
                return AuthorResult::failure(
                    source.locked ? TimelineEditAuthorError::LockedClip
                                  : lane_locked(nodes, source)
                                  ? TimelineEditAuthorError::LockedLane
                                  : TimelineEditAuthorError::InvalidPlan);
            }
            const auto allocated = allocate(created.node.value());
            if (!allocated.has_value()) {
                return AuthorResult::failure(
                    TimelineEditAuthorError::IdExhausted);
            }
            splits.push_back(SplitSource{source.node, *allocated});
        }

        TimelineEditAuthorPlan plan{};
        plan.document = document;
        plan.next_node = next_node;

        // Apply existing full-placement replacements without disturbing asset
        // identity, transforms, styles, or any unknown future media property.
        std::vector<const scene::ClipPlacement*> sorted_updates;
        sorted_updates.reserve(edit.updates.size());
        for (const scene::ClipPlacement& clip : edit.updates) {
            sorted_updates.push_back(&clip);
        }
        std::sort(
            sorted_updates.begin(), sorted_updates.end(),
            [](const scene::ClipPlacement* left,
               const scene::ClipPlacement* right) noexcept {
                return left->node < right->node;
            });
        for (const scene::ClipPlacement* clip : sorted_updates) {
            doc::Node updated = nodes.at(clip->node);
            if (!apply_placement(updated, *clip)) {
                return AuthorResult::failure(
                    TimelineEditAuthorError::InvalidPlan);
            }
            plan.document = plan.document.set(std::move(updated));
            plan.changed.push_back(clip->node);
        }

        // Materialize split media before allocating subgraphs. The source node
        // is cloned first, so asset id and every non-placement extension are
        // retained automatically.
        for (std::size_t index = 0U; index < splits.size(); ++index) {
            doc::Node created = nodes.at(splits[index].original);
            created.id = splits[index].created;
            if (!apply_placement(created, *split_creates[index])) {
                return AuthorResult::failure(
                    TimelineEditAuthorError::InvalidPlan);
            }
            plan.document = plan.document.set(created);
            plan.created.push_back(std::move(created));
        }

        std::vector<SubgraphClone> clones;
        for (const SplitSource& split : splits) {
            std::set<doc::NodeId> effects;
            for (const auto& [id, node] : nodes) {
                if (node.type == scene::schema::effect_type &&
                    reference(node, scene::schema::effect_target) ==
                        split.original) {
                    effects.insert(id);
                }
            }
            for (const auto& [id, node] : nodes) {
                if (node.type != scene::schema::effect_type &&
                    node.type != scene::schema::channel_type) {
                    continue;
                }
                const auto target = node.type == scene::schema::effect_type
                    ? reference(node, scene::schema::effect_target)
                    : reference(node, scene::schema::channel_target);
                const bool belongs =
                    node.type == scene::schema::effect_type
                    ? target == split.original
                    : target.has_value() &&
                          (*target == split.original ||
                           effects.contains(*target));
                if (belongs) {
                    clones.push_back(SubgraphClone{
                        split.original, split.created, node, {}});
                }
            }
        }
        std::stable_sort(
            clones.begin(), clones.end(),
            [](const SubgraphClone& left,
               const SubgraphClone& right) noexcept {
                return std::pair{left.split_source, left.original.id} <
                       std::pair{right.split_source, right.original.id};
            });

        for (SubgraphClone& clone : clones) {
            const auto allocated = allocate(cursor);
            if (!allocated.has_value()) {
                return AuthorResult::failure(
                    TimelineEditAuthorError::IdExhausted);
            }
            clone.created = *allocated;
        }

        // Per split, map every original effect/channel id before materializing
        // nodes so a low-id channel may safely target a higher-id effect.
        for (const SplitSource& split : splits) {
            std::map<doc::NodeId, doc::NodeId> remap;
            for (const SubgraphClone& clone : clones) {
                if (clone.split_source == split.original) {
                    remap.emplace(clone.original.id, clone.created);
                }
            }
            for (const SubgraphClone& clone : clones) {
                if (clone.split_source != split.original) {
                    continue;
                }
                doc::Node created = clone.original;
                created.id = clone.created;
                const std::string_view target_property =
                    created.type == scene::schema::effect_type
                    ? scene::schema::effect_target
                    : scene::schema::channel_target;
                const auto old_target = reference(clone.original,
                                                  target_property);
                if (!old_target.has_value()) {
                    return AuthorResult::failure(
                        TimelineEditAuthorError::InvalidPlan);
                }
                if (*old_target == split.original) {
                    put_reference(created, target_property, split.created);
                } else {
                    const auto mapped = remap.find(*old_target);
                    if (mapped == remap.end()) {
                        return AuthorResult::failure(
                            TimelineEditAuthorError::InvalidPlan);
                    }
                    put_reference(created, target_property, mapped->second);
                }
                // Key frame properties are intentionally copied unchanged:
                // split is in-place in show time, unlike clipboard paste.
                plan.document = plan.document.set(created);
                plan.created.push_back(std::move(created));
            }
        }

        // Cascading erasure is computed against the pre-edit snapshot. Direct
        // clip effects go first into the set, then channels targeting either a
        // clip or one of those effects. Applying in id order is deterministic;
        // Root persistence makes the whole function atomic on failure.
        std::set<doc::NodeId> cascade = erase_ids;
        for (const auto& [id, node] : nodes) {
            if (node.type != scene::schema::effect_type) {
                continue;
            }
            const auto target = reference(node, scene::schema::effect_target);
            if (target.has_value() && erase_ids.contains(*target)) {
                cascade.insert(id);
            }
        }
        for (const auto& [id, node] : nodes) {
            if (node.type != scene::schema::channel_type) {
                continue;
            }
            const auto target = reference(node, scene::schema::channel_target);
            if (target.has_value() && cascade.contains(*target)) {
                cascade.insert(id);
            }
        }
        for (doc::NodeId id : cascade) {
            const auto erased = plan.document.erase(id);
            if (!erased.has_value()) {
                return AuthorResult::failure(
                    TimelineEditAuthorError::InvalidDocument);
            }
            plan.document = *erased;
            plan.erased.push_back(id);
        }

        std::sort(plan.changed.begin(), plan.changed.end());
        std::sort(plan.created.begin(), plan.created.end(),
                  [](const doc::Node& left, const doc::Node& right) noexcept {
                      return left.id < right.id;
                  });
        plan.next_node = plan.created.empty() ? next_node : cursor;
        return AuthorResult::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    }
}

} // namespace mediaserver::app
