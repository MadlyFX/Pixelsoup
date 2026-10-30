#include <mediaserver/scene/key_edit.hpp>

#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <numeric>
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
    std::array<std::optional<doc::PropertyValue>, 5U> tangent{};
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
            schema::intensity,
            schema::color_r,
            schema::color_g,
            schema::color_b,
            schema::range,
            schema::fov_turns,
            schema::shutter_seconds,
            schema::focus_distance_m,
            schema::aperture_f_number,
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
            schema::intensity,
            schema::color_r,
            schema::color_g,
            schema::color_b,
            schema::range,
            schema::fov_turns,
            schema::shutter_seconds,
            schema::focus_distance_m,
            schema::aperture_f_number,
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
        for (std::size_t slot = 0U;
             slot < schema::channel_key_tangent_suffixes.size(); ++slot) {
            if (const auto* const held = find(
                    node.properties,
                    key_property(
                        index, schema::channel_key_tangent_suffixes[slot]))) {
                key.tangent[slot] = *held;
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
        || type == schema::model_type
        || type == schema::particle_effect_type;
}

[[nodiscard]] bool property_compatible(const doc::Node& target,
                                       PropertyChannel property) noexcept {
    if (target.type == schema::stage_type) return is_environment_channel(property);
    if (target.type == schema::camera_type) {
        return (property >= PropertyChannel::PositionX
                && property <= PropertyChannel::RotationRoll)
            || (property >= PropertyChannel::CameraFieldOfView
                && property <= PropertyChannel::CameraAperture);
    }
    if (target.type == schema::light_type) {
        return (property >= PropertyChannel::PositionX
                && property <= PropertyChannel::PositionZ)
            || is_light_channel(property);
    }
    // Output depends on Scene; keep its document vocabulary at this boundary
    // rather than introducing the reverse module dependency.
    if (target.type == "output.surface") {
        return (property >= PropertyChannel::PositionX
                && property <= PropertyChannel::PositionZ)
            || property == PropertyChannel::RotationYaw;
    }

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
    const bool default_locked = target->type == "output.surface";
    if (!read_boolean(*target, schema::locked, default_locked, locked)) {
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
        key.tangent,
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
        record.tangent,
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
        for (const std::string_view suffix :
             schema::channel_key_tangent_suffixes) {
            rebuilt.properties.erase(key_property(index, suffix));
        }
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
        for (std::size_t slot = 0U; slot < key.tangent.size(); ++slot) {
            if (key.tangent[slot].has_value()) {
                rebuilt.properties.insert_or_assign(
                    key_property(persisted_index,
                                 schema::channel_key_tangent_suffixes[slot]),
                    *key.tangent[slot]);
            }
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

[[nodiscard]] bool evaluates(const ParsedChannel& channel,
                             const std::vector<ParsedKey>& keys);

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
        // Undone below if the result would not evaluate.
        const std::size_t applied_before = plan.report.applied;
        const std::size_t selected_before = plan.selection_after.size();
        const std::size_t collisions_before = plan.report.collisions.size();
        const std::size_t skips_before = plan.report.skips.size();
        std::vector<KeyAddress> sources;
        for (const Candidate& candidate : channel_candidates) {
            sources.push_back(candidate.record.address);
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
        // Authored handle weights travel unchanged; an interval shortened
        // below the handles across it would stop the whole channel
        // evaluating, so that channel is left as it was.
        if (evaluates(*channel, channel->keys) && !evaluates(*channel, working)) {
            plan.report.applied = applied_before;
            plan.selection_after.resize(selected_before);
            plan.report.collisions.resize(collisions_before);
            plan.report.skips.resize(skips_before);
            for (const KeyAddress& source : sources) {
                plan.report.skips.push_back(
                    KeyEditSkip{source, KeyEditSkipReason::InvalidTangent});
            }
            continue;
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

// Tangent carrier slots, in schema::channel_key_tangent_suffixes order.
constexpr std::size_t tangent_mode_slot = 0U;
constexpr std::size_t in_slope_slot = 1U;
constexpr std::size_t out_slope_slot = 2U;
constexpr std::size_t in_weight_slot = 3U;
constexpr std::size_t out_weight_slot = 4U;

[[nodiscard]] std::optional<double> carried_number(
    const std::optional<doc::PropertyValue>& held) noexcept {
    if (!held.has_value()) {
        return std::nullopt;
    }
    if (const auto* const real = std::get_if<double>(&*held)) {
        return *real;
    }
    if (const auto* const whole = std::get_if<std::int64_t>(&*held)) {
        return static_cast<double>(*whole);
    }
    return std::nullopt;
}

[[nodiscard]] KeyTangentMode carried_mode(const ParsedKey& key) noexcept {
    const auto& held = key.tangent[tangent_mode_slot];
    const auto* const name =
        held.has_value() ? std::get_if<std::string>(&*held) : nullptr;
    if (name == nullptr) {
        return KeyTangentMode::Auto;
    }
    if (*name == schema::tangent_aligned) {
        return KeyTangentMode::Aligned;
    }
    if (*name == schema::tangent_broken) {
        return KeyTangentMode::Broken;
    }
    if (*name == schema::tangent_flat) {
        return KeyTangentMode::Flat;
    }
    return KeyTangentMode::Auto;
}

// The keys as the evaluator sees them, for their evaluated slopes and to
// check a planned change still evaluates.
[[nodiscard]] ChannelKeyframes evaluated_view(
    const ParsedChannel& channel, const std::vector<ParsedKey>& keys) {
    ChannelKeyframes view{};
    view.target = channel.target;
    view.component = channel.property;
    for (const ParsedKey& key : keys) {
        view.frames.push_back(key.frame);
        view.values.push_back(key.value);
        view.interpolations.push_back(key.interpolation);
        if (key.interpolation != KeyInterpolation::Cubic) {
            view.tangents.emplace_back();
            continue;
        }
        KeyTangent tangent{};
        tangent.mode = carried_mode(key);
        if (tangent.mode != KeyTangentMode::Auto) {
            tangent.in_weight = carried_number(key.tangent[in_weight_slot]);
            tangent.out_weight = carried_number(key.tangent[out_weight_slot]);
        }
        if (tangent.mode == KeyTangentMode::Aligned
            || tangent.mode == KeyTangentMode::Broken) {
            tangent.in_slope =
                carried_number(key.tangent[in_slope_slot]).value_or(0.0);
            tangent.out_slope =
                carried_number(key.tangent[out_slope_slot]).value_or(0.0);
        }
        if (tangent.mode == KeyTangentMode::Aligned) {
            const auto in = carried_number(key.tangent[in_slope_slot]);
            const auto out = carried_number(key.tangent[out_slope_slot]);
            const double shared = in.has_value()
                ? (out.has_value() ? std::midpoint(*in, *out) : *in)
                : out.value_or(0.0);
            tangent.in_slope = shared;
            tangent.out_slope = shared;
        }
        view.tangents.emplace_back(tangent);
    }
    return view;
}

[[nodiscard]] bool evaluates(const ParsedChannel& channel,
                             const std::vector<ParsedKey>& keys) {
    const ChannelKeyframes view = evaluated_view(channel, keys);
    return !view.frames.empty()
        && sample_keyframes(view, view.frames.front()).has_value();
}

void make_cubic(ParsedKey& key) {
    key.interpolation = KeyInterpolation::Cubic;
    key.interpolation_authored = true;
}

void write_mode(ParsedKey& key, std::string_view mode) {
    key.tangent[tangent_mode_slot] = doc::PropertyValue{std::string{mode}};
}

// Keeps a Cubic, non-Auto key's authored weights; anything else has none.
void keep_weights_only(ParsedKey& key) {
    const bool kept = key.interpolation == KeyInterpolation::Cubic
        && carried_mode(key) != KeyTangentMode::Auto;
    const auto in = key.tangent[in_weight_slot];
    const auto out = key.tangent[out_weight_slot];
    key.tangent = {};
    if (kept) {
        key.tangent[in_weight_slot] = in;
        key.tangent[out_weight_slot] = out;
    }
}

void apply_ease(ParsedKey& key, KeyEase ease, KeySlopes slopes) {
    switch (ease) {
    case KeyEase::Linear:
        key.interpolation = KeyInterpolation::Linear;
        key.interpolation_authored = true;
        key.tangent = {};
        return;
    case KeyEase::Hold:
        key.interpolation = KeyInterpolation::Hold;
        key.interpolation_authored = true;
        key.tangent = {};
        return;
    case KeyEase::Auto:
        make_cubic(key);
        key.tangent = {};
        return;
    case KeyEase::EasyEase:
    case KeyEase::Flat:
        keep_weights_only(key);
        make_cubic(key);
        write_mode(key, schema::tangent_flat);
        return;
    case KeyEase::EaseIn:
        keep_weights_only(key);
        make_cubic(key);
        write_mode(key, schema::tangent_broken);
        key.tangent[in_slope_slot] = doc::PropertyValue{0.0};
        key.tangent[out_slope_slot] = doc::PropertyValue{slopes.out};
        return;
    case KeyEase::EaseOut:
        keep_weights_only(key);
        make_cubic(key);
        write_mode(key, schema::tangent_broken);
        key.tangent[in_slope_slot] = doc::PropertyValue{slopes.in};
        key.tangent[out_slope_slot] = doc::PropertyValue{0.0};
        return;
    case KeyEase::Aligned: {
        keep_weights_only(key);
        write_mode(key, schema::tangent_aligned);
        const double shared = std::midpoint(slopes.in, slopes.out);
        key.tangent[in_slope_slot] = doc::PropertyValue{shared};
        key.tangent[out_slope_slot] = doc::PropertyValue{shared};
        return;
    }
    case KeyEase::Broken:
        keep_weights_only(key);
        write_mode(key, schema::tangent_broken);
        key.tangent[in_slope_slot] = doc::PropertyValue{slopes.in};
        key.tangent[out_slope_slot] = doc::PropertyValue{slopes.out};
        return;
    }
}

[[nodiscard]] bool mode_only(KeyEase ease) noexcept {
    return ease == KeyEase::Aligned || ease == KeyEase::Broken
        || ease == KeyEase::Flat;
}

[[nodiscard]] bool eases_in(KeyEase ease) noexcept {
    return ease == KeyEase::EasyEase || ease == KeyEase::EaseIn;
}

[[nodiscard]] bool valid_tangent(const KeyTangent& tangent) noexcept {
    const auto weight_ok = [](const std::optional<double>& weight) {
        return !weight.has_value()
            || (std::isfinite(*weight) && *weight > 0.0);
    };
    return std::isfinite(tangent.in_slope) && std::isfinite(tangent.out_slope)
        && weight_ok(tangent.in_weight) && weight_ok(tangent.out_weight)
        && tangent.mode <= KeyTangentMode::Flat;
}

// Applies `change` to the selected keys of each selected channel, keeping a
// channel unchanged (and its keys skipped as InvalidTangent) when the result
// would not evaluate. `change` returns false to skip one key with `skip`.
template <typename Change>
[[nodiscard]] KeyEditPlan make_tangent_plan(const doc::Root& document,
                                            const DocumentIndex& index,
                                            KeySelection selection,
                                            Change&& change) {
    KeyEditPlan plan{};
    plan.report.selected = selection.records.size();
    plan.report.duplicate_inputs = selection.duplicate_inputs;
    plan.report.skips = std::move(selection.skips);

    std::map<ChannelIdentity, std::set<core::i64>> by_channel;
    for (const KeyRecord& record : selection.records) {
        by_channel[ChannelIdentity{record.address.target,
                                   record.address.property}]
            .insert(record.address.frame);
    }
    Mutations mutations;
    std::map<KeyAddress, KeyRecord> refreshed;
    for (const auto& [identity, frames] : by_channel) {
        const ParsedChannel* const channel = index.channel(identity);
        if (channel == nullptr) {
            continue;
        }
        const ChannelKeyframes before = evaluated_view(*channel, channel->keys);
        std::vector<ParsedKey> working = channel->keys;
        std::vector<std::size_t> changed;
        for (std::size_t key = 0U; key < working.size(); ++key) {
            if (!frames.contains(working[key].frame)) {
                continue;
            }
            const KeyAddress address{channel->target, channel->property,
                                     working[key].frame};
            KeyEditSkipReason skip{KeyEditSkipReason::NotCubic};
            const KeySlopes slopes =
                evaluated_key_slopes(before, key).value_or(KeySlopes{});
            if (!change(working, key, frames, slopes, skip)) {
                plan.report.skips.push_back(KeyEditSkip{address, skip});
                continue;
            }
            changed.push_back(key);
        }
        if (changed.empty()) {
            continue;
        }
        if (evaluates(*channel, channel->keys)
            && !evaluates(*channel, working)) {
            for (const std::size_t key : changed) {
                plan.report.skips.push_back(KeyEditSkip{
                    KeyAddress{channel->target, channel->property,
                               working[key].frame},
                    KeyEditSkipReason::InvalidTangent});
            }
            continue;
        }
        for (const std::size_t key : changed) {
            const KeyRecord record = make_record(*channel, working[key]);
            refreshed.insert_or_assign(record.address, record);
        }
        plan.report.applied += changed.size();
        record_channel_mutation(index, channel->node, working, mutations);
    }
    for (const KeyRecord& record : selection.records) {
        const auto found = refreshed.find(record.address);
        plan.selection_after.push_back(
            found == refreshed.end() ? record : found->second);
    }
    sort_report(plan);
    finalize_plan(document, std::move(mutations), plan);
    return plan;
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

KeyEditResult plan_key_ease(const doc::Root& document,
                            std::span<const KeyAddress> selection,
                            KeyEase ease) {
    try {
        const DocumentIndex index{document};
        return KeyEditResult::success(make_tangent_plan(
            document, index, inspect_selection(index, selection),
            [ease](std::vector<ParsedKey>& keys, std::size_t key,
                   const std::set<core::i64>& selected, KeySlopes slopes,
                   KeyEditSkipReason& skip) {
                if (mode_only(ease)
                    && keys[key].interpolation != KeyInterpolation::Cubic) {
                    skip = KeyEditSkipReason::NotCubic;
                    return false;
                }
                apply_ease(keys[key], ease, slopes);
                if (eases_in(ease) && key > 0U
                    && !selected.contains(keys[key - 1U].frame)
                    && keys[key - 1U].interpolation
                        == KeyInterpolation::Linear) {
                    apply_ease(keys[key - 1U], KeyEase::Auto, {});
                }
                return true;
            }));
    } catch (const std::bad_alloc&) {
        return KeyEditResult::failure(KeyEditError::AllocationFailed);
    }
}

KeyEditResult plan_key_tangent(const doc::Root& document,
                               const KeyAddress& address,
                               const KeyTangent& tangent) {
    if (!valid_tangent(tangent)) {
        return KeyEditResult::failure(KeyEditError::InvalidTangent);
    }
    try {
        const DocumentIndex index{document};
        return KeyEditResult::success(make_tangent_plan(
            document, index, inspect_selection(index, {&address, 1U}),
            [&tangent](std::vector<ParsedKey>& keys, std::size_t key,
                       const std::set<core::i64>&, KeySlopes,
                       KeyEditSkipReason&) {
                ParsedKey& edited = keys[key];
                make_cubic(edited);
                edited.tangent = {};
                if (tangent.mode == KeyTangentMode::Auto) {
                    return true;
                }
                switch (tangent.mode) {
                case KeyTangentMode::Aligned: {
                    write_mode(edited, schema::tangent_aligned);
                    const double shared =
                        std::midpoint(tangent.in_slope, tangent.out_slope);
                    edited.tangent[in_slope_slot] = doc::PropertyValue{shared};
                    edited.tangent[out_slope_slot] = doc::PropertyValue{shared};
                    break;
                }
                case KeyTangentMode::Broken:
                    write_mode(edited, schema::tangent_broken);
                    edited.tangent[in_slope_slot] =
                        doc::PropertyValue{tangent.in_slope};
                    edited.tangent[out_slope_slot] =
                        doc::PropertyValue{tangent.out_slope};
                    break;
                case KeyTangentMode::Flat:
                case KeyTangentMode::Auto:
                    write_mode(edited, schema::tangent_flat);
                    break;
                }
                if (tangent.in_weight.has_value()) {
                    edited.tangent[in_weight_slot] =
                        doc::PropertyValue{*tangent.in_weight};
                }
                if (tangent.out_weight.has_value()) {
                    edited.tangent[out_weight_slot] =
                        doc::PropertyValue{*tangent.out_weight};
                }
                return true;
            }));
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
