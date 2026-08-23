#include "protocol_test_support.hpp"
#include "test_support.hpp"

#include <mediaserver/cmd/loopback_transport.hpp>

#include <vector>

namespace {

namespace command = mediaserver::cmd;
namespace document = mediaserver::doc;
namespace support = mediaserver::test_support;

void deliver_front(command::LoopbackTransport& transport, std::size_t node) {
    while (transport.pending(node) > 0U) {
        MS_CHECK(transport.deliver(node, 0U).has_value());
    }
}

void deliver_back(command::LoopbackTransport& transport, std::size_t node) {
    while (transport.pending(node) > 0U) {
        MS_CHECK(transport.deliver(node, transport.pending(node) - 1U).has_value());
    }
}

MS_TEST("loopback nodes converge under opposite delivery permutations") {
    command::DirectorScheduler director;
    MS_CHECK(director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U}).has_value());
    MS_CHECK(director.schedule_edit(
        support::put(2U, 20), support::erase(2U), command::UndoContextId{1U},
        command::TransactionId{1U}, command::FrameNumber{1U}).has_value());
    const auto seal = director.seal_frame(command::FrameNumber{1U});
    MS_CHECK(seal.has_value());

    command::NodeCommandBus first;
    command::NodeCommandBus second;
    command::LoopbackTransport transport{{&first, &second}};
    for (const auto& authored : director.commands_for(command::FrameNumber{1U})) {
        transport.broadcast(authored);
    }
    transport.broadcast(seal.value());
    deliver_front(transport, 0U);
    deliver_back(transport, 1U);

    MS_CHECK(first.document().empty());
    MS_CHECK(second.document().empty());
    MS_CHECK(first.commit_frame(command::FrameNumber{1U}).status
        == command::CommitStatus::Committed);
    MS_CHECK(second.commit_frame(command::FrameNumber{1U}).status
        == command::CommitStatus::Committed);
    MS_CHECK(first.cursor() == second.cursor());
    MS_CHECK(document::diff(first.document(), second.document()).changes.empty());
}

MS_TEST("Director history survives node resync and undo uses the sealed path") {
    command::DirectorScheduler director;
    auto create = director.schedule_edit(
        support::put(1U, 10), support::erase(1U), command::UndoContextId{1U},
        command::TransactionId{7U}, command::FrameNumber{1U});
    const auto first_seal = director.seal_frame(command::FrameNumber{1U});
    MS_CHECK(create.has_value());
    MS_CHECK(first_seal.has_value());

    command::NodeCommandBus first;
    command::NodeCommandBus resynced;
    MS_CHECK(first.receive(create.value()) != command::ReceiveStatus::NeedsResync);
    MS_CHECK(first.receive(first_seal.value()) == command::ReceiveStatus::Prepared);
    MS_CHECK(first.commit_frame(command::FrameNumber{1U}).status
        == command::CommitStatus::Committed);
    MS_CHECK(resynced.install_checkpoint(director.checkpoint()));

    auto undo = director.schedule_undo(
        command::UndoContextId{1U}, command::TransactionId{8U},
        command::FrameNumber{2U});
    const auto undo_seal = director.seal_frame(command::FrameNumber{2U});
    MS_CHECK(undo.has_value());
    MS_CHECK(undo_seal.has_value());

    command::LoopbackTransport transport{{&first, &resynced}};
    for (const auto& authored : undo.value()) {
        transport.broadcast(authored);
    }
    transport.broadcast(undo_seal.value());
    deliver_back(transport, 0U);
    deliver_front(transport, 1U);
    MS_CHECK(first.commit_frame(command::FrameNumber{2U}).status
        == command::CommitStatus::Committed);
    MS_CHECK(resynced.commit_frame(command::FrameNumber{2U}).status
        == command::CommitStatus::Committed);
    MS_CHECK(first.document().empty());
    MS_CHECK(resynced.document().empty());
    MS_CHECK(document::diff(first.document(), resynced.document()).changes.empty());
}

} // namespace
