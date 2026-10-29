#include "timeline_lane_authoring.hpp"

#include "channel_node_id.hpp"

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/scene/scene_schema.hpp>

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
        throw std::runtime_error{std::string{"line "} + std::to_string(line)
                                 + ": check failed: " + expression};
    }
}

#define LANE_CHECK(...)                                                        \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] doc::PropertyValue whole(std::int64_t value) {
    return doc::PropertyValue{value};
}

[[nodiscard]] doc::Node stage(core::u64 id) {
    return doc::Node{doc::NodeId{id}, std::string{scene::schema::stage_type}, {}};
}

[[nodiscard]] doc::Node lane(core::u64 id, core::u64 owner,
                             std::int64_t order, std::string name) {
    doc::Node node{};
    node.id = doc::NodeId{id};
    node.type = std::string{scene::schema::timeline_lane_type};
    node.properties.emplace(std::string{scene::schema::scene_id},
                            whole(static_cast<std::int64_t>(owner)));
    node.properties.emplace(std::string{scene::schema::lane_order}, whole(order));
    node.properties.emplace(std::string{scene::schema::lane_name}, std::move(name));
    node.properties.emplace(
        std::string{scene::schema::lane_compositing_mode},
        std::string{scene::schema::lane_normal_mode});
    return node;
}

[[nodiscard]] doc::Node clip(core::u64 id, core::u64 owner, core::u64 lane_id) {
    doc::Node node{};
    node.id = doc::NodeId{id};
    node.type = std::string{scene::schema::media_type};
    node.properties.emplace(std::string{scene::schema::scene_id},
                            whole(static_cast<std::int64_t>(owner)));
    node.properties.emplace(std::string{scene::schema::lane_id},
                            whole(static_cast<std::int64_t>(lane_id)));
    node.properties.emplace(std::string{scene::schema::clip_start}, whole(0));
    node.properties.emplace(std::string{scene::schema::clip_duration}, whole(10));
    return node;
}

[[nodiscard]] doc::Node effect(core::u64 id, core::u64 owner,
                               core::u64 target) {
    doc::Node node{};
    node.id = doc::NodeId{id};
    node.type = std::string{scene::schema::effect_type};
    node.properties.emplace(std::string{scene::schema::scene_id},
                            whole(static_cast<std::int64_t>(owner)));
    node.properties.emplace(std::string{scene::schema::effect_target},
                            whole(static_cast<std::int64_t>(target)));
    node.properties.emplace(std::string{scene::schema::effect_kind},
                            std::string{scene::schema::brightness_kind});
    return node;
}

[[nodiscard]] doc::Node channel(core::u64 id, core::u64 owner,
                                core::u64 target) {
    doc::Node node{};
    node.id = doc::NodeId{id};
    node.type = std::string{scene::schema::channel_type};
    node.properties.emplace(std::string{scene::schema::scene_id},
                            whole(static_cast<std::int64_t>(owner)));
    node.properties.emplace(std::string{scene::schema::channel_target},
                            whole(static_cast<std::int64_t>(target)));
    node.properties.emplace(std::string{scene::schema::channel_component},
                            std::string{scene::schema::brightness});
    return node;
}

[[nodiscard]] std::int64_t integer(const doc::Node& node,
                                   std::string_view property) {
    const auto found = node.properties.find(property);
    LANE_CHECK(found != node.properties.end());
    const auto* const held = std::get_if<std::int64_t>(&found->second);
    LANE_CHECK(held != nullptr);
    return *held;
}

[[nodiscard]] bool boolean(const doc::Node& node, std::string_view property) {
    const auto found = node.properties.find(property);
    LANE_CHECK(found != node.properties.end());
    const auto* const held = std::get_if<bool>(&found->second);
    LANE_CHECK(held != nullptr);
    return *held;
}

[[nodiscard]] std::string text(const doc::Node& node,
                               std::string_view property) {
    const auto found = node.properties.find(property);
    LANE_CHECK(found != node.properties.end());
    const auto* const held = std::get_if<std::string>(&found->second);
    LANE_CHECK(held != nullptr);
    return *held;
}

[[nodiscard]] doc::Root base_document() {
    doc::Root root;
    root = root.set(stage(1U));
    root = root.set(stage(2U));
    root = root.set(lane(10U, 1U, 100, "Foreground"));
    root = root.set(lane(20U, 1U, -50, "Background"));
    root = root.set(lane(30U, 2U, 0, "Other Scene"));
    return root;
}

void create_normalizes_order_and_allocates_one_stable_identity() {
    doc::Root root = base_document();
    root = root.set(doc::Node{doc::NodeId{40U}, "occupied", {}});
    auto result = app::plan_lane_authoring(
        root,
        app::LaneAuthorRequest{app::LaneAuthorAction::Create,
                               scene::SceneId{1U}, {}, {}, "Graphics"},
        40U);
    LANE_CHECK(result);
    const app::LaneAuthorPlan& plan = result.value();
    LANE_CHECK(plan.created == doc::NodeId{41U});
    LANE_CHECK(plan.next_node == 42U);
    LANE_CHECK(integer(*plan.document.get(doc::NodeId{20U}),
                       scene::schema::lane_order) == 0);
    LANE_CHECK(integer(*plan.document.get(doc::NodeId{10U}),
                       scene::schema::lane_order) == 10);
    const doc::Node& created = *plan.document.get(doc::NodeId{41U});
    LANE_CHECK(integer(created, scene::schema::lane_order) == 20);
    LANE_CHECK(text(created, scene::schema::lane_name) == "Graphics");
    LANE_CHECK(integer(created, scene::schema::scene_id) == 1);
}

void rename_toggle_and_move_are_deterministic_and_scene_scoped() {
    doc::Root root = base_document();
    auto renamed = app::plan_lane_authoring(
        root,
        app::LaneAuthorRequest{app::LaneAuthorAction::Rename,
                               scene::SceneId{1U}, doc::NodeId{10U}, {}, "Hero"},
        100U);
    LANE_CHECK(renamed);
    LANE_CHECK(text(*renamed.value().document.get(doc::NodeId{10U}),
                    scene::schema::lane_name) == "Hero");

    auto muted = app::plan_lane_authoring(
        renamed.value().document,
        app::LaneAuthorRequest{app::LaneAuthorAction::ToggleMute,
                               scene::SceneId{1U}, doc::NodeId{10U}},
        100U);
    LANE_CHECK(muted);
    LANE_CHECK(boolean(*muted.value().document.get(doc::NodeId{10U}),
                       scene::schema::lane_muted));

    auto locked = app::plan_lane_authoring(
        muted.value().document,
        app::LaneAuthorRequest{app::LaneAuthorAction::ToggleLock,
                               scene::SceneId{1U}, doc::NodeId{10U}},
        100U);
    LANE_CHECK(locked);
    LANE_CHECK(boolean(*locked.value().document.get(doc::NodeId{10U}),
                       scene::schema::lane_locked));

    // Sync lock is its own switch: on, then off again, the lock untouched.
    auto synced = app::plan_lane_authoring(
        locked.value().document,
        app::LaneAuthorRequest{app::LaneAuthorAction::ToggleSyncLock,
                               scene::SceneId{1U}, doc::NodeId{10U}},
        100U);
    LANE_CHECK(synced);
    LANE_CHECK(boolean(*synced.value().document.get(doc::NodeId{10U}),
                       scene::schema::lane_sync_locked));
    LANE_CHECK(boolean(*synced.value().document.get(doc::NodeId{10U}),
                       scene::schema::lane_locked));
    auto unsynced = app::plan_lane_authoring(
        synced.value().document,
        app::LaneAuthorRequest{app::LaneAuthorAction::ToggleSyncLock,
                               scene::SceneId{1U}, doc::NodeId{10U}},
        100U);
    LANE_CHECK(unsynced);
    LANE_CHECK(!boolean(*unsynced.value().document.get(doc::NodeId{10U}),
                        scene::schema::lane_sync_locked));

    auto moved = app::plan_lane_authoring(
        locked.value().document,
        app::LaneAuthorRequest{app::LaneAuthorAction::MoveEarlier,
                               scene::SceneId{1U}, doc::NodeId{10U}},
        100U);
    LANE_CHECK(moved);
    LANE_CHECK(integer(*moved.value().document.get(doc::NodeId{10U}),
                       scene::schema::lane_order) == 0);
    LANE_CHECK(integer(*moved.value().document.get(doc::NodeId{20U}),
                       scene::schema::lane_order) == 10);
    LANE_CHECK(integer(*moved.value().document.get(doc::NodeId{30U}),
                       scene::schema::lane_order) == 0);

    // A UI drop may cross any number of bands, but it remains one absolute
    // authoring operation: the source takes the target's former stack place.
    doc::Root with_third =
        moved.value().document.set(lane(40U, 1U, 20, "Top"));
    auto moved_to = app::plan_lane_authoring(
        with_third,
        app::LaneAuthorRequest{app::LaneAuthorAction::MoveTo,
                               scene::SceneId{1U}, doc::NodeId{10U},
                               doc::NodeId{40U}},
        100U);
    LANE_CHECK(moved_to);
    LANE_CHECK(integer(*moved_to.value().document.get(doc::NodeId{20U}),
                       scene::schema::lane_order) == 0);
    LANE_CHECK(integer(*moved_to.value().document.get(doc::NodeId{40U}),
                       scene::schema::lane_order) == 10);
    LANE_CHECK(integer(*moved_to.value().document.get(doc::NodeId{10U}),
                       scene::schema::lane_order) == 20);

    auto moved_back = app::plan_lane_authoring(
        moved_to.value().document,
        app::LaneAuthorRequest{app::LaneAuthorAction::MoveTo,
                               scene::SceneId{1U}, doc::NodeId{10U},
                               doc::NodeId{20U}},
        100U);
    LANE_CHECK(moved_back);
    LANE_CHECK(integer(*moved_back.value().document.get(doc::NodeId{10U}),
                       scene::schema::lane_order) == 0);
    LANE_CHECK(integer(*moved_back.value().document.get(doc::NodeId{20U}),
                       scene::schema::lane_order) == 10);
    LANE_CHECK(integer(*moved_back.value().document.get(doc::NodeId{40U}),
                       scene::schema::lane_order) == 20);
}

void nonempty_delete_requires_a_choice_and_delete_clips_cascades() {
    doc::Root root = base_document();
    root = root.set(clip(50U, 1U, 10U));
    root = root.set(effect(51U, 1U, 50U));
    root = root.set(channel(52U, 1U, 51U));
    root = root.set(channel(53U, 1U, 20U));
    auto rejected = app::plan_lane_authoring(
        root,
        app::LaneAuthorRequest{app::LaneAuthorAction::DeleteEmpty,
                               scene::SceneId{1U}, doc::NodeId{10U}},
        100U);
    LANE_CHECK(!rejected);
    LANE_CHECK(rejected.error() == app::LaneAuthorError::NonEmptyLane);

    auto deleted = app::plan_lane_authoring(
        root,
        app::LaneAuthorRequest{app::LaneAuthorAction::DeleteClips,
                               scene::SceneId{1U}, doc::NodeId{10U}},
        100U);
    LANE_CHECK(deleted);
    for (const core::u64 id : {10U, 50U, 51U, 52U}) {
        LANE_CHECK(!deleted.value().document.contains(doc::NodeId{id}));
    }
    LANE_CHECK(deleted.value().document.contains(doc::NodeId{53U}));
    LANE_CHECK(integer(*deleted.value().document.get(doc::NodeId{20U}),
                       scene::schema::lane_order) == 0);
}

void delete_move_rewrites_lane_reference_and_rejects_cross_scene() {
    doc::Root root = base_document();
    root = root.set(clip(50U, 1U, 10U));
    auto moved = app::plan_lane_authoring(
        root,
        app::LaneAuthorRequest{app::LaneAuthorAction::DeleteMoveClips,
                               scene::SceneId{1U}, doc::NodeId{10U},
                               doc::NodeId{20U}},
        100U);
    LANE_CHECK(moved);
    LANE_CHECK(!moved.value().document.contains(doc::NodeId{10U}));
    LANE_CHECK(integer(*moved.value().document.get(doc::NodeId{50U}),
                       scene::schema::lane_id) == 20);

    auto cross_scene = app::plan_lane_authoring(
        root,
        app::LaneAuthorRequest{app::LaneAuthorAction::DeleteMoveClips,
                               scene::SceneId{1U}, doc::NodeId{10U},
                               doc::NodeId{30U}},
        100U);
    LANE_CHECK(!cross_scene);
    LANE_CHECK(cross_scene.error()
               == app::LaneAuthorError::CrossSceneDestination);
}

void validation_rejects_missing_scene_name_and_exhausted_identity() {
    const doc::Root root = base_document();
    auto scene_missing = app::plan_lane_authoring(
        root,
        app::LaneAuthorRequest{app::LaneAuthorAction::Create,
                               scene::SceneId{99U}},
        100U);
    LANE_CHECK(!scene_missing);
    LANE_CHECK(scene_missing.error() == app::LaneAuthorError::InvalidScene);

    auto bad_name = app::plan_lane_authoring(
        root,
        app::LaneAuthorRequest{app::LaneAuthorAction::Rename,
                               scene::SceneId{1U}, doc::NodeId{10U}, {}, {}},
        100U);
    LANE_CHECK(!bad_name);
    LANE_CHECK(bad_name.error() == app::LaneAuthorError::InvalidName);

    auto exhausted = app::plan_lane_authoring(
        root,
        app::LaneAuthorRequest{app::LaneAuthorAction::Create,
                               scene::SceneId{1U}},
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max()) + 1U);
    LANE_CHECK(!exhausted);
    LANE_CHECK(exhausted.error() == app::LaneAuthorError::IdExhausted);
}

void create_respects_the_evaluated_scene_lane_budget() {
    doc::Root root;
    root = root.set(stage(1U));
    for (core::u32 index = 0U; index < scene::max_scene_lanes; ++index) {
        root = root.set(lane(
            1'000U + index, 1U, static_cast<std::int64_t>(index), "Lane"));
    }
    auto result = app::plan_lane_authoring(
        root,
        app::LaneAuthorRequest{app::LaneAuthorAction::Create,
                               scene::SceneId{1U}},
        2'000U);
    LANE_CHECK(!result);
    LANE_CHECK(result.error() == app::LaneAuthorError::LaneLimit);
}

void one_lane_plan_is_one_atomic_undoable_transaction() {
    const doc::Root root = base_document();
    auto created = app::plan_lane_authoring(
        root,
        app::LaneAuthorRequest{app::LaneAuthorAction::Create,
                               scene::SceneId{1U}, {}, {}, "Overlay"},
        100U);
    LANE_CHECK(created);
    cmd::DirectorScheduler scheduler{root};
    auto scheduled = scheduler.schedule_document_replace(
        created.value().document, cmd::UndoContextId{2U},
        cmd::TransactionId{7U}, cmd::FrameNumber{1U});
    LANE_CHECK(scheduled);
    LANE_CHECK(scheduler.undo_depth(cmd::UndoContextId{2U}) == 1U);
    LANE_CHECK(scheduler.projected_document().contains(created.value().created));
    auto undone = scheduler.schedule_undo(
        cmd::UndoContextId{2U}, cmd::TransactionId{8U},
        cmd::FrameNumber{1U});
    LANE_CHECK(undone);
    LANE_CHECK(doc::diff(scheduler.projected_document(), root).changes.empty());
}

[[nodiscard]] doc::Node placed(core::u64 id, core::u64 lane_id,
                               std::int64_t start, std::int64_t duration) {
    doc::Node node = clip(id, 1U, lane_id);
    node.properties.insert_or_assign(std::string{scene::schema::clip_start},
                                     whole(start));
    node.properties.insert_or_assign(std::string{scene::schema::clip_duration},
                                     whole(duration));
    node.properties.insert_or_assign(
        std::string{scene::schema::sequence_count}, whole(500));
    return node;
}

[[nodiscard]] doc::NodeId channel_of(core::u64 target,
                                     scene::PropertyChannel component) {
    return app::channel_node_id(doc::NodeId{target},
                                static_cast<core::u64>(component));
}

[[nodiscard]] app::LaneAuthorRequest split_at(core::u64 lane_id,
                                              std::int64_t frame) {
    app::LaneAuthorRequest request{app::LaneAuthorAction::Split,
                                   scene::SceneId{1U}, doc::NodeId{lane_id}};
    request.frame = frame;
    request.show_rate = scene::MediaFrameRate{24U, 1U};
    return request;
}

// Foreground (lane 10) holds 0..10, 20..40 with ramps, and 40..50, plus an
// opacity channel and a brightness effect with its own channel. Background
// (lane 20) holds 20..40, which a cut on Foreground must not touch.
[[nodiscard]] doc::Root split_document() {
    doc::Root root = base_document();
    doc::Node screen = *root.get(doc::NodeId{10U});
    screen.properties.insert_or_assign(std::string{scene::schema::position_x},
                                       doc::PropertyValue{3.5});
    root = root.set(screen);
    root = root.set(placed(50U, 10U, 0, 10));
    doc::Node ramped = placed(51U, 10U, 20, 20);
    ramped.properties.insert_or_assign(std::string{scene::schema::fade_in},
                                       whole(2));
    ramped.properties.insert_or_assign(std::string{scene::schema::fade_out},
                                       whole(4));
    root = root.set(ramped);
    root = root.set(placed(52U, 10U, 40, 10));
    root = root.set(placed(53U, 20U, 20, 20));
    root = root.set(channel(
        channel_of(10U, scene::PropertyChannel::Opacity).value(), 1U, 10U));
    root = root.set(effect(60U, 1U, 10U));
    root = root.set(channel(
        channel_of(60U, scene::PropertyChannel::Brightness).value(), 1U, 60U));
    return root;
}

void split_gives_what_follows_the_cut_to_a_copy_of_the_track() {
    const doc::Root root = split_document();
    auto result = app::plan_lane_authoring(root, split_at(10U, 25), 100U);
    LANE_CHECK(result);
    const doc::Root& after = result.value().document;
    // The new track is the next free identity, the same screen under its own
    // name, directly in front of the track it came from.
    LANE_CHECK(result.value().created == doc::NodeId{100U});
    const doc::Node& second = *after.get(doc::NodeId{100U});
    LANE_CHECK(second.type == scene::schema::timeline_lane_type);
    LANE_CHECK(text(second, scene::schema::lane_name) == "Foreground (2)");
    const auto x = second.properties.find(scene::schema::position_x);
    LANE_CHECK(x != second.properties.end()
               && std::get<double>(x->second) == 3.5);
    LANE_CHECK(integer(*after.get(doc::NodeId{20U}), scene::schema::lane_order)
               == 0);
    LANE_CHECK(integer(*after.get(doc::NodeId{10U}), scene::schema::lane_order)
               == 10);
    LANE_CHECK(integer(second, scene::schema::lane_order) == 20);

    // Before the cut stays, from it moves, and across it is cut in two: the
    // head keeps its node and its head ramp, the tail is the next identity
    // after the copied effect, starting on the source frame showing at the
    // cut and keeping the tail ramp.
    LANE_CHECK(*after.get(doc::NodeId{50U}) == *root.get(doc::NodeId{50U}));
    LANE_CHECK(integer(*after.get(doc::NodeId{52U}), scene::schema::lane_id)
               == 100);
    const doc::Node& head = *after.get(doc::NodeId{51U});
    LANE_CHECK(integer(head, scene::schema::lane_id) == 10);
    LANE_CHECK(integer(head, scene::schema::clip_start) == 20);
    LANE_CHECK(integer(head, scene::schema::clip_duration) == 5);
    LANE_CHECK(integer(head, scene::schema::fade_in) == 2);
    LANE_CHECK(integer(head, scene::schema::fade_out) == 0);
    const doc::Node& tail = *after.get(doc::NodeId{102U});
    LANE_CHECK(tail.type == scene::schema::media_type);
    LANE_CHECK(integer(tail, scene::schema::lane_id) == 100);
    LANE_CHECK(integer(tail, scene::schema::clip_start) == 25);
    LANE_CHECK(integer(tail, scene::schema::clip_duration) == 15);
    LANE_CHECK(integer(tail, scene::schema::source_in) == 5);
    LANE_CHECK(integer(tail, scene::schema::fade_in) == 0);
    LANE_CHECK(integer(tail, scene::schema::fade_out) == 4);
    // Another track's clip under the cut is not this track's to cut.
    LANE_CHECK(*after.get(doc::NodeId{53U}) == *root.get(doc::NodeId{53U}));

    // The track's keys and look come with it, each channel at the node the
    // new owner's own key edits address.
    const doc::Node* const opacity =
        after.get(channel_of(100U, scene::PropertyChannel::Opacity));
    LANE_CHECK(opacity != nullptr);
    LANE_CHECK(integer(*opacity, scene::schema::channel_target) == 100);
    const doc::Node& look = *after.get(doc::NodeId{101U});
    LANE_CHECK(look.type == scene::schema::effect_type);
    LANE_CHECK(integer(look, scene::schema::effect_target) == 100);
    const doc::Node* const brightness =
        after.get(channel_of(101U, scene::PropertyChannel::Brightness));
    LANE_CHECK(brightness != nullptr);
    LANE_CHECK(integer(*brightness, scene::schema::channel_target) == 101);
    // The originals are untouched.
    LANE_CHECK(*after.get(channel_of(10U, scene::PropertyChannel::Opacity))
               == *root.get(channel_of(10U, scene::PropertyChannel::Opacity)));
    LANE_CHECK(*after.get(doc::NodeId{60U}) == *root.get(doc::NodeId{60U}));
    LANE_CHECK(result.value().next_node == 103U);

    // Splitting either half again counts on rather than nesting names.
    auto again = app::plan_lane_authoring(after, split_at(100U, 45), 103U);
    LANE_CHECK(again);
    LANE_CHECK(text(*again.value().document.get(again.value().created),
                    scene::schema::lane_name)
               == "Foreground (3)");

    // The split is one undo step.
    cmd::DirectorScheduler scheduler{root};
    LANE_CHECK(scheduler.schedule_document_replace(
        after, cmd::UndoContextId{2U}, cmd::TransactionId{7U},
        cmd::FrameNumber{1U}));
    LANE_CHECK(scheduler.schedule_undo(cmd::UndoContextId{2U},
                                       cmd::TransactionId{8U},
                                       cmd::FrameNumber{1U}));
    LANE_CHECK(doc::diff(scheduler.projected_document(), root).changes.empty());
}

void split_refuses_a_move_and_what_is_locked() {
    const doc::Root root = split_document();
    // A cut before every clip, or after them all, would leave one track
    // empty: that is a move, and is not done as one.
    for (const std::int64_t frame : {std::int64_t{0}, std::int64_t{50}}) {
        auto refused = app::plan_lane_authoring(root, split_at(10U, frame), 100U);
        LANE_CHECK(!refused);
        LANE_CHECK(refused.error() == app::LaneAuthorError::NothingToSplit);
    }
    // A gap between clips is a clean place to split; nothing is cut.
    auto gap = app::plan_lane_authoring(root, split_at(10U, 15), 100U);
    LANE_CHECK(gap);
    LANE_CHECK(integer(*gap.value().document.get(doc::NodeId{51U}),
                       scene::schema::lane_id) == 100);
    LANE_CHECK(*gap.value().document.get(doc::NodeId{50U})
               == *root.get(doc::NodeId{50U}));

    doc::Node locked_lane = *root.get(doc::NodeId{10U});
    locked_lane.properties.insert_or_assign(
        std::string{scene::schema::lane_locked}, doc::PropertyValue{true});
    auto lane_locked = app::plan_lane_authoring(
        root.set(locked_lane), split_at(10U, 25), 100U);
    LANE_CHECK(!lane_locked);
    LANE_CHECK(lane_locked.error() == app::LaneAuthorError::Locked);

    // A locked clip the cut would move refuses it; one before the cut does
    // not care.
    doc::Node pinned = *root.get(doc::NodeId{52U});
    pinned.properties.insert_or_assign(std::string{scene::schema::locked},
                                       doc::PropertyValue{true});
    auto clip_locked = app::plan_lane_authoring(
        root.set(pinned), split_at(10U, 25), 100U);
    LANE_CHECK(!clip_locked);
    LANE_CHECK(clip_locked.error() == app::LaneAuthorError::Locked);
    doc::Node early = *root.get(doc::NodeId{50U});
    early.properties.insert_or_assign(std::string{scene::schema::locked},
                                      doc::PropertyValue{true});
    LANE_CHECK(app::plan_lane_authoring(root.set(early), split_at(10U, 25),
                                        100U));
}

} // namespace

int main() {
    struct Test final {
        const char* name;
        void (*run)();
    };
    constexpr std::array tests{
        Test{"create normalizes order and allocates stable identity",
             &create_normalizes_order_and_allocates_one_stable_identity},
        Test{"rename toggle and move are deterministic and scoped",
             &rename_toggle_and_move_are_deterministic_and_scene_scoped},
        Test{"nonempty delete choice and clip cascade",
             &nonempty_delete_requires_a_choice_and_delete_clips_cascades},
        Test{"delete move rewrites references and rejects cross scene",
             &delete_move_rewrites_lane_reference_and_rejects_cross_scene},
        Test{"lane validation rejects invalid inputs",
             &validation_rejects_missing_scene_name_and_exhausted_identity},
        Test{"lane creation respects the scene budget",
             &create_respects_the_evaluated_scene_lane_budget},
        Test{"lane plan schedules one atomic undo transaction",
             &one_lane_plan_is_one_atomic_undoable_transaction},
        Test{"split gives what follows the cut to a copy of the track",
             &split_gives_what_follows_the_cut_to_a_copy_of_the_track},
        Test{"split refuses a move and what is locked",
             &split_refuses_a_move_and_what_is_locked},
    };
    std::size_t passed{};
    for (const Test& test : tests) {
        try {
            test.run();
            ++passed;
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << passed << '/' << tests.size() << " tests passed\n";
    return passed == tests.size() ? 0 : 1;
}
