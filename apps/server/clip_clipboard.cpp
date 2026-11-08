#include "clip_clipboard.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <mediaserver/scene/scene_schema.hpp>

namespace mediaserver::app {
namespace {

[[nodiscard]] const std::int64_t* integer_property(
    const doc::Node& node,
    std::string_view name) noexcept {
    const auto found = node.properties.find(name);
    if (found == node.properties.end()) {
        return nullptr;
    }
    return std::get_if<std::int64_t>(&found->second);
}

[[nodiscard]] std::optional<doc::NodeId> node_id_property(
    const doc::Node& node,
    std::string_view name) noexcept {
    const std::int64_t* const held = integer_property(node, name);
    if (held == nullptr || *held < 0) {
        return std::nullopt;
    }
    return doc::NodeId{static_cast<core::u64>(*held)};
}

[[nodiscard]] bool is_unlocked(const doc::Node& node) noexcept {
    const auto found = node.properties.find(scene::schema::locked);
    if (found == node.properties.end()) {
        return true;
    }
    const bool* const held = std::get_if<bool>(&found->second);
    return held != nullptr && !*held;
}

[[nodiscard]] std::optional<core::i64> placed_start(
    const doc::Node& node) noexcept {
    const std::int64_t* const duration =
        integer_property(node, scene::schema::clip_duration);
    if (duration == nullptr || *duration <= 0) {
        return std::nullopt;
    }
    const auto found = node.properties.find(scene::schema::clip_start);
    if (found == node.properties.end()) {
        return core::i64{0};
    }
    const std::int64_t* const start =
        std::get_if<std::int64_t>(&found->second);
    if (start == nullptr) {
        return std::nullopt;
    }
    return *start;
}

[[nodiscard]] bool contains(
    std::span<const doc::NodeId> sorted,
    doc::NodeId id) noexcept {
    return std::binary_search(sorted.begin(), sorted.end(), id);
}

[[nodiscard]] std::optional<core::i64> shifted_frame(
    core::i64 value,
    core::i64 anchor,
    core::u64 playhead) noexcept {
    constexpr core::u64 signed_max =
        static_cast<core::u64>(std::numeric_limits<core::i64>::max());
    constexpr core::u64 signed_min_magnitude = signed_max + 1U;
    if (playhead > signed_max) {
        return std::nullopt;
    }

    if (value >= anchor) {
        const core::u64 relative =
            static_cast<core::u64>(value) - static_cast<core::u64>(anchor);
        if (relative > signed_max - playhead) {
            return std::nullopt;
        }
        return static_cast<core::i64>(playhead + relative);
    }

    const core::u64 distance =
        static_cast<core::u64>(anchor) - static_cast<core::u64>(value);
    if (playhead >= distance) {
        return static_cast<core::i64>(playhead - distance);
    }
    const core::u64 magnitude = distance - playhead;
    if (magnitude > signed_min_magnitude) {
        return std::nullopt;
    }
    if (magnitude == signed_min_magnitude) {
        return std::numeric_limits<core::i64>::min();
    }
    return -static_cast<core::i64>(magnitude);
}

[[nodiscard]] bool key_frame_property(std::string_view name) noexcept {
    constexpr std::string_view prefix = scene::schema::channel_key_prefix;
    constexpr std::string_view suffix =
        scene::schema::channel_key_frame_suffix;
    if (!name.starts_with(prefix) || !name.ends_with(suffix) ||
        name.size() <= prefix.size() + suffix.size()) {
        return false;
    }
    const std::string_view index = name.substr(
        prefix.size(), name.size() - prefix.size() - suffix.size());
    return std::all_of(index.begin(), index.end(), [](char value) noexcept {
        return value >= '0' && value <= '9';
    });
}

[[nodiscard]] std::optional<doc::NodeId> remapped(
    std::span<const std::pair<doc::NodeId, doc::NodeId>> ids,
    doc::NodeId original) noexcept {
    const auto found = std::lower_bound(
        ids.begin(), ids.end(), original,
        [](const auto& pair, doc::NodeId wanted) noexcept {
            return pair.first < wanted;
        });
    if (found == ids.end() || found->first != original) {
        return std::nullopt;
    }
    return found->second;
}

[[nodiscard]] doc::PropertyValue id_value(doc::NodeId id) noexcept {
    return doc::PropertyValue{static_cast<std::int64_t>(id.value())};
}

[[nodiscard]] bool valid_target_scene(
    const doc::Root& document,
    scene::SceneId target) {
    bool target_is_stage = false;
    const doc::Delta all = doc::diff(doc::Root{}, document);
    for (const doc::NodeChange& change : all.changes) {
        if (!change.after || change.after->type != scene::schema::stage_type) {
            continue;
        }
        target_is_stage = target_is_stage || change.id == target;
    }
    return target_is_stage;
}

} // namespace

bool ClipClipboard::copy(
    const doc::Root& document,
    std::span<const doc::NodeId> selection) {
    std::vector<doc::NodeId> selected{selection.begin(), selection.end()};
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());

    std::vector<doc::Node> copied;
    std::vector<doc::NodeId> clips;
    std::optional<core::i64> anchor;
    std::optional<core::i64> end;
    std::optional<doc::NodeId> source_lane;
    bool multiple_source_lanes = false;
    copied.reserve(selected.size());
    clips.reserve(selected.size());
    for (const doc::NodeId id : selected) {
        const doc::Node* const node = document.get(id);
        if (node == nullptr || node->type != scene::schema::media_type ||
            !is_unlocked(*node)) {
            continue;
        }
        const std::optional<core::i64> start = placed_start(*node);
        if (!start.has_value()) {
            continue;
        }
        const std::int64_t* const duration =
            integer_property(*node, scene::schema::clip_duration);
        if (duration == nullptr || *duration <= 0 ||
            *start > std::numeric_limits<core::i64>::max() - *duration) {
            continue;
        }
        const core::i64 clip_end = *start + *duration;
        const doc::NodeId lane =
            node_id_property(*node, scene::schema::lane_id)
                .value_or(doc::NodeId{});
        if (!source_lane.has_value()) {
            source_lane = lane;
        } else if (*source_lane != lane) {
            multiple_source_lanes = true;
        }
        copied.push_back(*node);
        clips.push_back(id);
        anchor = anchor.has_value() ? std::min(*anchor, *start) : *start;
        end = end.has_value() ? std::max(*end, clip_end) : clip_end;
    }

    if (clips.empty()) {
        clear();
        return false;
    }
    const core::u64 span = static_cast<core::u64>(*end) -
                           static_cast<core::u64>(*anchor);
    if (span > static_cast<core::u64>(std::numeric_limits<core::i64>::max())) {
        clear();
        return false;
    }

    std::vector<doc::Node> effects;
    std::vector<doc::NodeId> effect_ids;
    const doc::Delta all = doc::diff(doc::Root{}, document);
    for (const doc::NodeChange& change : all.changes) {
        if (!change.after ||
            change.after->type != scene::schema::effect_type) {
            continue;
        }
        const std::optional<doc::NodeId> target =
            node_id_property(*change.after, scene::schema::effect_target);
        if (target.has_value() && contains(clips, *target)) {
            effects.push_back(*change.after);
            effect_ids.push_back(change.id);
        }
    }
    std::sort(effect_ids.begin(), effect_ids.end());

    for (const doc::NodeChange& change : all.changes) {
        if (!change.after ||
            change.after->type != scene::schema::channel_type) {
            continue;
        }
        const std::optional<doc::NodeId> target =
            node_id_property(*change.after, scene::schema::channel_target);
        if (target.has_value() &&
            (contains(clips, *target) || contains(effect_ids, *target))) {
            copied.push_back(*change.after);
        }
    }
    copied.insert(copied.end(), effects.begin(), effects.end());
    std::sort(copied.begin(), copied.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });

    nodes_ = std::move(copied);
    anchor_ = *anchor;
    span_duration_ = static_cast<core::i64>(span);
    multiple_source_lanes_ = multiple_source_lanes;
    return true;
}

core::Result<ClipPastePlan, ClipPasteError> ClipClipboard::paste(
    const doc::Root& document,
    scene::SceneId target_scene,
    core::u64 playhead,
    core::u64 next_node,
    std::optional<doc::NodeId> target_lane) const {
    if (nodes_.empty()) {
        return core::Result<ClipPastePlan, ClipPasteError>::failure(
            ClipPasteError::Empty);
    }
    constexpr core::u64 signed_max =
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());
    if (target_scene.value() > signed_max ||
        !valid_target_scene(document, target_scene)) {
        return core::Result<ClipPastePlan, ClipPasteError>::failure(
            ClipPasteError::InvalidTargetScene);
    }
    if (playhead > signed_max) {
        return core::Result<ClipPastePlan, ClipPasteError>::failure(
            ClipPasteError::FrameOutOfRange);
    }
    if (target_lane.has_value() &&
        (multiple_source_lanes_ || target_lane->value() > signed_max)) {
        return core::Result<ClipPastePlan, ClipPasteError>::failure(
            ClipPasteError::InvalidSnapshot);
    }

    std::vector<std::pair<doc::NodeId, doc::NodeId>> ids;
    ids.reserve(nodes_.size());
    core::u64 cursor = next_node;
    for (const doc::Node& original : nodes_) {
        while (document.contains(doc::NodeId{cursor})) {
            if (cursor == std::numeric_limits<core::u64>::max()) {
                return core::Result<ClipPastePlan, ClipPasteError>::failure(
                    ClipPasteError::IdExhausted);
            }
            ++cursor;
        }
        if (cursor > signed_max) {
            return core::Result<ClipPastePlan, ClipPasteError>::failure(
                ClipPasteError::IdExhausted);
        }
        ids.emplace_back(original.id, doc::NodeId{cursor});
        ++cursor;
    }

    ClipPastePlan plan{};
    plan.document = document;
    plan.next_node = cursor;
    plan.created.reserve(nodes_.size());
    for (const doc::Node& original : nodes_) {
        const std::optional<doc::NodeId> new_id = remapped(ids, original.id);
        if (!new_id.has_value()) {
            return core::Result<ClipPastePlan, ClipPasteError>::failure(
                ClipPasteError::InvalidSnapshot);
        }
        doc::Node created = original;
        created.id = *new_id;
        created.properties.insert_or_assign(
            std::string{scene::schema::scene_id}, id_value(target_scene));

        if (created.type == scene::schema::media_type) {
            const std::optional<core::i64> start = placed_start(original);
            if (!start.has_value()) {
                return core::Result<ClipPastePlan, ClipPasteError>::failure(
                    ClipPasteError::InvalidSnapshot);
            }
            const std::optional<core::i64> shifted =
                shifted_frame(*start, anchor_, playhead);
            const auto* duration = integer_property(original, scene::schema::clip_duration);
            if (!shifted.has_value() || duration == nullptr || *duration <= 0 ||
                *shifted > std::numeric_limits<core::i64>::max() - *duration) {
                return core::Result<ClipPastePlan, ClipPasteError>::failure(
                    ClipPasteError::FrameOutOfRange);
            }
            created.properties.insert_or_assign(
                std::string{scene::schema::clip_start},
                doc::PropertyValue{*shifted});
            if (target_lane.has_value()) {
                if (*target_lane == doc::NodeId{}) {
                    created.properties.erase(
                        std::string{scene::schema::lane_id});
                } else {
                    created.properties.insert_or_assign(
                        std::string{scene::schema::lane_id},
                        id_value(*target_lane));
                }
            }
        } else if (created.type == scene::schema::effect_type) {
            const std::optional<doc::NodeId> target =
                node_id_property(original, scene::schema::effect_target);
            const std::optional<doc::NodeId> mapped =
                target.has_value() ? remapped(ids, *target) : std::nullopt;
            if (!mapped.has_value()) {
                return core::Result<ClipPastePlan, ClipPasteError>::failure(
                    ClipPasteError::InvalidSnapshot);
            }
            created.properties.insert_or_assign(
                std::string{scene::schema::effect_target}, id_value(*mapped));
        } else if (created.type == scene::schema::channel_type) {
            const std::optional<doc::NodeId> target =
                node_id_property(original, scene::schema::channel_target);
            const std::optional<doc::NodeId> mapped =
                target.has_value() ? remapped(ids, *target) : std::nullopt;
            if (!mapped.has_value()) {
                return core::Result<ClipPastePlan, ClipPasteError>::failure(
                    ClipPasteError::InvalidSnapshot);
            }
            created.properties.insert_or_assign(
                std::string{scene::schema::channel_target}, id_value(*mapped));
            for (auto& [name, value] : created.properties) {
                if (!key_frame_property(name)) {
                    continue;
                }
                const std::int64_t* const frame =
                    std::get_if<std::int64_t>(&value);
                if (frame == nullptr) {
                    return core::Result<ClipPastePlan, ClipPasteError>::failure(
                        ClipPasteError::InvalidSnapshot);
                }
                const std::optional<core::i64> shifted =
                    shifted_frame(*frame, anchor_, playhead);
                if (!shifted.has_value()) {
                    return core::Result<ClipPastePlan, ClipPasteError>::failure(
                        ClipPasteError::FrameOutOfRange);
                }
                value = doc::PropertyValue{*shifted};
            }
        } else {
            return core::Result<ClipPastePlan, ClipPasteError>::failure(
                ClipPasteError::InvalidSnapshot);
        }

        plan.document = plan.document.set(created);
        plan.created.push_back(std::move(created));
    }
    return core::Result<ClipPastePlan, ClipPasteError>::success(std::move(plan));
}

void ClipClipboard::clear() noexcept {
    nodes_.clear();
    anchor_ = 0;
    span_duration_ = 0;
    multiple_source_lanes_ = false;
}

} // namespace mediaserver::app
