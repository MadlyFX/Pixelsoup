#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/scene/key_edit.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
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

namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace persist = mediaserver::persist;
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

// --- Easing and tangents -------------------------------------------------

[[nodiscard]] Property real(std::string_view name, double value) {
    return {std::string{name}, value};
}

// A rising Position X ramp, every key Linear: 0, 12, 18, 30 at frames 0, 30,
// 60 and 90. Its interior Auto slopes are the Fritsch-Carlson harmonic mean
// of the 0.4 and 0.2 secants with equal 30-frame widths: 180 / (90 / 0.4 +
// 90 / 0.2) = 4 / 15.
constexpr double ramp_interior_slope = 4.0 / 15.0;

[[nodiscard]] doc::Root ramp_root(bool locked = false) {
    doc::Root root;
    root = root.set(node(1U, schema::stage_type));
    root = root.set(node(10U, schema::object_type,
                         {whole(schema::scene_id, 1),
                          flag(schema::locked, locked)}));
    root = root.set(channel(
        40U, 10U, schema::position_x,
        {key(0, 0.0, schema::interp_linear),
         key(30, 12.0, schema::interp_linear),
         key(60, 18.0, schema::interp_linear),
         key(90, 30.0, schema::interp_linear)}));
    return root;
}

// The ramp with its frame 30 key Cubic and Broken: slopes 0.1 in and 0.5
// out, handle lengths 4 and 6 frames.
[[nodiscard]] doc::Root broken_ramp_root() {
    doc::Root root = ramp_root();
    doc::Node changed = *root.get(doc::NodeId{40U});
    changed.properties.insert_or_assign(
        key_name(1U, schema::channel_key_interp_suffix),
        std::string{schema::interp_cubic});
    changed.properties.insert_or_assign(
        key_name(1U, schema::channel_key_tangent_mode_suffix),
        std::string{schema::tangent_broken});
    changed.properties.insert_or_assign(
        key_name(1U, schema::channel_key_in_slope_suffix), 0.1);
    changed.properties.insert_or_assign(
        key_name(1U, schema::channel_key_out_slope_suffix), 0.5);
    changed.properties.insert_or_assign(
        key_name(1U, schema::channel_key_in_weight_suffix), 4.0);
    changed.properties.insert_or_assign(
        key_name(1U, schema::channel_key_out_weight_suffix), 6.0);
    return root.set(std::move(changed));
}

[[nodiscard]] const doc::Node& ramp(const doc::Root& root) {
    const doc::Node* const found = root.get(doc::NodeId{40U});
    KEY_CHECK(found != nullptr);
    return *found;
}

[[nodiscard]] std::string key_text(const doc::Node& channel_node,
                                   std::size_t index,
                                   std::string_view suffix) {
    const auto* const held = property(channel_node, key_name(index, suffix));
    const auto* const name =
        held == nullptr ? nullptr : std::get_if<std::string>(held);
    return name == nullptr ? std::string{"<none>"} : *name;
}

[[nodiscard]] std::optional<double> key_number(const doc::Node& channel_node,
                                               std::size_t index,
                                               std::string_view suffix) {
    const auto* const held = property(channel_node, key_name(index, suffix));
    const auto* const number =
        held == nullptr ? nullptr : std::get_if<double>(held);
    return number == nullptr ? std::nullopt : std::optional<double>{*number};
}

[[nodiscard]] bool near(std::optional<double> value, double expected) {
    return value.has_value() && std::fabs(*value - expected) < 1e-12;
}

// No tangent property of any kind on key `index`.
[[nodiscard]] bool untangented(const doc::Node& channel_node,
                               std::size_t index) {
    for (const std::string_view suffix :
         schema::channel_key_tangent_suffixes) {
        if (property(channel_node, key_name(index, suffix)) != nullptr) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] doc::Root eased(const doc::Root& root,
                              std::initializer_list<core::i64> frames,
                              scene::KeyEase ease) {
    std::vector<scene::KeyAddress> selection;
    for (const core::i64 frame : frames) {
        selection.push_back(
            address(10U, scene::PropertyChannel::PositionX, frame));
    }
    auto planned = scene::plan_key_ease(root, selection, ease);
    KEY_CHECK(planned.has_value());
    return planned.value().after;
}

void easing_presets_write_exactly_their_documented_tangents() {
    using schema::channel_key_interp_suffix;
    using schema::channel_key_tangent_mode_suffix;
    using schema::channel_key_in_slope_suffix;
    using schema::channel_key_out_slope_suffix;
    using schema::channel_key_in_weight_suffix;
    using schema::channel_key_out_weight_suffix;

    // Ease In: the key goes Bezier with a flat in slope and keeps the slope
    // it left with; its Linear neighbour before it goes Auto Bezier so the
    // incoming segment curves. The keys after are untouched.
    const doc::Node ease_in =
        ramp(eased(ramp_root(), {60}, scene::KeyEase::EaseIn));
    KEY_CHECK(key_text(ease_in, 2U, channel_key_interp_suffix) == "cubic");
    KEY_CHECK(key_text(ease_in, 2U, channel_key_tangent_mode_suffix)
              == "broken");
    KEY_CHECK(near(key_number(ease_in, 2U, channel_key_in_slope_suffix), 0.0));
    KEY_CHECK(near(key_number(ease_in, 2U, channel_key_out_slope_suffix),
                   ramp_interior_slope));
    KEY_CHECK(!key_number(ease_in, 2U, channel_key_in_weight_suffix));
    KEY_CHECK(key_text(ease_in, 1U, channel_key_interp_suffix) == "cubic");
    KEY_CHECK(untangented(ease_in, 1U));
    KEY_CHECK(key_text(ease_in, 0U, channel_key_interp_suffix) == "linear");
    KEY_CHECK(key_text(ease_in, 3U, channel_key_interp_suffix) == "linear");

    // Ease Out flattens only the outgoing side and leaves the key before it.
    const doc::Node ease_out =
        ramp(eased(ramp_root(), {30}, scene::KeyEase::EaseOut));
    KEY_CHECK(key_text(ease_out, 1U, channel_key_tangent_mode_suffix)
              == "broken");
    KEY_CHECK(near(key_number(ease_out, 1U, channel_key_in_slope_suffix),
                   ramp_interior_slope));
    KEY_CHECK(near(key_number(ease_out, 1U, channel_key_out_slope_suffix),
                   0.0));
    KEY_CHECK(key_text(ease_out, 0U, channel_key_interp_suffix) == "linear");

    // Easy Ease on two neighbours: both Flat with no slopes written; only the
    // unselected key before the first one is turned Auto Bezier.
    const doc::Node easy =
        ramp(eased(ramp_root(), {30, 60}, scene::KeyEase::EasyEase));
    for (const std::size_t index : {1U, 2U}) {
        KEY_CHECK(key_text(easy, index, channel_key_interp_suffix) == "cubic");
        KEY_CHECK(key_text(easy, index, channel_key_tangent_mode_suffix)
                  == "flat");
        KEY_CHECK(!key_number(easy, index, channel_key_in_slope_suffix));
        KEY_CHECK(!key_number(easy, index, channel_key_out_slope_suffix));
    }
    KEY_CHECK(key_text(easy, 0U, channel_key_interp_suffix) == "cubic");
    KEY_CHECK(untangented(easy, 0U));
    KEY_CHECK(key_text(easy, 3U, channel_key_interp_suffix) == "linear");

    // Aligned averages the slopes it evaluated with and keeps the weights.
    const doc::Node aligned =
        ramp(eased(broken_ramp_root(), {30}, scene::KeyEase::Aligned));
    KEY_CHECK(key_text(aligned, 1U, channel_key_tangent_mode_suffix)
              == "aligned");
    KEY_CHECK(near(key_number(aligned, 1U, channel_key_in_slope_suffix), 0.3));
    KEY_CHECK(near(key_number(aligned, 1U, channel_key_out_slope_suffix), 0.3));
    KEY_CHECK(near(key_number(aligned, 1U, channel_key_in_weight_suffix), 4.0));
    KEY_CHECK(near(key_number(aligned, 1U, channel_key_out_weight_suffix),
                   6.0));

    // Flat keeps the weights and drops the slopes.
    const doc::Node flat =
        ramp(eased(broken_ramp_root(), {30}, scene::KeyEase::Flat));
    KEY_CHECK(key_text(flat, 1U, channel_key_tangent_mode_suffix) == "flat");
    KEY_CHECK(!key_number(flat, 1U, channel_key_in_slope_suffix));
    KEY_CHECK(near(key_number(flat, 1U, channel_key_out_weight_suffix), 6.0));

    // Broken writes the slopes an Auto key evaluated with.
    const doc::Node broken = ramp(eased(
        eased(ramp_root(), {60}, scene::KeyEase::Auto), {60},
        scene::KeyEase::Broken));
    KEY_CHECK(key_text(broken, 2U, channel_key_tangent_mode_suffix)
              == "broken");
    KEY_CHECK(near(key_number(broken, 2U, channel_key_in_slope_suffix),
                   ramp_interior_slope));
    KEY_CHECK(near(key_number(broken, 2U, channel_key_out_slope_suffix),
                   ramp_interior_slope));

    // Hold, Linear and Auto remove every tangent property.
    for (const scene::KeyEase ease :
         {scene::KeyEase::Hold, scene::KeyEase::Linear,
          scene::KeyEase::Auto}) {
        const doc::Node cleared = ramp(eased(broken_ramp_root(), {30}, ease));
        KEY_CHECK(untangented(cleared, 1U));
        KEY_CHECK(key_text(cleared, 1U, channel_key_interp_suffix)
                  == (ease == scene::KeyEase::Hold     ? "hold"
                      : ease == scene::KeyEase::Linear ? "linear"
                                                       : "cubic"));
    }

    // A tangent mode on a Linear key is skipped and changes nothing.
    const std::array selection{
        address(10U, scene::PropertyChannel::PositionX, 60)};
    auto skipped = scene::plan_key_ease(ramp_root(), selection,
                                        scene::KeyEase::Flat);
    KEY_CHECK(skipped.has_value());
    KEY_CHECK(skipped.value().puts.empty());
    KEY_CHECK(skipped.value().report.applied == 0U);
    KEY_CHECK(skipped.value().report.skips.size() == 1U
              && skipped.value().report.skips[0].reason
                  == scene::KeyEditSkipReason::NotCubic);
    KEY_CHECK(selected_frames(skipped.value())
              == std::vector<core::i64>{60});
}

void tangent_edits_are_validated_and_refused_on_locked_targets() {
    const scene::KeyAddress thirty =
        address(10U, scene::PropertyChannel::PositionX, 30);
    scene::KeyTangent tangent{scene::KeyTangentMode::Broken, 0.5, -0.25, 7.0,
                              9.0};
    auto planned = scene::plan_key_tangent(ramp_root(), thirty, tangent);
    KEY_CHECK(planned.has_value());
    KEY_CHECK(planned.value().report.applied == 1U);
    const doc::Node& written = ramp(planned.value().after);
    KEY_CHECK(key_text(written, 1U, schema::channel_key_interp_suffix)
              == "cubic");
    KEY_CHECK(key_text(written, 1U, schema::channel_key_tangent_mode_suffix)
              == "broken");
    KEY_CHECK(near(key_number(written, 1U, schema::channel_key_in_slope_suffix),
                   0.5));
    KEY_CHECK(near(key_number(written, 1U, schema::channel_key_out_slope_suffix),
                   -0.25));
    KEY_CHECK(near(key_number(written, 1U, schema::channel_key_in_weight_suffix),
                   7.0));
    KEY_CHECK(near(key_number(written, 1U,
                              schema::channel_key_out_weight_suffix),
                   9.0));
    KEY_CHECK(planned.value().selection_after.size() == 1U
              && planned.value().selection_after[0].interpolation
                  == scene::KeyInterpolation::Cubic);

    // Aligned stores one slope, the mean of the two asked for.
    auto aligned = scene::plan_key_tangent(
        ramp_root(), thirty,
        scene::KeyTangent{scene::KeyTangentMode::Aligned, 0.2, 0.4,
                          std::nullopt, 5.0});
    KEY_CHECK(aligned.has_value());
    const doc::Node& shared = ramp(aligned.value().after);
    KEY_CHECK(near(key_number(shared, 1U, schema::channel_key_in_slope_suffix),
                   0.3));
    KEY_CHECK(near(key_number(shared, 1U, schema::channel_key_out_slope_suffix),
                   0.3));
    KEY_CHECK(!key_number(shared, 1U, schema::channel_key_in_weight_suffix));

    // Values no evaluator could use are refused outright.
    for (const scene::KeyTangent& bad :
         {scene::KeyTangent{scene::KeyTangentMode::Broken,
                            std::numeric_limits<double>::quiet_NaN(), 0.0},
          scene::KeyTangent{scene::KeyTangentMode::Broken, 0.0,
                            std::numeric_limits<double>::infinity()},
          scene::KeyTangent{scene::KeyTangentMode::Broken, 0.0, 0.0, 0.0},
          scene::KeyTangent{scene::KeyTangentMode::Aligned, 0.0, 0.0,
                            std::nullopt, -3.0}}) {
        const auto refused = scene::plan_key_tangent(ramp_root(), thirty, bad);
        KEY_CHECK(!refused.has_value()
                  && refused.error() == scene::KeyEditError::InvalidTangent);
    }

    // A handle longer than its interval would stop the channel evaluating:
    // the key is skipped and nothing changes.
    auto too_long = scene::plan_key_tangent(
        ramp_root(), thirty,
        scene::KeyTangent{scene::KeyTangentMode::Broken, 0.0, 0.0,
                          std::nullopt, 31.0});
    KEY_CHECK(too_long.has_value());
    KEY_CHECK(too_long.value().puts.empty());
    KEY_CHECK(too_long.value().report.skips.size() == 1U
              && too_long.value().report.skips[0].reason
                  == scene::KeyEditSkipReason::InvalidTangent);

    // A locked target is refused for both kinds of edit.
    auto locked = scene::plan_key_tangent(ramp_root(true), thirty, tangent);
    KEY_CHECK(locked.has_value());
    KEY_CHECK(locked.value().puts.empty());
    KEY_CHECK(locked.value().report.skips.size() == 1U
              && locked.value().report.skips[0].reason
                  == scene::KeyEditSkipReason::TargetLocked);
    const std::array selection{thirty};
    auto locked_ease = scene::plan_key_ease(ramp_root(true), selection,
                                            scene::KeyEase::EasyEase);
    KEY_CHECK(locked_ease.has_value());
    KEY_CHECK(locked_ease.value().puts.empty());
    KEY_CHECK(locked_ease.value().report.skips[0].reason
              == scene::KeyEditSkipReason::TargetLocked);
}

void moved_duplicated_and_reindexed_keys_carry_their_tangents() {
    const std::array thirty{
        address(10U, scene::PropertyChannel::PositionX, 30)};
    auto moved = scene::plan_key_move(broken_ramp_root(), thirty, 5);
    KEY_CHECK(moved.has_value());
    const doc::Node& after_move = ramp(moved.value().after);
    KEY_CHECK(frames(after_move) == (std::vector<core::i64>{0, 35, 60, 90}));
    KEY_CHECK(key_text(after_move, 1U,
                       schema::channel_key_tangent_mode_suffix) == "broken");
    KEY_CHECK(near(key_number(after_move, 1U,
                              schema::channel_key_out_weight_suffix),
                   6.0));
    KEY_CHECK(moved.value().selection_after[0].tangent
              == scene::inspect_key_selection(broken_ramp_root(), thirty)
                     .value()
                     .records[0]
                     .tangent);

    // Moved to frame 57, its 6-frame outgoing handle no longer fits the
    // 3 frames to the next key: the channel is left as it was.
    auto cramped = scene::plan_key_move(broken_ramp_root(), thirty, 27);
    KEY_CHECK(cramped.has_value());
    KEY_CHECK(cramped.value().puts.empty());
    KEY_CHECK(cramped.value().report.applied == 0U);
    KEY_CHECK(cramped.value().selection_after.empty());
    KEY_CHECK(cramped.value().report.skips.size() == 1U
              && cramped.value().report.skips[0].reason
                  == scene::KeyEditSkipReason::InvalidTangent);

    // Deleting the first key moves every later key down a slot; the broken
    // key's handles move with it and no slot keeps a stale one.
    const std::array zero{address(10U, scene::PropertyChannel::PositionX, 0)};
    auto removed = scene::plan_key_delete(broken_ramp_root(), zero);
    KEY_CHECK(removed.has_value());
    const doc::Node& after_delete = ramp(removed.value().after);
    KEY_CHECK(frames(after_delete) == (std::vector<core::i64>{30, 60, 90}));
    KEY_CHECK(near(key_number(after_delete, 0U,
                              schema::channel_key_in_slope_suffix),
                   0.1));
    KEY_CHECK(untangented(after_delete, 1U));
    KEY_CHECK(untangented(after_delete, 2U));
    KEY_CHECK(untangented(after_delete, 3U));

    // A duplicate copies the handles.
    auto copied = scene::plan_key_duplicate(broken_ramp_root(), thirty, 10);
    KEY_CHECK(copied.has_value());
    const doc::Node& after_copy = ramp(copied.value().after);
    KEY_CHECK(frames(after_copy) == (std::vector<core::i64>{0, 30, 40, 60, 90}));
    KEY_CHECK(near(key_number(after_copy, 2U,
                              schema::channel_key_in_weight_suffix),
                   4.0));
}

[[nodiscard]] scene::SceneModel model_of(const doc::Root& root) {
    scene::SceneModel model;
    model.reset(root);
    return model;
}

[[nodiscard]] core::u64 channel_digest(const doc::Root& root) {
    const auto channels = model_of(root).channel_keyframes();
    KEY_CHECK(channels.size() == 1U);
    return scene::digest(channels[0]);
}

[[nodiscard]] core::u64 frame_digest(const doc::Root& root, core::u64 frame) {
    return scene::digest(model_of(root).evaluate(
        doc::NodeId{1U}, scene::SceneTime{frame, 60U, 1U}));
}

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("mediaserver-scene-key-edit-" + std::to_string(stamp) + "-"
               + std::to_string(sequence.fetch_add(1U)));
        std::error_code error;
        if (!std::filesystem::create_directory(path_, error) || error) {
            throw std::runtime_error{"could not create key test directory"};
        }
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] std::filesystem::path file() const {
        return path_ / "keys.msproject";
    }

private:
    std::filesystem::path path_;
};

void eased_keys_undo_redo_save_load_and_stream_exactly() {
    const doc::Root original = ramp_root();
    const doc::Root easy = eased(original, {30}, scene::KeyEase::EasyEase);
    const doc::Root edited = [&] {
        auto planned = scene::plan_key_tangent(
            easy, address(10U, scene::PropertyChannel::PositionX, 30),
            scene::KeyTangent{scene::KeyTangentMode::Broken, 0.75, -0.5, 3.0,
                              11.0});
        KEY_CHECK(planned.has_value());
        return planned.value().after;
    }();

    // The curve and both digests see the change: frame 45 sits between the
    // eased key and the next.
    KEY_CHECK(channel_digest(easy) != channel_digest(original));
    KEY_CHECK(channel_digest(edited) != channel_digest(easy));
    KEY_CHECK(frame_digest(easy, 45U) != frame_digest(original, 45U));
    KEY_CHECK(frame_digest(edited, 45U) != frame_digest(easy, 45U));
    // Keys themselves still evaluate to their own values.
    const auto edited_channels = model_of(edited).channel_keyframes();
    KEY_CHECK(scene::sample_keyframes(edited_channels[0], 30) == 12.0);
    KEY_CHECK(scene::sample_keyframes(edited_channels[0], 60) == 18.0);

    // Each is one transaction; undo and redo restore exact tangents.
    cmd::DirectorScheduler director{original};
    const cmd::UndoContextId context{2U};
    KEY_CHECK(director.schedule_document_replace(
        easy, context, cmd::TransactionId{5U}, cmd::FrameNumber{1U}));
    KEY_CHECK(director.schedule_document_replace(
        edited, context, cmd::TransactionId{6U}, cmd::FrameNumber{1U}));
    KEY_CHECK(director.undo_depth(context) == 2U);
    KEY_CHECK(director.seal_frame(cmd::FrameNumber{1U}));
    KEY_CHECK(director.schedule_undo(context, cmd::TransactionId{7U},
                                     cmd::FrameNumber{2U}));
    KEY_CHECK(ramp(director.projected_document()) == ramp(easy));
    KEY_CHECK(director.schedule_undo(context, cmd::TransactionId{8U},
                                     cmd::FrameNumber{2U}));
    KEY_CHECK(ramp(director.projected_document()) == ramp(original));
    KEY_CHECK(director.seal_frame(cmd::FrameNumber{2U}));
    KEY_CHECK(director.schedule_redo(context, cmd::TransactionId{9U},
                                     cmd::FrameNumber{3U}));
    KEY_CHECK(director.schedule_redo(context, cmd::TransactionId{10U},
                                     cmd::FrameNumber{3U}));
    KEY_CHECK(ramp(director.projected_document()) == ramp(edited));

    // Saved and loaded, the handles come back bit for bit.
    TempDirectory temporary;
    KEY_CHECK(persist::save_project_file(temporary.file(), edited));
    const auto loaded = persist::load_project_file(temporary.file());
    KEY_CHECK(loaded.has_value());
    KEY_CHECK(ramp(loaded.value()) == ramp(edited));
    KEY_CHECK(channel_digest(loaded.value()) == channel_digest(edited));

    // A node fed the edits one change at a time agrees with one that loads
    // the result.
    scene::SceneModel streamed;
    streamed.reset(original);
    streamed.apply(doc::diff(original, easy));
    streamed.apply(doc::diff(easy, edited));
    const scene::SceneModel checkpointed = model_of(edited);
    KEY_CHECK(streamed.channel_keyframes() == checkpointed.channel_keyframes());
    KEY_CHECK(scene::digest(streamed.evaluate(
                  doc::NodeId{1U}, scene::SceneTime{45U, 60U, 1U}))
              == frame_digest(edited, 45U));
}


struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 14U> cases{{
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
    {"easing presets write exactly their documented tangents",
     &easing_presets_write_exactly_their_documented_tangents},
    {"tangent edits are validated and refused on locked targets",
     &tangent_edits_are_validated_and_refused_on_locked_targets},
    {"moved duplicated and reindexed keys carry their tangents",
     &moved_duplicated_and_reindexed_keys_carry_their_tangents},
    {"eased keys undo redo save load and stream exactly",
     &eased_keys_undo_redo_save_load_and_stream_exactly},
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
