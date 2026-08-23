#include <mediaserver/net/unresolved_cohort_recovery.hpp>

#include <mediaserver/persist/durable_director.hpp>
#include <mediaserver/persist/release_ledger_recovery_release_authority.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace net = mediaserver::net;
namespace persist = mediaserver::persist;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define RECOVERY_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempStore final {
public:
    TempStore() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const auto stem = std::string{"mediaserver-cohort-recovery-"}
            + std::to_string(timestamp) + "-"
            + std::to_string(sequence.fetch_add(1U));
        const auto temporary = std::filesystem::temp_directory_path();
        wal_ = temporary / (stem + ".wal");
        ledger_ = temporary / (stem + ".release");
        journal_ = temporary / (stem + ".cohort");
    }

    ~TempStore() {
        std::error_code ignored;
        std::filesystem::remove(wal_, ignored);
        std::filesystem::remove(ledger_, ignored);
        std::filesystem::remove(journal_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& wal() const noexcept {
        return wal_;
    }
    [[nodiscard]] const std::filesystem::path& ledger() const noexcept {
        return ledger_;
    }
    [[nodiscard]] const std::filesystem::path& journal() const noexcept {
        return journal_;
    }

private:
    std::filesystem::path wal_;
    std::filesystem::path ledger_;
    std::filesystem::path journal_;
};

template <typename Id>
[[nodiscard]] Id id(std::uint64_t value) noexcept {
    return Id{value, value + 0x1000U};
}

[[nodiscard]] cmd::StreamCursor genesis() noexcept {
    return cmd::StreamCursor{
        cmd::FrameNumber{0U},
        doc::Version{0U},
        cmd::CommandId{1U},
    };
}

[[nodiscard]] persist::LogCursor as_log_cursor(
    const cmd::StreamCursor& cursor) noexcept {
    return persist::LogCursor{
        cursor.through_frame.value(),
        cursor.revision.value(),
        cursor.next_command.value(),
    };
}

[[nodiscard]] cmd::StreamCursor as_stream_cursor(
    persist::LogCursor cursor) noexcept {
    return cmd::StreamCursor{
        cmd::FrameNumber{cursor.through_frame},
        doc::Version{cursor.revision},
        cmd::CommandId{cursor.next_command},
    };
}

[[nodiscard]] persist::RecoveryCohortMember member(
    std::uint64_t value) noexcept {
    return persist::RecoveryCohortMember{
        id<persist::RecoveryNodeId>(value),
        id<persist::RecoveryNodeIncarnation>(value + 100U),
    };
}

[[nodiscard]] persist::RecoveryCohort cohort(
    std::initializer_list<std::uint64_t> values) {
    persist::RecoveryCohort result;
    result.reserve(values.size());
    for (const auto value : values) {
        result.push_back(member(value));
    }
    return result;
}

[[nodiscard]] net::RecoverySessionToken session(
    std::uint64_t value) noexcept {
    return net::RecoverySessionToken{value, value + 0x2000U};
}

[[nodiscard]] persist::CommandLog open_log(
    const std::filesystem::path& path) {
    persist::OpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    auto opened = persist::CommandLog::open_or_create(path, genesis(), options);
    RECOVERY_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] persist::ReleaseLedger open_ledger(
    const std::filesystem::path& path,
    const persist::CommandLog& log) {
    auto snapshot = log.capture_committed();
    RECOVERY_CHECK(snapshot);
    persist::ReleaseLedgerOpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    auto opened = persist::ReleaseLedger::open_or_create(
        path, snapshot.value(), options);
    RECOVERY_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] persist::UnresolvedReleaseInventory inspect(
    const persist::CommandLogSnapshot& snapshot,
    const persist::ReleaseLedger& ledger) {
    auto inspected = persist::UnresolvedReleaseInventory::inspect(
        snapshot, ledger);
    RECOVERY_CHECK(inspected);
    return std::move(inspected).value();
}

struct Capture final {
    persist::CommandLogSnapshot wal;
    persist::UnresolvedReleaseInventory inventory;
};

struct Fixture final {
    TempStore paths;
    persist::CommandLog log;
    persist::ReleaseLedger ledger;
    persist::CohortJournal journal;
    cmd::DirectorScheduler director;
    persist::DurableDirector durable;
    persist::ReleaseLedgerRecoveryReleaseAuthority release_authority;

    Fixture()
        : log(open_log(paths.wal())),
          ledger(open_empty_ledger()),
          journal(open_empty_journal()),
          durable(director, log),
          release_authority(ledger) {}

    [[nodiscard]] persist::WalFrameIdentity plan_frame(
        persist::RecoveryCohort members,
        bool add_document_node = false,
        bool corrupt_bound_crc = false) {
        const auto before = log.tail_cursor();
        RECOVERY_CHECK(before == director.checkpoint().cursor);
        const auto next_frame = before.through_frame.value() + 1U;
        persist::BeginFrame begin;
        begin.plan_token = id<persist::RecoveryPlanToken>(100U + next_frame);
        begin.expected_before = as_log_cursor(before);
        begin.next_frame = next_frame;
        begin.cut_director_epoch =
            id<persist::RecoveryDirectorEpoch>(200U + next_frame);
        begin.plan_generation = 1U;
        begin.cohort = std::move(members);
        begin.empty_confirmation = begin.cohort.empty()
            ? persist::EmptyCohortConfirmation::ExplicitlyEmpty
            : persist::EmptyCohortConfirmation::None;
        begin.plan_fingerprint = persist::recovery_plan_fingerprint(
            begin.plan_generation,
            begin.plan_token,
            begin.cohort,
            begin.empty_confirmation);
        const auto begin_ordinal = journal.next_ordinal();
        auto began = journal.append(begin_ordinal, begin);
        RECOVERY_CHECK(began && began.value().durable());

        if (add_document_node) {
            auto scheduled = director.schedule_edit(
                cmd::PutNode{doc::Node{
                    doc::NodeId{next_frame},
                    "recovery.test",
                    {{"frame", static_cast<std::int64_t>(next_frame)}},
                }},
                cmd::EraseNode{doc::NodeId{next_frame}},
                cmd::UndoContextId{1U},
                cmd::TransactionId{next_frame},
                cmd::FrameNumber{next_frame});
            RECOVERY_CHECK(scheduled);
        }
        auto sealed = durable.seal_frame(cmd::FrameNumber{next_frame});
        RECOVERY_CHECK(sealed);
        const auto& batch = sealed.value();
        const auto identity = persist::WalFrameIdentity{
            as_log_cursor(batch.before_cursor()),
            batch.receipt().cursor,
            batch.receipt().offset,
            batch.receipt().bytes_written,
            batch.receipt().entry_crc32c,
        };
        auto bound_identity = identity;
        if (corrupt_bound_crc) {
            bound_identity.entry_crc32c ^= 0x1U;
        }
        const persist::BindFrame bind{
            begin.plan_token,
            begin_ordinal,
            bound_identity,
            begin.plan_generation,
            begin.plan_token,
            begin.plan_fingerprint,
        };
        auto bound = journal.append(journal.next_ordinal(), bind);
        RECOVERY_CHECK(bound && bound.value().durable());
        return identity;
    }

    [[nodiscard]] persist::WalFrameIdentity append_unplanned_frame() {
        const auto before = log.tail_cursor();
        const auto next_frame = before.through_frame.value() + 1U;
        auto sealed = durable.seal_frame(cmd::FrameNumber{next_frame});
        RECOVERY_CHECK(sealed);
        return persist::WalFrameIdentity{
            as_log_cursor(sealed.value().before_cursor()),
            sealed.value().receipt().cursor,
            sealed.value().receipt().offset,
            sealed.value().receipt().bytes_written,
            sealed.value().receipt().entry_crc32c,
        };
    }

    [[nodiscard]] Capture capture() const {
        auto snapshot = log.capture_committed();
        RECOVERY_CHECK(snapshot);
        auto inventory = inspect(snapshot.value(), ledger);
        return Capture{std::move(snapshot).value(), std::move(inventory)};
    }

    void reopen_journal(
        const persist::UnresolvedReleaseInventory& inventory) {
        persist::CohortJournalOpenOptions options;
        options.sync_policy = persist::SyncPolicy::Data;
        auto reopened = persist::CohortJournal::open_or_create(
            paths.journal(),
            id<persist::CohortJournalUuid>(1U),
            id<persist::RecoveryShowId>(2U),
            inventory,
            options);
        RECOVERY_CHECK(reopened);
        journal = std::move(reopened).value();
    }

private:
    [[nodiscard]] persist::ReleaseLedger open_empty_ledger() {
        return open_ledger(paths.ledger(), log);
    }

    [[nodiscard]] persist::CohortJournal open_empty_journal() {
        auto snapshot = log.capture_committed();
        RECOVERY_CHECK(snapshot);
        const auto inventory = inspect(snapshot.value(), ledger);
        persist::CohortJournalOpenOptions options;
        options.sync_policy = persist::SyncPolicy::Data;
        auto opened = persist::CohortJournal::open_or_create(
            paths.journal(),
            id<persist::CohortJournalUuid>(1U),
            id<persist::RecoveryShowId>(2U),
            inventory,
            options);
        RECOVERY_CHECK(opened);
        return std::move(opened).value();
    }
};

[[nodiscard]] net::RecoveryAttemptRequest attempt(
    std::uint64_t value) noexcept {
    return net::RecoveryAttemptRequest{
        id<persist::RecoveryAttemptToken>(value),
        id<persist::RecoveryDirectorEpoch>(value + 100U),
        id<persist::RecoverySessionNamespace>(value + 200U),
    };
}

[[nodiscard]] net::RecoveryCheckpointAction take_checkpoint(
    net::UnresolvedCohortRecovery& recovery) {
    auto next = recovery.take_next_action();
    RECOVERY_CHECK(next && next.value());
    const auto* checkpoint =
        std::get_if<net::RecoveryCheckpointAction>(&*next.value());
    RECOVERY_CHECK(checkpoint != nullptr);
    return *checkpoint;
}

void complete_checkpoint(
    net::UnresolvedCohortRecovery& recovery,
    const net::RecoveryCheckpointAction& action) {
    auto completed = recovery.complete_checkpoint(
        net::RecoveryCheckpointCompletion{
            action.action,
            action.session,
            action.checkpoint->cursor,
            true,
        });
    RECOVERY_CHECK(completed);
}

[[nodiscard]] net::RecoveryPrepareAction take_prepare(
    net::UnresolvedCohortRecovery& recovery) {
    auto next = recovery.take_next_action();
    RECOVERY_CHECK(next && next.value());
    const auto* prepare =
        std::get_if<net::RecoveryPrepareAction>(&*next.value());
    RECOVERY_CHECK(prepare != nullptr);
    return *prepare;
}

void complete_prepare(
    net::UnresolvedCohortRecovery& recovery,
    const net::RecoveryPrepareAction& action,
    net::RecoveryFrameAcknowledgementKind kind =
        net::RecoveryFrameAcknowledgementKind::Prepared) {
    auto completed = recovery.complete_prepare(net::RecoveryPrepareCompletion{
        action.action,
        action.session,
        kind,
        action.frame->identity,
        as_stream_cursor(action.frame->identity.before),
        as_stream_cursor(action.frame->identity.after),
    });
    RECOVERY_CHECK(completed);
}

void drive_one_member_to_quorum(
    net::UnresolvedCohortRecovery& recovery,
    const persist::RecoveryCohortMember& planned_member,
    std::uint64_t attempt_value) {
    auto started = recovery.start_attempt(attempt(attempt_value));
    RECOVERY_CHECK(started && started.value().durable());
    RECOVERY_CHECK(recovery.admit(planned_member, session(attempt_value)));
    const auto checkpoint_action = take_checkpoint(recovery);
    complete_checkpoint(recovery, checkpoint_action);
    const auto prepare_action = take_prepare(recovery);
    complete_prepare(recovery, prepare_action);
    RECOVERY_CHECK(
        recovery.state() == net::UnresolvedCohortRecoveryState::PreparedQuorum);
}

void incomplete_or_identity_divergent_journal_fails_before_attempt() {
    {
        Fixture fixture;
        static_cast<void>(fixture.plan_frame(cohort({10U})));
        static_cast<void>(fixture.append_unplanned_frame());
        auto capture = fixture.capture();
        auto created = net::UnresolvedCohortRecovery::create(
            std::move(capture.wal),
            capture.inventory,
            fixture.journal,
            fixture.release_authority);
        RECOVERY_CHECK(!created);
        RECOVERY_CHECK(created.error().code
            == net::UnresolvedCohortRecoveryErrorCode::JournalMismatch);
        RECOVERY_CHECK(fixture.journal.records().size() == 2U);
    }
    {
        Fixture fixture;
        static_cast<void>(fixture.plan_frame(cohort({10U}), false, true));
        auto capture = fixture.capture();
        auto created = net::UnresolvedCohortRecovery::create(
            std::move(capture.wal),
            capture.inventory,
            fixture.journal,
            fixture.release_authority);
        RECOVERY_CHECK(!created);
        RECOVERY_CHECK(created.error().code
            == net::UnresolvedCohortRecoveryErrorCode::MissingPlan);
    }
}

void checkpoint_is_exact_release_frontier_not_wal_tail() {
    Fixture fixture;
    const auto first = fixture.plan_frame(cohort({10U}), true);
    auto first_capture = fixture.capture();
    auto first_release = fixture.release_authority.persist_exact(
        first_capture.wal,
        first_capture.inventory,
        first,
        first_capture.inventory.next_release_ordinal);
    RECOVERY_CHECK(first_release);
    const auto second = fixture.plan_frame(cohort({10U}), true);
    auto capture = fixture.capture();
    fixture.reopen_journal(capture.inventory);
    auto created = net::UnresolvedCohortRecovery::create(
        std::move(capture.wal),
        capture.inventory,
        fixture.journal,
        fixture.release_authority);
    RECOVERY_CHECK(created);
    auto recovery = std::move(created).value();
    RECOVERY_CHECK(recovery.checkpoint().cursor == as_stream_cursor(first.after));
    RECOVERY_CHECK(recovery.checkpoint().cursor
        != as_stream_cursor(second.after));
    RECOVERY_CHECK(recovery.checkpoint().document.contains(doc::NodeId{1U}));
    RECOVERY_CHECK(!recovery.checkpoint().document.contains(doc::NodeId{2U}));
    RECOVERY_CHECK(recovery.frame().identity == second);
}

void attempt_precedes_actions_and_exact_cohort_prepares_concurrently() {
    Fixture fixture;
    const auto members = cohort({10U, 30U});
    const auto frame = fixture.plan_frame(members);
    auto capture = fixture.capture();
    auto created = net::UnresolvedCohortRecovery::create(
        std::move(capture.wal),
        capture.inventory,
        fixture.journal,
        fixture.release_authority);
    RECOVERY_CHECK(created);
    auto recovery = std::move(created).value();
    RECOVERY_CHECK(recovery.state() == net::UnresolvedCohortRecoveryState::Ready);
    auto no_attempt = recovery.take_next_action();
    RECOVERY_CHECK(!no_attempt);

    const auto records_before = fixture.journal.records().size();
    const auto durable_attempt = attempt(500U);
    auto started = recovery.start_attempt(durable_attempt);
    RECOVERY_CHECK(started && started.value().durable());
    RECOVERY_CHECK(fixture.journal.records().size() == records_before + 1U);
    RECOVERY_CHECK(std::holds_alternative<persist::AttemptStarted>(
        fixture.journal.records().back().mutation));
    auto incomplete = recovery.take_next_action();
    RECOVERY_CHECK(incomplete && !incomplete.value());
    auto wrong = member(10U);
    wrong.incarnation = id<persist::RecoveryNodeIncarnation>(999U);
    auto wrong_admission = recovery.admit(wrong, session(1U));
    RECOVERY_CHECK(!wrong_admission);
    RECOVERY_CHECK(recovery.admit(members[0], session(1U)));
    RECOVERY_CHECK(recovery.admit(members[1], session(2U)));

    const auto checkpoint_one = take_checkpoint(recovery);
    const auto checkpoint_two = take_checkpoint(recovery);
    RECOVERY_CHECK(checkpoint_one.attempt.attempt_token
        == durable_attempt.attempt_token);
    RECOVERY_CHECK(checkpoint_one.attempt.director_epoch
        == durable_attempt.director_epoch);
    RECOVERY_CHECK(checkpoint_one.attempt.session_namespace
        == durable_attempt.session_namespace);
    RECOVERY_CHECK(checkpoint_two.attempt.attempt_token
        == durable_attempt.attempt_token);
    RECOVERY_CHECK(checkpoint_one.action != checkpoint_two.action);
    auto all_inflight = recovery.take_next_action();
    RECOVERY_CHECK(all_inflight && !all_inflight.value());
    complete_checkpoint(recovery, checkpoint_two);
    complete_checkpoint(recovery, checkpoint_one);
    const auto prepare_one = take_prepare(recovery);
    const auto prepare_two = take_prepare(recovery);
    RECOVERY_CHECK(prepare_one.attempt.attempt_token
        == durable_attempt.attempt_token);
    RECOVERY_CHECK(prepare_one.attempt.director_epoch
        == durable_attempt.director_epoch);
    RECOVERY_CHECK(prepare_one.attempt.session_namespace
        == durable_attempt.session_namespace);
    RECOVERY_CHECK(prepare_one.frame->identity == frame);
    RECOVERY_CHECK(prepare_two.frame->identity == frame);
    complete_prepare(recovery, prepare_two);
    complete_prepare(recovery, prepare_one);
    const auto report = recovery.report();
    RECOVERY_CHECK(report.prepared == 2U);
    RECOVERY_CHECK(report.checkpointed == 2U);
    RECOVERY_CHECK(report.state
        == net::UnresolvedCohortRecoveryState::PreparedQuorum);

    auto released = recovery.persist_release();
    RECOVERY_CHECK(released && released.value().frame() == frame);
    RECOVERY_CHECK(recovery.state()
        == net::UnresolvedCohortRecoveryState::RestartRequired);
    RECOVERY_CHECK(fixture.ledger.frontier() == frame.after);
    auto forbidden = recovery.take_next_action();
    RECOVERY_CHECK(!forbidden);
}

void stale_actions_are_fenced_and_committed_is_not_prepared() {
    Fixture fixture;
    const auto planned = member(10U);
    static_cast<void>(fixture.plan_frame(persist::RecoveryCohort{planned}));
    auto capture = fixture.capture();
    auto created = net::UnresolvedCohortRecovery::create(
        std::move(capture.wal),
        capture.inventory,
        fixture.journal,
        fixture.release_authority);
    RECOVERY_CHECK(created);
    auto recovery = std::move(created).value();

    RECOVERY_CHECK(recovery.start_attempt(attempt(600U)));
    RECOVERY_CHECK(recovery.admit(planned, session(1U)));
    const auto stale_checkpoint = take_checkpoint(recovery);
    RECOVERY_CHECK(recovery.fail_attempt());
    RECOVERY_CHECK(recovery.start_attempt(attempt(601U)));
    RECOVERY_CHECK(recovery.admit(planned, session(1U)));
    const auto current_checkpoint = take_checkpoint(recovery);
    RECOVERY_CHECK(stale_checkpoint.action != current_checkpoint.action);
    auto stale = recovery.complete_checkpoint(net::RecoveryCheckpointCompletion{
        stale_checkpoint.action,
        stale_checkpoint.session,
        stale_checkpoint.checkpoint->cursor,
        true,
    });
    RECOVERY_CHECK(!stale);
    RECOVERY_CHECK(stale.error().code
        == net::UnresolvedCohortRecoveryErrorCode::ActionMismatch);
    RECOVERY_CHECK(recovery.state()
        == net::UnresolvedCohortRecoveryState::Checkpointing);
    complete_checkpoint(recovery, current_checkpoint);
    const auto prepare = take_prepare(recovery);
    auto committed = recovery.complete_prepare(net::RecoveryPrepareCompletion{
        prepare.action,
        prepare.session,
        net::RecoveryFrameAcknowledgementKind::Committed,
        prepare.frame->identity,
        as_stream_cursor(prepare.frame->identity.before),
        as_stream_cursor(prepare.frame->identity.after),
    });
    RECOVERY_CHECK(!committed);
    RECOVERY_CHECK(committed.error().code
        == net::UnresolvedCohortRecoveryErrorCode::AcknowledgementMismatch);
    RECOVERY_CHECK(recovery.state() == net::UnresolvedCohortRecoveryState::Ready);
    auto release = recovery.persist_release();
    RECOVERY_CHECK(!release);
}

void durable_reconfiguration_is_the_only_membership_change() {
    Fixture fixture;
    const auto old_member = member(10U);
    const auto new_member = member(30U);
    static_cast<void>(fixture.plan_frame(persist::RecoveryCohort{old_member}));
    auto capture = fixture.capture();
    auto created = net::UnresolvedCohortRecovery::create(
        std::move(capture.wal),
        capture.inventory,
        fixture.journal,
        fixture.release_authority);
    RECOVERY_CHECK(created);
    auto recovery = std::move(created).value();
    const auto old_plan = recovery.active_plan();
    persist::ReconfigureFrame mutation;
    mutation.frame = old_plan.frame;
    mutation.expected_plan_generation = old_plan.generation;
    mutation.expected_plan_token = old_plan.token;
    mutation.expected_plan_fingerprint = old_plan.fingerprint;
    mutation.expected_plan_journal_ordinal = old_plan.journal_ordinal;
    mutation.expected_release_frontier = recovery.inventory().released_frontier;
    mutation.expected_wal_tail = recovery.inventory().wal_tail;
    mutation.new_plan_generation = old_plan.generation + 1U;
    mutation.new_plan_token = id<persist::RecoveryPlanToken>(900U);
    mutation.operator_request_id =
        id<persist::RecoveryOperatorRequestId>(901U);
    mutation.reason = persist::ReconfigureFrameReason::NodeUnavailable;
    mutation.cohort = persist::RecoveryCohort{new_member};
    mutation.new_plan_fingerprint = persist::recovery_plan_fingerprint(
        mutation.new_plan_generation,
        mutation.new_plan_token,
        mutation.cohort,
        mutation.empty_confirmation);
    auto changed = recovery.reconfigure(mutation);
    RECOVERY_CHECK(changed && changed.value().durable());
    RECOVERY_CHECK(recovery.active_plan().cohort
        == persist::RecoveryCohort{new_member});
    auto replayed = recovery.reconfigure(mutation);
    RECOVERY_CHECK(replayed && replayed.value().replayed());

    RECOVERY_CHECK(recovery.start_attempt(attempt(700U)));
    auto old_rejected = recovery.admit(old_member, session(1U));
    RECOVERY_CHECK(!old_rejected);
    RECOVERY_CHECK(recovery.admit(new_member, session(2U)));
}

void explicit_empty_cohort_is_quorum_only_after_durable_attempt() {
    Fixture fixture;
    const auto frame = fixture.plan_frame({});
    auto capture = fixture.capture();
    auto created = net::UnresolvedCohortRecovery::create(
        std::move(capture.wal),
        capture.inventory,
        fixture.journal,
        fixture.release_authority);
    RECOVERY_CHECK(created);
    auto recovery = std::move(created).value();
    RECOVERY_CHECK(recovery.active_plan().empty_confirmation
        == persist::EmptyCohortConfirmation::ExplicitlyEmpty);
    RECOVERY_CHECK(recovery.state() == net::UnresolvedCohortRecoveryState::Ready);
    auto before_attempt = recovery.persist_release();
    RECOVERY_CHECK(!before_attempt);
    const auto ordinal = fixture.journal.next_ordinal();
    auto started = recovery.start_attempt(attempt(800U));
    RECOVERY_CHECK(started && started.value().ordinal() == ordinal);
    RECOVERY_CHECK(recovery.state()
        == net::UnresolvedCohortRecoveryState::PreparedQuorum);
    auto released = recovery.persist_release();
    RECOVERY_CHECK(released && released.value().frame() == frame);
}

class CapturingFailureAuthority final : public persist::RecoveryReleaseAuthority {
public:
    [[nodiscard]] core::Result<
        persist::DurableReleaseReceipt,
        persist::RecoveryReleaseFailure>
    persist_exact(
        const persist::CommandLogSnapshot& wal,
        const persist::UnresolvedReleaseInventory& inventory,
        persist::WalFrameIdentity frame,
        std::uint64_t ordinal) noexcept override {
        called = true;
        tail = as_log_cursor(wal.tail_cursor());
        captured_frontier = inventory.released_frontier;
        captured_frame = frame;
        captured_ordinal = ordinal;
        return core::Result<
            persist::DurableReleaseReceipt,
            persist::RecoveryReleaseFailure>::failure(
            persist::RecoveryReleaseFailure{
                persist::RecoveryReleaseFailureCode::LedgerFailure});
    }

    [[nodiscard]] bool faulted() const noexcept override { return false; }

    bool called{};
    persist::LogCursor tail{};
    persist::LogCursor captured_frontier{};
    persist::WalFrameIdentity captured_frame{};
    std::uint64_t captured_ordinal{};
};

void release_failure_receives_exact_request_and_faults_coordinator() {
    Fixture fixture;
    const auto planned = member(10U);
    const auto frame = fixture.plan_frame(persist::RecoveryCohort{planned});
    auto capture = fixture.capture();
    const auto expected_tail = capture.inventory.wal_tail;
    const auto expected_frontier = capture.inventory.released_frontier;
    const auto expected_ordinal = capture.inventory.next_release_ordinal;
    CapturingFailureAuthority authority;
    auto created = net::UnresolvedCohortRecovery::create(
        std::move(capture.wal),
        capture.inventory,
        fixture.journal,
        authority);
    RECOVERY_CHECK(created);
    auto recovery = std::move(created).value();
    drive_one_member_to_quorum(recovery, planned, 900U);
    auto failed = recovery.persist_release();
    RECOVERY_CHECK(!failed);
    RECOVERY_CHECK(authority.called);
    RECOVERY_CHECK(authority.tail == expected_tail);
    RECOVERY_CHECK(authority.captured_frontier == expected_frontier);
    RECOVERY_CHECK(authority.captured_frame == frame);
    RECOVERY_CHECK(authority.captured_ordinal == expected_ordinal);
    RECOVERY_CHECK(recovery.state() == net::UnresolvedCohortRecoveryState::Faulted);
}

void crash_before_release_reopens_with_a_higher_durable_attempt() {
    Fixture fixture;
    const auto planned = member(10U);
    const auto frame = fixture.plan_frame(persist::RecoveryCohort{planned});
    {
        auto capture = fixture.capture();
        auto created = net::UnresolvedCohortRecovery::create(
            std::move(capture.wal),
            capture.inventory,
            fixture.journal,
            fixture.release_authority);
        RECOVERY_CHECK(created);
        auto recovery = std::move(created).value();
        drive_one_member_to_quorum(recovery, planned, 950U);
        RECOVERY_CHECK(recovery.active_plan().last_attempt_generation == 1U);
        // Simulated process loss: Prepared evidence is intentionally not
        // durable and no release decision has been written.
    }

    auto capture = fixture.capture();
    RECOVERY_CHECK(capture.inventory.released_frontier == frame.before);
    fixture.reopen_journal(capture.inventory);
    auto reopened = net::UnresolvedCohortRecovery::create(
        std::move(capture.wal),
        capture.inventory,
        fixture.journal,
        fixture.release_authority);
    RECOVERY_CHECK(reopened);
    auto recovery = std::move(reopened).value();
    RECOVERY_CHECK(recovery.active_plan().last_attempt_generation == 1U);
    auto second_attempt = recovery.start_attempt(attempt(951U));
    RECOVERY_CHECK(second_attempt && second_attempt.value().durable());
    const auto* durable = std::get_if<persist::AttemptStarted>(
        &fixture.journal.records().back().mutation);
    RECOVERY_CHECK(durable != nullptr);
    RECOVERY_CHECK(durable->attempt_generation == 2U);
    RECOVERY_CHECK(recovery.admit(planned, session(951U)));
    const auto checkpoint_action = take_checkpoint(recovery);
    RECOVERY_CHECK(checkpoint_action.attempt.attempt_token
        == durable->attempt_token);
    complete_checkpoint(recovery, checkpoint_action);
    const auto prepare_action = take_prepare(recovery);
    complete_prepare(recovery, prepare_action);
    RECOVERY_CHECK(recovery.persist_release());
}

void multi_frame_suffix_requires_repeated_clean_recovery_starts() {
    Fixture fixture;
    const auto first = fixture.plan_frame({});
    const auto second = fixture.plan_frame({});
    {
        auto capture = fixture.capture();
        auto created = net::UnresolvedCohortRecovery::create(
            std::move(capture.wal),
            capture.inventory,
            fixture.journal,
            fixture.release_authority);
        RECOVERY_CHECK(created);
        auto recovery = std::move(created).value();
        RECOVERY_CHECK(recovery.frame().identity == first);
        RECOVERY_CHECK(recovery.start_attempt(attempt(1'000U)));
        RECOVERY_CHECK(recovery.persist_release());
        RECOVERY_CHECK(recovery.state()
            == net::UnresolvedCohortRecoveryState::RestartRequired);
    }
    {
        auto capture = fixture.capture();
        fixture.reopen_journal(capture.inventory);
        auto created = net::UnresolvedCohortRecovery::create(
            std::move(capture.wal),
            capture.inventory,
            fixture.journal,
            fixture.release_authority);
        RECOVERY_CHECK(created);
        auto recovery = std::move(created).value();
        RECOVERY_CHECK(recovery.frame().identity == second);
        RECOVERY_CHECK(recovery.checkpoint().cursor
            == as_stream_cursor(first.after));
        RECOVERY_CHECK(recovery.start_attempt(attempt(1'001U)));
        RECOVERY_CHECK(recovery.persist_release());
    }
    auto complete = fixture.capture();
    RECOVERY_CHECK(complete.inventory.unresolved_frame_count == 0U);
    fixture.reopen_journal(complete.inventory);
    auto no_recovery = net::UnresolvedCohortRecovery::create(
        std::move(complete.wal),
        complete.inventory,
        fixture.journal,
        fixture.release_authority);
    RECOVERY_CHECK(!no_recovery);
    RECOVERY_CHECK(no_recovery.error().code
        == net::UnresolvedCohortRecoveryErrorCode::NoUnresolvedSuffix);
}

static_assert(!std::is_copy_constructible_v<net::UnresolvedCohortRecovery>);
static_assert(std::is_move_constructible_v<net::UnresolvedCohortRecovery>);

template <typename Coordinator>
concept HasCommitPermitSurface = requires(Coordinator& coordinator) {
    coordinator.send_commit_permit();
};

static_assert(!HasCommitPermitSurface<net::UnresolvedCohortRecovery>);

} // namespace

int main() {
    struct Test final {
        const char* name;
        void (*run)();
    };
    const std::array tests{
        Test{"journal coverage and identity are exact",
             &incomplete_or_identity_divergent_journal_fails_before_attempt},
        Test{"checkpoint stops at durable release frontier",
             &checkpoint_is_exact_release_frontier_not_wal_tail},
        Test{"attempt, admission, and concurrent prepared quorum",
             &attempt_precedes_actions_and_exact_cohort_prepares_concurrently},
        Test{"stale actions and Committed are fenced",
             &stale_actions_are_fenced_and_committed_is_not_prepared},
        Test{"durable reconfiguration controls membership",
             &durable_reconfiguration_is_the_only_membership_change},
        Test{"explicit empty requires durable attempt",
             &explicit_empty_cohort_is_quorum_only_after_durable_attempt},
        Test{"release failure receives exact request",
             &release_failure_receives_exact_request_and_faults_coordinator},
        Test{"crash before release starts a higher attempt",
             &crash_before_release_reopens_with_a_higher_durable_attempt},
        Test{"multi-frame suffix restarts per release",
             &multi_frame_suffix_requires_repeated_clean_recovery_starts},
    };
    std::size_t failures = 0U;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": "
                      << exception.what() << '\n';
        }
    }
    return failures == 0U ? 0 : 1;
}
