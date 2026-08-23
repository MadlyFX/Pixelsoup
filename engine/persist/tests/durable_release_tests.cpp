#include <mediaserver/persist/durable_director.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace command = mediaserver::cmd;
namespace document = mediaserver::doc;
namespace persist = mediaserver::persist;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define RELEASE_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempStore final {
public:
    TempStore() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const auto stem = std::string{"mediaserver-durable-release-"}
            + std::to_string(timestamp) + "-"
            + std::to_string(sequence.fetch_add(1U));
        wal_ = std::filesystem::temp_directory_path() / (stem + ".wal");
        ledger_ = std::filesystem::temp_directory_path() / (stem + ".rel");
    }

    ~TempStore() {
        std::error_code ignored;
        std::filesystem::remove(wal_, ignored);
        std::filesystem::remove(ledger_, ignored);
    }

    TempStore(const TempStore&) = delete;
    TempStore& operator=(const TempStore&) = delete;

    [[nodiscard]] const std::filesystem::path& wal() const noexcept {
        return wal_;
    }
    [[nodiscard]] const std::filesystem::path& ledger() const noexcept {
        return ledger_;
    }

private:
    std::filesystem::path wal_;
    std::filesystem::path ledger_;
};

class RecordingRegistration final
    : public persist::ResumeSourceRegistration {
public:
    [[nodiscard]] bool register_captured_tail(
        const command::StreamCursor& tail) noexcept override {
        ++calls;
        last_tail = tail;
        return true;
    }

    std::uint64_t calls{};
    command::StreamCursor last_tail{};
};

[[nodiscard]] command::StreamCursor genesis_cursor() noexcept {
    return command::StreamCursor{
        command::FrameNumber{0U},
        document::Version{0U},
        command::CommandId{1U},
    };
}

[[nodiscard]] persist::CommandLog open_log(
    const std::filesystem::path& path) {
    persist::OpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    auto opened = persist::CommandLog::open_or_create(
        path, genesis_cursor(), options);
    RELEASE_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] persist::ReleaseLedger open_ledger(
    const std::filesystem::path& path,
    const persist::CommandLog& log) {
    auto snapshot = log.capture_committed();
    RELEASE_CHECK(snapshot);
    persist::ReleaseLedgerOpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    auto opened = persist::ReleaseLedger::open_or_create(
        path, snapshot.value(), options);
    RELEASE_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] document::Node make_node(
    std::uint64_t id,
    std::int64_t value) {
    return document::Node{
        document::NodeId{id},
        "release.test.node",
        {{"value", value}},
    };
}

void schedule_put(
    command::DirectorScheduler& director,
    std::uint64_t frame) {
    auto scheduled = director.schedule_edit(
        command::PutNode{make_node(frame, static_cast<std::int64_t>(frame))},
        command::EraseNode{document::NodeId{frame}},
        command::UndoContextId{1U},
        command::TransactionId{frame},
        command::FrameNumber{frame});
    RELEASE_CHECK(scheduled);
}

void pending_release_blocks_resume_then_exact_release_reopens_it() {
    static_assert(
        !std::is_copy_constructible_v<persist::DurableReleaseReceipt>);
    static_assert(
        std::is_nothrow_move_constructible_v<persist::DurableReleaseReceipt>);

    TempStore store;
    auto log = open_log(store.wal());
    auto ledger = open_ledger(store.ledger(), log);
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log, ledger};
    RELEASE_CHECK(durable.release_gated());
    RELEASE_CHECK(durable.state() == persist::DurableDirectorState::Ready);
    RELEASE_CHECK(durable.next_release_ordinal() == 1U);
    RELEASE_CHECK(durable.capture_resume_source());

    schedule_put(director, 1U);
    auto frame = durable.seal_frame(command::FrameNumber{1U});
    RELEASE_CHECK(frame);
    RecordingRegistration registration;
    auto pending = durable.capture_resume_source(registration);
    RELEASE_CHECK(!pending);
    RELEASE_CHECK(
        pending.error().code
        == persist::DurableDirectorErrorCode::ReleasePending);
    RELEASE_CHECK(registration.calls == 0U);
    RELEASE_CHECK(durable.state() == persist::DurableDirectorState::Ready);

    command::NodeCommandBus local_node;
    command::LoopbackTransport transport{
        std::vector<command::NodeCommandBus*>{&local_node}};
    RELEASE_CHECK(!durable.broadcast_committed(frame.value(), transport));
    RELEASE_CHECK(transport.pending(0U) == 0U);

    auto released = durable.persist_release(frame.value(), 1U);
    RELEASE_CHECK(released);
    RELEASE_CHECK(released.value().durable());
    RELEASE_CHECK(released.value().ordinal() == 1U);
    RELEASE_CHECK(
        released.value().frame().before
        == persist::LogCursor{0U, 0U, 1U});
    RELEASE_CHECK(
        released.value().frame().after
        == frame.value().receipt().cursor);
    RELEASE_CHECK(durable.next_release_ordinal() == 2U);
    RELEASE_CHECK(
        durable.release_ordinal(
            frame.value().before_cursor(), frame.value().after_cursor())
        == 1U);
    auto wrong_before = frame.value().before_cursor();
    wrong_before.revision = document::Version{99U};
    RELEASE_CHECK(!durable.release_ordinal(
        wrong_before, frame.value().after_cursor()));

    auto source = durable.capture_resume_source(registration);
    RELEASE_CHECK(source);
    RELEASE_CHECK(registration.calls == 1U);
    RELEASE_CHECK(registration.last_tail == log.tail_cursor());
    RELEASE_CHECK(source.value().checkpoint().cursor == log.tail_cursor());
    RELEASE_CHECK(source.value().log().tail_cursor() == log.tail_cursor());
}

void foreign_out_of_order_and_wrong_ordinal_are_prewrite_rejections() {
    TempStore primary_store;
    auto log = open_log(primary_store.wal());
    auto ledger = open_ledger(primary_store.ledger(), log);
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log, ledger};
    schedule_put(director, 1U);
    auto first = durable.seal_frame(command::FrameNumber{1U});
    RELEASE_CHECK(first);
    auto second = durable.seal_frame(command::FrameNumber{2U});
    RELEASE_CHECK(second);

    TempStore foreign_store;
    auto foreign_log = open_log(foreign_store.wal());
    command::DirectorScheduler foreign_director;
    schedule_put(foreign_director, 1U);
    persist::DurableDirector foreign{foreign_director, foreign_log};
    auto foreign_frame = foreign.seal_frame(command::FrameNumber{1U});
    RELEASE_CHECK(foreign_frame);

    const auto initial_size = std::filesystem::file_size(primary_store.ledger());
    auto foreign_rejected = durable.persist_release(foreign_frame.value(), 1U);
    RELEASE_CHECK(!foreign_rejected);
    RELEASE_CHECK(
        foreign_rejected.error().code
        == persist::DurableDirectorErrorCode::ReleasePrewriteRejected);
    RELEASE_CHECK(
        std::filesystem::file_size(primary_store.ledger()) == initial_size);

    auto out_of_order = durable.persist_release(second.value(), 1U);
    RELEASE_CHECK(!out_of_order);
    RELEASE_CHECK(
        out_of_order.error().code
        == persist::DurableDirectorErrorCode::ReleasePrewriteRejected);
    RELEASE_CHECK(
        std::filesystem::file_size(primary_store.ledger()) == initial_size);

    auto wrong_ordinal = durable.persist_release(first.value(), 2U);
    RELEASE_CHECK(!wrong_ordinal);
    RELEASE_CHECK(
        wrong_ordinal.error().code
        == persist::DurableDirectorErrorCode::ReleasePrewriteRejected);
    RELEASE_CHECK(
        std::filesystem::file_size(primary_store.ledger()) == initial_size);
    RELEASE_CHECK(durable.state() == persist::DurableDirectorState::Ready);
    RELEASE_CHECK(ledger.recovery_report().decision_count == 0U);

    RELEASE_CHECK(durable.persist_release(first.value(), 1U));
    RELEASE_CHECK(durable.persist_release(second.value(), 2U));
    RELEASE_CHECK(ledger.recovery_report().decision_count == 2U);
    RELEASE_CHECK(durable.capture_resume_source());
}

void nonretryable_ledger_failure_latches_recovery() {
    TempStore store;
    auto log = open_log(store.wal());
    auto ledger = open_ledger(store.ledger(), log);
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log, ledger};
    schedule_put(director, 1U);
    auto frame = durable.seal_frame(command::FrameNumber{1U});
    RELEASE_CHECK(frame);

    std::error_code remove_error;
    const auto removed = std::filesystem::remove(store.ledger(), remove_error);
    RELEASE_CHECK(removed);
    RELEASE_CHECK(!remove_error);
    auto failed = durable.persist_release(frame.value(), 1U);
    RELEASE_CHECK(!failed);
    RELEASE_CHECK(
        failed.error().code
        == persist::DurableDirectorErrorCode::ReleasePersistenceFailure);
    RELEASE_CHECK(failed.error().release_ledger_error.has_value());
    RELEASE_CHECK(
        failed.error().release_ledger_error->code
        == persist::ReleaseLedgerErrorCode::ReadFailed);
    RELEASE_CHECK(
        durable.state() == persist::DurableDirectorState::RecoveryRequired);
    RELEASE_CHECK(durable.next_release_ordinal() == 0U);
    RELEASE_CHECK(!durable.release_ordinal(
        frame.value().before_cursor(), frame.value().after_cursor()));

    auto source = durable.capture_resume_source();
    RELEASE_CHECK(!source);
    RELEASE_CHECK(
        source.error().code
        == persist::DurableDirectorErrorCode::RecoveryRequired);
    auto retry = durable.persist_release(frame.value(), 1U);
    RELEASE_CHECK(!retry);
    RELEASE_CHECK(
        retry.error().code
        == persist::DurableDirectorErrorCode::RecoveryRequired);
}

void construction_with_unresolved_existing_suffix_fails_closed() {
    TempStore store;
    auto log = open_log(store.wal());
    auto ledger = open_ledger(store.ledger(), log);
    command::DirectorScheduler director;
    schedule_put(director, 1U);
    persist::DurableDirector author{director, log};
    RELEASE_CHECK(author.seal_frame(command::FrameNumber{1U}));

    persist::DurableDirector restarted{director, log, ledger};
    RELEASE_CHECK(
        restarted.state() == persist::DurableDirectorState::RecoveryRequired);
    RELEASE_CHECK(restarted.next_release_ordinal() == 0U);
    auto source = restarted.capture_resume_source();
    RELEASE_CHECK(!source);
    RELEASE_CHECK(
        source.error().code
        == persist::DurableDirectorErrorCode::RecoveryRequired);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"pending blocks resume; exact release reopens",
         &pending_release_blocks_resume_then_exact_release_reopens_it},
        {"foreign/out-of-order/ordinal reject prewrite",
         &foreign_out_of_order_and_wrong_ordinal_are_prewrite_rejections},
        {"nonretryable ledger failure latches",
         &nonretryable_ledger_failure_latches_recovery},
        {"unresolved startup suffix fails closed",
         &construction_with_unresolved_existing_suffix_fails_closed},
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
    std::cout << (tests.size() - failures) << '/' << tests.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
