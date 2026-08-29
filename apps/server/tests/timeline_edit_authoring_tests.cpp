#include "timeline_edit_authoring.hpp"

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define AUTHOR_CHECK(...)                                                      \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr scene::SceneId scene_id{1U};
constexpr doc::NodeId lane_id{20U};
constexpr scene::MediaFrameRate show_60{60U, 1U};
constexpr scene::MediaFrameRate native_rate{30'000U, 1'001U};

[[nodiscard]] doc::PropertyValue whole(std::int64_t value) {
    return doc::PropertyValue{value};
}

[[nodiscard]] doc::Node stage() {
    return doc::Node{scene_id, std::string{scene::schema::stage_type}, {}};
}

[[nodiscard]] doc::Node lane(bool locked = false) {
    doc::Node node{};
    node.id = lane_id;
    node.type = std::string{scene::schema::timeline_lane_type};
    node.properties.emplace(std::string{scene::schema::scene_id}, whole(1));
    node.properties.emplace(std::string{scene::schema::lane_order}, whole(0));
    node.properties.emplace(std::string{scene::schema::lane_name},
                            std::string{"Video 1"});
    node.properties.emplace(std::string{scene::schema::lane_locked}, locked);
    return node;
}

[[nodiscard]] doc::Node media(
    core::u64 id, core::i64 start, core::i64 duration,
    core::i64 source_in = 0, bool locked = false,
    bool native = false) {
    doc::Node node{};
    node.id = doc::NodeId{id};
    node.type = std::string{scene::schema::media_type};
    node.properties.emplace(std::string{scene::schema::scene_id}, whole(1));
    node.properties.emplace(std::string{scene::schema::lane_id}, whole(20));
    node.properties.emplace(std::string{scene::schema::clip_order},
                            whole(static_cast<std::int64_t>(id)));
    node.properties.emplace(std::string{scene::schema::clip_start},
                            whole(start));
    node.properties.emplace(std::string{scene::schema::clip_duration},
                            whole(duration));
    node.properties.emplace(std::string{scene::schema::source_in},
                            whole(source_in));
    node.properties.emplace(std::string{scene::schema::fade_in}, whole(2));
    node.properties.emplace(std::string{scene::schema::fade_out}, whole(3));
    node.properties.emplace(std::string{scene::schema::locked}, locked);
    node.properties.emplace(std::string{scene::schema::source},
                            std::string{"camera-a.mov"});
    node.properties.emplace(std::string{scene::schema::asset_id}, whole(501));
    node.properties.emplace(std::string{scene::schema::sequence_start},
                            whole(100));
    node.properties.emplace(std::string{scene::schema::sequence_count},
                            whole(100));
    if (native) {
        node.properties.emplace(
            std::string{scene::schema::source_rate_numerator}, whole(30'000));
        node.properties.emplace(
            std::string{scene::schema::source_rate_denominator}, whole(1'001));
    }
    node.properties.emplace(std::string{scene::schema::position_x}, 12.5);
    node.properties.emplace("outside_planner", std::string{"keep-me"});
    return node;
}

[[nodiscard]] doc::Node effect(core::u64 id, core::u64 target) {
    doc::Node node{};
    node.id = doc::NodeId{id};
    node.type = std::string{scene::schema::effect_type};
    node.properties.emplace(std::string{scene::schema::scene_id}, whole(1));
    node.properties.emplace(
        std::string{scene::schema::effect_target},
        whole(static_cast<std::int64_t>(target)));
    node.properties.emplace(std::string{scene::schema::effect_kind},
                            std::string{scene::schema::brightness_kind});
    node.properties.emplace(std::string{scene::schema::brightness}, 1.25);
    return node;
}

[[nodiscard]] doc::Node channel(core::u64 id, core::u64 target,
                                core::i64 first, core::i64 second) {
    doc::Node node{};
    node.id = doc::NodeId{id};
    node.type = std::string{scene::schema::channel_type};
    node.properties.emplace(std::string{scene::schema::scene_id}, whole(1));
    node.properties.emplace(
        std::string{scene::schema::channel_target},
        whole(static_cast<std::int64_t>(target)));
    node.properties.emplace(
        std::string{scene::schema::channel_component},
        std::string{scene::schema::position_x});
    node.properties.emplace("key0_frame", whole(first));
    node.properties.emplace("key0_value", 1.0);
    node.properties.emplace("key0_interp", std::string{"linear"});
    node.properties.emplace("key1_frame", whole(second));
    node.properties.emplace("key1_value", 2.0);
    node.properties.emplace("key1_interp", std::string{"hold"});
    return node;
}

[[nodiscard]] std::int64_t integer(
    const doc::Node& node, std::string_view property) {
    const auto found = node.properties.find(property);
    AUTHOR_CHECK(found != node.properties.end());
    const auto* const value = std::get_if<std::int64_t>(&found->second);
    AUTHOR_CHECK(value != nullptr);
    return *value;
}

[[nodiscard]] std::string text(
    const doc::Node& node, std::string_view property) {
    const auto found = node.properties.find(property);
    AUTHOR_CHECK(found != node.properties.end());
    const auto* const value = std::get_if<std::string>(&found->second);
    AUTHOR_CHECK(value != nullptr);
    return *value;
}

[[nodiscard]] std::vector<scene::ClipPlacement> placements(
    const doc::Root& root) {
    scene::SceneModel model;
    model.reset(root);
    return model.clips(scene_id);
}

[[nodiscard]] const doc::Node& created(
    const app::TimelineEditAuthorPlan& plan, doc::NodeId id) {
    const auto found = std::lower_bound(
        plan.created.begin(), plan.created.end(), id,
        [](const doc::Node& node, doc::NodeId wanted) noexcept {
            return node.id < wanted;
        });
    AUTHOR_CHECK(found != plan.created.end());
    AUTHOR_CHECK(found->id == id);
    return *found;
}

void fractional_roll_authors_full_timing_and_preserves_identity() {
    doc::Root root;
    root = root.set(stage());
    root = root.set(lane());
    root = root.set(media(10U, 0, 20, 0, false, true));
    root = root.set(media(20U, 20, 10, 10, false, true));

    const auto pure = scene::plan_roll(
        placements(root), doc::NodeId{10U}, doc::NodeId{20U}, 3, show_60);
    AUTHOR_CHECK(pure.has_value());
    const auto authored =
        app::plan_timeline_edit_authoring(root, pure.value(), 1'000U);
    AUTHOR_CHECK(authored.has_value());
    AUTHOR_CHECK((authored.value().changed ==
                  std::vector<doc::NodeId>{doc::NodeId{10U},
                                           doc::NodeId{20U}}));
    AUTHOR_CHECK(authored.value().created.empty());
    AUTHOR_CHECK(authored.value().erased.empty());
    AUTHOR_CHECK(authored.value().next_node == 1'000U);

    const doc::Node* const left =
        authored.value().document.get(doc::NodeId{10U});
    const doc::Node* const right =
        authored.value().document.get(doc::NodeId{20U});
    AUTHOR_CHECK(left != nullptr && right != nullptr);
    AUTHOR_CHECK(integer(*left, scene::schema::clip_duration) == 23);
    AUTHOR_CHECK(integer(*right, scene::schema::clip_start) == 23);
    AUTHOR_CHECK(integer(*right, scene::schema::clip_duration) == 7);
    AUTHOR_CHECK(integer(*right, scene::schema::source_in) == 11);
    AUTHOR_CHECK(integer(*right, scene::schema::asset_id) == 501);
    AUTHOR_CHECK(integer(*right, scene::schema::lane_id) == 20);
    AUTHOR_CHECK(
        integer(*right, scene::schema::source_rate_numerator) == 30'000);
    AUTHOR_CHECK(
        integer(*right, scene::schema::source_rate_denominator) == 1'001);
    AUTHOR_CHECK(text(*right, "outside_planner") == "keep-me");
}

void ripple_delete_cascades_clip_effect_and_channel_nodes() {
    doc::Root root;
    root = root.set(stage());
    root = root.set(lane());
    root = root.set(media(10U, 0, 10));
    root = root.set(media(20U, 10, 10));
    root = root.set(effect(30U, 10U));
    root = root.set(effect(31U, 20U));
    root = root.set(channel(40U, 10U, 0, 9));
    root = root.set(channel(50U, 30U, 0, 9));
    root = root.set(channel(51U, 31U, 10, 19));

    constexpr std::array selection{doc::NodeId{10U}};
    const auto pure = scene::plan_ripple_delete(placements(root), selection);
    AUTHOR_CHECK(pure.has_value());
    const auto authored =
        app::plan_timeline_edit_authoring(root, pure.value(), 1'000U);
    AUTHOR_CHECK(authored.has_value());
    AUTHOR_CHECK((authored.value().erased ==
                  std::vector<doc::NodeId>{
                      doc::NodeId{10U}, doc::NodeId{30U}, doc::NodeId{40U},
                      doc::NodeId{50U}}));
    for (const doc::NodeId id : authored.value().erased) {
        AUTHOR_CHECK(!authored.value().document.contains(id));
    }
    AUTHOR_CHECK(authored.value().document.contains(doc::NodeId{31U}));
    AUTHOR_CHECK(authored.value().document.contains(doc::NodeId{51U}));
    AUTHOR_CHECK(integer(*authored.value().document.get(doc::NodeId{20U}),
                         scene::schema::clip_start) == 0);
}

void overwrite_split_clones_and_retargets_the_complete_subgraph() {
    doc::Root root;
    root = root.set(stage());
    root = root.set(lane());
    root = root.set(media(10U, 0, 20, 0, false, true));
    root = root.set(effect(30U, 10U));
    root = root.set(channel(40U, 10U, 2, 18));
    root = root.set(channel(50U, 30U, 3, 17));
    root = root.set(doc::Node{doc::NodeId{100U}, "occupied", {}});
    root = root.set(doc::Node{doc::NodeId{101U}, "occupied", {}});

    const auto pure = scene::plan_overwrite(
        placements(root), scene::TimelineLaneRef{scene_id, lane_id}, 5, 5,
        show_60, doc::NodeId{100U});
    AUTHOR_CHECK(pure.has_value());
    AUTHOR_CHECK(pure.value().creates.size() == 1U);
    const auto authored =
        app::plan_timeline_edit_authoring(root, pure.value(), 100U);
    AUTHOR_CHECK(authored.has_value());
    const auto& plan = authored.value();
    AUTHOR_CHECK(plan.next_node == 106U);
    AUTHOR_CHECK(plan.created.size() == 4U);
    AUTHOR_CHECK((std::vector<doc::NodeId>{
                      plan.created[0].id, plan.created[1].id,
                      plan.created[2].id, plan.created[3].id} ==
                  std::vector<doc::NodeId>{
                      doc::NodeId{102U}, doc::NodeId{103U},
                      doc::NodeId{104U}, doc::NodeId{105U}}));

    const doc::Node& right = created(plan, doc::NodeId{102U});
    AUTHOR_CHECK(right.type == scene::schema::media_type);
    AUTHOR_CHECK(integer(right, scene::schema::clip_start) == 10);
    AUTHOR_CHECK(integer(right, scene::schema::clip_duration) == 10);
    AUTHOR_CHECK(integer(right, scene::schema::source_in) == 5);
    AUTHOR_CHECK(integer(right, scene::schema::asset_id) == 501);
    AUTHOR_CHECK(integer(right, scene::schema::lane_id) == 20);
    AUTHOR_CHECK(
        integer(right, scene::schema::source_rate_numerator) == 30'000);
    AUTHOR_CHECK(text(right, "outside_planner") == "keep-me");

    const doc::Node& cloned_effect = created(plan, doc::NodeId{103U});
    const doc::Node& direct_channel = created(plan, doc::NodeId{104U});
    const doc::Node& effect_channel = created(plan, doc::NodeId{105U});
    AUTHOR_CHECK(integer(cloned_effect, scene::schema::effect_target) == 102);
    AUTHOR_CHECK(integer(direct_channel, scene::schema::channel_target) == 102);
    AUTHOR_CHECK(integer(effect_channel, scene::schema::channel_target) == 103);
    // Split stays in the same show-time domain, so interpolation history and
    // future keys retain their absolute ordinals.
    AUTHOR_CHECK(integer(direct_channel, "key0_frame") == 2);
    AUTHOR_CHECK(integer(direct_channel, "key1_frame") == 18);
    AUTHOR_CHECK(integer(effect_channel, "key0_frame") == 3);
    AUTHOR_CHECK(integer(effect_channel, "key1_frame") == 17);

    AUTHOR_CHECK(integer(*plan.document.get(doc::NodeId{30U}),
                         scene::schema::effect_target) == 10);
    AUTHOR_CHECK(integer(*plan.document.get(doc::NodeId{40U}),
                         scene::schema::channel_target) == 10);
    AUTHOR_CHECK(integer(*plan.document.get(doc::NodeId{50U}),
                         scene::schema::channel_target) == 30);
}

void one_replacement_undoes_and_redoes_to_exact_snapshots() {
    doc::Root root;
    root = root.set(stage());
    root = root.set(lane());
    root = root.set(media(10U, 0, 10));
    root = root.set(media(20U, 10, 10));
    root = root.set(effect(30U, 10U));
    root = root.set(channel(40U, 30U, 0, 9));

    constexpr std::array selection{doc::NodeId{10U}};
    const auto pure = scene::plan_ripple_delete(placements(root), selection);
    AUTHOR_CHECK(pure.has_value());
    const auto authored =
        app::plan_timeline_edit_authoring(root, pure.value(), 1'000U);
    AUTHOR_CHECK(authored.has_value());

    cmd::DirectorScheduler scheduler{root};
    constexpr cmd::UndoContextId context{7U};
    auto scheduled = scheduler.schedule_document_replace(
        authored.value().document, context, cmd::TransactionId{1U},
        cmd::FrameNumber{1U});
    AUTHOR_CHECK(scheduled.has_value());
    AUTHOR_CHECK(doc::diff(scheduler.projected_document(),
                           authored.value().document)
                     .changes.empty());
    AUTHOR_CHECK(scheduler.undo_depth(context) == 1U);

    auto undone = scheduler.schedule_undo(
        context, cmd::TransactionId{2U}, cmd::FrameNumber{1U});
    AUTHOR_CHECK(undone.has_value());
    AUTHOR_CHECK(doc::diff(scheduler.projected_document(), root).changes.empty());
    AUTHOR_CHECK(scheduler.redo_depth(context) == 1U);

    auto redone = scheduler.schedule_redo(
        context, cmd::TransactionId{3U}, cmd::FrameNumber{1U});
    AUTHOR_CHECK(redone.has_value());
    AUTHOR_CHECK(doc::diff(scheduler.projected_document(),
                           authored.value().document)
                     .changes.empty());
    AUTHOR_CHECK(scheduler.undo_depth(context) == 1U);
}

void locks_and_forged_identity_changes_fail_closed() {
    doc::Root clip_locked;
    clip_locked = clip_locked.set(stage());
    clip_locked = clip_locked.set(lane());
    clip_locked = clip_locked.set(media(10U, 0, 10, 5, true));
    const auto pure_locked = scene::plan_slip(
        placements(clip_locked), doc::NodeId{10U}, 1, show_60);
    AUTHOR_CHECK(!pure_locked.has_value());
    AUTHOR_CHECK(pure_locked.error() == scene::TimelineEditError::LockedClip);

    doc::Root lane_locked;
    lane_locked = lane_locked.set(stage());
    lane_locked = lane_locked.set(lane(true));
    lane_locked = lane_locked.set(media(10U, 0, 10, 5));
    const auto pure = scene::plan_slip(
        placements(lane_locked), doc::NodeId{10U}, 1, show_60);
    AUTHOR_CHECK(pure.has_value());
    const auto rejected =
        app::plan_timeline_edit_authoring(lane_locked, pure.value(), 100U);
    AUTHOR_CHECK(!rejected.has_value());
    AUTHOR_CHECK(rejected.error() == app::TimelineEditAuthorError::LockedLane);

    doc::Root unlocked;
    unlocked = unlocked.set(stage());
    unlocked = unlocked.set(lane());
    unlocked = unlocked.set(media(10U, 0, 10, 5));
    auto valid = scene::plan_slip(
        placements(unlocked), doc::NodeId{10U}, 1, show_60);
    AUTHOR_CHECK(valid.has_value());
    valid.value().updates.front().source = "forged.mov";
    const auto forged =
        app::plan_timeline_edit_authoring(unlocked, valid.value(), 100U);
    AUTHOR_CHECK(!forged.has_value());
    AUTHOR_CHECK(forged.error() == app::TimelineEditAuthorError::InvalidPlan);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 5U> cases{{
    {"fractional roll authors full timing and preserves identity",
     &fractional_roll_authors_full_timing_and_preserves_identity},
    {"ripple delete cascades clip effect and channel nodes",
     &ripple_delete_cascades_clip_effect_and_channel_nodes},
    {"overwrite split clones and retargets the complete subgraph",
     &overwrite_split_clones_and_retargets_the_complete_subgraph},
    {"one replacement undoes and redoes to exact snapshots",
     &one_replacement_undoes_and_redoes_to_exact_snapshots},
    {"locks and forged identity changes fail closed",
     &locks_and_forged_identity_changes_fail_closed},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& entry : cases) {
        try {
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
