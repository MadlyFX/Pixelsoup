#include "timeline_lane_authoring.hpp"

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
