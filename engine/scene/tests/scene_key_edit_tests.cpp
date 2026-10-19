#include <mediaserver/scene/key_edit.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;
namespace schema = mediaserver::scene::schema;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define KEY_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] Property whole(std::string_view name, core::i64 value) {
    return {std::string{name}, value};
}

[[nodiscard]] Property flag(std::string_view name, bool value) {
    return {std::string{name}, value};
}

[[nodiscard]] Property text(std::string_view name, std::string_view value) {
    return {std::string{name}, std::string{value}};
}

[[nodiscard]] doc::Node node(
    core::u64 id, std::string_view type,
    std::initializer_list<Property> properties = {}) {
    doc::Node result{};
    result.id = doc::NodeId{id};
    result.type = std::string{type};
    for (const auto& [name, value] : properties) {
        result.properties.emplace(name, value);
    }
    return result;
}

struct AuthoredKey final {
    core::i64 frame{};
    std::optional<doc::PropertyValue> value;
    std::optional<std::string> interpolation;
};

[[nodiscard]] AuthoredKey key(core::i64 frame, core::i64 value,
                              std::string_view interpolation) {
    return AuthoredKey{
        frame, doc::PropertyValue{value}, std::string{interpolation}};
}

[[nodiscard]] AuthoredKey key(core::i64 frame, double value,
                              std::string_view interpolation) {
    return AuthoredKey{
        frame, doc::PropertyValue{value}, std::string{interpolation}};
}

[[nodiscard]] AuthoredKey implicit_key(core::i64 frame) {
    return AuthoredKey{frame, std::nullopt, std::nullopt};
}

[[nodiscard]] doc::Node channel(
    core::u64 id, core::u64 target, std::string_view component,
    const std::vector<AuthoredKey>& keys) {
    doc::Node result = node(
        id, schema::channel_type,
        {whole(schema::channel_target, static_cast<core::i64>(target)),
         text(schema::channel_component, component)});
    for (std::size_t index = 0U; index < keys.size(); ++index) {
        const std::string base = std::string{schema::channel_key_prefix}
            + std::to_string(index);
        result.properties.emplace(
            base + std::string{schema::channel_key_frame_suffix},
            keys[index].frame);
        if (keys[index].value.has_value()) {
            result.properties.emplace(
                base + std::string{schema::channel_key_value_suffix},
                *keys[index].value);
        }
        if (keys[index].interpolation.has_value()) {
            result.properties.emplace(
                base + std::string{schema::channel_key_interp_suffix},
                *keys[index].interpolation);
        }
    }
    return result;
}

[[nodiscard]] doc::Root basic_root() {
    doc::Root root;
    root = root.set(node(1U, schema::stage_type));
    root = root.set(node(
        10U, schema::object_type, {whole(schema::scene_id, 1)}));
    return root;
}

[[nodiscard]] scene::KeyAddress address(
    core::u64 target, scene::PropertyChannel property,
    core::i64 frame) {
    return scene::KeyAddress{doc::NodeId{target}, property, frame};
}

[[nodiscard]] const doc::PropertyValue* property(
    const doc::Node& node, std::string_view name) {
    const auto found = node.properties.find(name);
    return found == node.properties.end() ? nullptr : &found->second;
}

[[nodiscard]] std::string key_name(std::size_t index,
                                   std::string_view suffix) {
    return std::string{schema::channel_key_prefix} + std::to_string(index)
        + std::string{suffix};
}

[[nodiscard]] std::vector<core::i64> frames(const doc::Node& channel_node) {
    std::vector<core::i64> result;
    for (std::size_t index = 0U; index < scene::max_channel_keys; ++index) {
        const auto* const held = property(
            channel_node,
            key_name(index, schema::channel_key_frame_suffix));
        if (held == nullptr) {
            break;
        }
        const auto* const value = std::get_if<core::i64>(held);
        KEY_CHECK(value != nullptr);
        result.push_back(*value);
    }
    return result;
}

[[nodiscard]] std::vector<core::i64> selected_frames(
    const scene::KeyEditPlan& plan) {
    std::vector<core::i64> result;
    for (const scene::KeyRecord& record : plan.selection_after) {
        result.push_back(record.address.frame);
    }
    std::sort(result.begin(), result.end());
    return result;
}

void selection_is_typed_unique_exact_and_lock_aware() {
    doc::Root root = basic_root();
    root = root.set(channel(
        20U, 10U, schema::position_x,
        {key(-4, core::i64{7}, schema::interp_hold), implicit_key(8)}));
    root = root.set(node(
        11U, schema::group_type,
        {whole(schema::scene_id, 1), flag(schema::locked, true)}));
    root = root.set(node(
        12U, schema::media_type,
        {whole(schema::scene_id, 1), whole(schema::parent_group, 11)}));
    root = root.set(channel(
        21U, 12U, schema::position_x,
        {key(0, 1.0, schema::interp_linear)}));
    root = root.set(node(
        30U, schema::timeline_lane_type,
        {whole(schema::scene_id, 1), flag(schema::lane_locked, true)}));
    root = root.set(node(
        13U, schema::media_type,
        {whole(schema::scene_id, 1), whole(schema::lane_id, 30)}));
    root = root.set(channel(
        22U, 13U, schema::position_y,
        {key(0, 2.0, schema::interp_cubic)}));

    const std::array selection{
        address(10U, scene::PropertyChannel::PositionX, 8),
        address(10U, scene::PropertyChannel::PositionX, -4),
        address(10U, scene::PropertyChannel::PositionX, -4),
        address(12U, scene::PropertyChannel::PositionX, 0),
        address(13U, scene::PropertyChannel::PositionY, 0),
        address(99U, scene::PropertyChannel::PositionX, 0),
        address(10U, scene::PropertyChannel::FilmGrainAmount, 0),
    };
    const auto inspected = scene::inspect_key_selection(root, selection);
    KEY_CHECK(inspected.has_value());
    KEY_CHECK(inspected.value().duplicate_inputs == 1U);
    KEY_CHECK(inspected.value().records.size() == 2U);
    KEY_CHECK(inspected.value().records[0U].address.frame == -4);
    KEY_CHECK(inspected.value().records[0U].channel == doc::NodeId{20U});
    KEY_CHECK(inspected.value().records[0U].carrier_authored);
    KEY_CHECK(std::holds_alternative<core::i64>(
        inspected.value().records[0U].carrier));
    KEY_CHECK(inspected.value().records[0U].interpolation
              == scene::KeyInterpolation::Hold);
    KEY_CHECK(inspected.value().records[1U].address.frame == 8);
    KEY_CHECK(!inspected.value().records[1U].carrier_authored);
    KEY_CHECK(!inspected.value().records[1U].interpolation_authored);
    KEY_CHECK(inspected.value().skips.size() == 4U);
    KEY_CHECK(inspected.value().skips[0U].reason
              == scene::KeyEditSkipReason::InvalidTarget);
    KEY_CHECK(inspected.value().skips[1U].reason
              == scene::KeyEditSkipReason::TargetLocked);
    KEY_CHECK(inspected.value().skips[2U].reason
              == scene::KeyEditSkipReason::TargetLocked);
    KEY_CHECK(inspected.value().skips[3U].reason
              == scene::KeyEditSkipReason::TargetMissing);
}

void lowest_valid_channel_wins_and_ambiguous_channels_are_untouched() {
    doc::Root root = basic_root();
    root = root.set(channel(
        20U, 10U, schema::position_x,
        {key(1, 1.0, schema::interp_linear),
         key(1, 2.0, schema::interp_hold)}));
    root = root.set(channel(
        21U, 10U, schema::position_x,
        {key(1, 99.0, schema::interp_cubic)}));
    root = root.set(channel(
        22U, 10U, schema::position_y,
        {key(2, 3.0, schema::interp_linear)}));
    const std::array selection{
        address(10U, scene::PropertyChannel::PositionX, 1),
        address(10U, scene::PropertyChannel::PositionY, 2),
    };
    const auto planned = scene::plan_key_move(root, selection, 4);
    KEY_CHECK(planned.has_value());
    KEY_CHECK(planned.value().report.selected == 1U);
    KEY_CHECK(planned.value().report.skips.size() == 1U);
    KEY_CHECK(planned.value().report.skips.front().reason
              == scene::KeyEditSkipReason::AmbiguousChannel);
    KEY_CHECK(planned.value().after.get(doc::NodeId{20U}) != nullptr);
    KEY_CHECK(*planned.value().after.get(doc::NodeId{20U})
              == *root.get(doc::NodeId{20U}));
    KEY_CHECK(*planned.value().after.get(doc::NodeId{21U})
              == *root.get(doc::NodeId{21U}));
    KEY_CHECK((frames(*planned.value().after.get(doc::NodeId{22U}))
               == std::vector<core::i64>{6}));
}

void camera_light_and_surface_keys_support_batch_edits() {
    doc::Root root = basic_root();
    root = root.set(node(11U, schema::camera_type));
    root = root.set(node(12U, schema::light_type));
    root = root.set(node(13U, "output.surface", {flag(schema::locked, false)}));
    root = root.set(channel(21U, 11U, schema::fov_turns,
                           {key(5, 0.173, schema::interp_linear)}));
    root = root.set(channel(22U, 12U, schema::intensity,
                           {key(5, 17.3, schema::interp_linear)}));
    root = root.set(channel(23U, 13U, schema::position_z,
                           {key(5, -3.7, schema::interp_linear)}));
    const std::array selection{
        address(11U, scene::PropertyChannel::CameraFieldOfView, 5),
        address(12U, scene::PropertyChannel::LightIntensity, 5),
        address(13U, scene::PropertyChannel::PositionZ, 5),
    };
    const auto planned = scene::plan_key_move(root, selection, 9);
    KEY_CHECK(planned.has_value());
    KEY_CHECK(planned.value().report.applied == 3U);
    KEY_CHECK(planned.value().report.skips.empty());
    for (core::u64 channel_id : {21U, 22U, 23U}) {
        KEY_CHECK(frames(*planned.value().after.get(doc::NodeId{channel_id})) ==
                  std::vector<core::i64>{14});
    }

    const auto default_locked = root.set(node(13U, "output.surface"));
    const std::array wall_selection{
        address(13U, scene::PropertyChannel::PositionZ, 5)};
    const auto check_locked = [&](const scene::KeyEditResult& result) {
        KEY_CHECK(result.has_value());
        KEY_CHECK(result.value().report.applied == 0U);
        KEY_CHECK(result.value().report.skips.size() == 1U);
        KEY_CHECK(result.value().report.skips.front().reason ==
                  scene::KeyEditSkipReason::TargetLocked);
        KEY_CHECK(doc::diff(default_locked, result.value().after).changes.empty());
    };
    check_locked(scene::plan_key_move(default_locked, wall_selection, 9));
    check_locked(scene::plan_key_duplicate(default_locked, wall_selection, 9));
    check_locked(scene::plan_key_delete(default_locked, wall_selection));
}

void move_is_atomic_sorted_and_replaces_stationary_destinations() {
    doc::Root root = basic_root();
    root = root.set(channel(
        20U, 10U, schema::position_x,
        {key(-5, core::i64{2}, schema::interp_hold),
         key(0, 3.0, schema::interp_linear), implicit_key(5)}));
    root = root.set(channel(
        19U, 10U, schema::position_y,
        {key(-7, 4.0, schema::interp_cubic)}));
    const std::array selection{
        address(10U, scene::PropertyChannel::PositionX, -5),
        address(10U, scene::PropertyChannel::PositionX, 0),
        address(10U, scene::PropertyChannel::PositionY, -7),
    };
    const auto planned = scene::plan_key_move(root, selection, 5);
    KEY_CHECK(planned.has_value());
    const auto& plan = planned.value();
    KEY_CHECK(plan.report.selected == 3U);
    KEY_CHECK(plan.report.applied == 3U);
    KEY_CHECK(plan.puts.size() == 2U);
    KEY_CHECK(plan.puts[0U].id == doc::NodeId{19U});
    KEY_CHECK(plan.puts[1U].id == doc::NodeId{20U});
    KEY_CHECK(plan.report.collisions.size() == 1U);
    KEY_CHECK(plan.report.collisions.front().stationary_loser);
    KEY_CHECK(plan.report.collisions.front().destination_frame == 5);
    const doc::Node& moved = *plan.after.get(doc::NodeId{20U});
    KEY_CHECK((frames(moved) == std::vector<core::i64>{0, 5}));
    const auto* first_value = property(
        moved, key_name(0U, schema::channel_key_value_suffix));
    KEY_CHECK(first_value != nullptr);
    KEY_CHECK(std::get<core::i64>(*first_value) == 2);
    KEY_CHECK(std::get<std::string>(*property(
                  moved, key_name(0U, schema::channel_key_interp_suffix)))
              == schema::interp_hold);
    KEY_CHECK(std::get<double>(*property(
                  moved, key_name(1U, schema::channel_key_value_suffix)))
              == 3.0);
}

void duplicate_preserves_sources_and_reports_capacity_and_collisions() {
    doc::Root root = basic_root();
    root = root.set(channel(
        20U, 10U, schema::position_x,
        {key(0, 1.0, schema::interp_hold),
         key(10, 2.0, schema::interp_cubic)}));
    std::vector<AuthoredKey> full;
    for (core::i64 frame = 0; frame < scene::max_channel_keys; ++frame) {
        full.push_back(key(frame, static_cast<double>(frame),
                           schema::interp_linear));
    }
    root = root.set(channel(21U, 10U, schema::position_y, full));
    root = root.set(channel(
        22U, 10U, schema::position_z,
        {key(-5, 8.0, schema::interp_linear)}));
    const std::array selection{
        address(10U, scene::PropertyChannel::PositionX, 0),
        address(10U, scene::PropertyChannel::PositionY, 63),
        address(10U, scene::PropertyChannel::PositionZ, -5),
    };
    const auto planned = scene::plan_key_duplicate(root, selection, 10);
    KEY_CHECK(planned.has_value());
    const auto& plan = planned.value();
    KEY_CHECK(plan.report.selected == 3U);
    KEY_CHECK(plan.report.applied == 2U);
    KEY_CHECK(plan.report.skips.size() == 1U);
    KEY_CHECK(plan.report.skips.front().reason
              == scene::KeyEditSkipReason::ChannelCapacity);
    KEY_CHECK(plan.report.collisions.size() == 1U);
    KEY_CHECK(plan.report.collisions.front().stationary_loser);
    KEY_CHECK((frames(*plan.after.get(doc::NodeId{20U}))
               == std::vector<core::i64>{0, 10}));
    KEY_CHECK(*plan.after.get(doc::NodeId{21U})
              == *root.get(doc::NodeId{21U}));
    KEY_CHECK((frames(*plan.after.get(doc::NodeId{22U}))
               == std::vector<core::i64>{-5, 5}));
    KEY_CHECK((selected_frames(plan) == std::vector<core::i64>{5, 10}));
}

void delete_erases_empty_channels_and_reindexes_other_channels() {
    doc::Root root = basic_root();
    root = root.set(channel(
        20U, 10U, schema::position_x,
        {key(0, 1.0, schema::interp_linear)}));
    root = root.set(channel(
        19U, 10U, schema::position_y,
        {key(0, 2.0, schema::interp_hold),
         key(10, 3.0, schema::interp_cubic)}));
    const std::array selection{
        address(10U, scene::PropertyChannel::PositionX, 0),
        address(10U, scene::PropertyChannel::PositionY, 0),
    };
    const auto planned = scene::plan_key_delete(root, selection);
    KEY_CHECK(planned.has_value());
    const auto& plan = planned.value();
    KEY_CHECK(plan.report.applied == 2U);
    KEY_CHECK((plan.erases
               == std::vector<doc::NodeId>{doc::NodeId{20U}}));
    KEY_CHECK(plan.puts.size() == 1U);
    KEY_CHECK(plan.puts.front().id == doc::NodeId{19U});
    KEY_CHECK(plan.after.get(doc::NodeId{20U}) == nullptr);
    KEY_CHECK((frames(*plan.after.get(doc::NodeId{19U}))
               == std::vector<core::i64>{10}));
    KEY_CHECK(plan.selection_after.empty());
}

void rational_scaling_supports_every_anchor_negative_time_and_mirroring() {
    doc::Root root = basic_root();
    root = root.set(channel(
        20U, 10U, schema::position_x,
        {key(-3, -3.0, schema::interp_hold),
         key(3, 3.0, schema::interp_cubic)}));
    const std::array both{
        address(10U, scene::PropertyChannel::PositionX, -3),
        address(10U, scene::PropertyChannel::PositionX, 3),
    };
    const auto rounded = scene::plan_key_scale(
        root, both,
        scene::KeyTimeScale{1, 2, scene::KeyTimeAnchor::User, 0, 0});
    KEY_CHECK(rounded.has_value());
    KEY_CHECK((selected_frames(rounded.value())
               == std::vector<core::i64>{-2, 2}));
    const auto mirrored = scene::plan_key_scale(
        root, both,
        scene::KeyTimeScale{-1, 1, scene::KeyTimeAnchor::User, 0, 0});
    KEY_CHECK(mirrored.has_value());
    KEY_CHECK((selected_frames(mirrored.value())
               == std::vector<core::i64>{-3, 3}));
    const doc::Node& mirrored_channel =
        *mirrored.value().after.get(doc::NodeId{20U});
    KEY_CHECK(std::get<double>(*property(
                  mirrored_channel,
                  key_name(0U, schema::channel_key_value_suffix)))
              == 3.0);
    KEY_CHECK(std::get<double>(*property(
                  mirrored_channel,
                  key_name(1U, schema::channel_key_value_suffix)))
              == -3.0);

    doc::Root anchors = basic_root();
    anchors = anchors.set(channel(
        20U, 10U, schema::position_x,
        {key(10, 1.0, schema::interp_linear),
         key(20, 2.0, schema::interp_linear)}));
    const std::array anchor_selection{
        address(10U, scene::PropertyChannel::PositionX, 10),
        address(10U, scene::PropertyChannel::PositionX, 20),
    };
    const auto first = scene::plan_key_scale(
        anchors, anchor_selection,
        scene::KeyTimeScale{2, 1, scene::KeyTimeAnchor::FirstSelected});
    const auto last = scene::plan_key_scale(
        anchors, anchor_selection,
        scene::KeyTimeScale{2, 1, scene::KeyTimeAnchor::LastSelected});
    const auto playhead = scene::plan_key_scale(
        anchors, anchor_selection,
        scene::KeyTimeScale{2, 1, scene::KeyTimeAnchor::Playhead, 15});
    const auto user = scene::plan_key_scale(
        anchors, anchor_selection,
        scene::KeyTimeScale{2, 1, scene::KeyTimeAnchor::User, 0, 0});
    KEY_CHECK(first.has_value() && last.has_value() && playhead.has_value()
              && user.has_value());
    KEY_CHECK((selected_frames(first.value())
               == std::vector<core::i64>{10, 30}));
    KEY_CHECK((selected_frames(last.value())
               == std::vector<core::i64>{0, 20}));
    KEY_CHECK((selected_frames(playhead.value())
               == std::vector<core::i64>{5, 25}));
    KEY_CHECK((selected_frames(user.value())
               == std::vector<core::i64>{20, 40}));
}

void collapsed_scaling_uses_the_latest_original_key_as_winner() {
    doc::Root root = basic_root();
    root = root.set(channel(
        20U, 10U, schema::position_x,
        {key(0, 1.0, schema::interp_hold),
         key(10, 2.0, schema::interp_linear),
         key(20, 3.0, schema::interp_cubic)}));
    const std::array selection{
        address(10U, scene::PropertyChannel::PositionX, 0),
        address(10U, scene::PropertyChannel::PositionX, 10),
        address(10U, scene::PropertyChannel::PositionX, 20),
    };
    const auto planned = scene::plan_key_scale(
        root, selection,
        scene::KeyTimeScale{0, 1, scene::KeyTimeAnchor::Playhead, 7});
    KEY_CHECK(planned.has_value());
    const auto& plan = planned.value();
    KEY_CHECK(plan.report.selected == 3U);
    KEY_CHECK(plan.report.applied == 1U);
    KEY_CHECK(plan.report.collisions.size() == 2U);
    KEY_CHECK(plan.report.collisions[0U].winner.frame == 20);
    KEY_CHECK(plan.report.collisions[1U].winner.frame == 20);
    KEY_CHECK((frames(*plan.after.get(doc::NodeId{20U}))
               == std::vector<core::i64>{7}));
    KEY_CHECK(std::get<double>(*property(
                  *plan.after.get(doc::NodeId{20U}),
                  key_name(0U, schema::channel_key_value_suffix)))
              == 3.0);
    KEY_CHECK(plan.selection_after.front().address.frame == 7);
    KEY_CHECK(plan.selection_after.front().interpolation
              == scene::KeyInterpolation::Cubic);
}

void full_span_identity_and_overflow_failures_are_exact_and_atomic() {
    constexpr core::i64 minimum = std::numeric_limits<core::i64>::min();
    constexpr core::i64 maximum = std::numeric_limits<core::i64>::max();
    doc::Root root = basic_root();
    root = root.set(channel(
        20U, 10U, schema::position_x,
        {key(minimum, 1.0, schema::interp_linear),
         key(maximum, 2.0, schema::interp_linear)}));
    const std::array selection{
        address(10U, scene::PropertyChannel::PositionX, minimum),
        address(10U, scene::PropertyChannel::PositionX, maximum),
    };
    const auto identity = scene::plan_key_scale(
        root, selection,
        scene::KeyTimeScale{
            1, 1, scene::KeyTimeAnchor::FirstSelected});
    KEY_CHECK(identity.has_value());
    KEY_CHECK(identity.value().puts.empty());
    KEY_CHECK(identity.value().erases.empty());
    KEY_CHECK(identity.value().after.same_snapshot(root));
    KEY_CHECK((selected_frames(identity.value())
               == std::vector<core::i64>{minimum, maximum}));

    const std::array only_max{
        address(10U, scene::PropertyChannel::PositionX, maximum)};
    const auto moved = scene::plan_key_move(root, only_max, 1);
    KEY_CHECK(!moved.has_value());
    KEY_CHECK(moved.error() == scene::KeyEditError::IntegerOverflow);
    const auto scaled = scene::plan_key_scale(
        root, only_max,
        scene::KeyTimeScale{2, 1, scene::KeyTimeAnchor::User, 0, 0});
    KEY_CHECK(!scaled.has_value());
    KEY_CHECK(scaled.error() == scene::KeyEditError::IntegerOverflow);
    const auto invalid = scene::plan_key_scale(
        root, selection,
        scene::KeyTimeScale{1, 0, scene::KeyTimeAnchor::User, 0, 0});
    KEY_CHECK(!invalid.has_value());
    KEY_CHECK(invalid.error() == scene::KeyEditError::InvalidScale);
    KEY_CHECK(*root.get(doc::NodeId{20U})
              == channel(
                  20U, 10U, schema::position_x,
                  {key(minimum, 1.0, schema::interp_linear),
                   key(maximum, 2.0, schema::interp_linear)}));
}

void spatial_effect_channels_are_typed_and_kind_compatible() {
    doc::Root root = basic_root();
    root = root.set(node(
        20U, schema::effect_type,
        {whole(schema::effect_target, 10U),
         text(schema::effect_kind, schema::box_blur_kind)}));
    root = root.set(node(
        21U, schema::effect_type,
        {whole(schema::effect_target, 10U),
         text(schema::effect_kind, schema::directional_blur_kind)}));
    root = root.set(channel(
        30U, 20U, schema::box_blur_radius,
        {key(0, 2.5, schema::interp_linear)}));
    root = root.set(channel(
        31U, 21U, schema::directional_blur_length,
        {key(0, 12.0, schema::interp_linear)}));

    const std::array selection{
        address(20U, scene::PropertyChannel::BoxBlurRadius, 0),
        address(21U, scene::PropertyChannel::DirectionalBlurLength, 0),
        address(20U, scene::PropertyChannel::DirectionalBlurLength, 0),
        address(21U, scene::PropertyChannel::BoxBlurRadius, 0),
    };
    const auto inspected = scene::inspect_key_selection(root, selection);
    KEY_CHECK(inspected.has_value());
    KEY_CHECK(inspected.value().records.size() == 2U);
    KEY_CHECK(inspected.value().records[0U].address.property ==
              scene::PropertyChannel::BoxBlurRadius);
    KEY_CHECK(inspected.value().records[1U].address.property ==
              scene::PropertyChannel::DirectionalBlurLength);
    KEY_CHECK(inspected.value().skips.size() == 2U);
    KEY_CHECK(inspected.value().skips[0U].reason ==
              scene::KeyEditSkipReason::InvalidTarget);
    KEY_CHECK(inspected.value().skips[1U].reason ==
              scene::KeyEditSkipReason::InvalidTarget);

    const std::array valid{
        address(20U, scene::PropertyChannel::BoxBlurRadius, 0),
        address(21U, scene::PropertyChannel::DirectionalBlurLength, 0),
    };
    const auto moved = scene::plan_key_move(root, valid, 5);
    KEY_CHECK(moved.has_value());
    KEY_CHECK(moved.value().report.applied == 2U);
    KEY_CHECK((frames(*moved.value().after.get(doc::NodeId{30U})) ==
               std::vector<core::i64>{5}));
    KEY_CHECK((frames(*moved.value().after.get(doc::NodeId{31U})) ==
               std::vector<core::i64>{5}));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 10U> cases{{
    {"camera light and surface keys support batch edits", &camera_light_and_surface_keys_support_batch_edits},
    {"selection is typed unique exact and lock aware",
     &selection_is_typed_unique_exact_and_lock_aware},
    {"lowest valid channel wins and ambiguous channels are untouched",
     &lowest_valid_channel_wins_and_ambiguous_channels_are_untouched},
    {"move is atomic sorted and replaces stationary destinations",
     &move_is_atomic_sorted_and_replaces_stationary_destinations},
    {"duplicate preserves sources and reports capacity and collisions",
     &duplicate_preserves_sources_and_reports_capacity_and_collisions},
    {"delete erases empty channels and reindexes other channels",
     &delete_erases_empty_channels_and_reindexes_other_channels},
    {"rational scaling supports every anchor negative time and mirroring",
     &rational_scaling_supports_every_anchor_negative_time_and_mirroring},
    {"collapsed scaling uses the latest original key as winner",
     &collapsed_scaling_uses_the_latest_original_key_as_winner},
    {"full span identity and overflow failures are exact and atomic",
     &full_span_identity_and_overflow_failures_are_exact_and_atomic},
    {"spatial effect channels are typed and kind compatible",
     &spatial_effect_channels_are_typed_and_kind_compatible},
}};

} // namespace

int main() {
    std::size_t failures{};
    for (const Case& test : cases) {
        try {
            std::cout << "[run ] " << test.name << std::endl;
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
