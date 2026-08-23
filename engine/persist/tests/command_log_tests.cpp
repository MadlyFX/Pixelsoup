#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/persist/command_log.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace command = mediaserver::cmd;
namespace core = mediaserver::core;
namespace document = mediaserver::doc;
namespace persist = mediaserver::persist;
namespace wire = mediaserver::wire;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line) + ": check failed: " + expression};
    }
}

#define COMMAND_LOG_CHECK(expression) \
    check(static_cast<bool>(expression), #expression, __LINE__)

class TempWal final {
public:
    TempWal() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("mediaserver-command-log-" + std::to_string(timestamp) + "-" +
                 std::to_string(sequence.fetch_add(1U)) + ".bin");
    }

    ~TempWal() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TempWal(const TempWal&) = delete;
    TempWal& operator=(const TempWal&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] persist::OpenOptions test_options() {
    persist::OpenOptions options;
    options.sync_policy = persist::SyncPolicy::None;
    return options;
}

[[nodiscard]] document::Node node(std::uint64_t id, std::int64_t value) {
    return document::Node{
        document::NodeId{id},
        "test.node",
        {{"value", value}},
    };
}

[[nodiscard]] command::Payload put(std::uint64_t id, std::int64_t value) {
    return command::PutNode{node(id, value)};
}

[[nodiscard]] command::Payload erase(std::uint64_t id) {
    return command::EraseNode{document::NodeId{id}};
}

[[nodiscard]] command::Command make_command(
    std::uint64_t id,
    std::uint64_t base_revision,
    std::uint64_t frame,
    command::Payload forward,
    command::Payload inverse) {
    return command::Command{
        command::CommandId{id},
        document::Version{base_revision},
        document::Version{base_revision + 1U},
        command::FrameNumber{frame},
        command::TransactionId{1U},
        command::UndoContextId{1U},
        std::move(forward),
        std::move(inverse),
    };
}

[[nodiscard]] command::FrameSeal seal_for(const command::Command& authored) {
    return command::FrameSeal{
        authored.apply_at,
        authored.base,
        authored.result,
        {{authored.id, command::fingerprint(authored)}},
    };
}

[[nodiscard]] persist::RecordBytes encode_message(const wire::Message& message) {
    auto encoded = wire::encode(message);
    COMMAND_LOG_CHECK(encoded);
    return std::move(encoded).value();
}

void append_raw_frame(
    const std::filesystem::path& path,
    const persist::LogCursor& base,
    persist::SealedFrameBytes frame) {
    auto open = persist::SealedFrameLog::open_or_create(path, base, test_options());
    COMMAND_LOG_CHECK(open);
    auto log = std::move(open).value();
    COMMAND_LOG_CHECK(log.append(frame));
}

struct BatchVisitFixture final {
    persist::CommandLog log;
    command::Command first;
    command::FrameSeal first_seal;
    command::FrameSeal empty_seal;
    command::Command last;
    command::FrameSeal last_seal;
    command::StreamCursor base;
    command::StreamCursor after_first;
    command::StreamCursor after_empty;
    command::StreamCursor tail;
};

[[nodiscard]] BatchVisitFixture make_batch_visit_fixture(
    const std::filesystem::path& path) {
    const command::StreamCursor base{
        command::FrameNumber{0U}, document::Version{0U}, command::CommandId{1U}};
    auto open = persist::CommandLog::open_or_create(path, base, test_options());
    COMMAND_LOG_CHECK(open);
    auto log = std::move(open).value();

    const auto first = make_command(1U, 0U, 1U, put(1U, 10), erase(1U));
    const auto first_seal = seal_for(first);
    COMMAND_LOG_CHECK(log.append(
        std::span<const command::Command>{&first, 1U}, first_seal));

    const command::FrameSeal empty_seal{
        command::FrameNumber{2U},
        document::Version{1U},
        document::Version{1U},
        {},
    };
    COMMAND_LOG_CHECK(log.append(
        std::span<const command::Command>{}, empty_seal));

    const auto last = make_command(2U, 1U, 3U, put(2U, 20), erase(2U));
    const auto last_seal = seal_for(last);
    COMMAND_LOG_CHECK(log.append(
        std::span<const command::Command>{&last, 1U}, last_seal));

    return BatchVisitFixture{
        std::move(log),
        first,
        first_seal,
        empty_seal,
        last,
        last_seal,
        base,
        {command::FrameNumber{1U}, document::Version{1U}, command::CommandId{2U}},
        {command::FrameNumber{2U}, document::Version{1U}, command::CommandId{2U}},
        {command::FrameNumber{3U}, document::Version{2U}, command::CommandId{3U}},
    };
}

void batch_visit_covers_base_interior_tail_and_empty_boundaries() {
    TempWal temporary;
    auto fixture = make_batch_visit_fixture(temporary.path());

    std::vector<command::StreamCursor> before;
    std::vector<command::StreamCursor> after;
    std::vector<std::vector<command::Command>> commands;
    std::vector<command::FrameSeal> seals;
    auto all = fixture.log.visit_batches_after(
        fixture.base,
        fixture.tail,
        [&before, &after, &commands, &seals](
            const persist::ValidatedCommandBatchView& batch) {
            before.push_back(batch.before);
            after.push_back(batch.after);
            commands.emplace_back(batch.commands.begin(), batch.commands.end());
            seals.push_back(batch.seal);
            return persist::BatchVisitControl::Continue;
        });
    COMMAND_LOG_CHECK(all);
    COMMAND_LOG_CHECK(all.value().completed);
    COMMAND_LOG_CHECK(all.value().frames == 3U);
    COMMAND_LOG_CHECK(all.value().commands == 2U);
    COMMAND_LOG_CHECK(all.value().cursor == fixture.tail);
    COMMAND_LOG_CHECK(before.size() == 3U);
    COMMAND_LOG_CHECK(before[0] == fixture.base);
    COMMAND_LOG_CHECK(after[0] == fixture.after_first);
    COMMAND_LOG_CHECK(before[1] == fixture.after_first);
    COMMAND_LOG_CHECK(after[1] == fixture.after_empty);
    COMMAND_LOG_CHECK(before[2] == fixture.after_empty);
    COMMAND_LOG_CHECK(after[2] == fixture.tail);
    COMMAND_LOG_CHECK(commands[0] == std::vector<command::Command>{fixture.first});
    COMMAND_LOG_CHECK(commands[1].empty());
    COMMAND_LOG_CHECK(commands[2] == std::vector<command::Command>{fixture.last});
    COMMAND_LOG_CHECK(seals[0] == fixture.first_seal);
    COMMAND_LOG_CHECK(seals[1] == fixture.empty_seal);
    COMMAND_LOG_CHECK(seals[2] == fixture.last_seal);

    std::vector<command::FrameNumber> interior_frames;
    auto interior = fixture.log.visit_batches_after(
        fixture.after_first,
        fixture.tail,
        [&interior_frames](const persist::ValidatedCommandBatchView& batch) {
            interior_frames.push_back(batch.seal.frame);
            return persist::BatchVisitControl::Continue;
        });
    COMMAND_LOG_CHECK(interior);
    COMMAND_LOG_CHECK(interior.value().completed);
    COMMAND_LOG_CHECK(interior.value().frames == 2U);
    COMMAND_LOG_CHECK(interior.value().commands == 1U);
    COMMAND_LOG_CHECK(interior.value().cursor == fixture.tail);
    const std::vector<command::FrameNumber> expected_interior_frames{
        command::FrameNumber{2U}, command::FrameNumber{3U}};
    COMMAND_LOG_CHECK(interior_frames == expected_interior_frames);

    std::size_t prefix_callbacks = 0U;
    auto prefix = fixture.log.visit_batches_after(
        fixture.base,
        fixture.after_empty,
        [&prefix_callbacks](const persist::ValidatedCommandBatchView&) {
            ++prefix_callbacks;
            return persist::BatchVisitControl::Continue;
        });
    COMMAND_LOG_CHECK(prefix);
    COMMAND_LOG_CHECK(prefix.value().completed);
    COMMAND_LOG_CHECK(prefix.value().frames == 2U);
    COMMAND_LOG_CHECK(prefix.value().commands == 1U);
    COMMAND_LOG_CHECK(prefix.value().cursor == fixture.after_empty);
    COMMAND_LOG_CHECK(prefix_callbacks == 2U);

    std::size_t tail_callbacks = 0U;
    auto no_more = fixture.log.visit_batches_after(
        fixture.tail,
        fixture.tail,
        [&tail_callbacks](const persist::ValidatedCommandBatchView&) {
            ++tail_callbacks;
            return persist::BatchVisitControl::Continue;
        });
    COMMAND_LOG_CHECK(no_more);
    COMMAND_LOG_CHECK(no_more.value().completed);
    COMMAND_LOG_CHECK(no_more.value().frames == 0U);
    COMMAND_LOG_CHECK(no_more.value().commands == 0U);
    COMMAND_LOG_CHECK(no_more.value().cursor == fixture.tail);
    COMMAND_LOG_CHECK(tail_callbacks == 0U);
}

void batch_visit_prevalidates_target_and_order_before_callback() {
    TempWal temporary;
    auto fixture = make_batch_visit_fixture(temporary.path());

    auto invalid_target = fixture.after_empty;
    invalid_target.revision = document::Version{99U};
    std::size_t invalid_callbacks = 0U;
    auto invalid = fixture.log.visit_batches_after(
        fixture.base,
        invalid_target,
        [&invalid_callbacks](const persist::ValidatedCommandBatchView&) {
            ++invalid_callbacks;
            return persist::BatchVisitControl::Continue;
        });
    COMMAND_LOG_CHECK(!invalid);
    COMMAND_LOG_CHECK(invalid_callbacks == 0U);
    const auto* missing = std::get_if<persist::LogFailure>(&invalid.error());
    COMMAND_LOG_CHECK(missing != nullptr);
    COMMAND_LOG_CHECK(missing->error.code == persist::LogErrorCode::CursorNotFound);

    auto invalid_from = fixture.after_first;
    invalid_from.next_command = command::CommandId{99U};
    std::size_t invalid_from_callbacks = 0U;
    auto invalid_start = fixture.log.visit_batches_after(
        invalid_from,
        fixture.tail,
        [&invalid_from_callbacks](const persist::ValidatedCommandBatchView&) {
            ++invalid_from_callbacks;
            return persist::BatchVisitControl::Continue;
        });
    COMMAND_LOG_CHECK(!invalid_start);
    COMMAND_LOG_CHECK(invalid_from_callbacks == 0U);
    const auto* missing_start =
        std::get_if<persist::LogFailure>(&invalid_start.error());
    COMMAND_LOG_CHECK(missing_start != nullptr);
    COMMAND_LOG_CHECK(
        missing_start->error.code == persist::LogErrorCode::CursorNotFound);

    std::size_t reversed_callbacks = 0U;
    auto reversed = fixture.log.visit_batches_after(
        fixture.tail,
        fixture.after_first,
        [&reversed_callbacks](const persist::ValidatedCommandBatchView&) {
            ++reversed_callbacks;
            return persist::BatchVisitControl::Continue;
        });
    COMMAND_LOG_CHECK(!reversed);
    COMMAND_LOG_CHECK(reversed_callbacks == 0U);
    const auto* ordering = std::get_if<persist::LogFailure>(&reversed.error());
    COMMAND_LOG_CHECK(ordering != nullptr);
    COMMAND_LOG_CHECK(ordering->error.code == persist::LogErrorCode::ChainMismatch);
}

void batch_visit_stop_is_successful_and_incomplete() {
    TempWal temporary;
    auto fixture = make_batch_visit_fixture(temporary.path());

    std::size_t callbacks = 0U;
    auto stopped = fixture.log.visit_batches_after(
        fixture.base,
        fixture.tail,
        [&callbacks](const persist::ValidatedCommandBatchView& batch) {
            ++callbacks;
            COMMAND_LOG_CHECK(batch.seal.frame == command::FrameNumber{1U});
            return persist::BatchVisitControl::Stop;
        });
    COMMAND_LOG_CHECK(stopped);
    COMMAND_LOG_CHECK(!stopped.value().completed);
    COMMAND_LOG_CHECK(stopped.value().frames == 1U);
    COMMAND_LOG_CHECK(stopped.value().commands == 1U);
    COMMAND_LOG_CHECK(stopped.value().cursor == fixture.after_first);
    COMMAND_LOG_CHECK(callbacks == 1U);
}

void immutable_snapshot_ignores_growth_and_never_holds_append_lock() {
    static_assert(!std::is_copy_constructible_v<persist::CommandLogSnapshot>);
    static_assert(std::is_nothrow_move_constructible_v<persist::CommandLogSnapshot>);

    TempWal temporary;
    const command::StreamCursor base{
        command::FrameNumber{0U}, document::Version{0U}, command::CommandId{1U}};
    auto opened = persist::CommandLog::open_or_create(
        temporary.path(), base, test_options());
    COMMAND_LOG_CHECK(opened);
    auto log = std::move(opened).value();

    const auto first = make_command(1U, 0U, 1U, put(1U, 10), erase(1U));
    const auto first_seal = seal_for(first);
    COMMAND_LOG_CHECK(log.append(
        std::span<const command::Command>{&first, 1U}, first_seal));

    auto captured = log.capture_committed();
    COMMAND_LOG_CHECK(captured);
    auto snapshot = std::move(captured).value();
    const auto captured_tail = snapshot.tail_cursor();
    const auto captured_bytes = snapshot.valid_bytes();
    COMMAND_LOG_CHECK((captured_tail == command::StreamCursor{
        command::FrameNumber{1U}, document::Version{1U}, command::CommandId{2U}}));
    COMMAND_LOG_CHECK(snapshot.path() == log.path());

    const auto second = make_command(2U, 1U, 2U, put(2U, 20), erase(2U));
    const auto second_seal = seal_for(second);

    std::mutex gate;
    std::condition_variable changed;
    bool visitor_entered = false;
    bool release_visitor = false;
    bool append_finished = false;
    std::size_t callbacks = 0U;
    command::FrameNumber visited_frame{};
    std::optional<core::Result<persist::BatchVisitReport, persist::CommandLogError>>
        visit_result;
    std::optional<core::Result<persist::AppendReceipt, persist::CommandLogError>>
        append_result;
    std::exception_ptr visit_exception;
    std::exception_ptr append_exception;

    std::thread visitor_thread{[&] {
        try {
            visit_result.emplace(snapshot.visit_batches_after(
                base,
                captured_tail,
                [&](const persist::ValidatedCommandBatchView& batch) {
                    std::unique_lock lock{gate};
                    ++callbacks;
                    visited_frame = batch.seal.frame;
                    visitor_entered = true;
                    changed.notify_all();
                    changed.wait(lock, [&] { return release_visitor; });
                    return persist::BatchVisitControl::Continue;
                }));
        } catch (...) {
            visit_exception = std::current_exception();
            std::lock_guard lock{gate};
            visitor_entered = true;
            changed.notify_all();
        }
    }};

    bool entered_before_timeout = false;
    {
        std::unique_lock lock{gate};
        entered_before_timeout = changed.wait_for(
            lock, std::chrono::seconds{5}, [&] { return visitor_entered; });
    }

    std::thread append_thread{[&] {
        try {
            append_result.emplace(log.append(
                std::span<const command::Command>{&second, 1U}, second_seal));
        } catch (...) {
            append_exception = std::current_exception();
        }
        std::lock_guard lock{gate};
        append_finished = true;
        changed.notify_all();
    }};

    bool append_completed_while_visitor_blocked = false;
    {
        std::unique_lock lock{gate};
        append_completed_while_visitor_blocked = changed.wait_for(
            lock, std::chrono::seconds{5}, [&] { return append_finished; });
        release_visitor = true;
        changed.notify_all();
    }

    visitor_thread.join();
    append_thread.join();

    COMMAND_LOG_CHECK(entered_before_timeout);
    COMMAND_LOG_CHECK(append_completed_while_visitor_blocked);
    COMMAND_LOG_CHECK(visit_exception == nullptr);
    COMMAND_LOG_CHECK(append_exception == nullptr);
    COMMAND_LOG_CHECK(visit_result.has_value());
    COMMAND_LOG_CHECK(visit_result->has_value());
    COMMAND_LOG_CHECK(visit_result->value().completed);
    COMMAND_LOG_CHECK(visit_result->value().frames == 1U);
    COMMAND_LOG_CHECK(visit_result->value().commands == 1U);
    COMMAND_LOG_CHECK(visit_result->value().cursor == captured_tail);
    COMMAND_LOG_CHECK(callbacks == 1U);
    COMMAND_LOG_CHECK(visited_frame == command::FrameNumber{1U});
    COMMAND_LOG_CHECK(append_result.has_value());
    COMMAND_LOG_CHECK(append_result->has_value());
    COMMAND_LOG_CHECK((log.tail_cursor() == command::StreamCursor{
        command::FrameNumber{2U}, document::Version{2U}, command::CommandId{3U}}));
    COMMAND_LOG_CHECK(snapshot.tail_cursor() == captured_tail);
    COMMAND_LOG_CHECK(snapshot.valid_bytes() == captured_bytes);
    COMMAND_LOG_CHECK(log.recovery_report().valid_bytes > captured_bytes);
}

void checkpointed_fresh_node_replays_content_and_empty_frames() {
    command::DirectorScheduler director;
    COMMAND_LOG_CHECK(director.schedule_edit(
        put(1U, 10),
        erase(1U),
        command::UndoContextId{1U},
        command::TransactionId{1U},
        command::FrameNumber{1U}));
    COMMAND_LOG_CHECK(director.seal_frame(command::FrameNumber{1U}));
    const auto checkpoint = director.checkpoint();

    TempWal temporary;
    auto open = persist::CommandLog::open_or_create(
        temporary.path(), checkpoint.cursor, test_options());
    COMMAND_LOG_CHECK(open);
    auto log = std::move(open).value();

    COMMAND_LOG_CHECK(director.schedule_edit(
        put(2U, 20),
        erase(2U),
        command::UndoContextId{1U},
        command::TransactionId{2U},
        command::FrameNumber{2U}));
    auto content_seal = director.seal_frame(command::FrameNumber{2U});
    COMMAND_LOG_CHECK(content_seal);
    COMMAND_LOG_CHECK(log.append(
        director.commands_for(command::FrameNumber{2U}), content_seal.value()));

    auto empty_seal = director.seal_frame(command::FrameNumber{3U});
    COMMAND_LOG_CHECK(empty_seal);
    COMMAND_LOG_CHECK(log.append(
        std::span<const command::Command>{}, empty_seal.value()));

    command::NodeCommandBus fresh_node;
    COMMAND_LOG_CHECK(fresh_node.install_checkpoint(checkpoint));
    auto replay = log.replay_into(fresh_node);
    COMMAND_LOG_CHECK(replay);
    COMMAND_LOG_CHECK(replay.value().frames == 2U);
    COMMAND_LOG_CHECK(replay.value().commands == 1U);
    COMMAND_LOG_CHECK(replay.value().cursor.through_frame == command::FrameNumber{3U});
    COMMAND_LOG_CHECK(replay.value().cursor.revision == document::Version{2U});
    COMMAND_LOG_CHECK(replay.value().cursor.next_command == command::CommandId{3U});
    COMMAND_LOG_CHECK(fresh_node.document().contains(document::NodeId{1U}));
    COMMAND_LOG_CHECK(fresh_node.document().contains(document::NodeId{2U}));
    COMMAND_LOG_CHECK(document::diff(
        fresh_node.document(), director.sealed_document()).changes.empty());

    auto no_more = log.replay_into(fresh_node);
    COMMAND_LOG_CHECK(no_more);
    COMMAND_LOG_CHECK(no_more.value().frames == 0U);
}

void append_validates_a_complete_batch_before_writing() {
    TempWal temporary;
    const command::StreamCursor base{
        command::FrameNumber{0U}, document::Version{0U}, command::CommandId{1U}};
    auto open = persist::CommandLog::open_or_create(temporary.path(), base, test_options());
    COMMAND_LOG_CHECK(open);
    auto log = std::move(open).value();

    const auto authored = make_command(1U, 0U, 1U, put(1U, 10), erase(1U));
    auto seal = seal_for(authored);
    seal.commands.front().fingerprint = command::CommandFingerprint{7U};
    auto append = log.append(std::span<const command::Command>{&authored, 1U}, seal);
    COMMAND_LOG_CHECK(!append);
    const auto* failure = std::get_if<persist::ProtocolFailure>(&append.error());
    COMMAND_LOG_CHECK(failure != nullptr);
    COMMAND_LOG_CHECK(
        failure->error == persist::CommandProtocolError::CommandFingerprintMismatch);
    COMMAND_LOG_CHECK(log.recovery_report().entry_count == 0U);

    const auto valid_seal = seal_for(authored);
    COMMAND_LOG_CHECK(log.append(
        std::span<const command::Command>{&authored, 1U}, valid_seal));
    COMMAND_LOG_CHECK(log.recovery_report().entry_count == 1U);
}

void replay_rejects_a_wrong_wire_message_type_before_node_delivery() {
    TempWal temporary;
    const persist::LogCursor base{0U, 0U, 1U};
    const command::FrameSeal seal{
        command::FrameNumber{1U},
        document::Version{0U},
        document::Version{1U},
        {{command::CommandId{1U}, command::CommandFingerprint{123U}}},
    };
    std::vector<persist::RecordBytes> records;
    records.push_back(encode_message(wire::Message{command::StreamCursor{
        command::FrameNumber{0U}, document::Version{0U}, command::CommandId{1U}}}));
    records.push_back(encode_message(wire::Message{seal}));
    append_raw_frame(
        temporary.path(),
        base,
        persist::SealedFrameBytes{1U, 0U, 1U, 1U, 2U, std::move(records)});

    const command::StreamCursor typed_base{
        command::FrameNumber{0U}, document::Version{0U}, command::CommandId{1U}};
    auto open = persist::CommandLog::open_or_create(
        temporary.path(), typed_base, test_options());
    COMMAND_LOG_CHECK(open);
    auto log = std::move(open).value();

    std::size_t batch_callbacks = 0U;
    auto batch_visit = log.visit_batches_after(
        typed_base,
        command::StreamCursor{
            command::FrameNumber{1U},
            document::Version{1U},
            command::CommandId{2U}},
        [&batch_callbacks](const persist::ValidatedCommandBatchView&) {
            ++batch_callbacks;
            return persist::BatchVisitControl::Continue;
        });
    COMMAND_LOG_CHECK(!batch_visit);
    COMMAND_LOG_CHECK(batch_callbacks == 0U);
    const auto* batch_failure =
        std::get_if<persist::ProtocolFailure>(&batch_visit.error());
    COMMAND_LOG_CHECK(batch_failure != nullptr);
    COMMAND_LOG_CHECK(
        batch_failure->error == persist::CommandProtocolError::ExpectedCommandRecord);
    COMMAND_LOG_CHECK(batch_failure->record_index == 0U);

    command::NodeCommandBus node_bus;
    auto replay = log.replay_into(node_bus);
    COMMAND_LOG_CHECK(!replay);
    const auto* failure = std::get_if<persist::ProtocolFailure>(&replay.error());
    COMMAND_LOG_CHECK(failure != nullptr);
    COMMAND_LOG_CHECK(
        failure->error == persist::CommandProtocolError::ExpectedCommandRecord);
    COMMAND_LOG_CHECK(failure->record_index == 0U);
    COMMAND_LOG_CHECK(node_bus.cursor().through_frame == command::FrameNumber{0U});
    COMMAND_LOG_CHECK(node_bus.document().empty());
    COMMAND_LOG_CHECK(!node_bus.needs_resync());
}

void replay_cross_checks_outer_wal_metadata_before_node_delivery() {
    TempWal temporary;
    const auto authored = make_command(1U, 0U, 2U, put(1U, 10), erase(1U));
    const auto seal = seal_for(authored);
    std::vector<persist::RecordBytes> records;
    records.push_back(encode_message(wire::Message{authored}));
    records.push_back(encode_message(wire::Message{seal}));
    append_raw_frame(
        temporary.path(),
        persist::LogCursor{0U, 0U, 1U},
        persist::SealedFrameBytes{1U, 0U, 1U, 1U, 2U, std::move(records)});

    const command::StreamCursor base{
        command::FrameNumber{0U}, document::Version{0U}, command::CommandId{1U}};
    auto open = persist::CommandLog::open_or_create(temporary.path(), base, test_options());
    COMMAND_LOG_CHECK(open);
    auto log = std::move(open).value();
    command::NodeCommandBus node_bus;
    auto replay = log.replay_into(node_bus);
    COMMAND_LOG_CHECK(!replay);
    const auto* failure = std::get_if<persist::ProtocolFailure>(&replay.error());
    COMMAND_LOG_CHECK(failure != nullptr);
    COMMAND_LOG_CHECK(
        failure->error == persist::CommandProtocolError::OuterMetadataMismatch);
    COMMAND_LOG_CHECK(node_bus.document().empty());
    COMMAND_LOG_CHECK(!node_bus.needs_resync());
}

void replay_surfaces_wire_decode_and_node_semantic_failures_distinctly() {
    const command::StreamCursor base{
        command::FrameNumber{0U}, document::Version{0U}, command::CommandId{1U}};

    TempWal invalid_wire_file;
    const auto valid_first = make_command(1U, 0U, 1U, put(1U, 10), erase(1U));
    const command::FrameSeal syntactic_seal{
        command::FrameNumber{1U},
        document::Version{0U},
        document::Version{2U},
        {
            {valid_first.id, command::fingerprint(valid_first)},
            {command::CommandId{2U}, command::CommandFingerprint{123U}},
        },
    };
    append_raw_frame(
        invalid_wire_file.path(),
        persist::LogCursor{0U, 0U, 1U},
        persist::SealedFrameBytes{
            1U,
            0U,
            2U,
            1U,
            3U,
            {
                encode_message(wire::Message{valid_first}),
                {std::byte{0x01U}},
                encode_message(wire::Message{syntactic_seal}),
            },
        });
    auto invalid_open = persist::CommandLog::open_or_create(
        invalid_wire_file.path(), base, test_options());
    COMMAND_LOG_CHECK(invalid_open);
    auto invalid_log = std::move(invalid_open).value();
    command::NodeCommandBus untouched_node;
    auto invalid_replay = invalid_log.replay_into(untouched_node);
    COMMAND_LOG_CHECK(!invalid_replay);
    const auto* decode_failure =
        std::get_if<persist::DecodeFailure>(&invalid_replay.error());
    COMMAND_LOG_CHECK(decode_failure != nullptr);
    COMMAND_LOG_CHECK(decode_failure->error == wire::DecodeError::InvalidBuffer);
    COMMAND_LOG_CHECK(decode_failure->record_index == 1U);
    COMMAND_LOG_CHECK(!untouched_node.needs_resync());
    COMMAND_LOG_CHECK(
        untouched_node.receive(valid_first) == command::ReceiveStatus::Accepted);

    TempWal semantic_file;
    auto semantic_open = persist::CommandLog::open_or_create(
        semantic_file.path(), base, test_options());
    COMMAND_LOG_CHECK(semantic_open);
    auto semantic_log = std::move(semantic_open).value();
    const auto invalid_semantics = make_command(
        1U, 0U, 1U, erase(99U), command::PutNode{node(99U, 99)});
    const auto semantic_seal = seal_for(invalid_semantics);
    COMMAND_LOG_CHECK(semantic_log.append(
        std::span<const command::Command>{&invalid_semantics, 1U}, semantic_seal));
    command::NodeCommandBus failing_node;
    auto semantic_replay = semantic_log.replay_into(failing_node);
    COMMAND_LOG_CHECK(!semantic_replay);
    const auto* node_failure = std::get_if<persist::NodeFailure>(&semantic_replay.error());
    COMMAND_LOG_CHECK(node_failure != nullptr);
    COMMAND_LOG_CHECK(
        node_failure->operation == persist::NodeReplayOperation::ReceiveSeal);
    COMMAND_LOG_CHECK(node_failure->fault.has_value());
    COMMAND_LOG_CHECK(node_failure->fault->code == command::FaultCode::PayloadRejected);
    COMMAND_LOG_CHECK(failing_node.document().empty());
}

void append_surfaces_encode_and_log_failures_distinctly() {
    const command::StreamCursor base{
        command::FrameNumber{0U}, document::Version{0U}, command::CommandId{1U}};

    TempWal encode_file;
    auto encode_open = persist::CommandLog::open_or_create(
        encode_file.path(), base, test_options());
    COMMAND_LOG_CHECK(encode_open);
    auto encode_log = std::move(encode_open).value();
    auto invalid_node = node(1U, 10);
    invalid_node.type = std::string(1U, static_cast<char>(0xff));
    const auto invalid_encoding = make_command(
        1U, 0U, 1U, command::PutNode{invalid_node}, erase(1U));
    const auto invalid_encoding_seal = seal_for(invalid_encoding);
    auto encoded = encode_log.append(
        std::span<const command::Command>{&invalid_encoding, 1U},
        invalid_encoding_seal);
    COMMAND_LOG_CHECK(!encoded);
    const auto* encode_failure = std::get_if<persist::EncodeFailure>(&encoded.error());
    COMMAND_LOG_CHECK(encode_failure != nullptr);
    COMMAND_LOG_CHECK(encode_failure->error == wire::EncodeError::InvalidNode);
    COMMAND_LOG_CHECK(encode_log.recovery_report().entry_count == 0U);

    TempWal log_file;
    auto log_open = persist::CommandLog::open_or_create(
        log_file.path(), base, test_options());
    COMMAND_LOG_CHECK(log_open);
    auto log = std::move(log_open).value();
    const auto skipped_frame = make_command(1U, 0U, 2U, put(1U, 10), erase(1U));
    const auto skipped_seal = seal_for(skipped_frame);
    auto skipped = log.append(
        std::span<const command::Command>{&skipped_frame, 1U}, skipped_seal);
    COMMAND_LOG_CHECK(!skipped);
    const auto* log_failure = std::get_if<persist::LogFailure>(&skipped.error());
    COMMAND_LOG_CHECK(log_failure != nullptr);
    COMMAND_LOG_CHECK(log_failure->error.code == persist::LogErrorCode::ChainMismatch);
}

void append_honors_the_configured_wire_limits() {
    TempWal temporary;
    const command::StreamCursor base{
        command::FrameNumber{0U}, document::Version{0U}, command::CommandId{1U}};
    auto limits = wire::DecodeLimits{};
    limits.max_string_bytes = 3U;
    auto open = persist::CommandLog::open_or_create(
        temporary.path(), base, test_options(), limits);
    COMMAND_LOG_CHECK(open);
    auto log = std::move(open).value();

    const auto authored = make_command(1U, 0U, 1U, put(1U, 10), erase(1U));
    const auto seal = seal_for(authored);
    auto append = log.append(std::span<const command::Command>{&authored, 1U}, seal);
    COMMAND_LOG_CHECK(!append);
    const auto* failure = std::get_if<persist::EncodeFailure>(&append.error());
    COMMAND_LOG_CHECK(failure != nullptr);
    COMMAND_LOG_CHECK(failure->error == wire::EncodeError::SizeLimitExceeded);
    COMMAND_LOG_CHECK(failure->record_index == 0U);
    COMMAND_LOG_CHECK(log.recovery_report().entry_count == 0U);

    TempWal verifier_file;
    limits = wire::DecodeLimits{};
    limits.verifier_max_tables = 1U;
    auto verifier_open = persist::CommandLog::open_or_create(
        verifier_file.path(), base, test_options(), limits);
    COMMAND_LOG_CHECK(verifier_open);
    auto verifier_log = std::move(verifier_open).value();
    auto verifier_append = verifier_log.append(
        std::span<const command::Command>{&authored, 1U}, seal);
    COMMAND_LOG_CHECK(!verifier_append);
    const auto* decode_failure =
        std::get_if<persist::DecodeFailure>(&verifier_append.error());
    COMMAND_LOG_CHECK(decode_failure != nullptr);
    COMMAND_LOG_CHECK(decode_failure->record_index == 0U);
    COMMAND_LOG_CHECK(verifier_log.recovery_report().entry_count == 0U);
}

void append_preflights_wal_count_and_payload_limits() {
    const command::StreamCursor base{
        command::FrameNumber{0U}, document::Version{0U}, command::CommandId{1U}};
    const auto authored = make_command(1U, 0U, 1U, put(1U, 10), erase(1U));
    const auto seal = seal_for(authored);

    TempWal count_file;
    auto count_options = test_options();
    count_options.limits.max_records_per_frame = 1U;
    auto count_open = persist::CommandLog::open_or_create(
        count_file.path(), base, count_options);
    COMMAND_LOG_CHECK(count_open);
    auto count_log = std::move(count_open).value();
    auto count_result = count_log.append(
        std::span<const command::Command>{&authored, 1U}, seal);
    COMMAND_LOG_CHECK(!count_result);
    const auto* count_failure = std::get_if<persist::LogFailure>(&count_result.error());
    COMMAND_LOG_CHECK(count_failure != nullptr);
    COMMAND_LOG_CHECK(count_failure->error.code == persist::LogErrorCode::LimitExceeded);
    COMMAND_LOG_CHECK(count_log.recovery_report().entry_count == 0U);

    TempWal payload_file;
    auto payload_options = test_options();
    payload_options.limits.max_payload_bytes = 64U;
    auto payload_open = persist::CommandLog::open_or_create(
        payload_file.path(), base, payload_options);
    COMMAND_LOG_CHECK(payload_open);
    auto payload_log = std::move(payload_open).value();
    auto payload_result = payload_log.append(
        std::span<const command::Command>{&authored, 1U}, seal);
    COMMAND_LOG_CHECK(!payload_result);
    const auto* payload_failure = std::get_if<persist::LogFailure>(&payload_result.error());
    COMMAND_LOG_CHECK(payload_failure != nullptr);
    COMMAND_LOG_CHECK(payload_failure->error.code == persist::LogErrorCode::LimitExceeded);
    COMMAND_LOG_CHECK(payload_log.recovery_report().entry_count == 0U);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"checkpointed fresh node replay", &checkpointed_fresh_node_replays_content_and_empty_frames},
        {"append validates complete batch", &append_validates_a_complete_batch_before_writing},
        {"wrong wire message type", &replay_rejects_a_wrong_wire_message_type_before_node_delivery},
        {"outer metadata cross-check", &replay_cross_checks_outer_wal_metadata_before_node_delivery},
        {"decode and node failures", &replay_surfaces_wire_decode_and_node_semantic_failures_distinctly},
        {"encode and log failures", &append_surfaces_encode_and_log_failures_distinctly},
        {"configured wire limits", &append_honors_the_configured_wire_limits},
        {"WAL count and payload preflight", &append_preflights_wal_count_and_payload_limits},
        {"typed batch boundary ranges", &batch_visit_covers_base_interior_tail_and_empty_boundaries},
        {"typed batch cursor prevalidation", &batch_visit_prevalidates_target_and_order_before_callback},
        {"typed batch visitor stop", &batch_visit_stop_is_successful_and_incomplete},
        {"immutable snapshot permits concurrent append",
         &immutable_snapshot_ignores_growth_and_never_holds_append_lock},
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
