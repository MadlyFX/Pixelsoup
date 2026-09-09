#include <mediaserver/scene/key_edit.hpp>

#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace mediaserver::scene {
namespace {

using ChannelIdentity = std::pair<doc::NodeId, PropertyChannel>;

struct ParsedKey final {
    core::i64 frame{};
    doc::PropertyValue carrier{0.0};
    double value{};
    bool carrier_authored{};
    KeyInterpolation interpolation{KeyInterpolation::Linear};
    bool interpolation_authored{};
};

struct ParsedChannel final {
    doc::NodeId node{};
    doc::NodeId target{};
    PropertyChannel property{};
    std::vector<ParsedKey> keys;
    bool ambiguous{};
};

[[nodiscard]] const doc::PropertyValue* find(
    const doc::PropertyMap& properties, std::string_view name) noexcept {
    const auto found = properties.find(name);
    return found == properties.end() ? nullptr : &found->second;
}

[[nodiscard]] std::optional<PropertyChannel> property_from_name(
    std::string_view name) noexcept {
    constexpr std::array<std::string_view,
                         static_cast<std::size_t>(PropertyChannel::Count)>
        names{
            schema::position_x,
            schema::position_y,
            schema::position_z,
            schema::rotation_yaw,
            schema::rotation_pitch,
            schema::rotation_roll,
            schema::scale_x,
            schema::scale_y,
            schema::scale_z,
            schema::opacity,
            schema::brightness,
            schema::saturation,
            schema::blur_radius,
            schema::film_grain_amount,
            schema::box_blur_radius,
            schema::directional_blur_length,
            schema::chroma_key_mix,
            schema::luma_key_mix,
            schema::sky_time_of_day,
            schema::environment_map_intensity,
            schema::environment_map_rotation,
            schema::environment_fog_density,
            schema::environment_fog_height,
            schema::environment_fog_height_falloff,
            schema::environment_fog_anisotropy,
            schema::sky_haze,
            schema::sky_solar_illuminance,
        };
    for (std::size_t index = 0U; index < names.size(); ++index) {
        if (name == names[index]) {
            return static_cast<PropertyChannel>(index);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string_view> property_name(
    PropertyChannel property) noexcept {
    constexpr std::array<std::string_view,
                         static_cast<std::size_t>(PropertyChannel::Count)>
        names{
            schema::position_x,
            schema::position_y,
            schema::position_z,
            schema::rotation_yaw,
            schema::rotation_pitch,
            schema::rotation_roll,
            schema::scale_x,
            schema::scale_y,
            schema::scale_z,
            schema::opacity,
            schema::brightness,
            schema::saturation,
            schema::blur_radius,
            schema::film_grain_amount,
            schema::box_blur_radius,
            schema::directional_blur_length,
            schema::chroma_key_mix,
            schema::luma_key_mix,
            schema::sky_time_of_day,
            schema::environment_map_intensity,
            schema::environment_map_rotation,
            schema::environment_fog_density,
            schema::environment_fog_height,
            schema::environment_fog_height_falloff,
            schema::environment_fog_anisotropy,
            schema::sky_haze,
            schema::sky_solar_illuminance,
        };
    const std::size_t index = static_cast<std::size_t>(property);
    return index < names.size() ? std::optional<std::string_view>{names[index]}
                                : std::nullopt;
}

[[nodiscard]] std::string key_property(core::u32 index,
                                       std::string_view suffix) {
    return std::string{schema::channel_key_prefix} + std::to_string(index)
        + std::string{suffix};
}

[[nodiscard]] bool parse_channel(const doc::Node& node,
                                 ParsedChannel& parsed) {
    if (node.type != schema::channel_type) {
        return false;
    }
    const auto* const target_value =
        find(node.properties, schema::channel_target);
    const auto* const target = target_value == nullptr
        ? nullptr
        : std::get_if<std::int64_t>(target_value);
    const auto* const component_value =
        find(node.properties, schema::channel_component);
    const auto* const component = component_value == nullptr
        ? nullptr
        : std::get_if<std::string>(component_value);
    if (target == nullptr || *target < 0 || component == nullptr) {
        return false;
    }
    const auto property = property_from_name(*component);
    if (!property.has_value()) {
        return false;
    }

    parsed.node = node.id;
    parsed.target = doc::NodeId{static_cast<core::u64>(*target)};
    parsed.property = *property;
    parsed.keys.clear();
    parsed.ambiguous = false;
    constexpr double maximum =
        static_cast<double>(std::numeric_limits<float>::max());
    for (core::u32 index = 0U; index < max_channel_keys; ++index) {
        const auto* const frame_value = find(
            node.properties,
            key_property(index, schema::channel_key_frame_suffix));
        if (frame_value == nullptr) {
            break;
        }
        const auto* const frame = std::get_if<std::int64_t>(frame_value);
        if (frame == nullptr) {
            return false;
        }

        ParsedKey key{};
        key.frame = *frame;
        if (const auto* const value = find(
                node.properties,
                key_property(index, schema::channel_key_value_suffix))) {
            key.carrier_authored = true;
            if (const auto* const real = std::get_if<double>(value)) {
                key.carrier = *real;
                key.value = *real;
            } else if (const auto* const whole =
                           std::get_if<std::int64_t>(value)) {
                key.carrier = *whole;
                key.value = static_cast<double>(*whole);
            } else {
                return false;
            }
        }
        if (!std::isfinite(key.value) || key.value < -maximum
            || key.value > maximum) {
            return false;
        }

        if (const auto* const interpolation = find(
                node.properties,
                key_property(index, schema::channel_key_interp_suffix))) {
            const auto* const name = std::get_if<std::string>(interpolation);
            if (name == nullptr) {
                return false;
            }
            key.interpolation_authored = true;
            if (*name == schema::interp_hold) {
                key.interpolation = KeyInterpolation::Hold;
            } else if (*name == schema::interp_linear) {
                key.interpolation = KeyInterpolation::Linear;
            } else if (*name == schema::interp_cubic) {
                key.interpolation = KeyInterpolation::Cubic;
            } else {
                return false;
            }
        }
        parsed.keys.push_back(std::move(key));
    }
    if (parsed.keys.empty()) {
        return false;
    }
    std::stable_sort(
        parsed.keys.begin(), parsed.keys.end(),
        [](const ParsedKey& left, const ParsedKey& right) noexcept {
            return left.frame < right.frame;
        });
    parsed.ambiguous = std::adjacent_find(
        parsed.keys.begin(), parsed.keys.end(),
        [](const ParsedKey& left, const ParsedKey& right) noexcept {
            return left.frame == right.frame;
        }) != parsed.keys.end();
    return true;
}

class DocumentIndex final {
public:
    explicit DocumentIndex(const doc::Root& document) {
        for (const doc::NodeChange& change : doc::diff(doc::Root{}, document).changes) {
            if (change.after != nullptr) {
                nodes.emplace(change.id, *change.after);
            }
        }
        for (const auto& [id, node] : nodes) {
            ParsedChannel parsed{};
            if (!parse_channel(node, parsed)) {
                continue;
            }
            active_channels.try_emplace(
                ChannelIdentity{parsed.target, parsed.property},
                std::move(parsed));
        }
        for (const auto& [id, node] : nodes) {
            if (node.type == schema::stage_type) {
                default_scene = id;
                break;
            }
        }
    }

    [[nodiscard]] const doc::Node* node(doc::NodeId id) const noexcept {
        const auto found = nodes.find(id);
        return found == nodes.end() ? nullptr : &found->second;
    }

    [[nodiscard]] const ParsedChannel* channel(
        ChannelIdentity identity) const noexcept {
        const auto found = active_channels.find(identity);
        return found == active_channels.end() ? nullptr : &found->second;
    }

    std::map<doc::NodeId, doc::Node> nodes;
    std::map<ChannelIdentity, ParsedChannel> active_channels;
    SceneId default_scene{};
};

[[nodiscard]] bool read_boolean(const doc::Node& node,
                                std::string_view property, bool fallback,
                                bool& value) noexcept {
    const auto* const held = find(node.properties, property);
    if (held == nullptr) {
        value = fallback;
        return true;
    }
    if (const auto* const boolean = std::get_if<bool>(held)) {
        value = *boolean;
        return true;
    }
    if (const auto* const whole = std::get_if<std::int64_t>(held)) {
        value = *whole != 0;
        return true;
    }
    if (const auto* const real = std::get_if<double>(held)) {
        if (!std::isfinite(*real)) {
            return false;
        }
        value = *real >= 0.5;
        return true;
    }
    return false;
}

[[nodiscard]] bool read_optional_reference(
    const doc::Node& node, std::string_view property,
    std::optional<doc::NodeId>& value) noexcept {
    const auto* const held = find(node.properties, property);
    if (held == nullptr) {
        value.reset();
        return true;
    }
    const auto* const whole = std::get_if<std::int64_t>(held);
    if (whole == nullptr || *whole < 0) {
        return false;
    }
    value = doc::NodeId{static_cast<core::u64>(*whole)};
    return true;
}

[[nodiscard]] bool direct_scene(const doc::Node& node,
                                SceneId default_scene,
                                SceneId& scene) noexcept {
    if (node.type == schema::stage_type) { scene = node.id; return true; }
    std::optional<doc::NodeId> authored;
    if (!read_optional_reference(node, schema::scene_id, authored)) {
        return false;
    }
    scene = authored.value_or(default_scene);
    return true;
}

[[nodiscard]] bool is_transform(PropertyChannel property) noexcept {
    return property >= PropertyChannel::PositionX
        && property <= PropertyChannel::ScaleZ;
}

[[nodiscard]] bool is_drawable(std::string_view type) noexcept {
    return type == schema::object_type || type == schema::media_type
        || type == schema::model_type;
}

[[nodiscard]] bool property_compatible(const doc::Node& target,
                                       PropertyChannel property) noexcept {
    if (target.type == schema::stage_type) return is_environment_channel(property);
    if (target.type == schema::group_type) {
        return is_transform(property);
    }
    if (is_drawable(target.type)) {
        return is_transform(property) || property == PropertyChannel::Opacity
            || property == PropertyChannel::Brightness
            || property == PropertyChannel::Saturation
            || property == PropertyChannel::BlurRadius;
    }
    if (target.type != schema::effect_type) {
        return false;
    }
    const auto* const kind_value = find(target.properties, schema::effect_kind);
    const auto* const kind = kind_value == nullptr
        ? nullptr
        : std::get_if<std::string>(kind_value);
    if (kind == nullptr) {
        return false;
    }
    return (*kind == schema::brightness_kind
            && property == PropertyChannel::Brightness)
        || (*kind == schema::saturation_kind
            && property == PropertyChannel::Saturation)
        || (*kind == schema::blur_kind
            && property == PropertyChannel::BlurRadius)
        || (*kind == schema::film_grain_kind
            && property == PropertyChannel::FilmGrainAmount)
        || (*kind == schema::box_blur_kind
            && property == PropertyChannel::BoxBlurRadius)
        || (*kind == schema::directional_blur_kind
            && property == PropertyChannel::DirectionalBlurLength)
        || (*kind == schema::chroma_key_kind
            && property == PropertyChannel::ChromaKeyMix)
        || (*kind == schema::luma_key_kind
            && property == PropertyChannel::LumaKeyMix);
}

[[nodiscard]] std::optional<KeyEditSkipReason> target_status(
    const DocumentIndex& index, doc::NodeId target_id,
    PropertyChannel property) noexcept {
    const doc::Node* target = index.node(target_id);
    if (target == nullptr) {
        return KeyEditSkipReason::TargetMissing;
    }
    if (!property_compatible(*target, property)) {
        return KeyEditSkipReason::InvalidTarget;
    }

    bool locked{};
    if (!read_boolean(*target, schema::locked, false, locked)) {
        return KeyEditSkipReason::InvalidTarget;
    }
    if (locked) {
        return KeyEditSkipReason::TargetLocked;
    }

    const doc::Node* hierarchy_node = target;
    if (target->type == schema::effect_type) {
        std::optional<doc::NodeId> effect_target;
        if (!read_optional_reference(
                *target, schema::effect_target, effect_target)
            || !effect_target.has_value()) {
            return KeyEditSkipReason::InvalidTarget;
        }
        hierarchy_node = index.node(*effect_target);
        if (hierarchy_node == nullptr || !is_drawable(hierarchy_node->type)) {
            return KeyEditSkipReason::InvalidTarget;
        }
        if (!read_boolean(
                *hierarchy_node, schema::locked, false, locked)) {
            return KeyEditSkipReason::InvalidTarget;
        }
        if (locked) {
            return KeyEditSkipReason::TargetLocked;
        }
    }

    SceneId target_scene{};
    if (!direct_scene(*hierarchy_node, index.default_scene, target_scene)) {
        return KeyEditSkipReason::InvalidTarget;
    }

    if (hierarchy_node->type == schema::media_type) {
        std::optional<doc::NodeId> lane;
        if (!read_optional_reference(*hierarchy_node, schema::lane_id, lane)) {
            return KeyEditSkipReason::InvalidTarget;
        }
        if (lane.has_value()) {
            const doc::Node* lane_node = index.node(*lane);
            SceneId lane_scene{};
            if (lane_node == nullptr
                || lane_node->type != schema::timeline_lane_type
                || !direct_scene(*lane_node, index.default_scene, lane_scene)
                || lane_scene != target_scene
                || !read_boolean(
                    *lane_node, schema::lane_locked, false, locked)) {
                return KeyEditSkipReason::InvalidTarget;
            }
            if (locked) {
                return KeyEditSkipReason::TargetLocked;
            }
        }
    }

    std::set<doc::NodeId> visited;
    const doc::Node* current = hierarchy_node;
    while (true) {
        if (!visited.insert(current->id).second) {
            return KeyEditSkipReason::InvalidTarget;
        }
        std::optional<doc::NodeId> parent;
        if (!read_optional_reference(*current, schema::parent_group, parent)) {
            return KeyEditSkipReason::InvalidTarget;
        }
        if (!parent.has_value()) {
            break;
        }
        current = index.node(*parent);
        SceneId parent_scene{};
        if (current == nullptr || current->type != schema::group_type
            || !direct_scene(*current, index.default_scene, parent_scene)
            || parent_scene != target_scene
            || !read_boolean(*current, schema::locked, false, locked)) {
            return KeyEditSkipReason::InvalidTarget;
        }
        if (locked) {
            return KeyEditSkipReason::TargetLocked;
        }
    }
    return std::nullopt;
}

[[nodiscard]] KeyRecord make_record(const ParsedChannel& channel,
                                    const ParsedKey& key) {
    return KeyRecord{
        channel.node,
        KeyAddress{channel.target, channel.property, key.frame},
        key.carrier,
        key.value,
        key.carrier_authored,
        key.interpolation,
        key.interpolation_authored,
    };
}

[[nodiscard]] KeySelection inspect_selection(
    const DocumentIndex& index, std::span<const KeyAddress> input) {
    std::vector<KeyAddress> addresses{input.begin(), input.end()};
    std::sort(addresses.begin(), addresses.end());
    const auto unique_end = std::unique(addresses.begin(), addresses.end());
    const std::size_t duplicates =
        static_cast<std::size_t>(addresses.end() - unique_end);
    addresses.erase(unique_end, addresses.end());

    KeySelection result{};
    result.duplicate_inputs = duplicates;
    result.records.reserve(addresses.size());
    result.skips.reserve(addresses.size());
    for (const KeyAddress& address : addresses) {
        if (!property_name(address.property).has_value()) {
            result.skips.push_back(
                KeyEditSkip{address, KeyEditSkipReason::InvalidProperty});
            continue;
        }
        if (const auto status =
                target_status(index, address.target, address.property)) {
            result.skips.push_back(KeyEditSkip{address, *status});
            continue;
        }
        const ParsedChannel* const channel =
            index.channel(ChannelIdentity{address.target, address.property});
        if (channel == nullptr) {
            result.skips.push_back(
                KeyEditSkip{address, KeyEditSkipReason::ChannelMissing});
            continue;
        }
        if (channel->ambiguous) {
            result.skips.push_back(
                KeyEditSkip{address, KeyEditSkipReason::AmbiguousChannel});
            continue;
        }
        const auto key = std::lower_bound(
            channel->keys.begin(), channel->keys.end(), address.frame,
            [](const ParsedKey& candidate, core::i64 frame) noexcept {
                return candidate.frame < frame;
            });
        if (key == channel->keys.end() || key->frame != address.frame) {
            result.skips.push_back(
                KeyEditSkip{address, KeyEditSkipReason::KeyMissing});
            continue;
        }
        result.records.push_back(make_record(*channel, *key));
    }
    return result;
}

[[nodiscard]] ParsedKey key_from_record(const KeyRecord& record,
                                        core::i64 destination) {
    return ParsedKey{
        destination,
        record.carrier,
        record.value,
        record.carrier_authored,
        record.interpolation,
        record.interpolation_authored,
    };
}

[[nodiscard]] std::string_view interpolation_name(
    KeyInterpolation interpolation) noexcept {
    switch (interpolation) {
    case KeyInterpolation::Hold:
        return schema::interp_hold;
    case KeyInterpolation::Linear:
        return schema::interp_linear;
    case KeyInterpolation::Cubic:
        return schema::interp_cubic;
    }
    return schema::interp_linear;
}

[[nodiscard]] doc::Node rebuild_channel(const doc::Node& original,
                                        const std::vector<ParsedKey>& keys) {
    doc::Node rebuilt = original;
    for (core::u32 index = 0U; index < max_channel_keys; ++index) {
        rebuilt.properties.erase(
            key_property(index, schema::channel_key_frame_suffix));
        rebuilt.properties.erase(
            key_property(index, schema::channel_key_value_suffix));
        rebuilt.properties.erase(
            key_property(index, schema::channel_key_interp_suffix));
    }
    for (std::size_t index = 0U; index < keys.size(); ++index) {
        const ParsedKey& key = keys[index];
        const auto persisted_index = static_cast<core::u32>(index);
        rebuilt.properties.insert_or_assign(
            key_property(persisted_index, schema::channel_key_frame_suffix),
            key.frame);
        if (key.carrier_authored) {
            rebuilt.properties.insert_or_assign(
                key_property(
                    persisted_index, schema::channel_key_value_suffix),
                key.carrier);
        }
        if (key.interpolation_authored) {
            rebuilt.properties.insert_or_assign(
                key_property(
                    persisted_index, schema::channel_key_interp_suffix),
                std::string{interpolation_name(key.interpolation)});
        }
    }
    return rebuilt;
}

using Mutations = std::map<doc::NodeId, std::optional<doc::Node>>;

void record_channel_mutation(const DocumentIndex& index,
                             doc::NodeId channel,
                             const std::vector<ParsedKey>& keys,
                             Mutations& mutations) {
    const doc::Node* const original = index.node(channel);
    if (original == nullptr) {
        return;
    }
    if (keys.empty()) {
        mutations.insert_or_assign(channel, std::nullopt);
        return;
    }
    doc::Node rebuilt = rebuild_channel(*original, keys);
    if (rebuilt != *original) {
        mutations.insert_or_assign(channel, std::move(rebuilt));
    }
}

void finalize_plan(const doc::Root& document, Mutations mutations,
                   KeyEditPlan& plan) {
    plan.after = document;
    for (auto& [id, mutation] : mutations) {
        if (mutation.has_value()) {
            plan.puts.push_back(*mutation);
            plan.after = plan.after.set(std::move(*mutation));
        } else {
            plan.erases.push_back(id);
            const auto erased = plan.after.erase(id);
            if (erased.has_value()) {
                plan.after = std::move(*erased);
            }
        }
    }
}

struct Candidate final {
    KeyRecord record;
    core::i64 destination{};
};

[[nodiscard]] bool candidate_less(const Candidate& left,
                                  const Candidate& right) noexcept {
    if (left.record.address.frame != right.record.address.frame) {
        return left.record.address.frame < right.record.address.frame;
    }
    return left.record.channel < right.record.channel;
}

void sort_report(KeyEditPlan& plan) {
    std::sort(plan.selection_after.begin(), plan.selection_after.end(),
              [](const KeyRecord& left, const KeyRecord& right) noexcept {
                  return left.address < right.address;
              });
    std::sort(plan.report.skips.begin(), plan.report.skips.end(),
              [](const KeyEditSkip& left, const KeyEditSkip& right) noexcept {
                  return std::tie(left.address, left.reason)
                      < std::tie(right.address, right.reason);
              });
    std::sort(
        plan.report.collisions.begin(), plan.report.collisions.end(),
        [](const KeyCollision& left, const KeyCollision& right) noexcept {
            return std::tie(left.channel, left.destination_frame,
                            left.stationary_loser, left.loser, left.winner)
                < std::tie(right.channel, right.destination_frame,
                           right.stationary_loser, right.loser, right.winner);
        });
}

[[nodiscard]] KeyEditPlan make_transform_plan(
    const doc::Root& document, const DocumentIndex& index,
    KeySelection selection, std::vector<Candidate> candidates,
    bool duplicate) {
    KeyEditPlan plan{};
    plan.report.selected = selection.records.size();
    plan.report.duplicate_inputs = selection.duplicate_inputs;
    plan.report.skips = std::move(selection.skips);

    std::map<doc::NodeId, std::vector<Candidate>> by_channel;
    for (Candidate& candidate : candidates) {
        by_channel[candidate.record.channel].push_back(std::move(candidate));
    }

    Mutations mutations;
    for (auto& [channel_id, channel_candidates] : by_channel) {
        const ParsedChannel* channel = nullptr;
        if (!channel_candidates.empty()) {
            const KeyRecord& first = channel_candidates.front().record;
            channel = index.channel(
                ChannelIdentity{first.address.target, first.address.property});
        }
        if (channel == nullptr) {
            continue;
        }

        std::set<core::i64> selected_frames;
        for (const Candidate& candidate : channel_candidates) {
            selected_frames.insert(candidate.record.address.frame);
        }
        std::vector<ParsedKey> working;
        working.reserve(channel->keys.size());
        for (const ParsedKey& key : channel->keys) {
            if (duplicate || !selected_frames.contains(key.frame)) {
                working.push_back(key);
            }
        }

        std::map<core::i64, std::vector<Candidate>> by_destination;
        for (Candidate& candidate : channel_candidates) {
            by_destination[candidate.destination].push_back(
                std::move(candidate));
        }
        for (auto& [destination, contenders] : by_destination) {
            std::stable_sort(
                contenders.begin(), contenders.end(), candidate_less);
            const Candidate& winner = contenders.back();
            for (auto loser = contenders.begin();
                 loser + 1 != contenders.end(); ++loser) {
                plan.report.collisions.push_back(KeyCollision{
                    loser->record.address,
                    winner.record.address,
                    channel_id,
                    destination,
                    false,
                });
            }

            auto position = std::lower_bound(
                working.begin(), working.end(), destination,
                [](const ParsedKey& key, core::i64 frame) noexcept {
                    return key.frame < frame;
                });
            const bool occupied = position != working.end()
                && position->frame == destination;
            if (!occupied && working.size() >= max_channel_keys) {
                plan.report.skips.push_back(KeyEditSkip{
                    winner.record.address,
                    KeyEditSkipReason::ChannelCapacity,
                });
                continue;
            }

            if (occupied) {
                const KeyAddress stationary{
                    channel->target, channel->property, destination};
                if (stationary != winner.record.address) {
                    plan.report.collisions.push_back(KeyCollision{
                        stationary,
                        winner.record.address,
                        channel_id,
                        destination,
                        true,
                    });
                }
                *position = key_from_record(winner.record, destination);
            } else {
                working.insert(
                    position, key_from_record(winner.record, destination));
            }

            KeyRecord selected = winner.record;
            selected.address.frame = destination;
            plan.selection_after.push_back(std::move(selected));
            ++plan.report.applied;
        }
        record_channel_mutation(index, channel_id, working, mutations);
    }
    sort_report(plan);
    finalize_plan(document, std::move(mutations), plan);
    return plan;
}

[[nodiscard]] KeyEditPlan make_delete_plan(const doc::Root& document,
                                           const DocumentIndex& index,
                                           KeySelection selection) {
    KeyEditPlan plan{};
    plan.report.selected = selection.records.size();
    plan.report.duplicate_inputs = selection.duplicate_inputs;
    plan.report.skips = std::move(selection.skips);

    std::map<doc::NodeId, std::set<core::i64>> selected_by_channel;
    for (const KeyRecord& record : selection.records) {
        selected_by_channel[record.channel].insert(record.address.frame);
    }
    Mutations mutations;
    for (const auto& [channel_id, selected_frames] : selected_by_channel) {
        const KeyRecord* example = nullptr;
        for (const KeyRecord& record : selection.records) {
            if (record.channel == channel_id) {
                example = &record;
                break;
            }
        }
        if (example == nullptr) {
            continue;
        }
        const ParsedChannel* const channel = index.channel(ChannelIdentity{
            example->address.target, example->address.property});
        if (channel == nullptr) {
            continue;
        }
        std::vector<ParsedKey> remaining;
        remaining.reserve(channel->keys.size());
        for (const ParsedKey& key : channel->keys) {
            if (!selected_frames.contains(key.frame)) {
                remaining.push_back(key);
            }
        }
        plan.report.applied += selected_frames.size();
        record_channel_mutation(index, channel_id, remaining, mutations);
    }
    sort_report(plan);
    finalize_plan(document, std::move(mutations), plan);
    return plan;
}

[[nodiscard]] bool checked_add(core::i64 left, core::i64 right,
                               core::i64& out) noexcept {
    if ((right > 0
         && left > std::numeric_limits<core::i64>::max() - right)
        || (right < 0
            && left < std::numeric_limits<core::i64>::min() - right)) {
        return false;
    }
    out = left + right;
    return true;
}

struct WideUnsigned final {
    core::u64 high{};
    core::u64 low{};
};

struct WideDivision final {
    core::u64 quotient{};
    core::u64 remainder{};
    bool quotient_overflow{};
};

[[nodiscard]] core::u64 magnitude(core::i64 value) noexcept {
    return value >= 0 ? static_cast<core::u64>(value)
                      : static_cast<core::u64>(-(value + 1)) + 1U;
}

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
    const core::u64 middle = (low_product >> 32U) + (cross_left & mask)
        + (cross_right & mask);
    return WideUnsigned{
        high_product + (cross_left >> 32U) + (cross_right >> 32U)
            + (middle >> 32U),
        (middle << 32U) | (low_product & mask),
    };
}

[[nodiscard]] WideDivision divide_wide(WideUnsigned numerator,
                                       core::u64 denominator) noexcept {
    WideDivision result{};
    core::u64 remainder{};
    for (int bit = 127; bit >= 0; --bit) {
        const core::u64 input = bit >= 64
            ? (numerator.high >> (bit - 64)) & 1U
            : (numerator.low >> bit) & 1U;
        const bool carry = (remainder >> 63U) != 0U;
        remainder = (remainder << 1U) | input;
        if (carry || remainder >= denominator) {
            remainder -= denominator;
            if (bit >= 64) {
                result.quotient_overflow = true;
            } else {
                result.quotient |= core::u64{1U} << bit;
            }
        }
    }
    result.remainder = remainder;
    return result;
}

[[nodiscard]] core::u64 ordered_frame(core::i64 frame) noexcept {
    return static_cast<core::u64>(frame) ^ (core::u64{1U} << 63U);
}

[[nodiscard]] bool frame_from_ordered(core::u64 ordered,
                                      core::i64& frame) noexcept {
    const core::u64 raw = ordered ^ (core::u64{1U} << 63U);
    constexpr core::u64 positive_limit =
        static_cast<core::u64>(std::numeric_limits<core::i64>::max());
    if (raw <= positive_limit) {
        frame = static_cast<core::i64>(raw);
        return true;
    }
    const core::u64 negative_magnitude = (~raw) + 1U;
    if (negative_magnitude == core::u64{1U} << 63U) {
        frame = std::numeric_limits<core::i64>::min();
    } else {
        frame = -static_cast<core::i64>(negative_magnitude);
    }
    return true;
}

[[nodiscard]] bool add_distance(core::i64 anchor, core::u64 distance,
                                bool negative, core::i64& out) noexcept {
    core::u64 ordered = ordered_frame(anchor);
    if (negative) {
        if (distance > ordered) {
            return false;
        }
        ordered -= distance;
    } else {
        if (distance > std::numeric_limits<core::u64>::max() - ordered) {
            return false;
        }
        ordered += distance;
    }
    return frame_from_ordered(ordered, out);
}

[[nodiscard]] bool scale_frame(core::i64 source, core::i64 anchor,
                               core::i64 numerator, core::i64 denominator,
                               core::i64& destination) noexcept {
    const core::u64 source_ordered = ordered_frame(source);
    const core::u64 anchor_ordered = ordered_frame(anchor);
    const bool difference_negative = source_ordered < anchor_ordered;
    const core::u64 difference = difference_negative
        ? anchor_ordered - source_ordered
        : source_ordered - anchor_ordered;
    const bool numerator_negative = numerator < 0;
    WideDivision division = divide_wide(
        multiply_wide(difference, magnitude(numerator)),
        static_cast<core::u64>(denominator));
    if (division.quotient_overflow) {
        return false;
    }
    const core::u64 divisor = static_cast<core::u64>(denominator);
    if (division.remainder >= divisor - division.remainder) {
        if (division.quotient ==
            std::numeric_limits<core::u64>::max()) {
            return false;
        }
        ++division.quotient;
    }
    const bool scaled_negative = division.quotient != 0U
        && (difference_negative != numerator_negative);
    return add_distance(
        anchor, division.quotient, scaled_negative, destination);
}

[[nodiscard]] core::i64 scale_anchor(const KeySelection& selection,
                                     KeyTimeScale scale) noexcept {
    switch (scale.anchor) {
    case KeyTimeAnchor::FirstSelected:
        if (!selection.records.empty()) {
            return std::min_element(
                       selection.records.begin(), selection.records.end(),
                       [](const KeyRecord& left,
                          const KeyRecord& right) noexcept {
                           return left.address.frame < right.address.frame;
                       })
                ->address.frame;
        }
        return 0;
    case KeyTimeAnchor::LastSelected:
        if (!selection.records.empty()) {
            return std::max_element(
                       selection.records.begin(), selection.records.end(),
                       [](const KeyRecord& left,
                          const KeyRecord& right) noexcept {
                           return left.address.frame < right.address.frame;
                       })
                ->address.frame;
        }
        return 0;
    case KeyTimeAnchor::Playhead:
        return scale.playhead;
    case KeyTimeAnchor::User:
        return scale.user_anchor;
    }
    return 0;
}

} // namespace

KeySelectionResult inspect_key_selection(
    const doc::Root& document, std::span<const KeyAddress> selection) {
    try {
        const DocumentIndex index{document};
        return KeySelectionResult::success(
            inspect_selection(index, selection));
    } catch (const std::bad_alloc&) {
        return KeySelectionResult::failure(KeyEditError::AllocationFailed);
    }
}

KeyEditResult plan_key_move(const doc::Root& document,
                            std::span<const KeyAddress> selection,
                            core::i64 delta) {
    try {
        const DocumentIndex index{document};
        KeySelection inspected = inspect_selection(index, selection);
        std::vector<Candidate> candidates;
        candidates.reserve(inspected.records.size());
        for (const KeyRecord& record : inspected.records) {
            core::i64 destination{};
            if (!checked_add(record.address.frame, delta, destination)) {
                return KeyEditResult::failure(KeyEditError::IntegerOverflow);
            }
            candidates.push_back(Candidate{record, destination});
        }
        return KeyEditResult::success(make_transform_plan(
            document, index, std::move(inspected), std::move(candidates),
            false));
    } catch (const std::bad_alloc&) {
        return KeyEditResult::failure(KeyEditError::AllocationFailed);
    }
}

KeyEditResult plan_key_duplicate(const doc::Root& document,
                                 std::span<const KeyAddress> selection,
                                 core::i64 delta) {
    try {
        const DocumentIndex index{document};
        KeySelection inspected = inspect_selection(index, selection);
        std::vector<Candidate> candidates;
        candidates.reserve(inspected.records.size());
        for (const KeyRecord& record : inspected.records) {
            core::i64 destination{};
            if (!checked_add(record.address.frame, delta, destination)) {
                return KeyEditResult::failure(KeyEditError::IntegerOverflow);
            }
            candidates.push_back(Candidate{record, destination});
        }
        return KeyEditResult::success(make_transform_plan(
            document, index, std::move(inspected), std::move(candidates),
            true));
    } catch (const std::bad_alloc&) {
        return KeyEditResult::failure(KeyEditError::AllocationFailed);
    }
}

KeyEditResult plan_key_delete(const doc::Root& document,
                              std::span<const KeyAddress> selection) {
    try {
        const DocumentIndex index{document};
        return KeyEditResult::success(make_delete_plan(
            document, index, inspect_selection(index, selection)));
    } catch (const std::bad_alloc&) {
        return KeyEditResult::failure(KeyEditError::AllocationFailed);
    }
}

KeyEditResult plan_key_scale(const doc::Root& document,
                             std::span<const KeyAddress> selection,
                             KeyTimeScale scale) {
    if (scale.denominator <= 0) {
        return KeyEditResult::failure(KeyEditError::InvalidScale);
    }
    try {
        const DocumentIndex index{document};
        KeySelection inspected = inspect_selection(index, selection);
        const core::i64 anchor = scale_anchor(inspected, scale);
        std::vector<Candidate> candidates;
        candidates.reserve(inspected.records.size());
        for (const KeyRecord& record : inspected.records) {
            core::i64 destination{};
            if (!scale_frame(record.address.frame, anchor, scale.numerator,
                             scale.denominator, destination)) {
                return KeyEditResult::failure(KeyEditError::IntegerOverflow);
            }
            candidates.push_back(Candidate{record, destination});
        }
        return KeyEditResult::success(make_transform_plan(
            document, index, std::move(inspected), std::move(candidates),
            false));
    } catch (const std::bad_alloc&) {
        return KeyEditResult::failure(KeyEditError::AllocationFailed);
    }
}

} // namespace mediaserver::scene
