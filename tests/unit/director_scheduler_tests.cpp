#include "protocol_test_support.hpp"
#include "test_support.hpp"

#include <mediaserver/cmd/director_scheduler.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <variant>

namespace {

namespace command = mediaserver::cmd;
namespace document = mediaserver::doc;
namespace support = mediaserver::test_support;

MS_TEST("Director assigns chained revisions and seals every frame") {
    command::DirectorScheduler director;
    auto first = director.schedule_edit(
        support::put(1U, 10),
        support::erase(1U),
        command::UndoContextId{1U},
        command::TransactionId{1U},
        command::FrameNumber{2U});
    auto second = director.schedule_edit(
        support::put(2U, 20),
        support::erase(2U),
        command::UndoContextId{1U},
        command::TransactionId{1U},
        command::FrameNumber{2U});

    MS_CHECK(first.has_value());
    MS_CHECK(second.has_value());
    MS_CHECK(first.value().id == command::CommandId{1U});
    MS_CHECK(first.value().base == document::Version{0U});
    MS_CHECK(first.value().result == document::Version{1U});
    MS_CHECK(second.value().base == document::Version{1U});
    MS_CHECK(second.value().result == document::Version{2U});

    auto empty_seal = director.seal_frame(command::FrameNumber{1U});
    MS_CHECK(empty_seal.has_value());
    MS_CHECK(empty_seal.value().commands.empty());
    MS_CHECK(empty_seal.value().base == document::Version{0U});
    MS_CHECK(empty_seal.value().result == document::Version{0U});
    MS_CHECK(director.checkpoint().cursor.next_command == command::CommandId{1U});

    auto content_seal = director.seal_frame(command::FrameNumber{2U});
    MS_CHECK(content_seal.has_value());
    MS_CHECK(content_seal.value().commands.size() == 2U);
    MS_CHECK(content_seal.value().result == document::Version{2U});
    MS_CHECK(director.sealed_document().contains(document::NodeId{1U}));
    MS_CHECK(director.checkpoint().cursor.next_command == command::CommandId{3U});
    MS_CHECK(director.checkpoint().document.version() == document::Version{2U});
}

MS_TEST("Director rejects invalid inverses without consuming identity or revision") {
    command::DirectorScheduler director;
    auto invalid = director.schedule_edit(
        support::put(1U, 10),
        support::erase(99U),
        command::UndoContextId{1U},
        command::TransactionId{1U},
        command::FrameNumber{1U});
    MS_CHECK(!invalid.has_value());
    MS_CHECK(invalid.error() == command::ScheduleError::InvalidInverse);
    MS_CHECK(director.projected_document().version() == document::Version{0U});

    auto valid = director.schedule_edit(
        support::put(1U, 10),
        support::erase(1U),
        command::UndoContextId{1U},
        command::TransactionId{1U},
        command::FrameNumber{2U});
    MS_CHECK(valid.has_value());
    MS_CHECK(valid.value().id == command::CommandId{1U});

    auto regression = director.schedule_edit(
        support::put(2U, 20),
        support::erase(2U),
        command::UndoContextId{1U},
        command::TransactionId{2U},
        command::FrameNumber{1U});
    MS_CHECK(!regression.has_value());
    MS_CHECK(regression.error() == command::ScheduleError::FrameRegression);
}

MS_TEST("Director authors undo in reverse order with fresh chained commands") {
    command::DirectorScheduler director;
    const auto original = support::node(1U, 10);
    auto create = director.schedule_edit(
        command::PutNode{original},
        support::erase(1U),
        command::UndoContextId{1U},
        command::TransactionId{7U},
        command::FrameNumber{1U});
    auto update = director.schedule_edit(
        support::put(1U, 20),
        command::PutNode{original},
        command::UndoContextId{1U},
        command::TransactionId{7U},
        command::FrameNumber{1U});
    MS_CHECK(create.has_value());
    MS_CHECK(update.has_value());
    MS_CHECK(director.undo_depth(command::UndoContextId{1U}) == 1U);
    MS_CHECK(director.seal_frame(command::FrameNumber{1U}).has_value());

    auto undo = director.schedule_undo(
        command::UndoContextId{1U},
        command::TransactionId{8U},
        command::FrameNumber{2U});
    MS_CHECK(undo.has_value());
    MS_CHECK(undo.value().size() == 2U);
    MS_CHECK(undo.value()[0].id == command::CommandId{3U});
    MS_CHECK(undo.value()[0].base == document::Version{2U});
    MS_CHECK(undo.value()[0].result == document::Version{3U});
    MS_CHECK(undo.value()[1].base == document::Version{3U});
    MS_CHECK(undo.value()[1].result == document::Version{4U});
    MS_CHECK(director.projected_document().empty());
    MS_CHECK(director.undo_depth(command::UndoContextId{1U}) == 0U);
    MS_CHECK(director.redo_depth(command::UndoContextId{1U}) == 1U);

    auto redo = director.schedule_redo(
        command::UndoContextId{1U}, command::TransactionId{9U},
        command::FrameNumber{2U});
    MS_CHECK(redo.has_value());
    MS_CHECK(redo.value().size() == 2U);
    MS_CHECK(redo.value()[0].id == command::CommandId{5U});
    MS_CHECK(redo.value()[0].base == document::Version{4U});
    MS_CHECK(redo.value()[0].result == document::Version{5U});
    MS_CHECK(redo.value()[1].id == command::CommandId{6U});
    MS_CHECK(redo.value()[1].base == document::Version{5U});
    MS_CHECK(redo.value()[1].result == document::Version{6U});
    const auto* const redone =
        director.projected_document().get(document::NodeId{1U});
    MS_CHECK(redone != nullptr);
    MS_CHECK(*redone == support::node(1U, 20));
    MS_CHECK(director.undo_depth(command::UndoContextId{1U}) == 1U);
    MS_CHECK(director.redo_depth(command::UndoContextId{1U}) == 0U);
}

MS_TEST("Director undo and redo restore a complete Saturation transaction") {
    const document::Node original{
        document::NodeId{80U},
        "scene.effect",
        {
            {"effect_target", std::int64_t{100}},
            {"effect_kind", std::string{"saturation"}},
            {"effect_order", std::int64_t{3}},
            {"effect_enabled", true},
            {"saturation", 1.75},
        },
    };
    auto reordered = original;
    reordered.properties.insert_or_assign("effect_order", std::int64_t{-4});
    document::Root initial;
    initial = initial.set(original);
    command::DirectorScheduler director{initial};
    const command::UndoContextId scene{4U};
    const command::TransactionId gesture{20U};

    auto reorder = director.schedule_edit(
        command::PutNode{reordered}, command::PutNode{original}, scene, gesture,
        command::FrameNumber{1U});
    auto remove = director.schedule_edit(
        command::EraseNode{original.id}, command::PutNode{reordered}, scene,
        gesture, command::FrameNumber{1U});
    MS_CHECK(reorder.has_value());
    MS_CHECK(remove.has_value());
    MS_CHECK(!director.projected_document().contains(original.id));
    MS_CHECK(director.undo_depth(scene) == 1U);
    MS_CHECK(director.seal_frame(command::FrameNumber{1U}).has_value());

    auto undo = director.schedule_undo(
        scene, command::TransactionId{21U}, command::FrameNumber{2U});
    MS_CHECK(undo.has_value());
    MS_CHECK(undo.value().size() == 2U);
    const auto* const restored = director.projected_document().get(original.id);
    MS_CHECK(restored != nullptr);
    MS_CHECK(*restored == original);
    MS_CHECK(std::get<std::int64_t>(
                 restored->properties.at("effect_order")) == 3);
    MS_CHECK(director.redo_depth(scene) == 1U);

    auto redo = director.schedule_redo(
        scene, command::TransactionId{22U}, command::FrameNumber{2U});
    MS_CHECK(redo.has_value());
    MS_CHECK(redo.value().size() == 2U);
    MS_CHECK(!director.projected_document().contains(original.id));
    MS_CHECK(director.undo_depth(scene) == 1U);
    MS_CHECK(director.redo_depth(scene) == 0U);
}

MS_TEST("Director redo is LIFO across multiple undos") {
    command::DirectorScheduler director;
    const command::UndoContextId scene{2U};
    MS_CHECK(director
                 .schedule_edit(
                     support::put(1U, 10), support::erase(1U), scene,
                     command::TransactionId{1U}, command::FrameNumber{1U})
                 .has_value());
    MS_CHECK(director
                 .schedule_edit(
                     support::put(2U, 20), support::erase(2U), scene,
                     command::TransactionId{2U}, command::FrameNumber{1U})
                 .has_value());
    MS_CHECK(director.seal_frame(command::FrameNumber{1U}).has_value());

    MS_CHECK(director
                 .schedule_undo(
                     scene, command::TransactionId{10U},
                     command::FrameNumber{2U})
                 .has_value());
    MS_CHECK(!director.projected_document().contains(document::NodeId{2U}));
    MS_CHECK(director.projected_document().contains(document::NodeId{1U}));
    MS_CHECK(director
                 .schedule_undo(
                     scene, command::TransactionId{11U},
                     command::FrameNumber{2U})
                 .has_value());
    MS_CHECK(director.projected_document().empty());
    MS_CHECK(director.redo_depth(scene) == 2U);

    MS_CHECK(director
                 .schedule_redo(
                     scene, command::TransactionId{12U},
                     command::FrameNumber{2U})
                 .has_value());
    MS_CHECK(director.projected_document().contains(document::NodeId{1U}));
    MS_CHECK(!director.projected_document().contains(document::NodeId{2U}));
    MS_CHECK(director
                 .schedule_redo(
                     scene, command::TransactionId{13U},
                     command::FrameNumber{2U})
                 .has_value());
    MS_CHECK(director.projected_document().contains(document::NodeId{1U}));
    MS_CHECK(director.projected_document().contains(document::NodeId{2U}));
    MS_CHECK(director.redo_depth(scene) == 0U);
    MS_CHECK(director.undo_depth(scene) == 2U);
}

MS_TEST("a fresh edit invalidates redo only after it succeeds") {
    command::DirectorScheduler director;
    const command::UndoContextId scene{2U};
    MS_CHECK(director
                 .schedule_edit(
                     support::put(1U, 10), support::erase(1U), scene,
                     command::TransactionId{1U}, command::FrameNumber{1U})
                 .has_value());
    MS_CHECK(director.seal_frame(command::FrameNumber{1U}).has_value());
    MS_CHECK(director
                 .schedule_undo(
                     scene, command::TransactionId{2U},
                     command::FrameNumber{2U})
                 .has_value());
    MS_CHECK(director.redo_depth(scene) == 1U);

    auto rejected = director.schedule_edit(
        support::put(2U, 20), support::erase(99U), scene,
        command::TransactionId{3U}, command::FrameNumber{2U});
    MS_CHECK(!rejected.has_value());
    MS_CHECK(director.redo_depth(scene) == 1U);

    MS_CHECK(director
                 .schedule_edit(
                     support::put(2U, 20), support::erase(2U), scene,
                     command::TransactionId{4U}, command::FrameNumber{2U})
                 .has_value());
    MS_CHECK(director.redo_depth(scene) == 0U);
    MS_CHECK(director.authoring_footprint().undo_transactions == 1U);
    auto invalidated = director.schedule_redo(
        scene, command::TransactionId{5U}, command::FrameNumber{2U});
    MS_CHECK(!invalidated.has_value());
    MS_CHECK(invalidated.error() == command::DirectorUndoError::NoHistory);
}

MS_TEST("Director groups a multi-frame drag into one undoable transaction") {
    command::DirectorScheduler director;
    const command::UndoContextId scene{2U};

    // The clip exists first, as its own transaction and sealed frame.
    auto create = director.schedule_edit(
        support::put(1U, 10),
        support::erase(1U),
        scene,
        command::TransactionId{1U},
        command::FrameNumber{1U});
    MS_CHECK(create.has_value());
    MS_CHECK(director.seal_frame(command::FrameNumber{1U}).has_value());

    // A drag moves it across three sealed frames. Every frame's edit shares the
    // one gesture id used as the transaction, exactly as schedule_scene_edit
    // does - one command per frame (so nodes see the motion), one transaction.
    const command::TransactionId drag{100U};
    struct Step final {
        std::int64_t to;
        std::int64_t from;
        std::uint32_t frame;
    };
    for (const auto step : {Step{20, 10, 2U}, Step{30, 20, 3U}, Step{40, 30, 4U}}) {
        auto moved = director.schedule_edit(
            support::put(1U, step.to),
            support::put(1U, step.from),
            scene,
            drag,
            command::FrameNumber{step.frame});
        MS_CHECK(moved.has_value());
        MS_CHECK(director.seal_frame(command::FrameNumber{step.frame}).has_value());
    }

    // Three frames of motion, but create + drag is two undo units, not four.
    MS_CHECK(director.undo_depth(scene) == 2U);

    // Undoing the drag reverses all three moves back to the pre-drag value in a
    // single step, leaving only the create on the stack.
    auto undo = director.schedule_undo(
        scene, command::TransactionId{101U}, command::FrameNumber{5U});
    MS_CHECK(undo.has_value());
    MS_CHECK(undo.value().size() == 3U);
    MS_CHECK(director.undo_depth(scene) == 1U);
    const auto* const node = director.projected_document().get(document::NodeId{1U});
    MS_CHECK(node != nullptr);
    MS_CHECK(*node == support::node(1U, 10));
}

MS_TEST("semantic coalescing reuses one transaction across live frames") {
    document::Root initial;
    initial = initial.set(support::node(1U, 0));
    command::DirectorScheduler director{initial};
    const command::UndoContextId scene{2U};
    const command::TransactionId gesture{100U};
    const command::CoalescingKey key{11U, 22U, 33U, 1U, 4U};

    auto first = director.begin_coalesced_edit(
        support::put(1U, 10), support::put(1U, 0), scene, key, gesture,
        command::FrameNumber{1U});
    MS_CHECK(first.has_value());
    MS_CHECK(first.value().transaction == gesture);
    MS_CHECK(director.seal_frame(command::FrameNumber{1U}).has_value());

    auto second = director.continue_coalesced_edit(
        support::put(1U, 20), support::put(1U, 10), scene, key,
        command::FrameNumber{2U});
    MS_CHECK(second.has_value());
    MS_CHECK(second.value().transaction == gesture);
    MS_CHECK(director.seal_frame(command::FrameNumber{2U}).has_value());

    auto third = director.continue_coalesced_edit(
        support::put(1U, 30), support::put(1U, 20), scene, key,
        command::FrameNumber{3U});
    MS_CHECK(third.has_value());
    MS_CHECK(third.value().transaction == gesture);
    MS_CHECK(director.undo_depth(scene) == 1U);
    MS_CHECK(director.seal_frame(command::FrameNumber{3U}).has_value());

    auto undo = director.schedule_undo(
        scene, command::TransactionId{101U}, command::FrameNumber{4U});
    MS_CHECK(undo.has_value());
    MS_CHECK(undo.value().size() == 3U);
    MS_CHECK(*director.projected_document().get(document::NodeId{1U}) ==
             support::node(1U, 0));
}

MS_TEST("coalescing keys enforce every semantic identity boundary") {
    const command::CoalescingKey base{1U, 2U, 3U, 4U, 5U};
    MS_CHECK((base != command::CoalescingKey{9U, 2U, 3U, 4U, 5U}));
    MS_CHECK((base != command::CoalescingKey{1U, 9U, 3U, 4U, 5U}));
    MS_CHECK((base != command::CoalescingKey{1U, 2U, 9U, 4U, 5U}));
    MS_CHECK((base != command::CoalescingKey{1U, 2U, 3U, 9U, 5U}));
    MS_CHECK((base != command::CoalescingKey{1U, 2U, 3U, 4U, 9U}));

    document::Root initial;
    initial = initial.set(support::node(1U, 0));
    command::DirectorScheduler director{initial};
    const command::UndoContextId scene{2U};
    MS_CHECK(director
                 .begin_coalesced_edit(
                     support::put(1U, 10), support::put(1U, 0), scene, base,
                     command::TransactionId{1U}, command::FrameNumber{1U})
                 .has_value());

    const command::CoalescingKey other_property{1U, 2U, 9U, 4U, 5U};
    auto mismatch = director.continue_coalesced_edit(
        support::put(1U, 20), support::put(1U, 10), scene, other_property,
        command::FrameNumber{1U});
    MS_CHECK(!mismatch.has_value());
    MS_CHECK(mismatch.error() ==
             command::ScheduleError::CoalescingSessionMismatch);
    MS_CHECK(*director.projected_document().get(document::NodeId{1U}) ==
             support::node(1U, 10));

    // A mismatch closes the old session; returning to its key cannot silently
    // append to an earlier transaction.
    auto stale = director.continue_coalesced_edit(
        support::put(1U, 20), support::put(1U, 10), scene, base,
        command::FrameNumber{1U});
    MS_CHECK(!stale.has_value());
    MS_CHECK(stale.error() ==
             command::ScheduleError::CoalescingSessionMismatch);

    MS_CHECK(director
                 .begin_coalesced_edit(
                     support::put(1U, 20), support::put(1U, 10), scene,
                     other_property, command::TransactionId{2U},
                     command::FrameNumber{1U})
                 .has_value());
    MS_CHECK(director.undo_depth(scene) == 2U);
    director.end_coalescing();
    auto ended = director.continue_coalesced_edit(
        support::put(1U, 30), support::put(1U, 20), scene, other_property,
        command::FrameNumber{1U});
    MS_CHECK(!ended.has_value());
    MS_CHECK(ended.error() ==
             command::ScheduleError::CoalescingSessionMismatch);

    auto reused = director.begin_coalesced_edit(
        support::put(1U, 30), support::put(1U, 20), scene, other_property,
        command::TransactionId{2U}, command::FrameNumber{1U});
    MS_CHECK(!reused.has_value());
    MS_CHECK(reused.error() == command::ScheduleError::DuplicateTransaction);
    MS_CHECK(director.undo_depth(scene) == 2U);
}

MS_TEST("rejected coalesced edits create no history and preserve redo") {
    command::DirectorScheduler director;
    const command::UndoContextId scene{2U};
    MS_CHECK(director
                 .schedule_edit(
                     support::put(1U, 10), support::erase(1U), scene,
                     command::TransactionId{1U}, command::FrameNumber{1U})
                 .has_value());
    MS_CHECK(director.seal_frame(command::FrameNumber{1U}).has_value());
    MS_CHECK(director
                 .schedule_undo(
                     scene, command::TransactionId{2U},
                     command::FrameNumber{2U})
                 .has_value());
    MS_CHECK(director.undo_depth(scene) == 0U);
    MS_CHECK(director.redo_depth(scene) == 1U);
    const std::size_t commands_before =
        director.commands_for(command::FrameNumber{2U}).size();

    auto rejected = director.begin_coalesced_edit(
        support::put(2U, 20), support::erase(99U), scene,
        command::CoalescingKey{1U, 1U, 1U, 1U, 1U},
        command::TransactionId{3U}, command::FrameNumber{2U});
    MS_CHECK(!rejected.has_value());
    MS_CHECK(rejected.error() == command::ScheduleError::InvalidInverse);
    MS_CHECK(director.undo_depth(scene) == 0U);
    MS_CHECK(director.redo_depth(scene) == 1U);
    MS_CHECK(director.commands_for(command::FrameNumber{2U}).size() ==
             commands_before);
    MS_CHECK(!director.projected_document().contains(document::NodeId{2U}));

    MS_CHECK(director
                 .schedule_redo(
                     scene, command::TransactionId{4U},
                     command::FrameNumber{2U})
                 .has_value());
    MS_CHECK(director.projected_document().contains(document::NodeId{1U}));
}

MS_TEST("a rejected continuation keeps its accepted semantic gesture open") {
    document::Root initial;
    initial = initial.set(support::node(1U, 0));
    command::DirectorScheduler director{initial};
    const command::UndoContextId scene{2U};
    const command::CoalescingKey key{1U, 2U, 3U, 4U, 5U};
    MS_CHECK(director
                 .begin_coalesced_edit(
                     support::put(1U, 10), support::put(1U, 0), scene, key,
                     command::TransactionId{1U}, command::FrameNumber{1U})
                 .has_value());

    auto rejected = director.continue_coalesced_edit(
        support::put(1U, 20), support::put(1U, 999), scene, key,
        command::FrameNumber{1U});
    MS_CHECK(!rejected.has_value());
    MS_CHECK(rejected.error() == command::ScheduleError::InvalidInverse);
    MS_CHECK(director.undo_depth(scene) == 1U);
    MS_CHECK(director.commands_for(command::FrameNumber{1U}).size() == 1U);

    auto continued = director.continue_coalesced_edit(
        support::put(1U, 20), support::put(1U, 10), scene, key,
        command::FrameNumber{1U});
    MS_CHECK(continued.has_value());
    MS_CHECK(continued.value().transaction == command::TransactionId{1U});
    MS_CHECK(director.undo_depth(scene) == 1U);

    // A rejected ordinary edit is not an intervening command and therefore
    // cannot accidentally split the live gesture.
    auto ordinary_rejection = director.schedule_edit(
        support::put(2U, 20), support::erase(99U), scene,
        command::TransactionId{2U}, command::FrameNumber{1U});
    MS_CHECK(!ordinary_rejection.has_value());
    MS_CHECK(director
                 .continue_coalesced_edit(
                     support::put(1U, 30), support::put(1U, 20), scene, key,
                     command::FrameNumber{1U})
                 .has_value());

    // An accepted ordinary command is an explicit boundary even in a different
    // transaction.
    MS_CHECK(director
                 .schedule_edit(
                     support::put(2U, 20), support::erase(2U), scene,
                     command::TransactionId{2U}, command::FrameNumber{1U})
                 .has_value());
    auto closed = director.continue_coalesced_edit(
        support::put(1U, 40), support::put(1U, 30), scene, key,
        command::FrameNumber{1U});
    MS_CHECK(!closed.has_value());
    MS_CHECK(closed.error() ==
             command::ScheduleError::CoalescingSessionMismatch);
}

MS_TEST("history and replacement operations close semantic coalescing") {
    const command::UndoContextId scene{2U};
    const command::CoalescingKey key{1U, 2U, 3U, 4U, 5U};

    {
        document::Root initial;
        initial = initial.set(support::node(1U, 0));
        command::DirectorScheduler director{initial};
        MS_CHECK(director
                     .begin_coalesced_edit(
                         support::put(1U, 10), support::put(1U, 0), scene,
                         key, command::TransactionId{1U},
                         command::FrameNumber{1U})
                     .has_value());
        auto replaced = director.schedule_document_replace(
            director.projected_document(), scene, command::TransactionId{2U},
            command::FrameNumber{1U});
        MS_CHECK(replaced.has_value());
        MS_CHECK(replaced.value().empty());
        auto closed = director.continue_coalesced_edit(
            support::put(1U, 20), support::put(1U, 10), scene, key,
            command::FrameNumber{1U});
        MS_CHECK(!closed.has_value());
        MS_CHECK(closed.error() ==
                 command::ScheduleError::CoalescingSessionMismatch);
    }

    {
        command::DirectorScheduler director;
        MS_CHECK(director
                     .begin_coalesced_edit(
                         support::put(1U, 10), support::erase(1U), scene, key,
                         command::TransactionId{1U},
                         command::FrameNumber{1U})
                     .has_value());
        MS_CHECK(director
                     .schedule_undo(
                         scene, command::TransactionId{2U},
                         command::FrameNumber{1U})
                     .has_value());
        auto after_undo = director.continue_coalesced_edit(
            support::put(1U, 20), support::put(1U, 10), scene, key,
            command::FrameNumber{1U});
        MS_CHECK(!after_undo.has_value());
        MS_CHECK(after_undo.error() ==
                 command::ScheduleError::CoalescingSessionMismatch);

        MS_CHECK(director
                     .schedule_redo(
                         scene, command::TransactionId{3U},
                         command::FrameNumber{1U})
                     .has_value());
        MS_CHECK(director
                     .begin_coalesced_edit(
                         support::put(1U, 20), support::put(1U, 10), scene,
                         key, command::TransactionId{4U},
                         command::FrameNumber{1U})
                     .has_value());
        auto empty_redo = director.schedule_redo(
            scene, command::TransactionId{5U}, command::FrameNumber{1U});
        MS_CHECK(!empty_redo.has_value());
        MS_CHECK(empty_redo.error() == command::DirectorUndoError::NoHistory);
        auto after_redo = director.continue_coalesced_edit(
            support::put(1U, 30), support::put(1U, 20), scene, key,
            command::FrameNumber{1U});
        MS_CHECK(!after_redo.has_value());
        MS_CHECK(after_redo.error() ==
                 command::ScheduleError::CoalescingSessionMismatch);
    }

    {
        command::DirectorScheduler director;
        MS_CHECK(director
                     .begin_coalesced_edit(
                         support::put(1U, 10), support::erase(1U), scene, key,
                         command::TransactionId{1U},
                         command::FrameNumber{1U})
                     .has_value());
        director.clear_undo_history();
        MS_CHECK(director.undo_depth(scene) == 0U);
        auto after_clear = director.continue_coalesced_edit(
            support::put(1U, 20), support::put(1U, 10), scene, key,
            command::FrameNumber{1U});
        MS_CHECK(!after_clear.has_value());
        MS_CHECK(after_clear.error() ==
                 command::ScheduleError::CoalescingSessionMismatch);
    }
}

MS_TEST("coalescing metadata never changes serialized command identity") {
    document::Root initial;
    initial = initial.set(support::node(1U, 0));
    command::DirectorScheduler first{initial};
    command::DirectorScheduler second{initial};
    const command::UndoContextId scene{2U};
    const command::TransactionId transaction{7U};

    auto left = first.begin_coalesced_edit(
        support::put(1U, 10), support::put(1U, 0), scene,
        command::CoalescingKey{1U, 2U, 3U, 4U, 5U}, transaction,
        command::FrameNumber{1U});
    auto right = second.begin_coalesced_edit(
        support::put(1U, 10), support::put(1U, 0), scene,
        command::CoalescingKey{50U, 40U, 30U, 20U, 10U}, transaction,
        command::FrameNumber{1U});
    MS_CHECK(left.has_value());
    MS_CHECK(right.has_value());
    MS_CHECK(left.value() == right.value());
    MS_CHECK(command::fingerprint(left.value()) ==
             command::fingerprint(right.value()));
}

MS_TEST("Director detects undo conflicts caused by another context") {
    command::DirectorScheduler director;
    const auto first = support::node(1U, 10);
    auto comp_edit = director.schedule_edit(
        command::PutNode{first},
        support::erase(1U),
        command::UndoContextId{1U},
        command::TransactionId{1U},
        command::FrameNumber{1U});
    auto scene_edit = director.schedule_edit(
        support::put(1U, 20),
        command::PutNode{first},
        command::UndoContextId{2U},
        command::TransactionId{2U},
        command::FrameNumber{1U});
    MS_CHECK(comp_edit.has_value());
    MS_CHECK(scene_edit.has_value());

    auto undo = director.schedule_undo(
        command::UndoContextId{1U},
        command::TransactionId{3U},
        command::FrameNumber{2U});
    MS_CHECK(!undo.has_value());
    MS_CHECK(undo.error() == command::DirectorUndoError::HistoryConflict);
    MS_CHECK(director.projected_document().version() == document::Version{2U});
}

MS_TEST("Director detects redo conflicts caused by another context") {
    document::Root initial;
    initial = initial.set(support::node(1U, 10));
    command::DirectorScheduler director{initial};
    const command::UndoContextId scene{1U};
    MS_CHECK(director
                 .schedule_edit(
                     support::put(1U, 20), support::put(1U, 10), scene,
                     command::TransactionId{1U}, command::FrameNumber{1U})
                 .has_value());
    MS_CHECK(director.seal_frame(command::FrameNumber{1U}).has_value());
    MS_CHECK(director
                 .schedule_undo(
                     scene, command::TransactionId{2U},
                     command::FrameNumber{2U})
                 .has_value());
    MS_CHECK(director.redo_depth(scene) == 1U);

    MS_CHECK(director
                 .schedule_edit(
                     support::put(1U, 30), support::put(1U, 10),
                     command::UndoContextId{2U}, command::TransactionId{3U},
                     command::FrameNumber{2U})
                 .has_value());
    auto redo = director.schedule_redo(
        scene, command::TransactionId{4U}, command::FrameNumber{2U});
    MS_CHECK(!redo.has_value());
    MS_CHECK(redo.error() == command::DirectorUndoError::HistoryConflict);
    MS_CHECK(director.redo_depth(scene) == 1U);
    MS_CHECK(*director.projected_document().get(document::NodeId{1U}) ==
             support::node(1U, 30));
}

MS_TEST("Director prepare keeps committed state hidden until matching commit") {
    command::DirectorScheduler director;
    auto authored = director.schedule_edit(
        support::put(1U, 10),
        support::erase(1U),
        command::UndoContextId{1U},
        command::TransactionId{1U},
        command::FrameNumber{1U});
    MS_CHECK(authored.has_value());
    const auto before = director.checkpoint();

    auto prepared = director.prepare_seal(command::FrameNumber{1U});
    MS_CHECK(prepared.has_value());
    MS_CHECK(director.prepared_seal() != nullptr);
    MS_CHECK(*director.prepared_seal() == prepared.value().seal);
    MS_CHECK(director.seal_for(command::FrameNumber{1U}) == nullptr);
    MS_CHECK(director.sealed_through() == command::FrameNumber{0U});
    MS_CHECK(director.sealed_document().empty());
    MS_CHECK(director.checkpoint().cursor == before.cursor);

    auto wrong_token = prepared.value().token;
    wrong_token.frame = command::FrameNumber{2U};
    MS_CHECK(!director.commit_prepared_seal(wrong_token));
    MS_CHECK(director.prepared_seal() != nullptr);
    MS_CHECK(director.checkpoint().cursor == before.cursor);

    MS_CHECK(director.commit_prepared_seal(prepared.value().token));
    MS_CHECK(director.prepared_seal() == nullptr);
    MS_CHECK(director.sealed_through() == command::FrameNumber{1U});
    MS_CHECK(director.sealed_document().contains(document::NodeId{1U}));
    MS_CHECK(director.checkpoint().cursor.revision == document::Version{1U});
    MS_CHECK(director.checkpoint().cursor.next_command == command::CommandId{2U});
    MS_CHECK(director.seal_for(command::FrameNumber{1U}) != nullptr);
    MS_CHECK(*director.seal_for(command::FrameNumber{1U}) == prepared.value().seal);
    MS_CHECK(!director.commit_prepared_seal(prepared.value().token));
}

MS_TEST("Director abort removes staged visibility and retries the exact seal") {
    command::DirectorScheduler director;
    MS_CHECK(director.schedule_edit(
        support::put(1U, 10),
        support::erase(1U),
        command::UndoContextId{1U},
        command::TransactionId{1U},
        command::FrameNumber{1U}).has_value());

    auto skipped = director.prepare_seal(command::FrameNumber{2U});
    MS_CHECK(!skipped.has_value());
    MS_CHECK(skipped.error() == command::SealError::NonConsecutiveFrame);
    MS_CHECK(director.prepared_seal() == nullptr);

    auto first = director.prepare_seal(command::FrameNumber{1U});
    MS_CHECK(first.has_value());
    MS_CHECK(director.abort_prepared_seal(first.value().token));
    MS_CHECK(!director.abort_prepared_seal(first.value().token));
    MS_CHECK(director.prepared_seal() == nullptr);
    MS_CHECK(director.seal_for(command::FrameNumber{1U}) == nullptr);
    MS_CHECK(director.checkpoint().cursor.through_frame == command::FrameNumber{0U});

    auto retried = director.prepare_seal(command::FrameNumber{1U});
    MS_CHECK(retried.has_value());
    MS_CHECK(retried.value().seal == first.value().seal);
    MS_CHECK(retried.value().token != first.value().token);
    MS_CHECK(director.commit_prepared_seal(retried.value().token));
    MS_CHECK(*director.seal_for(command::FrameNumber{1U}) == first.value().seal);
}

MS_TEST("Director stale same-frame tokens cannot commit or abort a newer prepare") {
    command::DirectorScheduler director;
    MS_CHECK(director.schedule_edit(
        support::put(1U, 10),
        support::erase(1U),
        command::UndoContextId{1U},
        command::TransactionId{1U},
        command::FrameNumber{1U}).has_value());

    auto first = director.prepare_seal(command::FrameNumber{1U});
    MS_CHECK(first.has_value());
    MS_CHECK(director.abort_prepared_seal(first.value().token));

    auto additional = director.schedule_edit(
        support::put(2U, 20),
        support::erase(2U),
        command::UndoContextId{1U},
        command::TransactionId{2U},
        command::FrameNumber{1U});
    MS_CHECK(additional.has_value());
    MS_CHECK(additional.value().id == command::CommandId{2U});

    auto second = director.prepare_seal(command::FrameNumber{1U});
    MS_CHECK(second.has_value());
    MS_CHECK(second.value().token.frame == first.value().token.frame);
    MS_CHECK(second.value().token.generation > first.value().token.generation);
    MS_CHECK(second.value().seal.commands.size() == 2U);

    MS_CHECK(!director.commit_prepared_seal(first.value().token));
    MS_CHECK(!director.abort_prepared_seal(first.value().token));
    MS_CHECK(director.prepared_seal() != nullptr);
    MS_CHECK(*director.prepared_seal() == second.value().seal);
    MS_CHECK(director.seal_for(command::FrameNumber{1U}) == nullptr);
    MS_CHECK(director.checkpoint().cursor.through_frame == command::FrameNumber{0U});

    MS_CHECK(director.commit_prepared_seal(second.value().token));
    MS_CHECK(director.prepared_seal() == nullptr);
    MS_CHECK(director.sealed_document().contains(document::NodeId{1U}));
    MS_CHECK(director.sealed_document().contains(document::NodeId{2U}));
    MS_CHECK(director.checkpoint().cursor.next_command == command::CommandId{3U});
}

MS_TEST("Director rejects every scheduling mutation while a seal is prepared") {
    command::DirectorScheduler director;
    MS_CHECK(director.schedule_edit(
        support::put(1U, 10),
        support::erase(1U),
        command::UndoContextId{1U},
        command::TransactionId{1U},
        command::FrameNumber{1U}).has_value());
    auto prepared = director.prepare_seal(command::FrameNumber{1U});
    MS_CHECK(prepared.has_value());

    auto edit = director.schedule_edit(
        support::put(2U, 20),
        support::erase(2U),
        command::UndoContextId{1U},
        command::TransactionId{2U},
        command::FrameNumber{2U});
    MS_CHECK(!edit.has_value());
    MS_CHECK(edit.error() == command::ScheduleError::SealPrepared);

    auto undo = director.schedule_undo(
        command::UndoContextId{1U},
        command::TransactionId{2U},
        command::FrameNumber{2U});
    MS_CHECK(!undo.has_value());
    MS_CHECK(undo.error() == command::DirectorUndoError::SealPrepared);

    auto nested_prepare = director.prepare_seal(command::FrameNumber{1U});
    MS_CHECK(!nested_prepare.has_value());
    MS_CHECK(nested_prepare.error() == command::SealError::PrepareOutstanding);
    MS_CHECK(director.projected_document().version() == document::Version{1U});
    MS_CHECK(director.commands_for(command::FrameNumber{1U}).size() == 1U);
    MS_CHECK(director.undo_depth(command::UndoContextId{1U}) == 1U);

    MS_CHECK(director.abort_prepared_seal(prepared.value().token));
    auto resumed = director.schedule_edit(
        support::put(2U, 20),
        support::erase(2U),
        command::UndoContextId{1U},
        command::TransactionId{2U},
        command::FrameNumber{2U});
    MS_CHECK(resumed.has_value());
    MS_CHECK(resumed.value().id == command::CommandId{2U});
    MS_CHECK(resumed.value().base == document::Version{1U});
}

MS_TEST("Director convenience sealing never steals an outstanding prepare") {
    command::DirectorScheduler director;
    auto prepared = director.prepare_seal(command::FrameNumber{1U});
    MS_CHECK(prepared.has_value());

    auto convenience = director.seal_frame(command::FrameNumber{1U});
    MS_CHECK(!convenience.has_value());
    MS_CHECK(convenience.error() == command::SealError::PrepareOutstanding);
    MS_CHECK(director.prepared_seal() != nullptr);
    MS_CHECK(director.seal_for(command::FrameNumber{1U}) == nullptr);
    MS_CHECK(director.sealed_through() == command::FrameNumber{0U});

    MS_CHECK(director.commit_prepared_seal(prepared.value().token));
    MS_CHECK(director.seal_for(command::FrameNumber{1U}) != nullptr);
}

MS_TEST("command fingerprints cover payload and inverse content deterministically") {
    command::DirectorScheduler director;
    auto authored = director.schedule_edit(
        support::put(1U, 10),
        support::erase(1U),
        command::UndoContextId{1U},
        command::TransactionId{1U},
        command::FrameNumber{1U});
    MS_CHECK(authored.has_value());
    const auto copy = authored.value();
    auto changed = copy;
    changed.inverse = support::erase(2U);
    MS_CHECK(command::fingerprint(copy) == command::fingerprint(authored.value()));
    MS_CHECK(command::fingerprint(copy) != command::fingerprint(changed));
}

MS_TEST("Director retires authoring indexes so a long show has flat memory") {
    command::DirectorSchedulerLimits limits{};
    limits.retained_sealed_frames = 8U;
    limits.max_undo_transactions = 4U;
    command::DirectorScheduler director{
        {}, command::FrameNumber{0U}, command::CommandId{1U}, limits};

    // A frame carrying an edit and a long run of empty frames behind it, which
    // is exactly the shape a playing show produces once the transport is an
    // anchor.
    for (mediaserver::core::u64 frame = 1U; frame <= 200U; ++frame) {
        if (frame % 25U == 0U) {
            auto edit = director.schedule_edit(
                support::put(1U, static_cast<std::int64_t>(frame)),
                frame == 25U ? support::erase(1U)
                             : support::put(1U, static_cast<std::int64_t>(frame - 25U)),
                command::UndoContextId{1U},
                command::TransactionId{frame},
                command::FrameNumber{frame});
            MS_CHECK(edit.has_value());
        }
        MS_CHECK(director.seal_frame(command::FrameNumber{frame}).has_value());
    }

    const auto footprint = director.authoring_footprint();
    MS_CHECK(footprint.retained_seals == limits.retained_sealed_frames);
    // Only the frames inside the retention window that actually carried an
    // edit are still indexed.
    MS_CHECK(footprint.frames_with_commands <= limits.retained_sealed_frames);
    MS_CHECK(footprint.undo_transactions == limits.max_undo_transactions);
    MS_CHECK(footprint.undo_contexts == 1U);
    MS_CHECK(director.undo_depth(command::UndoContextId{1U}) == 4U);

    // The window is the most recent frames, and everything behind it is gone.
    MS_CHECK(director.seal_for(command::FrameNumber{200U}) != nullptr);
    MS_CHECK(director.seal_for(command::FrameNumber{193U}) != nullptr);
    MS_CHECK(director.seal_for(command::FrameNumber{192U}) == nullptr);
    MS_CHECK(!director.commands_for(command::FrameNumber{200U}).empty());
    MS_CHECK(director.commands_for(command::FrameNumber{175U}).empty());
    MS_CHECK(footprint.frames_with_commands == 1U);

    // Sealing continues to work, and the footprint does not grow.
    for (mediaserver::core::u64 frame = 201U; frame <= 400U; ++frame) {
        MS_CHECK(director.seal_frame(command::FrameNumber{frame}).has_value());
    }
    const auto later = director.authoring_footprint();
    MS_CHECK(later.retained_seals == footprint.retained_seals);
    MS_CHECK(later.undo_transactions == footprint.undo_transactions);
    MS_CHECK(later.frames_with_commands <= footprint.frames_with_commands);
    MS_CHECK(director.sealed_through() == command::FrameNumber{400U});
    MS_CHECK(director.sealed_document().version() == document::Version{8U});
}

MS_TEST("Director undo reaches back exactly as far as its retained depth") {
    command::DirectorSchedulerLimits limits{};
    limits.max_undo_transactions = 2U;
    command::DirectorScheduler director{
        {}, command::FrameNumber{0U}, command::CommandId{1U}, limits};

    MS_CHECK(director
                 .schedule_edit(
                     support::put(1U, 10),
                     support::erase(1U),
                     command::UndoContextId{1U},
                     command::TransactionId{1U},
                     command::FrameNumber{1U})
                 .has_value());
    MS_CHECK(director
                 .schedule_edit(
                     support::put(2U, 20),
                     support::erase(2U),
                     command::UndoContextId{1U},
                     command::TransactionId{2U},
                     command::FrameNumber{1U})
                 .has_value());
    MS_CHECK(director.undo_depth(command::UndoContextId{1U}) == 2U);

    // The third transaction pushes the first out of the retained depth, so it
    // is no longer undoable and its commands are no longer held.
    MS_CHECK(director
                 .schedule_edit(
                     support::put(3U, 30),
                     support::erase(3U),
                     command::UndoContextId{1U},
                     command::TransactionId{3U},
                     command::FrameNumber{1U})
                 .has_value());
    MS_CHECK(director.undo_depth(command::UndoContextId{1U}) == 2U);
    MS_CHECK(director.authoring_footprint().undo_transactions == 2U);
    MS_CHECK(director.seal_frame(command::FrameNumber{1U}).has_value());

    // Undo still works, and consumes the retained transactions newest first.
    auto undo = director.schedule_undo(
        command::UndoContextId{1U},
        command::TransactionId{10U},
        command::FrameNumber{2U});
    MS_CHECK(undo.has_value());
    MS_CHECK(!director.projected_document().contains(document::NodeId{3U}));
    MS_CHECK(director.projected_document().contains(document::NodeId{1U}));
    MS_CHECK(director.redo_depth(command::UndoContextId{1U}) == 1U);

    auto redo = director.schedule_redo(
        command::UndoContextId{1U}, command::TransactionId{11U},
        command::FrameNumber{2U});
    MS_CHECK(redo.has_value());
    MS_CHECK(director.projected_document().contains(document::NodeId{3U}));
    MS_CHECK(director.redo_depth(command::UndoContextId{1U}) == 0U);

    // Undo/redo replaces the retained payload for transaction 3 rather than
    // consuming a third logical-edit slot.
    MS_CHECK(director.authoring_footprint().undo_transactions == 2U);
}

MS_TEST("history navigation does not consume bounded edit retention") {
    command::DirectorSchedulerLimits limits{};
    limits.max_undo_transactions = 2U;
    command::DirectorScheduler director{
        {}, command::FrameNumber{0U}, command::CommandId{1U}, limits};
    const command::UndoContextId scene{2U};

    MS_CHECK(director
                 .schedule_edit(
                     support::put(1U, 10), support::erase(1U), scene,
                     command::TransactionId{1U}, command::FrameNumber{1U})
                 .has_value());
    MS_CHECK(director
                 .schedule_edit(
                     support::put(2U, 20), support::erase(2U), scene,
                     command::TransactionId{2U}, command::FrameNumber{1U})
                 .has_value());

    MS_CHECK(director
                 .schedule_undo(
                     scene, command::TransactionId{3U},
                     command::FrameNumber{2U})
                 .has_value());
    MS_CHECK(director.undo_depth(scene) == 1U);
    MS_CHECK(director.redo_depth(scene) == 1U);
    MS_CHECK(director.authoring_footprint().undo_transactions == 2U);

    // The older user edit is still present after navigating the newer one.
    MS_CHECK(director
                 .schedule_undo(
                     scene, command::TransactionId{4U},
                     command::FrameNumber{2U})
                 .has_value());
    MS_CHECK(director.projected_document().empty());
    MS_CHECK(director.undo_depth(scene) == 0U);
    MS_CHECK(director.redo_depth(scene) == 2U);
    MS_CHECK(director.authoring_footprint().undo_transactions == 2U);

    MS_CHECK(director
                 .schedule_redo(
                     scene, command::TransactionId{5U},
                     command::FrameNumber{2U})
                 .has_value());
    MS_CHECK(director.projected_document().contains(document::NodeId{1U}));
    MS_CHECK(!director.projected_document().contains(document::NodeId{2U}));
    MS_CHECK(director.authoring_footprint().undo_transactions == 2U);
}

MS_TEST("Director retains every index when the limits are disabled") {
    command::DirectorSchedulerLimits limits{};
    limits.retained_sealed_frames = 0U;
    limits.max_undo_transactions = 0U;
    command::DirectorScheduler director{
        {}, command::FrameNumber{0U}, command::CommandId{1U}, limits};

    for (mediaserver::core::u64 frame = 1U; frame <= 40U; ++frame) {
        MS_CHECK(director
                     .schedule_edit(
                         support::put(1U, static_cast<std::int64_t>(frame)),
                         frame == 1U
                             ? support::erase(1U)
                             : support::put(
                                   1U, static_cast<std::int64_t>(frame - 1U)),
                         command::UndoContextId{1U},
                         command::TransactionId{frame},
                         command::FrameNumber{frame})
                     .has_value());
        MS_CHECK(director.seal_frame(command::FrameNumber{frame}).has_value());
    }

    const auto footprint = director.authoring_footprint();
    MS_CHECK(footprint.retained_seals == 40U);
    MS_CHECK(footprint.frames_with_commands == 40U);
    MS_CHECK(footprint.undo_transactions == 40U);
    MS_CHECK(director.seal_for(command::FrameNumber{1U}) != nullptr);
    MS_CHECK(!director.commands_for(command::FrameNumber{1U}).empty());
}

MS_TEST("Director replaces a document as one ordered undo transaction") {
    document::Root initial;
    initial = initial.set(support::node(1U, 10));
    initial = initial.set(support::node(2U, 20));
    command::DirectorScheduler director{initial};

    document::Root replacement;
    replacement = replacement.set(support::node(2U, 200));
    replacement = replacement.set(support::node(3U, 30));
    auto scheduled = director.schedule_document_replace(
        replacement, command::UndoContextId{7U},
        command::TransactionId{44U}, command::FrameNumber{1U});

    MS_CHECK(scheduled.has_value());
    MS_CHECK(scheduled.value().size() == 3U);
    MS_CHECK(document::diff(director.projected_document(), replacement)
                 .changes.empty());
    MS_CHECK(director.undo_depth(command::UndoContextId{7U}) == 1U);
    MS_CHECK(director.commands_for(command::FrameNumber{1U}).size() == 3U);
}

MS_TEST("Director document replacement is atomic when a later edit exhausts") {
    const auto almost_exhausted = document::Root::restore(
        {}, document::Version{
                std::numeric_limits<mediaserver::core::u64>::max() - 1U});
    MS_CHECK(almost_exhausted.has_value());
    command::DirectorScheduler director{*almost_exhausted};

    document::Root replacement;
    replacement = replacement.set(support::node(1U, 10));
    replacement = replacement.set(support::node(2U, 20));
    auto scheduled = director.schedule_document_replace(
        replacement, command::UndoContextId{7U},
        command::TransactionId{1U}, command::FrameNumber{1U});

    MS_CHECK(!scheduled.has_value());
    MS_CHECK(scheduled.error() == command::ScheduleError::VersionExhausted);
    MS_CHECK(director.projected_document().empty());
    MS_CHECK(director.projected_document().version() ==
             almost_exhausted->version());
    MS_CHECK(director.commands_for(command::FrameNumber{1U}).empty());
    MS_CHECK(director.undo_depth(command::UndoContextId{7U}) == 0U);
}

MS_TEST("Director can forget project undo and redo without dropping commands") {
    command::DirectorScheduler director;
    MS_CHECK(director
                 .schedule_edit(
                     support::put(1U, 10), support::erase(1U),
                     command::UndoContextId{2U},
                     command::TransactionId{1U},
                     command::FrameNumber{1U})
                 .has_value());
    MS_CHECK(director.undo_depth(command::UndoContextId{2U}) == 1U);
    MS_CHECK(director.seal_frame(command::FrameNumber{1U}).has_value());
    MS_CHECK(director
                 .schedule_undo(
                     command::UndoContextId{2U},
                     command::TransactionId{2U},
                     command::FrameNumber{2U})
                 .has_value());
    MS_CHECK(director.redo_depth(command::UndoContextId{2U}) == 1U);

    director.clear_undo_history();

    MS_CHECK(director.undo_depth(command::UndoContextId{2U}) == 0U);
    MS_CHECK(director.redo_depth(command::UndoContextId{2U}) == 0U);
    MS_CHECK(director.authoring_footprint().undo_transactions == 0U);
    MS_CHECK(director.commands_for(command::FrameNumber{1U}).size() == 1U);
    MS_CHECK(director.commands_for(command::FrameNumber{2U}).size() == 1U);
    MS_CHECK(director.seal_frame(command::FrameNumber{2U}).has_value());
    MS_CHECK(!director.sealed_document().contains(document::NodeId{1U}));
}

} // namespace
