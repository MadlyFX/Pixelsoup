#include "protocol_test_support.hpp"
#include "test_support.hpp"

#include <mediaserver/cmd/node_command_bus.hpp>

#include <array>
#include <limits>

namespace {

namespace command = mediaserver::cmd;
namespace document = mediaserver::doc;
namespace support = mediaserver::test_support;

MS_TEST("node prepares a sealed frame from reordered delivery and publishes only at commit") {
    command::DirectorScheduler director;
    MS_CHECK(director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U}).has_value());
    MS_CHECK(director.schedule_edit(
        support::put(2U, 20), support::erase(2U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U}).has_value());
    auto seal = director.seal_frame(command::FrameNumber{1U});
    MS_CHECK(seal.has_value());
    const auto commands = director.commands_for(command::FrameNumber{1U});

    command::NodeCommandBus node;
    MS_CHECK(node.receive(seal.value()) == command::ReceiveStatus::Accepted);
    MS_CHECK(node.receive(commands[1]) == command::ReceiveStatus::Accepted);
    MS_CHECK(node.document().empty());
    MS_CHECK(node.receive(commands[0]) == command::ReceiveStatus::Prepared);
    MS_CHECK(node.is_prepared(command::FrameNumber{1U}));
    MS_CHECK(node.document().empty());

    const auto committed = node.commit_frame(command::FrameNumber{1U});
    MS_CHECK(committed.status == command::CommitStatus::Committed);
    MS_CHECK(committed.command_count == 2U);
    MS_CHECK(node.document().version() == document::Version{2U});
    MS_CHECK(node.document().contains(document::NodeId{1U}));
    MS_CHECK(node.document().contains(document::NodeId{2U}));
    MS_CHECK(node.receive(commands[0]) == command::ReceiveStatus::Duplicate);
    MS_CHECK(node.receive(seal.value()) == command::ReceiveStatus::Duplicate);
}

MS_TEST("missing sealed data faults at the boundary without publishing") {
    command::DirectorScheduler director;
    MS_CHECK(director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U}).has_value());
    MS_CHECK(director.schedule_edit(
        support::put(2U, 20), support::erase(2U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U}).has_value());
    const auto seal = director.seal_frame(command::FrameNumber{1U});
    const auto commands = director.commands_for(command::FrameNumber{1U});
    MS_CHECK(seal.has_value());

    command::NodeCommandBus node;
    MS_CHECK(node.receive(seal.value()) == command::ReceiveStatus::Accepted);
    MS_CHECK(node.receive(commands[0]) == command::ReceiveStatus::Accepted);
    const auto result = node.commit_frame(command::FrameNumber{1U});
    MS_CHECK(result.status == command::CommitStatus::NeedsResync);
    MS_CHECK(node.fault()->code == command::FaultCode::FrameNotPrepared);
    MS_CHECK(node.document().empty());
    MS_CHECK(node.document().version() == document::Version{0U});
}

MS_TEST("extra, conflicting, and fingerprint-mismatched traffic force resync") {
    command::DirectorScheduler director;
    auto authored = director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U});
    auto seal = director.seal_frame(command::FrameNumber{1U});
    MS_CHECK(authored.has_value());
    MS_CHECK(seal.has_value());

    command::NodeCommandBus extra_node;
    MS_CHECK(extra_node.receive(seal.value()) == command::ReceiveStatus::Accepted);
    auto extra = authored.value();
    extra.id = command::CommandId{99U};
    MS_CHECK(extra_node.receive(extra) == command::ReceiveStatus::NeedsResync);
    MS_CHECK(extra_node.fault()->code == command::FaultCode::UnexpectedCommand);

    command::NodeCommandBus conflict_node;
    MS_CHECK(conflict_node.receive(authored.value()) == command::ReceiveStatus::Accepted);
    auto conflicting = authored.value();
    conflicting.forward = support::put(1U, 99);
    MS_CHECK(conflict_node.receive(conflicting) == command::ReceiveStatus::NeedsResync);
    MS_CHECK(conflict_node.fault()->code == command::FaultCode::ConflictingCommand);

    command::NodeCommandBus fingerprint_node;
    auto corrupted_seal = seal.value();
    corrupted_seal.commands[0].fingerprint = command::CommandFingerprint{1U};
    MS_CHECK(fingerprint_node.receive(corrupted_seal) == command::ReceiveStatus::Accepted);
    MS_CHECK(fingerprint_node.receive(authored.value()) == command::ReceiveStatus::NeedsResync);
    MS_CHECK(fingerprint_node.fault()->code == command::FaultCode::FingerprintMismatch);
}

MS_TEST("semantic failure rejects a fully delivered frame atomically") {
    command::Command valid{
        command::CommandId{1U}, document::Version{0U}, document::Version{1U},
        command::FrameNumber{1U}, command::TransactionId{1U}, command::UndoContextId{1U},
        support::put(1U, 10), support::erase(1U)};
    command::Command invalid{
        command::CommandId{2U}, document::Version{1U}, document::Version{2U},
        command::FrameNumber{1U}, command::TransactionId{1U}, command::UndoContextId{1U},
        support::erase(99U), support::put(99U, 99)};
    command::FrameSeal seal{
        command::FrameNumber{1U}, document::Version{0U}, document::Version{2U},
        {{valid.id, command::fingerprint(valid)}, {invalid.id, command::fingerprint(invalid)}}};

    command::NodeCommandBus node;
    MS_CHECK(node.receive(valid) == command::ReceiveStatus::Accepted);
    MS_CHECK(node.receive(invalid) == command::ReceiveStatus::Accepted);
    MS_CHECK(node.receive(seal) == command::ReceiveStatus::NeedsResync);
    MS_CHECK(node.fault()->code == command::FaultCode::PayloadRejected);
    MS_CHECK(node.document().empty());
    MS_CHECK(node.document().version() == document::Version{0U});
}

MS_TEST("complete-batch receive prepares once and treats exact redelivery as duplicate") {
    command::DirectorScheduler director;
    MS_CHECK(director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U}).has_value());
    MS_CHECK(director.schedule_edit(
        support::put(2U, 20), support::erase(2U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U}).has_value());
    auto seal = director.seal_frame(command::FrameNumber{1U});
    MS_CHECK(seal.has_value());
    const auto commands = director.commands_for(command::FrameNumber{1U});

    command::NodeCommandBus node;
    auto received = node.receive_complete_batch(commands, seal.value());
    MS_CHECK(received.has_value());
    MS_CHECK(received.value() == command::ReceiveStatus::Prepared);
    MS_CHECK(node.is_prepared(command::FrameNumber{1U}));
    MS_CHECK(node.document().empty());
    const auto prepared = node.replay_history_diagnostics();
    MS_CHECK(prepared.frame_states == 1U);
    MS_CHECK(prepared.seen_commands == 2U);
    MS_CHECK(prepared.prepared_frames == 1U);
    MS_CHECK(!node.fault().has_value());

    auto duplicate = node.receive_complete_batch(commands, seal.value());
    MS_CHECK(duplicate.has_value());
    MS_CHECK(duplicate.value() == command::ReceiveStatus::Duplicate);
    MS_CHECK(node.replay_history_diagnostics() == prepared);
    MS_CHECK(!node.fault().has_value());

    const auto committed = node.commit_frame(command::FrameNumber{1U});
    MS_CHECK(committed.status == command::CommitStatus::Committed);
    MS_CHECK(committed.command_count == 2U);
    MS_CHECK(node.document().contains(document::NodeId{1U}));
    MS_CHECK(node.document().contains(document::NodeId{2U}));
}

MS_TEST("complete-batch conflict leaves prebuffered traffic and fault unchanged") {
    command::DirectorScheduler director;
    MS_CHECK(director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U}).has_value());
    MS_CHECK(director.schedule_edit(
        support::put(2U, 20), support::erase(2U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U}).has_value());
    auto seal = director.seal_frame(command::FrameNumber{1U});
    MS_CHECK(seal.has_value());
    const auto commands = director.commands_for(command::FrameNumber{1U});
    auto conflicting = commands[1U];
    conflicting.forward = support::put(2U, 99);

    command::NodeCommandBus node;
    MS_CHECK(node.receive(conflicting) == command::ReceiveStatus::Accepted);
    const auto cursor_before = node.cursor();
    const auto document_before = node.document();
    const auto history_before = node.replay_history_diagnostics();
    MS_CHECK(history_before.frame_states == 1U);
    MS_CHECK(history_before.seen_commands == 1U);
    MS_CHECK(!node.fault().has_value());
    MS_CHECK(node.prepared_checkpoint() == nullptr);

    auto received = node.receive_complete_batch(commands, seal.value());
    MS_CHECK(!received.has_value());
    MS_CHECK(received.error().code == command::BatchReceiveErrorCode::WouldFault);
    MS_CHECK(received.error().fault.has_value());
    MS_CHECK(received.error().fault->code == command::FaultCode::ConflictingCommand);
    MS_CHECK(received.error().fault->frame == command::FrameNumber{1U});
    MS_CHECK(received.error().fault->command == commands[1U].id);
    MS_CHECK(node.cursor() == cursor_before);
    MS_CHECK(document::diff(node.document(), document_before).changes.empty());
    MS_CHECK(node.replay_history_diagnostics() == history_before);
    MS_CHECK(!node.fault().has_value());
    MS_CHECK(!node.is_prepared(command::FrameNumber{1U}));
    MS_CHECK(node.prepared_checkpoint() == nullptr);
    MS_CHECK(node.receive(conflicting) == command::ReceiveStatus::Duplicate);
}

MS_TEST("complete-batch semantic rejection leaves all node state unchanged") {
    const command::Command valid{
        command::CommandId{1U}, document::Version{0U}, document::Version{1U},
        command::FrameNumber{1U}, command::TransactionId{1U},
        command::UndoContextId{1U}, support::put(1U, 10), support::erase(1U)};
    const command::Command invalid{
        command::CommandId{2U}, document::Version{1U}, document::Version{2U},
        command::FrameNumber{1U}, command::TransactionId{1U},
        command::UndoContextId{1U}, support::erase(99U), support::put(99U, 99)};
    const std::array commands{valid, invalid};
    const command::FrameSeal seal{
        command::FrameNumber{1U}, document::Version{0U}, document::Version{2U},
        {{valid.id, command::fingerprint(valid)},
         {invalid.id, command::fingerprint(invalid)}}};

    command::NodeCommandBus node;
    const auto cursor_before = node.cursor();
    const auto document_before = node.document();
    const auto history_before = node.replay_history_diagnostics();
    auto received = node.receive_complete_batch(commands, seal);
    MS_CHECK(!received.has_value());
    MS_CHECK(received.error().code == command::BatchReceiveErrorCode::WouldFault);
    MS_CHECK(received.error().fault.has_value());
    MS_CHECK(received.error().fault->code == command::FaultCode::PayloadRejected);
    MS_CHECK(received.error().fault->command == invalid.id);
    MS_CHECK(node.cursor() == cursor_before);
    MS_CHECK(document::diff(node.document(), document_before).changes.empty());
    MS_CHECK(node.replay_history_diagnostics() == history_before);
    MS_CHECK(!node.fault().has_value());
    MS_CHECK(!node.is_prepared(command::FrameNumber{1U}));
    MS_CHECK(node.prepared_checkpoint() == nullptr);
}

MS_TEST("complete-batch receive records a future frame and prepares the contiguous cascade") {
    command::DirectorScheduler director;
    MS_CHECK(director.schedule_edit(
        support::put(2U, 20), support::erase(2U), command::UndoContextId{1U},
        command::TransactionId{2U}, command::FrameNumber{2U}).has_value());
    auto first_seal = director.seal_frame(command::FrameNumber{1U});
    auto second_seal = director.seal_frame(command::FrameNumber{2U});
    MS_CHECK(first_seal.has_value());
    MS_CHECK(second_seal.has_value());
    const auto second_commands = director.commands_for(command::FrameNumber{2U});

    command::NodeCommandBus node;
    auto future = node.receive_complete_batch(second_commands, second_seal.value());
    MS_CHECK(future.has_value());
    MS_CHECK(future.value() == command::ReceiveStatus::Accepted);
    MS_CHECK(!node.is_prepared(command::FrameNumber{2U}));
    MS_CHECK(node.replay_history_diagnostics().frame_states == 1U);

    auto contiguous = node.receive_complete_batch(
        std::span<const command::Command>{}, first_seal.value());
    MS_CHECK(contiguous.has_value());
    MS_CHECK(contiguous.value() == command::ReceiveStatus::Prepared);
    MS_CHECK(node.is_prepared(command::FrameNumber{1U}));
    MS_CHECK(node.is_prepared(command::FrameNumber{2U}));
    const auto history = node.replay_history_diagnostics();
    MS_CHECK(history.frame_states == 2U);
    MS_CHECK(history.seen_commands == 1U);
    MS_CHECK(history.prepared_frames == 2U);
    MS_CHECK(!node.fault().has_value());

    MS_CHECK(node.commit_frame(command::FrameNumber{1U}).status
        == command::CommitStatus::Committed);
    MS_CHECK(node.commit_frame(command::FrameNumber{2U}).status
        == command::CommitStatus::Committed);
    MS_CHECK(node.document().contains(document::NodeId{2U}));
    MS_CHECK(node.cursor() == director.checkpoint().cursor);
}

MS_TEST("malformed identity chains, result revisions, and conflicting seals are rejected") {
    command::DirectorScheduler director;
    auto authored = director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U});
    auto seal = director.seal_frame(command::FrameNumber{1U});
    MS_CHECK(authored.has_value());
    MS_CHECK(seal.has_value());

    auto gapped_command = authored.value();
    gapped_command.id = command::CommandId{2U};
    auto gapped_seal = seal.value();
    gapped_seal.commands[0] = command::SealedCommand{
        gapped_command.id, command::fingerprint(gapped_command)};
    command::NodeCommandBus gap_node;
    MS_CHECK(gap_node.receive(gapped_command) == command::ReceiveStatus::Accepted);
    MS_CHECK(gap_node.receive(gapped_seal) == command::ReceiveStatus::NeedsResync);
    MS_CHECK(gap_node.fault()->code == command::FaultCode::MalformedSeal);

    auto wrong_result = authored.value();
    wrong_result.result = document::Version{9U};
    auto wrong_result_seal = seal.value();
    wrong_result_seal.result = document::Version{9U};
    wrong_result_seal.commands[0].fingerprint = command::fingerprint(wrong_result);
    command::NodeCommandBus result_node;
    MS_CHECK(result_node.receive(wrong_result) == command::ReceiveStatus::Accepted);
    MS_CHECK(result_node.receive(wrong_result_seal) == command::ReceiveStatus::NeedsResync);
    MS_CHECK(result_node.fault()->code == command::FaultCode::CommandResultMismatch);
    MS_CHECK(result_node.document().empty());

    command::NodeCommandBus seal_node;
    MS_CHECK(seal_node.receive(seal.value()) == command::ReceiveStatus::Accepted);
    auto conflicting_seal = seal.value();
    conflicting_seal.result = document::Version{99U};
    MS_CHECK(seal_node.receive(conflicting_seal) == command::ReceiveStatus::NeedsResync);
    MS_CHECK(seal_node.fault()->code == command::FaultCode::ConflictingSeal);
}

MS_TEST("empty seals and future frames preserve consecutive publication") {
    command::DirectorScheduler director;
    MS_CHECK(director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{2U}).has_value());
    const auto empty = director.seal_frame(command::FrameNumber{1U});
    const auto content = director.seal_frame(command::FrameNumber{2U});
    const auto commands = director.commands_for(command::FrameNumber{2U});
    MS_CHECK(empty.has_value());
    MS_CHECK(content.has_value());

    command::NodeCommandBus node;
    MS_CHECK(node.receive(commands[0]) == command::ReceiveStatus::Accepted);
    MS_CHECK(node.receive(content.value()) == command::ReceiveStatus::Accepted);
    MS_CHECK(!node.is_prepared(command::FrameNumber{2U}));
    MS_CHECK(node.receive(empty.value()) == command::ReceiveStatus::Prepared);
    MS_CHECK(node.is_prepared(command::FrameNumber{1U}));
    MS_CHECK(node.is_prepared(command::FrameNumber{2U}));

    const auto frame_one = node.commit_frame(command::FrameNumber{1U});
    MS_CHECK(frame_one.status == command::CommitStatus::Committed);
    MS_CHECK(frame_one.command_count == 0U);
    MS_CHECK(node.document().version() == document::Version{0U});
    MS_CHECK(node.commit_frame(command::FrameNumber{2U}).status
        == command::CommitStatus::Committed);
    MS_CHECK(node.document().version() == document::Version{1U});
}

MS_TEST("checkpoint resync ignores stale traffic and accepts replay from its cursor") {
    command::DirectorScheduler director;
    auto first = director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U});
    MS_CHECK(first.has_value());
    MS_CHECK(director.seal_frame(command::FrameNumber{1U}).has_value());

    command::NodeCommandBus node;
    MS_CHECK(node.commit_frame(command::FrameNumber{1U}).status
        == command::CommitStatus::NeedsResync);
    MS_CHECK(node.install_checkpoint(director.checkpoint()));
    MS_CHECK(node.document().contains(document::NodeId{1U}));
    MS_CHECK(node.receive(first.value()) == command::ReceiveStatus::Stale);
    MS_CHECK(node.receive(*director.seal_for(command::FrameNumber{1U}))
        == command::ReceiveStatus::Stale);

    auto second = director.schedule_edit(
        support::put(2U, 20), support::erase(2U), command::UndoContextId{1U},
        command::TransactionId{2U}, command::FrameNumber{2U});
    auto second_seal = director.seal_frame(command::FrameNumber{2U});
    MS_CHECK(second.has_value());
    MS_CHECK(second_seal.has_value());
    MS_CHECK(node.receive(second_seal.value()) == command::ReceiveStatus::Accepted);
    MS_CHECK(node.receive(second.value()) == command::ReceiveStatus::Prepared);
    MS_CHECK(node.commit_frame(command::FrameNumber{2U}).status
        == command::CommitStatus::Committed);
    MS_CHECK(node.document().contains(document::NodeId{2U}));
}

MS_TEST("checkpoint preparation is invisible and stale ABA tokens cannot publish") {
    command::DirectorScheduler director;
    MS_CHECK(director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U}).has_value());
    MS_CHECK(director.seal_frame(command::FrameNumber{1U}).has_value());
    const auto checkpoint = director.checkpoint();

    command::NodeCommandBus node;
    const auto before_cursor = node.cursor();
    const auto before_document = node.document();
    const auto before_history = node.replay_history_diagnostics();
    auto first = node.prepare_checkpoint(checkpoint);
    MS_CHECK(first.has_value());
    MS_CHECK(static_cast<bool>(first.value()));
    MS_CHECK(first.value().through_frame == command::FrameNumber{1U});
    MS_CHECK(node.prepared_checkpoint() != nullptr);
    MS_CHECK(node.prepared_checkpoint()->cursor == checkpoint.cursor);
    MS_CHECK(node.cursor() == before_cursor);
    MS_CHECK(document::diff(node.document(), before_document).changes.empty());
    MS_CHECK(node.replay_history_diagnostics() == before_history);

    MS_CHECK(node.abort_prepared_checkpoint(first.value()));
    MS_CHECK(node.prepared_checkpoint() == nullptr);
    auto second = node.prepare_checkpoint(checkpoint);
    MS_CHECK(second.has_value());
    MS_CHECK(second.value().through_frame == first.value().through_frame);
    MS_CHECK(second.value().generation > first.value().generation);
    MS_CHECK(!node.activate_prepared_checkpoint(first.value()));
    MS_CHECK(!node.abort_prepared_checkpoint(first.value()));
    MS_CHECK(node.prepared_checkpoint() != nullptr);
    MS_CHECK(node.prepared_checkpoint()->cursor == checkpoint.cursor);
    MS_CHECK(node.cursor() == before_cursor);

    MS_CHECK(node.activate_prepared_checkpoint(second.value()));
    MS_CHECK(node.prepared_checkpoint() == nullptr);
    MS_CHECK(node.cursor() == checkpoint.cursor);
    MS_CHECK(document::diff(node.document(), checkpoint.document).changes.empty());
    MS_CHECK(!node.activate_prepared_checkpoint(second.value()));
}

MS_TEST("checkpoint preparation rejects invalid and overflow cursors without stealing") {
    command::NodeCommandBus node;

    const command::StreamCheckpoint wrong_revision{
        command::StreamCursor{
            command::FrameNumber{1U}, document::Version{1U}, command::CommandId{1U}},
        document::Root{},
    };
    auto revision = node.prepare_checkpoint(wrong_revision);
    MS_CHECK(!revision.has_value());
    MS_CHECK(revision.error() == command::CheckpointPrepareError::RevisionMismatch);

    const command::StreamCheckpoint zero_next{
        command::StreamCursor{
            command::FrameNumber{1U}, document::Version{0U}, command::CommandId{0U}},
        document::Root{},
    };
    auto next = node.prepare_checkpoint(zero_next);
    MS_CHECK(!next.has_value());
    MS_CHECK(next.error() == command::CheckpointPrepareError::InvalidNextCommand);

    const command::StreamCheckpoint exhausted{
        command::StreamCursor{
            command::FrameNumber{std::numeric_limits<mediaserver::core::u64>::max()},
            document::Version{0U},
            command::CommandId{1U}},
        document::Root{},
    };
    auto frame = node.prepare_checkpoint(exhausted);
    MS_CHECK(!frame.has_value());
    MS_CHECK(frame.error() == command::CheckpointPrepareError::FrameExhausted);
    MS_CHECK(node.prepared_checkpoint() == nullptr);

    const command::StreamCheckpoint staged_checkpoint{
        command::StreamCursor{
            command::FrameNumber{3U}, document::Version{0U}, command::CommandId{1U}},
        document::Root{},
    };
    auto staged = node.prepare_checkpoint(staged_checkpoint);
    MS_CHECK(staged.has_value());
    auto outstanding = node.prepare_checkpoint(staged_checkpoint);
    MS_CHECK(!outstanding.has_value());
    MS_CHECK(
        outstanding.error() == command::CheckpointPrepareError::PreparationOutstanding);
    MS_CHECK(!node.install_checkpoint(staged_checkpoint));
    MS_CHECK(node.prepared_checkpoint() != nullptr);
    MS_CHECK(node.prepared_checkpoint()->cursor == staged_checkpoint.cursor);
    MS_CHECK(node.abort_prepared_checkpoint(staged.value()));
    MS_CHECK(node.install_checkpoint(staged_checkpoint));
    MS_CHECK(node.cursor() == staged_checkpoint.cursor);
}

MS_TEST("checkpoint-pending traffic is refused without state or fault mutation") {
    command::DirectorScheduler director;
    auto authored = director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U});
    auto seal = director.seal_frame(command::FrameNumber{1U});
    MS_CHECK(authored.has_value());
    MS_CHECK(seal.has_value());

    command::NodeCommandBus node;
    auto staged = node.prepare_checkpoint(director.checkpoint());
    MS_CHECK(staged.has_value());
    const auto cursor = node.cursor();
    const auto document_before = node.document();
    const auto history = node.replay_history_diagnostics();
    MS_CHECK(!node.fault().has_value());

    MS_CHECK(node.receive(authored.value()) == command::ReceiveStatus::CheckpointPending);
    MS_CHECK(node.receive(seal.value()) == command::ReceiveStatus::CheckpointPending);
    const std::array commands{authored.value()};
    const auto batch = node.receive_complete_batch(commands, seal.value());
    MS_CHECK(batch.has_value());
    MS_CHECK(batch.value() == command::ReceiveStatus::CheckpointPending);
    const auto committed = node.commit_frame(command::FrameNumber{1U});
    MS_CHECK(committed.status == command::CommitStatus::CheckpointPending);
    MS_CHECK(committed.frame == command::FrameNumber{1U});
    MS_CHECK(committed.version == document::Version{0U});
    MS_CHECK(committed.command_count == 0U);
    MS_CHECK(!node.retire_committed_replay_history());
    MS_CHECK(node.cursor() == cursor);
    MS_CHECK(document::diff(node.document(), document_before).changes.empty());
    MS_CHECK(node.replay_history_diagnostics() == history);
    MS_CHECK(!node.fault().has_value());
    MS_CHECK(node.prepared_checkpoint() != nullptr);
    MS_CHECK(node.abort_prepared_checkpoint(staged.value()));
}

MS_TEST("checkpoint activation clears fault and buffers then continues exactly") {
    command::DirectorScheduler director;
    auto first = director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U});
    auto first_seal = director.seal_frame(command::FrameNumber{1U});
    MS_CHECK(first.has_value());
    MS_CHECK(first_seal.has_value());
    const auto checkpoint = director.checkpoint();

    auto second = director.schedule_edit(
        support::put(2U, 20), support::erase(2U), command::UndoContextId{1U},
        command::TransactionId{2U}, command::FrameNumber{2U});
    auto second_seal = director.seal_frame(command::FrameNumber{2U});
    MS_CHECK(second.has_value());
    MS_CHECK(second_seal.has_value());

    command::NodeCommandBus node;
    MS_CHECK(node.receive(first.value()) == command::ReceiveStatus::Accepted);
    auto conflicting = first.value();
    conflicting.forward = support::put(1U, 99);
    MS_CHECK(node.receive(conflicting) == command::ReceiveStatus::NeedsResync);
    MS_CHECK(node.needs_resync());
    const auto buffered = node.replay_history_diagnostics();
    MS_CHECK(buffered.frame_states == 1U);
    MS_CHECK(buffered.seen_commands == 1U);

    auto staged = node.prepare_checkpoint(checkpoint);
    MS_CHECK(staged.has_value());
    MS_CHECK(node.needs_resync());
    MS_CHECK(node.replay_history_diagnostics() == buffered);
    MS_CHECK(node.activate_prepared_checkpoint(staged.value()));
    MS_CHECK(!node.needs_resync());
    const auto replaced = node.replay_history_diagnostics();
    MS_CHECK(replaced.frame_states == 0U);
    MS_CHECK(replaced.seen_commands == 0U);
    MS_CHECK(replaced.prepared_frames == 0U);
    MS_CHECK(replaced.committed_prepared_frames == 0U);
    MS_CHECK(replaced.retained_roots == 1U);
    MS_CHECK(node.cursor() == checkpoint.cursor);
    MS_CHECK(document::diff(node.document(), checkpoint.document).changes.empty());

    MS_CHECK(node.receive(second_seal.value()) == command::ReceiveStatus::Accepted);
    MS_CHECK(node.receive(second.value()) == command::ReceiveStatus::Prepared);
    MS_CHECK(node.commit_frame(command::FrameNumber{2U}).status
        == command::CommitStatus::Committed);
    MS_CHECK(node.document().contains(document::NodeId{1U}));
    MS_CHECK(node.document().contains(document::NodeId{2U}));
    MS_CHECK(node.cursor().through_frame == command::FrameNumber{2U});
    MS_CHECK(node.cursor().revision == document::Version{2U});
    MS_CHECK(node.cursor().next_command == command::CommandId{3U});
}

MS_TEST("offline replay retirement resets committed history and preserves continuation state") {
    command::DirectorScheduler director;
    auto first = director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U});
    auto first_seal = director.seal_frame(command::FrameNumber{1U});
    auto second = director.schedule_edit(
        support::put(2U, 20), support::erase(2U), command::UndoContextId{1U},
        command::TransactionId{2U}, command::FrameNumber{2U});
    auto second_seal = director.seal_frame(command::FrameNumber{2U});
    MS_CHECK(first.has_value());
    MS_CHECK(first_seal.has_value());
    MS_CHECK(second.has_value());
    MS_CHECK(second_seal.has_value());

    command::NodeCommandBus node;
    MS_CHECK(node.receive(first.value()) == command::ReceiveStatus::Accepted);
    MS_CHECK(node.receive(first_seal.value()) == command::ReceiveStatus::Prepared);
    MS_CHECK(node.commit_frame(command::FrameNumber{1U}).status
        == command::CommitStatus::Committed);
    MS_CHECK(node.receive(second.value()) == command::ReceiveStatus::Accepted);
    MS_CHECK(node.receive(second_seal.value()) == command::ReceiveStatus::Prepared);
    MS_CHECK(node.commit_frame(command::FrameNumber{2U}).status
        == command::CommitStatus::Committed);
    MS_CHECK(node.receive(first.value()) == command::ReceiveStatus::Duplicate);

    const auto before_cursor = node.cursor();
    const auto before_document = node.document();
    const auto before = node.replay_history_diagnostics();
    MS_CHECK(before.frame_states == 2U);
    MS_CHECK(before.seen_commands == 2U);
    MS_CHECK(before.prepared_frames == 2U);
    MS_CHECK(before.committed_prepared_frames == 2U);
    MS_CHECK(before.retained_roots == 1U);

    MS_CHECK(node.retire_committed_replay_history());
    MS_CHECK(node.replay_history_diagnostics() == command::ReplayHistoryDiagnostics{});
    MS_CHECK(node.cursor() == before_cursor);
    MS_CHECK(document::diff(node.document(), before_document).changes.empty());
    MS_CHECK(!node.needs_resync());
    MS_CHECK(node.receive(first.value()) == command::ReceiveStatus::Stale);

    auto third = director.schedule_edit(
        support::put(3U, 30), support::erase(3U), command::UndoContextId{1U},
        command::TransactionId{3U}, command::FrameNumber{3U});
    auto third_seal = director.seal_frame(command::FrameNumber{3U});
    MS_CHECK(third.has_value());
    MS_CHECK(third_seal.has_value());
    MS_CHECK(node.receive(third.value()) == command::ReceiveStatus::Accepted);
    MS_CHECK(node.receive(third_seal.value()) == command::ReceiveStatus::Prepared);
    MS_CHECK(node.commit_frame(command::FrameNumber{3U}).status
        == command::CommitStatus::Committed);
    MS_CHECK(node.document().contains(document::NodeId{3U}));
    MS_CHECK(node.cursor().next_command == command::CommandId{4U});
}

MS_TEST("offline replay retirement bounds sequential empty-frame history") {
    command::DirectorScheduler director;
    command::NodeCommandBus node;

    for (mediaserver::core::u64 frame = 1U; frame <= 64U; ++frame) {
        const auto number = command::FrameNumber{frame};
        auto seal = director.seal_frame(number);
        MS_CHECK(seal.has_value());
        MS_CHECK(node.receive(seal.value()) == command::ReceiveStatus::Prepared);
        MS_CHECK(node.commit_frame(number).status == command::CommitStatus::Committed);

        const auto retained = node.replay_history_diagnostics();
        MS_CHECK(retained.frame_states == 1U);
        MS_CHECK(retained.seen_commands == 0U);
        MS_CHECK(retained.prepared_frames == 1U);
        MS_CHECK(retained.committed_prepared_frames == 1U);
        MS_CHECK(node.retire_committed_replay_history());
        MS_CHECK(node.replay_history_diagnostics() == command::ReplayHistoryDiagnostics{});
    }

    MS_CHECK(node.cursor().through_frame == command::FrameNumber{64U});
    MS_CHECK(node.cursor().revision == document::Version{0U});
    MS_CHECK(node.cursor().next_command == command::CommandId{1U});
    MS_CHECK(node.document().empty());
    MS_CHECK(!node.needs_resync());
}

MS_TEST("offline replay retirement refuses partial and prepared future frames") {
    command::DirectorScheduler director;
    auto first = director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U});
    auto first_seal = director.seal_frame(command::FrameNumber{1U});
    auto second = director.schedule_edit(
        support::put(2U, 20), support::erase(2U), command::UndoContextId{1U},
        command::TransactionId{2U}, command::FrameNumber{2U});
    auto second_seal = director.seal_frame(command::FrameNumber{2U});
    MS_CHECK(first.has_value());
    MS_CHECK(first_seal.has_value());
    MS_CHECK(second.has_value());
    MS_CHECK(second_seal.has_value());

    command::NodeCommandBus node;
    MS_CHECK(node.receive(first.value()) == command::ReceiveStatus::Accepted);
    MS_CHECK(node.receive(first_seal.value()) == command::ReceiveStatus::Prepared);
    MS_CHECK(node.commit_frame(command::FrameNumber{1U}).status
        == command::CommitStatus::Committed);
    MS_CHECK(node.receive(second.value()) == command::ReceiveStatus::Accepted);
    const auto partial_state = node.replay_history_diagnostics();
    const auto partial_cursor = node.cursor();
    const auto partial_document = node.document();

    MS_CHECK(!node.retire_committed_replay_history());
    MS_CHECK(node.replay_history_diagnostics() == partial_state);
    MS_CHECK(node.cursor() == partial_cursor);
    MS_CHECK(document::diff(node.document(), partial_document).changes.empty());

    MS_CHECK(node.receive(second_seal.value()) == command::ReceiveStatus::Prepared);
    const auto prepared_state = node.replay_history_diagnostics();
    MS_CHECK(prepared_state.prepared_frames == 2U);
    MS_CHECK(prepared_state.committed_prepared_frames == 1U);
    MS_CHECK(!node.retire_committed_replay_history());
    MS_CHECK(node.replay_history_diagnostics() == prepared_state);
    MS_CHECK(node.is_prepared(command::FrameNumber{2U}));

    MS_CHECK(node.commit_frame(command::FrameNumber{2U}).status
        == command::CommitStatus::Committed);
    MS_CHECK(node.retire_committed_replay_history());
}

MS_TEST("offline replay retirement preserves a latched fault") {
    command::DirectorScheduler director;
    auto authored = director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U});
    auto seal = director.seal_frame(command::FrameNumber{1U});
    MS_CHECK(authored.has_value());
    MS_CHECK(seal.has_value());

    command::NodeCommandBus node;
    MS_CHECK(node.receive(authored.value()) == command::ReceiveStatus::Accepted);
    MS_CHECK(node.receive(seal.value()) == command::ReceiveStatus::Prepared);
    MS_CHECK(node.commit_frame(command::FrameNumber{1U}).status
        == command::CommitStatus::Committed);
    auto conflicting = authored.value();
    conflicting.forward = support::put(1U, 99);
    MS_CHECK(node.receive(conflicting) == command::ReceiveStatus::NeedsResync);
    const auto cursor = node.cursor();
    const auto version = node.document().version();
    MS_CHECK(node.fault().has_value());
    const auto fault = *node.fault();

    MS_CHECK(node.retire_committed_replay_history());
    MS_CHECK(node.replay_history_diagnostics() == command::ReplayHistoryDiagnostics{});
    MS_CHECK(node.cursor() == cursor);
    MS_CHECK(node.document().version() == version);
    MS_CHECK(node.fault().has_value());
    MS_CHECK(node.fault()->code == fault.code);
    MS_CHECK(node.fault()->frame == fault.frame);
    MS_CHECK(node.fault()->command == fault.command);
}

} // namespace
