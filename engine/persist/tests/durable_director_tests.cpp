#include <mediaserver/persist/durable_director.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace command = mediaserver::cmd;
namespace document = mediaserver::doc;
namespace persist = mediaserver::persist;
namespace wire = mediaserver::wire;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define DURABLE_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempWal final {
public:
    TempWal() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("mediaserver-durable-director-" + std::to_string(timestamp) + "-"
               + std::to_string(sequence.fetch_add(1U)) + ".bin");
    }

    ~TempWal() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TempWal(const TempWal&) = delete;
    TempWal& operator=(const TempWal&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] command::StreamCursor genesis_cursor() noexcept {
    return command::StreamCursor{
        command::FrameNumber{0U},
        document::Version{0U},
        command::CommandId{1U},
    };
}

[[nodiscard]] persist::OpenOptions options_for(persist::SyncPolicy policy) {
    persist::OpenOptions options;
    options.sync_policy = policy;
    return options;
}

[[nodiscard]] document::Node node(
    std::uint64_t id,
    std::int64_t value,
    std::string type = "test.node") {
    return document::Node{
        document::NodeId{id},
        std::move(type),
        {{"value", value}},
    };
}

[[nodiscard]] command::Payload put(
    std::uint64_t id,
    std::int64_t value,
    std::string type = "test.node") {
    return command::PutNode{node(id, value, std::move(type))};
}

[[nodiscard]] command::Payload erase(std::uint64_t id) {
    return command::EraseNode{document::NodeId{id}};
}

void schedule_put(
    command::DirectorScheduler& director,
    std::uint64_t node_id,
    std::int64_t value,
    std::uint64_t transaction,
    std::uint64_t frame,
    std::string type = "test.node") {
    auto scheduled = director.schedule_edit(
        put(node_id, value, std::move(type)),
        erase(node_id),
        command::UndoContextId{1U},
        command::TransactionId{transaction},
        command::FrameNumber{frame});
    DURABLE_CHECK(scheduled);
}

void append_external_byte(const std::filesystem::path& path) {
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    DURABLE_CHECK(stream);
    const char byte = static_cast<char>(0xa5);
    stream.write(&byte, 1);
    stream.flush();
    DURABLE_CHECK(stream);
}

void deliver_front(command::LoopbackTransport& transport, std::size_t node_index) {
    while (transport.pending(node_index) > 0U) {
        DURABLE_CHECK(transport.deliver(node_index, 0U).has_value());
    }
}

void durable_content_and_empty_frames_publish_in_protocol_order() {
    TempWal temporary;
    auto opened = persist::CommandLog::open_or_create(
        temporary.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Data));
    DURABLE_CHECK(opened);
    auto log = std::move(opened).value();
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log};

    schedule_put(director, 1U, 10, 1U, 1U);
    auto content = durable.seal_frame(command::FrameNumber{1U});
    DURABLE_CHECK(content);
    DURABLE_CHECK(content.value().commands().size() == 1U);
    DURABLE_CHECK(content.value().seal().frame == command::FrameNumber{1U});
    DURABLE_CHECK(content.value().receipt().durable);
    DURABLE_CHECK(content.value().receipt().cursor == persist::LogCursor{1U, 1U, 2U});
    DURABLE_CHECK(director.sealed_through() == command::FrameNumber{1U});
    DURABLE_CHECK(director.prepared_seal() == nullptr);
    DURABLE_CHECK(log.tail_cursor() == director.checkpoint().cursor);
    auto source = durable.capture_resume_source();
    DURABLE_CHECK(source.has_value());
    DURABLE_CHECK(source.value().log().path() == log.path());
    DURABLE_CHECK(source.value().log().tail_cursor() == log.tail_cursor());
    DURABLE_CHECK(source.value().log().valid_bytes()
                  == log.recovery_report().valid_bytes);
    DURABLE_CHECK(source.value().checkpoint().cursor == director.checkpoint().cursor);
    DURABLE_CHECK(source.value().checkpoint().document.same_snapshot(
        director.checkpoint().document));
    DURABLE_CHECK(durable.authorizes(content.value()));

    command::NodeCommandBus local_node;
    command::LoopbackTransport transport{
        std::vector<command::NodeCommandBus*>{&local_node}};
    DURABLE_CHECK(durable.broadcast_committed(content.value(), transport));
    DURABLE_CHECK(transport.pending(0U) == 2U);
    auto command_status = transport.deliver(0U, 0U);
    DURABLE_CHECK(command_status == command::ReceiveStatus::Accepted);
    auto seal_status = transport.deliver(0U, 0U);
    DURABLE_CHECK(seal_status == command::ReceiveStatus::Prepared);
    DURABLE_CHECK(local_node.commit_frame(command::FrameNumber{1U}).status
                  == command::CommitStatus::Committed);
    DURABLE_CHECK(local_node.document().contains(document::NodeId{1U}));

    auto empty = durable.seal_frame(command::FrameNumber{2U});
    DURABLE_CHECK(empty);
    DURABLE_CHECK(empty.value().commands().empty());
    DURABLE_CHECK(empty.value().seal().commands.empty());
    DURABLE_CHECK(empty.value().receipt().durable);
    DURABLE_CHECK(empty.value().receipt().cursor == persist::LogCursor{2U, 1U, 2U});
    DURABLE_CHECK(durable.broadcast_committed(empty.value(), transport));
    DURABLE_CHECK(transport.pending(0U) == 1U);
    auto empty_status = transport.deliver(0U, 0U);
    DURABLE_CHECK(empty_status == command::ReceiveStatus::Prepared);
    DURABLE_CHECK(local_node.commit_frame(command::FrameNumber{2U}).status
                  == command::CommitStatus::Committed);
    DURABLE_CHECK(local_node.cursor() == director.checkpoint().cursor);
    DURABLE_CHECK(log.tail_cursor() == director.checkpoint().cursor);
}

void moved_from_batch_is_not_publishable() {
    TempWal temporary;
    auto opened = persist::CommandLog::open_or_create(
        temporary.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Data));
    DURABLE_CHECK(opened);
    auto log = std::move(opened).value();
    command::DirectorScheduler director;
    schedule_put(director, 1U, 10, 1U, 1U);
    persist::DurableDirector durable{director, log};
    auto committed = durable.seal_frame(command::FrameNumber{1U});
    DURABLE_CHECK(committed);

    persist::CommittedFrameBatch destination{std::move(committed.value())};
    persist::DurableDirector foreign_coordinator{director, log};
    command::NodeCommandBus node_bus;
    command::LoopbackTransport transport{
        std::vector<command::NodeCommandBus*>{&node_bus}};
    DURABLE_CHECK(!durable.authorizes(committed.value()));
    DURABLE_CHECK(!durable.broadcast_committed(committed.value(), transport));
    DURABLE_CHECK(transport.pending(0U) == 0U);
    DURABLE_CHECK(!foreign_coordinator.authorizes(destination));
    DURABLE_CHECK(!foreign_coordinator.broadcast_committed(destination, transport));
    DURABLE_CHECK(transport.pending(0U) == 0U);
    DURABLE_CHECK(durable.authorizes(destination));
    DURABLE_CHECK(durable.broadcast_committed(destination, transport));
    DURABLE_CHECK(transport.pending(0U) == 2U);
    deliver_front(transport, 0U);
    DURABLE_CHECK(node_bus.commit_frame(command::FrameNumber{1U}).status
                  == command::CommitStatus::Committed);
}

void none_policy_is_rejected_before_storage_or_prepare() {
    TempWal temporary;
    auto opened = persist::CommandLog::open_or_create(
        temporary.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::None));
    DURABLE_CHECK(opened);
    auto log = std::move(opened).value();
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log};

    append_external_byte(temporary.path());
    const auto externally_changed_size = std::filesystem::file_size(temporary.path());
    auto rejected = durable.seal_frame(command::FrameNumber{1U});
    DURABLE_CHECK(!rejected);
    DURABLE_CHECK(
        rejected.error().code == persist::DurableDirectorErrorCode::NonDurablePolicy);
    DURABLE_CHECK(durable.state() == persist::DurableDirectorState::Ready);
    DURABLE_CHECK(director.prepared_seal() == nullptr);
    DURABLE_CHECK(director.sealed_through() == command::FrameNumber{0U});
    DURABLE_CHECK(std::filesystem::file_size(temporary.path()) == externally_changed_size);
}

void known_prewrite_rejection_aborts_exact_prepare_and_can_retry() {
    TempWal temporary;
    command::DirectorScheduler director;
    schedule_put(director, 1U, 10, 1U, 1U);

    {
        auto limits = wire::DecodeLimits{};
        limits.max_string_bytes = 3U;
        auto opened = persist::CommandLog::open_or_create(
            temporary.path(),
            genesis_cursor(),
            options_for(persist::SyncPolicy::Data),
            limits);
        DURABLE_CHECK(opened);
        auto constrained_log = std::move(opened).value();
        persist::DurableDirector durable{director, constrained_log};

        auto rejected = durable.seal_frame(command::FrameNumber{1U});
        DURABLE_CHECK(!rejected);
        DURABLE_CHECK(
            rejected.error().code == persist::DurableDirectorErrorCode::PrewriteRejected);
        DURABLE_CHECK(rejected.error().command_log_error.has_value());
        DURABLE_CHECK(std::holds_alternative<persist::EncodeFailure>(
            *rejected.error().command_log_error));
        DURABLE_CHECK(durable.state() == persist::DurableDirectorState::Ready);
        DURABLE_CHECK(director.prepared_seal() == nullptr);
        DURABLE_CHECK(director.seal_for(command::FrameNumber{1U}) == nullptr);
        DURABLE_CHECK(director.sealed_through() == command::FrameNumber{0U});
        DURABLE_CHECK(constrained_log.recovery_report().entry_count == 0U);
    }

    auto reopened = persist::CommandLog::open_or_create(
        temporary.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Data));
    DURABLE_CHECK(reopened);
    auto log = std::move(reopened).value();
    persist::DurableDirector retried{director, log};
    auto committed = retried.seal_frame(command::FrameNumber{1U});
    DURABLE_CHECK(committed);
    DURABLE_CHECK(committed.value().receipt().durable);
    DURABLE_CHECK(director.prepared_seal() == nullptr);
    DURABLE_CHECK(director.sealed_through() == command::FrameNumber{1U});
    DURABLE_CHECK(log.recovery_report().entry_count == 1U);
}

void cursor_mismatch_is_rejected_without_preparing() {
    TempWal temporary;
    auto opened = persist::CommandLog::open_or_create(
        temporary.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Data));
    DURABLE_CHECK(opened);
    auto log = std::move(opened).value();
    command::DirectorScheduler director;
    DURABLE_CHECK(director.seal_frame(command::FrameNumber{1U}));
    persist::DurableDirector durable{director, log};

    auto rejected = durable.seal_frame(command::FrameNumber{2U});
    DURABLE_CHECK(!rejected);
    DURABLE_CHECK(
        rejected.error().code == persist::DurableDirectorErrorCode::CursorMismatch);
    DURABLE_CHECK(durable.state() == persist::DurableDirectorState::Ready);
    DURABLE_CHECK(director.prepared_seal() == nullptr);
    DURABLE_CHECK(log.recovery_report().entry_count == 0U);

    auto source = durable.capture_resume_source();
    DURABLE_CHECK(!source);
    DURABLE_CHECK(
        source.error().code == persist::DurableDirectorErrorCode::CursorMismatch);
}

void resume_source_never_exposes_the_wal_before_director_commit_window() {
    static_assert(!std::is_copy_constructible_v<persist::DurableResumeSource>);
    static_assert(std::is_nothrow_move_constructible_v<persist::DurableResumeSource>);

    TempWal temporary;
    auto opened = persist::CommandLog::open_or_create(
        temporary.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Data));
    DURABLE_CHECK(opened);
    auto log = std::move(opened).value();
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log};

    schedule_put(director, 1U, 10, 1U, 1U);
    auto prepared = director.prepare_seal(command::FrameNumber{1U});
    DURABLE_CHECK(prepared);
    const auto token = prepared.value().token;
    const auto seal = prepared.value().seal;
    auto appended = log.append(
        director.commands_for(command::FrameNumber{1U}), seal);
    DURABLE_CHECK(appended);
    DURABLE_CHECK(appended.value().durable);
    DURABLE_CHECK(log.tail_cursor() == command::StreamCursor{
        command::FrameNumber{1U}, document::Version{1U}, command::CommandId{2U}});
    DURABLE_CHECK(director.checkpoint().cursor == genesis_cursor());

    auto mixed = durable.capture_resume_source();
    DURABLE_CHECK(!mixed);
    DURABLE_CHECK(mixed.error().code
                  == persist::DurableDirectorErrorCode::ResumeSourceUnavailable);

    DURABLE_CHECK(director.commit_prepared_seal(token));
    auto captured = durable.capture_resume_source();
    DURABLE_CHECK(captured);
    auto source = std::move(captured).value();
    DURABLE_CHECK(durable.authorizes(source));
    persist::DurableDirector foreign{director, log};
    DURABLE_CHECK(!foreign.authorizes(source));
    auto moved_source = std::move(source);
    DURABLE_CHECK(!durable.authorizes(source));
    DURABLE_CHECK(durable.authorizes(moved_source));
    DURABLE_CHECK(moved_source.checkpoint().cursor == log.tail_cursor());
    DURABLE_CHECK(moved_source.log().tail_cursor() == log.tail_cursor());
    DURABLE_CHECK(moved_source.checkpoint().document.contains(
        document::NodeId{1U}));
}

void external_size_failure_latches_prepare_and_blocks_publication() {
    TempWal temporary;
    auto opened = persist::CommandLog::open_or_create(
        temporary.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Data));
    DURABLE_CHECK(opened);
    auto log = std::move(opened).value();
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log};

    auto first = durable.seal_frame(command::FrameNumber{1U});
    DURABLE_CHECK(first);
    append_external_byte(temporary.path());
    const auto externally_changed_size = std::filesystem::file_size(temporary.path());

    auto failed = durable.seal_frame(command::FrameNumber{2U});
    DURABLE_CHECK(!failed);
    DURABLE_CHECK(
        failed.error().code == persist::DurableDirectorErrorCode::PersistenceFailure);
    auto source = durable.capture_resume_source();
    DURABLE_CHECK(!source);
    DURABLE_CHECK(
        source.error().code == persist::DurableDirectorErrorCode::RecoveryRequired);
    DURABLE_CHECK(failed.error().command_log_error.has_value());
    const auto* log_failure =
        std::get_if<persist::LogFailure>(&*failed.error().command_log_error);
    DURABLE_CHECK(log_failure != nullptr);
    DURABLE_CHECK(log_failure->error.code == persist::LogErrorCode::Unusable);
    DURABLE_CHECK(durable.state() == persist::DurableDirectorState::RecoveryRequired);
    DURABLE_CHECK(director.prepared_seal() != nullptr);
    DURABLE_CHECK(
        director.prepared_seal()->frame == command::FrameNumber{2U});
    DURABLE_CHECK(director.sealed_through() == command::FrameNumber{1U});

    command::NodeCommandBus unpublished_node;
    command::LoopbackTransport transport{
        std::vector<command::NodeCommandBus*>{&unpublished_node}};
    DURABLE_CHECK(!durable.broadcast_committed(first.value(), transport));
    DURABLE_CHECK(transport.pending(0U) == 0U);

    auto no_retry = durable.seal_frame(command::FrameNumber{2U});
    DURABLE_CHECK(!no_retry);
    DURABLE_CHECK(
        no_retry.error().code == persist::DurableDirectorErrorCode::RecoveryRequired);
    DURABLE_CHECK(director.prepared_seal() != nullptr);
    DURABLE_CHECK(std::filesystem::file_size(temporary.path()) == externally_changed_size);
}

void durable_append_before_director_commit_recovers_from_genesis() {
    TempWal temporary;
    auto opened = persist::CommandLog::open_or_create(
        temporary.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Data));
    DURABLE_CHECK(opened);
    auto log = std::move(opened).value();

    {
        command::DirectorScheduler crashed_director;
        schedule_put(crashed_director, 1U, 10, 1U, 1U);
        auto preparation = crashed_director.prepare_seal(command::FrameNumber{1U});
        DURABLE_CHECK(preparation);
        auto appended = log.append(
            crashed_director.commands_for(command::FrameNumber{1U}),
            preparation.value().seal);
        DURABLE_CHECK(appended);
        DURABLE_CHECK(appended.value().durable);
        DURABLE_CHECK(crashed_director.sealed_through() == command::FrameNumber{0U});
        DURABLE_CHECK(crashed_director.prepared_seal() != nullptr);
        // The Director and its uncommitted token are intentionally discarded
        // here, simulating a process crash after the durable WAL receipt.
    }

    auto recovered = persist::recover_genesis_runtime(log);
    DURABLE_CHECK(recovered);
    DURABLE_CHECK(recovered.value().replay.frames == 1U);
    DURABLE_CHECK(recovered.value().replay.commands == 1U);
    DURABLE_CHECK(
        recovered.value().director.sealed_through() == command::FrameNumber{1U});
    DURABLE_CHECK(
        recovered.value().local_node.committed_through() == command::FrameNumber{1U});
    DURABLE_CHECK(
        recovered.value().local_node.document().contains(document::NodeId{1U}));
    DURABLE_CHECK(recovered.value().reset.undo_and_transaction_history_reset);
    DURABLE_CHECK(recovered.value().reset.command_and_seal_history_reset);
}

void warm_genesis_restart_continues_exact_cursor_sequence() {
    TempWal temporary;
    auto opened = persist::CommandLog::open_or_create(
        temporary.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Data));
    DURABLE_CHECK(opened);
    auto log = std::move(opened).value();

    command::DirectorScheduler initial_director;
    schedule_put(initial_director, 1U, 10, 1U, 1U);
    persist::DurableDirector initial{initial_director, log};
    DURABLE_CHECK(initial.seal_frame(command::FrameNumber{1U}));
    DURABLE_CHECK(initial.seal_frame(command::FrameNumber{2U}));

    auto recovered = persist::recover_genesis_runtime(log);
    DURABLE_CHECK(recovered);
    auto& runtime = recovered.value();
    DURABLE_CHECK(runtime.replay.frames == 2U);
    DURABLE_CHECK(runtime.replay.commands == 1U);
    DURABLE_CHECK(runtime.director.checkpoint().cursor
                  == command::StreamCursor{
                      command::FrameNumber{2U},
                      document::Version{1U},
                      command::CommandId{2U}});
    DURABLE_CHECK(runtime.local_node.cursor() == runtime.director.checkpoint().cursor);
    DURABLE_CHECK(runtime.director.undo_depth(command::UndoContextId{1U}) == 0U);

    auto scheduled = runtime.director.schedule_edit(
        put(2U, 20),
        erase(2U),
        command::UndoContextId{1U},
        command::TransactionId{2U},
        command::FrameNumber{3U});
    DURABLE_CHECK(scheduled);
    DURABLE_CHECK(scheduled.value().id == command::CommandId{2U});
    DURABLE_CHECK(scheduled.value().base == document::Version{1U});
    DURABLE_CHECK(scheduled.value().result == document::Version{2U});

    persist::DurableDirector resumed{runtime.director, log};
    auto continued = resumed.seal_frame(command::FrameNumber{3U});
    DURABLE_CHECK(continued);
    DURABLE_CHECK(continued.value().seal().frame == command::FrameNumber{3U});
    DURABLE_CHECK(
        continued.value().receipt().cursor == persist::LogCursor{3U, 2U, 3U});
    command::LoopbackTransport transport{
        std::vector<command::NodeCommandBus*>{&runtime.local_node}};
    DURABLE_CHECK(resumed.broadcast_committed(continued.value(), transport));
    deliver_front(transport, 0U);
    DURABLE_CHECK(runtime.local_node.commit_frame(command::FrameNumber{3U}).status
                  == command::CommitStatus::Committed);
    DURABLE_CHECK(runtime.local_node.cursor()
                  == command::StreamCursor{
                      command::FrameNumber{3U},
                      document::Version{2U},
                      command::CommandId{3U}});
    DURABLE_CHECK(runtime.local_node.cursor() == runtime.director.checkpoint().cursor);
    DURABLE_CHECK(runtime.local_node.document().contains(document::NodeId{1U}));
    DURABLE_CHECK(runtime.local_node.document().contains(document::NodeId{2U}));
}

void genesis_recovery_retires_many_frames_with_bounded_history() {
    TempWal temporary;
    auto opened = persist::CommandLog::open_or_create(
        temporary.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Data));
    DURABLE_CHECK(opened);
    auto log = std::move(opened).value();
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log};

    constexpr std::uint64_t frame_count = 128U;
    constexpr std::uint64_t content_count = frame_count / 2U;
    for (std::uint64_t frame = 1U; frame <= frame_count; ++frame) {
        if ((frame % 2U) != 0U) {
            const auto node_id = (frame + 1U) / 2U;
            schedule_put(
                director,
                node_id,
                static_cast<std::int64_t>(node_id),
                node_id,
                frame);
        }
        auto sealed = durable.seal_frame(command::FrameNumber{frame});
        DURABLE_CHECK(sealed);
        DURABLE_CHECK(sealed.value().receipt().durable);
    }

    // The default overload preserves the original live-node retention
    // semantics and provides a useful contrast to bounded startup replay.
    command::NodeCommandBus retaining_probe;
    auto retained = log.replay_into(retaining_probe);
    DURABLE_CHECK(retained);
    DURABLE_CHECK(
        retained.value().frames == static_cast<std::size_t>(frame_count));
    DURABLE_CHECK(
        retained.value().commands == static_cast<std::size_t>(content_count));
    DURABLE_CHECK(retained.value().history.retirements == 0U);
    DURABLE_CHECK(
        retained.value().history.final.frame_states
        == static_cast<std::size_t>(frame_count));
    DURABLE_CHECK(
        retained.value().history.final.seen_commands
        == static_cast<std::size_t>(content_count));
    DURABLE_CHECK(
        retaining_probe.replay_history_diagnostics()
        == retained.value().history.final);

    auto recovered = persist::recover_genesis_runtime(log);
    DURABLE_CHECK(recovered);
    const auto& report = recovered.value().replay;
    DURABLE_CHECK(report.frames == static_cast<std::size_t>(frame_count));
    DURABLE_CHECK(report.commands == static_cast<std::size_t>(content_count));
    DURABLE_CHECK(
        report.history.retirements == static_cast<std::size_t>(frame_count));
    DURABLE_CHECK(report.history.peak.frame_states == 1U);
    DURABLE_CHECK(report.history.peak.seen_commands == 1U);
    DURABLE_CHECK(report.history.peak.prepared_frames == 1U);
    DURABLE_CHECK(report.history.peak.committed_prepared_frames == 1U);
    DURABLE_CHECK(report.history.peak.retained_roots <= 1U);
    DURABLE_CHECK(
        report.history.final == command::ReplayHistoryDiagnostics{});
    DURABLE_CHECK(
        report.cursor
        == command::StreamCursor{
            command::FrameNumber{frame_count},
            document::Version{content_count},
            command::CommandId{content_count + 1U}});
    DURABLE_CHECK(recovered.value().director.checkpoint().cursor == report.cursor);
    DURABLE_CHECK(recovered.value().local_node.cursor() == report.cursor);
    DURABLE_CHECK(
        recovered.value().local_node.document().size()
        == static_cast<std::size_t>(content_count));
    DURABLE_CHECK(document::diff(
        recovered.value().local_node.document(),
        director.sealed_document()).changes.empty());
}

void bounded_replay_surfaces_retirement_refusal_distinctly() {
    TempWal temporary;
    auto opened = persist::CommandLog::open_or_create(
        temporary.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Data));
    DURABLE_CHECK(opened);
    auto log = std::move(opened).value();
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log};
    DURABLE_CHECK(durable.seal_frame(command::FrameNumber{1U}));

    command::NodeCommandBus node_bus;
    const command::Command future{
        command::CommandId{1U},
        document::Version{0U},
        document::Version{1U},
        command::FrameNumber{2U},
        command::TransactionId{1U},
        command::UndoContextId{1U},
        put(99U, 99),
        erase(99U),
    };
    DURABLE_CHECK(
        node_bus.receive(future) == command::ReceiveStatus::Accepted);

    auto replay = log.replay_into(
        node_bus,
        persist::ReplayRetention::RetireCommitted);
    DURABLE_CHECK(!replay);
    const auto* retirement =
        std::get_if<persist::ReplayRetirementFailure>(&replay.error());
    DURABLE_CHECK(retirement != nullptr);
    DURABLE_CHECK(retirement->frame == command::FrameNumber{1U});
    DURABLE_CHECK(retirement->history.frame_states == 2U);
    DURABLE_CHECK(retirement->history.seen_commands == 1U);
    DURABLE_CHECK(
        retirement->history == node_bus.replay_history_diagnostics());
    DURABLE_CHECK(
        node_bus.cursor()
        == command::StreamCursor{
            command::FrameNumber{1U},
            document::Version{0U},
            command::CommandId{1U}});
    DURABLE_CHECK(!node_bus.needs_resync());
}

void corrupt_replay_and_non_genesis_logs_return_no_runtime() {
    TempWal temporary;
    {
        auto opened = persist::CommandLog::open_or_create(
            temporary.path(),
            genesis_cursor(),
            options_for(persist::SyncPolicy::Data));
        DURABLE_CHECK(opened);
        auto log = std::move(opened).value();
        command::DirectorScheduler director;
        schedule_put(director, 1U, 10, 1U, 1U);
        persist::DurableDirector durable{director, log};
        DURABLE_CHECK(durable.seal_frame(command::FrameNumber{1U}));
    }

    // Append a second frame with a fully authenticated WAL envelope but an
    // invalid typed record. Startup therefore commits the good first frame in
    // its temporary replay node before rejecting the later frame, and must
    // still return no partially recovered runtime.
    {
        auto raw_open = persist::SealedFrameLog::open_or_create(
            temporary.path(),
            persist::LogCursor{0U, 0U, 1U},
            options_for(persist::SyncPolicy::Data));
        DURABLE_CHECK(raw_open);
        auto raw_log = std::move(raw_open).value();
        auto corrupt_append = raw_log.append(persist::SealedFrameBytes{
            2U,
            1U,
            1U,
            2U,
            2U,
            {{std::byte{0x01U}}},
        });
        DURABLE_CHECK(corrupt_append);
        DURABLE_CHECK(corrupt_append.value().durable);
    }

    auto reopened = persist::CommandLog::open_or_create(
        temporary.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Data));
    DURABLE_CHECK(reopened);
    auto log = std::move(reopened).value();
    command::NodeCommandBus partial_probe;
    auto partial_replay = log.replay_into(partial_probe);
    DURABLE_CHECK(!partial_replay);
    DURABLE_CHECK(
        partial_probe.committed_through() == command::FrameNumber{1U});
    DURABLE_CHECK(partial_probe.document().contains(document::NodeId{1U}));

    auto corrupt = persist::recover_genesis_runtime(log);
    DURABLE_CHECK(!corrupt);
    DURABLE_CHECK(
        corrupt.error().code == persist::StartupRecoveryErrorCode::ReplayFailed);
    DURABLE_CHECK(corrupt.error().command_log_error.has_value());
    const auto* decode_failure =
        std::get_if<persist::DecodeFailure>(&*corrupt.error().command_log_error);
    DURABLE_CHECK(decode_failure != nullptr);
    DURABLE_CHECK(decode_failure->record_index == 0U);
    DURABLE_CHECK(decode_failure->error == wire::DecodeError::InvalidBuffer);

    TempWal checkpointed_file;
    const auto checkpointed_base = command::StreamCursor{
        command::FrameNumber{7U},
        document::Version{11U},
        command::CommandId{13U},
    };
    auto checkpointed_open = persist::CommandLog::open_or_create(
        checkpointed_file.path(),
        checkpointed_base,
        options_for(persist::SyncPolicy::Data));
    DURABLE_CHECK(checkpointed_open);
    auto checkpointed_log = std::move(checkpointed_open).value();
    auto non_genesis = persist::recover_genesis_runtime(checkpointed_log);
    DURABLE_CHECK(!non_genesis);
    DURABLE_CHECK(
        non_genesis.error().code == persist::StartupRecoveryErrorCode::NonGenesisLog);
}

// Durability cadence is not frame cadence. These cases pin the part of that
// decision that is load-bearing: an EditsOnly coordinator seals, publishes and
// releases exactly as a durable one does, and touches neither file.
void edits_only_seals_and_releases_without_writing_anything() {
    TempWal temporary;
    auto opened = persist::CommandLog::open_or_create(
        temporary.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Full));
    DURABLE_CHECK(opened);
    auto log = std::move(opened).value();
    command::DirectorScheduler director;
    persist::DurableDirector durable{
        director, log, persist::FrameDurability::EditsOnly};
    DURABLE_CHECK(
        durable.frame_durability() == persist::FrameDurability::EditsOnly);

    const auto bytes_before = log.recovery_report().valid_bytes;
    const auto entries_before = log.recovery_report().entry_count;

    // One frame carrying an edit, then a long run of empty frames: the shape a
    // playing show produces once the transport is an anchor.
    schedule_put(director, 1U, 10, 1U, 1U);
    for (std::uint64_t frame = 1U; frame <= 500U; ++frame) {
        auto sealed = durable.seal_frame(command::FrameNumber{frame});
        DURABLE_CHECK(sealed);
        const auto batch = std::move(sealed).value();

        // The receipt says plainly that nothing was written, and still names
        // the cursor this frame publishes at.
        DURABLE_CHECK(!batch.receipt().durable);
        DURABLE_CHECK(batch.receipt().bytes_written == 0U);
        DURABLE_CHECK(
            batch.after_cursor()
            == command::StreamCursor{
                command::FrameNumber{frame},
                document::Version{1U},
                command::CommandId{2U}});
        DURABLE_CHECK(durable.authorizes(batch));

        // Releases stay contiguous and ordinal-exact without the ledger.
        const auto ordinal = durable.next_release_ordinal();
        DURABLE_CHECK(ordinal == frame);
        DURABLE_CHECK(!durable.record_release(batch, ordinal + 1U));
        DURABLE_CHECK(durable.record_release(batch, ordinal));
        DURABLE_CHECK(!durable.record_release(batch, ordinal));
    }

    DURABLE_CHECK(log.recovery_report().valid_bytes == bytes_before);
    DURABLE_CHECK(log.recovery_report().entry_count == entries_before);
    DURABLE_CHECK(log.tail_cursor() == genesis_cursor());
    DURABLE_CHECK(
        durable.published_tail()
        == command::StreamCursor{
            command::FrameNumber{500U},
            document::Version{1U},
            command::CommandId{2U}});
    DURABLE_CHECK(durable.state() == persist::DurableDirectorState::Ready);
    DURABLE_CHECK(durable.unjournalled_edit_frames() == 1U);
}

void edits_only_answers_recent_releases_and_forgets_older_ones() {
    TempWal temporary;
    auto opened = persist::CommandLog::open_or_create(
        temporary.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Full));
    DURABLE_CHECK(opened);
    auto log = std::move(opened).value();
    command::DirectorScheduler director;
    persist::DurableDirector durable{
        director, log, persist::FrameDurability::EditsOnly};

    std::vector<std::pair<command::StreamCursor, command::StreamCursor>> window;
    const std::uint64_t total =
        static_cast<std::uint64_t>(persist::max_retained_memory_releases) + 20U;
    for (std::uint64_t frame = 1U; frame <= total; ++frame) {
        auto sealed = durable.seal_frame(command::FrameNumber{frame});
        DURABLE_CHECK(sealed);
        const auto batch = std::move(sealed).value();
        window.emplace_back(batch.before_cursor(), batch.after_cursor());
        DURABLE_CHECK(
            durable.record_release(batch, durable.next_release_ordinal()));
    }

    // The most recent decisions are answerable, which is what lets a catching
    // up session receive its permit after the barrier slot has been retired.
    const auto& recent = window.back();
    const auto found = durable.release_ordinal(recent.first, recent.second);
    DURABLE_CHECK(found.has_value());
    DURABLE_CHECK(*found == total);

    // The oldest are not, on purpose: an unbounded index is the leak this mode
    // exists to remove, and a session older than the window takes a checkpoint.
    const auto& ancient = window.front();
    DURABLE_CHECK(!durable.release_ordinal(ancient.first, ancient.second));
    DURABLE_CHECK(durable.next_release_ordinal() == total + 1U);
}

void edits_only_and_every_frame_refuse_each_other_s_release_path() {
    TempWal durable_wal;
    auto opened = persist::CommandLog::open_or_create(
        durable_wal.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Data));
    DURABLE_CHECK(opened);
    auto log = std::move(opened).value();
    command::DirectorScheduler director;
    persist::DurableDirector journalling{director, log};

    schedule_put(director, 1U, 10, 1U, 1U);
    auto sealed = journalling.seal_frame(command::FrameNumber{1U});
    DURABLE_CHECK(sealed);
    const auto batch = std::move(sealed).value();
    DURABLE_CHECK(batch.receipt().durable);
    // A durable coordinator cannot be talked into an in-memory release.
    DURABLE_CHECK(!journalling.record_release(batch, 1U));

    TempWal memory_wal;
    auto opened_memory = persist::CommandLog::open_or_create(
        memory_wal.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Data));
    DURABLE_CHECK(opened_memory);
    auto memory_log = std::move(opened_memory).value();
    command::DirectorScheduler memory_director;
    persist::DurableDirector memory{
        memory_director, memory_log, persist::FrameDurability::EditsOnly};

    auto memory_sealed = memory.seal_frame(command::FrameNumber{1U});
    DURABLE_CHECK(memory_sealed);
    const auto memory_batch = std::move(memory_sealed).value();
    // And an in-memory coordinator has no ledger entry point at all.
    auto persisted = memory.persist_release(memory_batch, 1U);
    DURABLE_CHECK(!persisted);
    DURABLE_CHECK(
        persisted.error().code
        == persist::DurableDirectorErrorCode::ReleaseUnavailable);

    // Neither coordinator authorizes the other's publication.
    DURABLE_CHECK(!memory.authorizes(batch));
    DURABLE_CHECK(!journalling.authorizes(memory_batch));
}

// A restart after an EditsOnly run resumes the durable prefix, not the show.
// That is exactly what restart-and-re-cue means, and it has to be true without
// the startup path refusing an unresolved suffix.
void edits_only_leaves_a_restartable_log_behind() {
    TempWal temporary;
    {
        auto opened = persist::CommandLog::open_or_create(
            temporary.path(),
            genesis_cursor(),
            options_for(persist::SyncPolicy::Data));
        DURABLE_CHECK(opened);
        auto log = std::move(opened).value();
        command::DirectorScheduler director;
        persist::DurableDirector durable{
            director, log, persist::FrameDurability::EditsOnly};

        schedule_put(director, 7U, 70, 1U, 1U);
        for (std::uint64_t frame = 1U; frame <= 64U; ++frame) {
            DURABLE_CHECK(durable.seal_frame(command::FrameNumber{frame}));
        }
    }

    auto reopened = persist::CommandLog::open_or_create(
        temporary.path(),
        genesis_cursor(),
        options_for(persist::SyncPolicy::Data));
    DURABLE_CHECK(reopened);
    auto restarted = std::move(reopened).value();
    DURABLE_CHECK(restarted.tail_cursor() == genesis_cursor());

    auto recovered = persist::recover_genesis_runtime(restarted);
    DURABLE_CHECK(recovered);
    DURABLE_CHECK(recovered.value().replay.frames == 0U);
    DURABLE_CHECK(
        recovered.value().director.sealed_through() == command::FrameNumber{0U});
    DURABLE_CHECK(recovered.value().local_node.document().empty());
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"durable content and empty publication",
         &durable_content_and_empty_frames_publish_in_protocol_order},
        {"moved-from batch rejected", &moved_from_batch_is_not_publishable},
        {"None rejected before prepare", &none_policy_is_rejected_before_storage_or_prepare},
        {"known prewrite abort and retry",
         &known_prewrite_rejection_aborts_exact_prepare_and_can_retry},
        {"cursor mismatch preflight", &cursor_mismatch_is_rejected_without_preparing},
        {"resume source excludes WAL/Director commit window",
         &resume_source_never_exposes_the_wal_before_director_commit_window},
        {"external size failure latches", &external_size_failure_latches_prepare_and_blocks_publication},
        {"durable crash window recovery",
         &durable_append_before_director_commit_recovers_from_genesis},
        {"warm genesis continuation", &warm_genesis_restart_continues_exact_cursor_sequence},
        {"bounded many-frame genesis recovery",
         &genesis_recovery_retires_many_frames_with_bounded_history},
        {"retirement refusal surfaced",
         &bounded_replay_surfaces_retirement_refusal_distinctly},
        {"corrupt and non-genesis recovery", &corrupt_replay_and_non_genesis_logs_return_no_runtime},
        {"edits-only seals without writing",
         &edits_only_seals_and_releases_without_writing_anything},
        {"edits-only release window is bounded",
         &edits_only_answers_recent_releases_and_forgets_older_ones},
        {"cadences refuse each other's release path",
         &edits_only_and_every_frame_refuse_each_other_s_release_path},
        {"edits-only leaves a restartable log",
         &edits_only_leaves_a_restartable_log_behind},
    };

    std::size_t failures = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[pass] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - failures) << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
