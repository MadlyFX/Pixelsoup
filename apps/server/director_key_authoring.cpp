#include "director_key_authoring.hpp"

#include <mediaserver/doc/node.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <variant>

namespace mediaserver::app {

void append_key_target_report(std::string &message, std::string_view label,
                              std::span<const doc::NodeId> targets) {
    if (targets.empty()) {
        return;
    }
    message.append("; ");
    message.append(label);
    message.push_back(' ');
    constexpr std::size_t visible_limit = 4U;
    const std::size_t visible = std::min(targets.size(), visible_limit);
    for (std::size_t index = 0U; index < visible; ++index) {
        if (index != 0U) {
            message.push_back(',');
        }
        message.append(std::to_string(targets[index].value()));
    }
    if (targets.size() > visible) {
        message.append(" +");
        message.append(std::to_string(targets.size() - visible));
    }
}

void append_key_incompatible_report(std::string &message,
                                    std::span<const KeyAddress> incompatible) {
    if (incompatible.empty()) {
        return;
    }
    message.append("; INCOMPATIBLE ");
    constexpr std::size_t visible_limit = 4U;
    const std::size_t visible = std::min(incompatible.size(), visible_limit);
    for (std::size_t index = 0U; index < visible; ++index) {
        if (index != 0U) {
            message.push_back(',');
        }
        message.append(std::to_string(incompatible[index].target.value()));
        message.push_back('@');
        message.append(std::to_string(incompatible[index].frame));
    }
    if (incompatible.size() > visible) {
        message.append(" +");
        message.append(std::to_string(incompatible.size() - visible));
    }
}

[[nodiscard]] std::string
key_operation_message(std::string_view operation, std::size_t accepted,
                      std::string_view accepted_label,
                      std::span<const doc::NodeId> locked,
                      std::span<const KeyAddress> incompatible) {
    std::string message{"KEY "};
    message.append(operation);
    message.append(" - ");
    message.append(std::to_string(accepted));
    message.push_back(' ');
    message.append(accepted_label);
    append_key_target_report(message, "LOCKED", locked);
    append_key_incompatible_report(message, incompatible);
    return message;
}

[[nodiscard]] std::vector<KeyAddress>
key_addresses(std::span<const editor::TimelineKeyAddress> addresses) {
    std::vector<KeyAddress> converted;
    converted.reserve(addresses.size());
    for (const editor::TimelineKeyAddress &address : addresses) {
        converted.push_back(
            KeyAddress{address.target, address.property, address.frame});
    }
    return converted;
}

[[nodiscard]] std::vector<scene::KeyAddress>
scene_key_addresses(std::span<const editor::TimelineKeyAddress> addresses) {
    std::vector<scene::KeyAddress> converted;
    converted.reserve(addresses.size());
    for (const editor::TimelineKeyAddress &address : addresses) {
        converted.push_back(
            scene::KeyAddress{address.target, address.property, address.frame});
    }
    return converted;
}

[[nodiscard]] std::string
key_batch_message(editor::TimelineKeyBatchAction action,
                  const scene::KeyEditReport &report) {
    std::string_view operation = "MOVE";
    switch (action) {
    case editor::TimelineKeyBatchAction::Move:
        operation = "MOVE";
        break;
    case editor::TimelineKeyBatchAction::Duplicate:
        operation = "DUPLICATE";
        break;
    case editor::TimelineKeyBatchAction::Delete:
        operation = "DELETE";
        break;
    case editor::TimelineKeyBatchAction::Scale:
        operation = "SCALE";
        break;
    case editor::TimelineKeyBatchAction::Ease:
        operation = "EASE";
        break;
    case editor::TimelineKeyBatchAction::Tangent:
        operation = "TANGENT";
        break;
    }
    std::string message{"KEY "};
    message.append(operation);
    message.append(" - ");
    message.append(std::to_string(report.applied));
    message.append(" APPLIED");
    if (!report.collisions.empty()) {
        message.append(" / ");
        message.append(std::to_string(report.collisions.size()));
        message.append(" COLLISIONS");
    }
    if (!report.skips.empty()) {
        message.append(" / ");
        message.append(std::to_string(report.skips.size()));
        message.append(" SKIPPED");
        const auto count = [&](scene::KeyEditSkipReason reason) {
            return static_cast<std::size_t>(std::count_if(
                report.skips.begin(), report.skips.end(),
                [reason](const scene::KeyEditSkip &skip) {
                    return skip.reason == reason;
                }));
        };
        const auto detail = [&](scene::KeyEditSkipReason reason,
                                std::string_view label) {
            if (const std::size_t skipped = count(reason); skipped != 0U) {
                message.append(" (");
                message.append(std::to_string(skipped));
                message.push_back(' ');
                message.append(label);
                message.push_back(')');
            }
        };
        detail(scene::KeyEditSkipReason::TargetLocked, "LOCKED");
        detail(scene::KeyEditSkipReason::NotCubic, "NOT BEZIER");
        detail(scene::KeyEditSkipReason::InvalidTangent,
               "HANDLES TOO LONG");
    }
    return message;
}

// A deterministic node id for the channel driving one typed property of
// `target`. Deterministic so keying finds-or-creates a channel without scanning
// the document. Preserve the original low-nibble address space for the first
// sixteen channels, while assigning later channels to a disjoint high-bit
// prefix with five component bits.
[[nodiscard]] doc::Node make_channel_node(doc::NodeId id, doc::NodeId target,
                                          std::string_view component) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{scene::schema::channel_type};
    node.properties.insert_or_assign(
        std::string{scene::schema::channel_target},
        doc::PropertyValue{static_cast<std::int64_t>(target.value())});
    node.properties.insert_or_assign(
        std::string{scene::schema::channel_component},
        doc::PropertyValue{std::string{component}});
    return node;
}

// Inserts or updates the key at `frame` in a channel node's indexed key
// properties. Keys are kept contiguous, so the first missing index is the next
// free slot and a matching frame just has its value replaced.
void upsert_channel_key(doc::Node &node, core::u64 frame, double value) {
    const std::string prefix{scene::schema::channel_key_prefix};
    const std::string frame_suffix{scene::schema::channel_key_frame_suffix};
    const std::string value_suffix{scene::schema::channel_key_value_suffix};
    core::u64 next_index = 0U;
    for (core::u64 i = 0U;; ++i) {
        const auto found =
            node.properties.find(prefix + std::to_string(i) + frame_suffix);
        if (found == node.properties.end()) {
            next_index = i;
            break;
        }
        const auto *const held = std::get_if<std::int64_t>(&found->second);
        if (held != nullptr && *held == static_cast<std::int64_t>(frame)) {
            node.properties.insert_or_assign(prefix + std::to_string(i) +
                                                 value_suffix,
                                             doc::PropertyValue{value});
            return;
        }
    }
    const auto index = std::to_string(next_index);
    node.properties.insert_or_assign(
        prefix + index + frame_suffix,
        doc::PropertyValue{static_cast<std::int64_t>(frame)});
    node.properties.insert_or_assign(prefix + index + value_suffix,
                                     doc::PropertyValue{value});
    node.properties.insert_or_assign(
        prefix + index + std::string{scene::schema::channel_key_interp_suffix},
        doc::PropertyValue{std::string{scene::schema::interp_linear}});
}

// Removes one indexed key and compacts every later key down by one slot. The
// channel schema stops parsing at its first missing index, so leaving a hole
// would silently hide all later animation from every node.
[[nodiscard]] bool erase_channel_key(doc::Node &node, core::u64 frame) {
    constexpr core::u64 maximum =
        static_cast<core::u64>(std::numeric_limits<core::i64>::max());
    if (frame > maximum) {
        return false;
    }
    const auto wanted = static_cast<std::int64_t>(frame);
    const std::string prefix{scene::schema::channel_key_prefix};
    const std::string frame_suffix{scene::schema::channel_key_frame_suffix};
    const std::string value_suffix{scene::schema::channel_key_value_suffix};
    const std::string interp_suffix{scene::schema::channel_key_interp_suffix};

    core::u64 count = 0U;
    core::u64 remove_at = std::numeric_limits<core::u64>::max();
    for (core::u64 index = 0U;; ++index) {
        const auto found =
            node.properties.find(prefix + std::to_string(index) + frame_suffix);
        if (found == node.properties.end()) {
            count = index;
            break;
        }
        const auto *const held = std::get_if<std::int64_t>(&found->second);
        if (held != nullptr && *held == wanted &&
            remove_at == std::numeric_limits<core::u64>::max()) {
            remove_at = index;
        }
    }
    if (remove_at == std::numeric_limits<core::u64>::max()) {
        return false;
    }

    // Every per-key property moves down with its key, tangents included, so
    // no later key inherits another's handles.
    std::vector<std::string_view> suffixes{frame_suffix, value_suffix,
                                           interp_suffix};
    suffixes.insert(suffixes.end(),
                    scene::schema::channel_key_tangent_suffixes.begin(),
                    scene::schema::channel_key_tangent_suffixes.end());
    for (core::u64 index = remove_at; index + 1U < count; ++index) {
        for (const std::string_view suffix : suffixes) {
            const std::string destination =
                prefix + std::to_string(index) + std::string{suffix};
            const std::string source =
                prefix + std::to_string(index + 1U) + std::string{suffix};
            const auto found = node.properties.find(source);
            if (found != node.properties.end()) {
                node.properties.insert_or_assign(destination, found->second);
            } else {
                node.properties.erase(destination);
            }
        }
    }
    const std::string last_index = std::to_string(count - 1U);
    for (const std::string_view suffix : suffixes) {
        node.properties.erase(prefix + last_index + std::string{suffix});
    }
    return true;
}

// Finds the key at `from` in a channel node's indexed properties and sets its
// frame to `to`. Returns false when there is no key at that frame. Value and
// interp stay; only the frame moves, which the model re-sorts on parse.
[[nodiscard]] bool retime_channel_key(doc::Node &node, core::i64 from,
                                      core::i64 to) {
    const std::string prefix{scene::schema::channel_key_prefix};
    const std::string frame_suffix{scene::schema::channel_key_frame_suffix};
    for (core::u64 i = 0U;; ++i) {
        const auto name = prefix + std::to_string(i) + frame_suffix;
        const auto found = node.properties.find(name);
        if (found == node.properties.end()) {
            return false;
        }
        const auto *const held = std::get_if<std::int64_t>(&found->second);
        if (held != nullptr && *held == from) {
            node.properties.insert_or_assign(
                name, doc::PropertyValue{static_cast<std::int64_t>(to)});
            return true;
        }
    }
}

// The node a particle parameter channel of `target` lives in. Derived from
// (target, key) so keying finds-or-creates it without scanning the document,
// in an id prefix no enum channel uses. Sixteen ids are tried, the matching
// channel first, then the first free one, so a hash collision moves on rather
// than overwriting another channel.
[[nodiscard]] std::optional<doc::NodeId> parameter_channel_node_id(
    const doc::Root &root, doc::NodeId target, std::string_view key) {
    core::u64 hash = 0xcbf29ce484222325ULL;
    for (core::u32 shift = 0U; shift < 64U; shift += 8U) {
        hash = (hash ^ ((target.value() >> shift) & 0xffU)) * 0x100000001b3ULL;
    }
    for (const char c : key) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 0x100000001b3ULL;
    }
    constexpr core::u64 parameter_channel_id_base = 0x7E00'0000'0000'0000ULL;
    const core::u64 base = parameter_channel_id_base | (hash & 0x00FF'FFFF'FFFF'FFF0ULL);
    const std::string component = std::string{scene::schema::particle_parameter_prefix} + std::string{key};
    std::optional<doc::NodeId> free;
    for (core::u64 probe = 0U; probe < 16U; ++probe) {
        const doc::NodeId id{base | probe};
        const doc::Node *const node = root.get(id);
        if (node == nullptr) {
            if (!free) free = id;
            continue;
        }
        const auto held_target = node->properties.find(scene::schema::channel_target);
        const auto held_component = node->properties.find(scene::schema::channel_component);
        if (node->type == scene::schema::channel_type && held_target != node->properties.end()
            && held_component != node->properties.end()
            && std::get_if<std::int64_t>(&held_target->second) != nullptr
            && *std::get_if<std::int64_t>(&held_target->second) == static_cast<std::int64_t>(target.value())
            && std::get_if<std::string>(&held_component->second) != nullptr
            && *std::get_if<std::string>(&held_component->second) == component) {
            return id;
        }
    }
    return free;
}

// Authors one keyframe on a single transform component: create-or-update the
// component's channel with a key at the frame, or clear the channel. On the
// same scene undo context as moves; the gesture groups a multi-component key.
[[nodiscard]] bool schedule_key_edit(cmd::DirectorScheduler &scheduler,
                                     const editor::KeyIntent &key,
                                     cmd::FrameNumber apply_at,
                                     EditCoalescingPlan *coalescing) {
    const auto schedule = [&](cmd::Payload forward, cmd::Payload inverse) {
        if (coalescing != nullptr) {
            return schedule_editor_coalesced_edit(scheduler, std::move(forward),
                                                  std::move(inverse),
                                                  *coalescing, apply_at);
        }
        return scheduler.schedule_edit(
            std::move(forward), std::move(inverse), cmd::UndoContextId{2U},
            cmd::TransactionId{key.gesture}, apply_at);
    };
    const bool parameter = key.component == scene::PropertyChannel::ParticleParameter;
    if (parameter && !scene::parse_particle_parameter_key(key.parameter)) {
        return true;
    }
    const std::optional<doc::NodeId> parameter_id = parameter
        ? parameter_channel_node_id(scheduler.projected_document(), key.target, key.parameter)
        : std::nullopt;
    if (parameter && !parameter_id) {
        return true;
    }
    const auto id = parameter
        ? *parameter_id
        : channel_node_id(key.target, static_cast<core::u64>(key.component));
    const auto *const existing = scheduler.projected_document().get(id);
    if (key.clear) {
        if (existing == nullptr) {
            return true;
        }
        auto scheduled = schedule(cmd::EraseNode{id}, cmd::PutNode{*existing});
        return static_cast<bool>(scheduled);
    }
    if (key.erase) {
        if (existing == nullptr) {
            return true;
        }
        doc::Node node = *existing;
        if (!erase_channel_key(node, key.frame)) {
            return true;
        }
        const std::string first_frame =
            std::string{scene::schema::channel_key_prefix} + "0" +
            std::string{scene::schema::channel_key_frame_suffix};
        cmd::Payload payload = node.properties.contains(first_frame)
                                   ? cmd::Payload{cmd::PutNode{std::move(node)}}
                                   : cmd::Payload{cmd::EraseNode{id}};
        auto scheduled = schedule(std::move(payload), cmd::PutNode{*existing});
        return static_cast<bool>(scheduled);
    }
    doc::Node node = existing != nullptr
                         ? *existing
                         : make_channel_node(id, key.target,
                                             parameter
                                                 ? std::string{scene::schema::particle_parameter_prefix} + key.parameter
                                                 : std::string{transform_property(key.component)});
    upsert_channel_key(node, key.frame, key.value);
    cmd::Payload inverse = existing != nullptr
                               ? cmd::Payload{cmd::PutNode{*existing}}
                               : cmd::Payload{cmd::EraseNode{id}};
    auto scheduled =
        schedule(cmd::PutNode{std::move(node)}, std::move(inverse));
    return static_cast<bool>(scheduled);
}

// Authors a retime: move the key at `from_frame` on (target, component) to
// `to_frame`. A missing channel or key is a no-op, not a failure.
[[nodiscard]] bool schedule_key_move(cmd::DirectorScheduler &scheduler,
                                     const editor::KeyMove &move,
                                     cmd::FrameNumber apply_at) {
    const auto id =
        channel_node_id(move.target, static_cast<core::u64>(move.component));
    const auto *const existing = scheduler.projected_document().get(id);
    if (existing == nullptr) {
        return true;
    }
    doc::Node node = *existing;
    if (!retime_channel_key(node, move.from_frame, move.to_frame)) {
        return true;
    }
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{std::move(node)}, cmd::PutNode{*existing},
        cmd::UndoContextId{2U}, cmd::TransactionId{move.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

// The document property a transform channel maps to. Position/rotation/scale
// reuse the object transform names, so a channel and a base edit name the same
// property.
[[nodiscard]] std::string_view
transform_property(scene::TransformChannel component) noexcept {
    switch (component) {
    case scene::TransformChannel::PositionX:
        return scene::schema::position_x;
    case scene::TransformChannel::PositionY:
        return scene::schema::position_y;
    case scene::TransformChannel::PositionZ:
        return scene::schema::position_z;
    case scene::TransformChannel::RotationYaw:
        return scene::schema::rotation_yaw;
    case scene::TransformChannel::RotationPitch:
        return scene::schema::rotation_pitch;
    case scene::TransformChannel::RotationRoll:
        return scene::schema::rotation_roll;
    case scene::TransformChannel::ScaleX:
        return scene::schema::scale_x;
    case scene::TransformChannel::ScaleY:
        return scene::schema::scale_y;
    case scene::TransformChannel::ScaleZ:
        return scene::schema::scale_z;
    case scene::PropertyChannel::Opacity:
        return scene::schema::opacity;
    case scene::PropertyChannel::Brightness:
        return scene::schema::brightness;
    case scene::PropertyChannel::Saturation:
        return scene::schema::saturation;
    case scene::PropertyChannel::BlurRadius:
        return scene::schema::blur_radius;
    case scene::PropertyChannel::FilmGrainAmount:
        return scene::schema::film_grain_amount;
    case scene::PropertyChannel::BoxBlurRadius:
        return scene::schema::box_blur_radius;
    case scene::PropertyChannel::DirectionalBlurLength:
        return scene::schema::directional_blur_length;
    case scene::PropertyChannel::ChromaKeyMix:
        return scene::schema::chroma_key_mix;
    case scene::PropertyChannel::LumaKeyMix:
        return scene::schema::luma_key_mix;
    case scene::PropertyChannel::EnvironmentIntensity:
        return scene::schema::environment_map_intensity;
    case scene::PropertyChannel::EnvironmentRotation:
        return scene::schema::environment_map_rotation;
    case scene::PropertyChannel::FogDensity:
        return scene::schema::environment_fog_density;
    case scene::PropertyChannel::FogHeight:
        return scene::schema::environment_fog_height;
    case scene::PropertyChannel::FogFalloff:
        return scene::schema::environment_fog_height_falloff;
    case scene::PropertyChannel::FogAnisotropy:
        return scene::schema::environment_fog_anisotropy;
    case scene::PropertyChannel::SkyHaze:
        return scene::schema::sky_haze;
    case scene::PropertyChannel::SkyIlluminance:
        return scene::schema::sky_solar_illuminance;
    case scene::PropertyChannel::SkyTimeOfDay:
        return scene::schema::sky_time_of_day;
    case scene::PropertyChannel::LightIntensity:
        return scene::schema::intensity;
    case scene::PropertyChannel::LightColorR:
        return scene::schema::color_r;
    case scene::PropertyChannel::LightColorG:
        return scene::schema::color_g;
    case scene::PropertyChannel::LightColorB:
        return scene::schema::color_b;
    case scene::PropertyChannel::LightRange:
        return scene::schema::range;
    case scene::PropertyChannel::CameraFieldOfView:
        return scene::schema::fov_turns;
    case scene::PropertyChannel::CameraShutter:
        return scene::schema::shutter_seconds;
    case scene::PropertyChannel::CameraFocusDistance:
        return scene::schema::focus_distance_m;
    case scene::PropertyChannel::CameraAperture:
        return scene::schema::aperture_f_number;
    case scene::TransformChannel::Count:
        break;
    }
    return {};
}

} // namespace mediaserver::app
