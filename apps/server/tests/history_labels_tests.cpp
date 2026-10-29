#include "history_labels.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace app = mediaserver::app;
namespace cmd = mediaserver::cmd;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            throw std::runtime_error{"check failed: " #condition};         \
        }                                                                   \
    } while (false)

[[nodiscard]] doc::Node node(std::uint64_t id, std::string type,
                             doc::PropertyMap properties = {}) {
    return doc::Node{doc::NodeId{id}, std::move(type), std::move(properties)};
}

[[nodiscard]] cmd::Command command(cmd::Payload forward, cmd::Payload inverse) {
    cmd::Command result{};
    result.forward = std::move(forward);
    result.inverse = std::move(inverse);
    return result;
}

[[nodiscard]] std::string label_of(std::span<const cmd::Command> commands) {
    return app::describe_history_step(commands, true).label;
}

void additions_and_deletions_name_what_came_and_went() {
    const doc::Node clip = node(1U, "scene.media", {{"source", std::string{"D:/shows/intro.mov"}}});
    const std::array add_clip{command(cmd::PutNode{clip}, cmd::EraseNode{clip.id})};
    CHECK(label_of(add_clip) == "Add clip \"intro.mov\"");
    const auto added = app::describe_history_step(add_clip, true);
    CHECK(added.kind == editor::HistoryStepKind::Add);
    CHECK(added.nodes == std::vector{doc::NodeId{1U}});

    const doc::Node group = node(2U, "scene.group", {{"group_name", std::string{"Lights"}}});
    const std::array delete_group{command(cmd::EraseNode{group.id}, cmd::PutNode{group})};
    CHECK(label_of(delete_group) == "Delete group \"Lights\"");
    CHECK(app::describe_history_step(delete_group, true).kind ==
          editor::HistoryStepKind::Delete);

    const std::array three_points{
        command(cmd::PutNode{node(3U, "scene.mask_point")}, cmd::EraseNode{doc::NodeId{3U}}),
        command(cmd::PutNode{node(4U, "scene.mask_point")}, cmd::EraseNode{doc::NodeId{4U}}),
        command(cmd::PutNode{node(5U, "scene.mask_point")}, cmd::EraseNode{doc::NodeId{5U}}),
    };
    CHECK(label_of(three_points) == "Add 3 mask points");

    const std::array mask_and_points{
        command(cmd::PutNode{node(6U, "scene.mask")}, cmd::EraseNode{doc::NodeId{6U}}),
        command(cmd::PutNode{node(7U, "scene.mask_point")}, cmd::EraseNode{doc::NodeId{7U}}),
        command(cmd::PutNode{node(8U, "scene.mask_point")}, cmd::EraseNode{doc::NodeId{8U}}),
    };
    CHECK(label_of(mask_and_points) == "Add mask and 2 more");

    const doc::Node blur = node(9U, "compositor.blur");
    const std::array add_blur{command(cmd::PutNode{blur}, cmd::EraseNode{blur.id})};
    CHECK(label_of(add_blur) == "Add blur node");
    const doc::Node effect = node(10U, "scene.effect", {{"effect_kind", std::string{"box_blur"}}});
    const std::array add_effect{command(cmd::PutNode{effect}, cmd::EraseNode{effect.id})};
    CHECK(label_of(add_effect) == "Add box blur effect");

    // Generators are all objects; the primitive is what the operator added.
    const doc::Node sphere = node(11U, "scene.object", {{"primitive", std::string{"sphere"}}});
    const std::array add_sphere{command(cmd::PutNode{sphere}, cmd::EraseNode{sphere.id})};
    CHECK(label_of(add_sphere) == "Add sphere");
    const std::array two_kinds{
        command(cmd::PutNode{sphere}, cmd::EraseNode{sphere.id}),
        command(cmd::PutNode{node(12U, "scene.object", {{"primitive", std::string{"box"}}})},
                cmd::EraseNode{doc::NodeId{12U}}),
    };
    CHECK(label_of(two_kinds) == "Add sphere and 1 more");
}

void changes_name_the_one_property_that_moved() {
    const doc::Node before = node(1U, "scene.object", {{"position_x", 0.0}, {"position_y", 0.0}, {"opacity", 1.0}});
    doc::Node moved = before;
    moved.properties.insert_or_assign("position_x", 2.0);
    moved.properties.insert_or_assign("position_y", 3.0);
    // A drag coalesces into several commands on the same node.
    doc::Node halfway = before;
    halfway.properties.insert_or_assign("position_x", 1.0);
    const std::array drag{
        command(cmd::PutNode{halfway}, cmd::PutNode{before}),
        command(cmd::PutNode{moved}, cmd::PutNode{halfway}),
    };
    const auto step = app::describe_history_step(drag, true);
    CHECK(step.label == "Change position of object");
    CHECK(step.kind == editor::HistoryStepKind::Change);
    CHECK(step.nodes.size() == 1U);

    doc::Node faded = moved;
    faded.properties.insert_or_assign("opacity", 0.5);
    const std::array two_properties{command(cmd::PutNode{faded}, cmd::PutNode{before})};
    CHECK(label_of(two_properties) == "Change object");

    const doc::Node other = node(2U, "scene.object", {{"position_x", 0.0}});
    doc::Node other_moved = other;
    other_moved.properties.insert_or_assign("position_x", 5.0);
    const std::array both{
        command(cmd::PutNode{moved}, cmd::PutNode{before}),
        command(cmd::PutNode{other_moved}, cmd::PutNode{other}),
    };
    CHECK(label_of(both) == "Change position of 2 objects");
}

void runtime_activation_and_round_trips_are_not_changes() {
    const doc::Node model = node(1U, "scene.model", {{"asset_commit_digest", std::int64_t{1}}});
    doc::Node activated = model;
    activated.properties.insert_or_assign("asset_commit_digest", std::int64_t{2});
    const std::array runtime_only{command(cmd::PutNode{activated}, cmd::PutNode{model})};
    const auto step = app::describe_history_step(runtime_only, true);
    CHECK(step.label == "No visible change");
    CHECK(step.nodes.empty());

    // Added and removed inside one step leaves nothing behind.
    const doc::Node scratch = node(2U, "scene.object");
    const std::array transient{
        command(cmd::PutNode{scratch}, cmd::EraseNode{scratch.id}),
        command(cmd::EraseNode{scratch.id}, cmd::PutNode{scratch}),
    };
    CHECK(app::describe_history_step(transient, true).nodes.empty());

    // A paste that also renumbers a sibling is still the paste.
    const doc::Node sibling = node(3U, "scene.object", {{"group_order", std::int64_t{1}}});
    doc::Node renumbered = sibling;
    renumbered.properties.insert_or_assign("group_order", std::int64_t{2});
    const std::array insert{
        command(cmd::PutNode{renumbered}, cmd::PutNode{sibling}),
        command(cmd::PutNode{node(4U, "scene.light")}, cmd::EraseNode{doc::NodeId{4U}}),
    };
    const auto mixed = app::describe_history_step(insert, true);
    CHECK(mixed.kind == editor::HistoryStepKind::Mixed);
    CHECK(mixed.label == "Add light");
    CHECK(mixed.nodes.size() == 2U);
}

void undone_steps_describe_the_original_edit() {
    cmd::DirectorScheduler scheduler;
    const cmd::UndoContextId scene{2U};
    const doc::Node before = node(1U, "scene.object", {{"opacity", 1.0}});
    doc::Node after = before;
    after.properties.insert_or_assign("opacity", 0.25);
    CHECK(scheduler.schedule_edit(cmd::PutNode{before}, cmd::EraseNode{before.id}, scene,
                                  cmd::TransactionId{1U}, cmd::FrameNumber{1U}).has_value());
    CHECK(scheduler.schedule_edit(cmd::PutNode{after}, cmd::PutNode{before}, scene,
                                  cmd::TransactionId{2U}, cmd::FrameNumber{1U}).has_value());

    app::HistoryDescriptions labels;
    const auto applied = labels.describe(scheduler, scene);
    CHECK(applied.steps.size() == 2U);
    CHECK(applied.steps[0].label == "Add object");
    CHECK(applied.steps[1].label == "Change opacity of object");

    const std::array undo{cmd::TransactionId{3U}, cmd::TransactionId{4U}};
    CHECK(scheduler.schedule_history_jump(scene, cmd::HistoryStepId{}, undo,
                                          cmd::FrameNumber{2U}).has_value());
    const auto undone = labels.describe(scheduler, scene);
    CHECK(undone.revision > applied.revision);
    for (std::size_t index = 0U; index < 2U; ++index) {
        CHECK(!undone.steps[index].applied);
        CHECK(undone.steps[index].step == applied.steps[index].step);
        CHECK(undone.steps[index].label == applied.steps[index].label);
        CHECK(undone.steps[index].kind == applied.steps[index].kind);
        CHECK(undone.steps[index].nodes == applied.steps[index].nodes);
    }
}

void explicit_names_follow_their_step_until_it_goes() {
    cmd::DirectorScheduler scheduler;
    const cmd::UndoContextId scene{2U};
    const cmd::UndoContextId compositor{6U};
    app::HistoryDescriptions labels;
    CHECK(scheduler.schedule_edit(cmd::PutNode{node(1U, "scene.object")}, cmd::EraseNode{doc::NodeId{1U}},
                                  scene, cmd::TransactionId{1U}, cmd::FrameNumber{1U}).has_value());
    labels.name(scene, cmd::TransactionId{1U}, "Paste 1 clip");
    // A name for a transaction that never authored anything is dropped.
    labels.name(scene, cmd::TransactionId{99U}, "Never happened");
    CHECK(labels.describe(scheduler, scene).steps.front().label == "Paste 1 clip");

    CHECK(scheduler.schedule_undo(scene, cmd::TransactionId{2U}, cmd::FrameNumber{2U}).has_value());
    CHECK(labels.describe(scheduler, scene).steps.front().label == "Paste 1 clip");
    CHECK(scheduler.schedule_redo(scene, cmd::TransactionId{3U}, cmd::FrameNumber{2U}).has_value());
    CHECK(labels.describe(scheduler, scene).steps.front().label == "Paste 1 clip");

    // The same transaction number in another context is a different step.
    CHECK(scheduler.schedule_edit(cmd::PutNode{node(5U, "compositor.blur")}, cmd::EraseNode{doc::NodeId{5U}},
                                  compositor, cmd::TransactionId{1U}, cmd::FrameNumber{2U}).has_value());
    CHECK(labels.describe(scheduler, compositor).steps.front().label == "Add blur node");

    // Undo then branch discards the named step; a new step with the same
    // transaction number is not given its name.
    CHECK(scheduler.schedule_undo(scene, cmd::TransactionId{4U}, cmd::FrameNumber{2U}).has_value());
    CHECK(scheduler.schedule_edit(cmd::PutNode{node(2U, "scene.light")}, cmd::EraseNode{doc::NodeId{2U}},
                                  scene, cmd::TransactionId{5U}, cmd::FrameNumber{2U}).has_value());
    const auto branched = labels.describe(scheduler, scene);
    CHECK(branched.steps.size() == 1U);
    CHECK(branched.steps.front().label == "Add light");
    labels.name(scene, cmd::TransactionId{99U}, "Still never");
    CHECK(scheduler.schedule_edit(cmd::PutNode{node(99U, "scene.light")}, cmd::EraseNode{doc::NodeId{99U}},
                                  scene, cmd::TransactionId{99U}, cmd::FrameNumber{2U}).has_value());
    CHECK(labels.describe(scheduler, scene).steps.back().label == "Still never");
}

struct Case final {
    const char* name;
    void (*run)();
};

constexpr std::array cases{
    Case{"additions and deletions name what came and went",
         &additions_and_deletions_name_what_came_and_went},
    Case{"changes name the one property that moved",
         &changes_name_the_one_property_that_moved},
    Case{"runtime activation and round trips are not changes",
         &runtime_activation_and_round_trips_are_not_changes},
    Case{"undone steps describe the original edit",
         &undone_steps_describe_the_original_edit},
    Case{"explicit names follow their step until it goes",
         &explicit_names_follow_their_step_until_it_goes},
};

} // namespace

int main() {
    std::size_t failures{};
    for (const Case& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << cases.size() - failures << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
