#include <mediaserver/net/director_session_service.hpp>

#include <mediaserver/net/heartbeat_schedule.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/socket_base.hpp>

namespace mediaserver::net {
namespace {

using ServiceStartResult = core::Result<
    asio::ip::tcp::endpoint,
    DirectorSessionServiceError>;

enum class WorkerPhase : std::uint8_t {
    Idle,
    Resuming,
    Registering,
    Live,
    Draining,
    Cleaning,
    Done,
};

enum class WorkerTermination : std::uint8_t {
    None,
    ServiceStopped,
    PeerGoodbye,
    HeartbeatTimeout,
    HubClosed,
    ResyncRequired,
    SessionFailure,
    ObserverRejected,
};

[[nodiscard]] DirectorSessionLeaveReason leave_reason(
    WorkerTermination reason) noexcept {
    switch (reason) {
    case WorkerTermination::ServiceStopped:
        return DirectorSessionLeaveReason::ServiceStopped;
    case WorkerTermination::PeerGoodbye:
        return DirectorSessionLeaveReason::PeerGoodbye;
    case WorkerTermination::HubClosed:
        return DirectorSessionLeaveReason::HubClosed;
    case WorkerTermination::ResyncRequired:
        return DirectorSessionLeaveReason::ResyncRequired;
    case WorkerTermination::ObserverRejected:
        return DirectorSessionLeaveReason::ObserverRejected;
    case WorkerTermination::None:
    case WorkerTermination::HeartbeatTimeout:
    case WorkerTermination::SessionFailure:
        return DirectorSessionLeaveReason::SessionFailure;
    }
    return DirectorSessionLeaveReason::SessionFailure;
}

[[nodiscard]] bool same_cursor(
    const cmd::StreamCursor& left,
    const cmd::StreamCursor& right) noexcept {
    return left.through_frame == right.through_frame && left.revision == right.revision
        && left.next_command == right.next_command;
}

[[nodiscard]] bool fits_size_t(core::u64 value) noexcept {
    if constexpr (sizeof(std::size_t) >= sizeof(core::u64)) {
        return true;
    } else {
        return value <= static_cast<core::u64>(
                            std::numeric_limits<std::size_t>::max());
    }
}

[[nodiscard]] bool valid_session_defaults(
    const DirectorSessionConfig& config) noexcept {
    const auto& limits = config.limits;
    const auto& codec = config.codec_limits;
    const auto& wire = config.wire_limits;
    const auto zero = std::chrono::milliseconds::zero();
    return config.show_id && config.director_epoch && !config.session_id
        && limits.max_packet_bytes != 0U && limits.max_frame_bytes != 0U
        && limits.max_checkpoint_bytes != 0U && limits.max_inflight_frames != 0U
        && limits.max_inflight_bytes != 0U
        && limits.max_packet_bytes <= limits.max_frame_bytes
        && limits.max_frame_bytes <= limits.max_inflight_bytes
        && limits.max_checkpoint_bytes <= limits.max_inflight_bytes
        && fits_size_t(limits.max_packet_bytes)
        && fits_size_t(limits.max_frame_bytes)
        && fits_size_t(limits.max_checkpoint_bytes)
        && limits.max_packet_bytes <= codec.max_packet_bytes
        && limits.max_frame_bytes <= codec.max_aggregate_packet_bytes
        && limits.max_checkpoint_bytes <= codec.max_checkpoint_bytes
        && limits.max_packet_bytes <= wire.max_message_bytes
        && limits.max_checkpoint_bytes <= wire.max_message_bytes
        && config.io.handshake_timeout >= zero && config.io.read_timeout >= zero
        && config.io.write_timeout > zero && !config.io.admission_budget;
}

[[nodiscard]] bool valid_config(
    const DirectorSessionServiceConfig& config) noexcept {
    const auto minimum_heartbeat = std::chrono::milliseconds{
        min_heartbeat_response_timeout_ms};
    const auto maximum_heartbeat = std::chrono::milliseconds{
        max_heartbeat_response_timeout_ms};
    const auto read_timeout = config.session_defaults.io.read_timeout;
    const auto interval_count = config.heartbeat_interval.count();
    const auto response_count = config.heartbeat_response_timeout.count();
    const auto can_add_heartbeat_window = interval_count >= 0
        && response_count >= 0
        && interval_count
            <= std::numeric_limits<std::chrono::milliseconds::rep>::max()
                - response_count;
    const auto heartbeat_window_fits_read =
        read_timeout == std::chrono::milliseconds::zero()
        || (can_add_heartbeat_window
            && read_timeout
                >= std::chrono::milliseconds{
                    interval_count + response_count});
    return config.max_workers != 0U && config.session_id_namespace != 0U
        && config.first_session_sequence != 0U
        && config.max_receive_bytes
            >= config.session_defaults.codec_limits.max_message_bytes
        && config.heartbeat_interval > std::chrono::milliseconds::zero()
        && config.heartbeat_response_timeout >= minimum_heartbeat
        && config.heartbeat_response_timeout <= maximum_heartbeat
        && config.heartbeat_interval < config.heartbeat_response_timeout
        && heartbeat_window_fits_read
        && valid_session_defaults(config.session_defaults);
}

[[nodiscard]] DirectorSessionServiceError service_error(
    DirectorSessionServiceErrorCode code,
    std::error_code system_error = {}) noexcept {
    return DirectorSessionServiceError{code, system_error};
}

[[nodiscard]] bool inflight_window_full(
    const ResumeSessionError& error) noexcept {
    const auto* protocol = std::get_if<SessionProtocolFailure>(&error);
    return protocol != nullptr
        && protocol->code == SessionProtocolErrorCode::InflightWindowFull;
}

[[nodiscard]] WorkerTermination subscription_termination(
    DirectorSubscriptionState state) noexcept {
    return state == DirectorSubscriptionState::ResyncRequired
        ? WorkerTermination::ResyncRequired
        : WorkerTermination::HubClosed;
}

void close_socket(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

} // namespace

namespace detail {

class DirectorSessionServiceState final {
public:
    struct AtomicCounters final {
        std::atomic_uint64_t accepted_connections{};
        std::atomic_uint64_t capacity_rejections{};
        std::atomic_uint64_t session_id_exhaustions{};
        std::atomic_uint64_t worker_start_failures{};
        std::atomic_uint64_t subscription_failures{};
        std::atomic_uint64_t resume_failures{};
        std::atomic_uint64_t live_registrations{};
        std::atomic_uint64_t observer_rejections{};
        std::atomic_uint64_t validated_acknowledgements{};
        std::atomic_uint64_t validated_heartbeat_acknowledgements{};
        std::atomic_uint64_t sent_heartbeats{};
        std::atomic_uint64_t heartbeat_timeouts{};
        std::atomic_uint64_t graceful_goodbyes{};
        std::atomic_uint64_t resync_disconnects{};
        std::atomic_uint64_t hub_closed_disconnects{};
        std::atomic_uint64_t session_failures{};
    };

    struct Worker final {
        struct PendingHeartbeatAcknowledgement final {
            HeartbeatAcknowledgementReport report;
            std::chrono::steady_clock::time_point received_at;
            bool schedule_accounted{};
            bool observer_complete{};
            bool observer_accepted{};
        };

        std::atomic_bool active{};
        std::atomic<WorkerPhase> phase{WorkerPhase::Idle};
        std::atomic<WorkerTermination> termination{WorkerTermination::None};
        std::mutex resources_mutex;
        std::shared_ptr<DirectorResumeSession> session;
        std::shared_ptr<DirectorLiveSubscription> live;
        std::mutex progress_mutex;
        std::condition_variable progress_changed;
        std::uint64_t progress_generation{};
        std::optional<PendingHeartbeatAcknowledgement> heartbeat_acknowledgement;
        std::mutex observer_mutex;
        bool observer_registered{};
        std::thread thread;
    };

    DirectorSessionServiceState(
        DirectorSessionServiceConfig config_value,
        DirectorSessionHub& hub_value,
        const persist::DurableDirector& authority_value,
        NodeSessionLeaseAuthority& leases_value,
        DirectorSessionObserver* observer_value)
        : config(std::move(config_value)),
          hub(&hub_value),
          authority(&authority_value),
          leases(&leases_value),
          observer(observer_value),
          next_session_sequence(config.first_session_sequence) {}

    void set_termination(
        Worker& worker,
        WorkerTermination reason) noexcept {
        auto expected = WorkerTermination::None;
        static_cast<void>(worker.termination.compare_exchange_strong(
            expected,
            reason,
            std::memory_order_acq_rel,
            std::memory_order_acquire));
        worker.progress_changed.notify_all();
    }

    void count_termination(WorkerTermination reason) noexcept {
        switch (reason) {
        case WorkerTermination::ResyncRequired:
            counters.resync_disconnects.fetch_add(1U, std::memory_order_relaxed);
            break;
        case WorkerTermination::HubClosed:
            counters.hub_closed_disconnects.fetch_add(
                1U, std::memory_order_relaxed);
            break;
        case WorkerTermination::SessionFailure:
            counters.session_failures.fetch_add(1U, std::memory_order_relaxed);
            break;
        case WorkerTermination::HeartbeatTimeout:
            counters.heartbeat_timeouts.fetch_add(1U, std::memory_order_relaxed);
            break;
        case WorkerTermination::PeerGoodbye:
            break;
        case WorkerTermination::ObserverRejected:
            break;
        case WorkerTermination::None:
        case WorkerTermination::ServiceStopped:
            break;
        }
    }

    void notify_all_worker_progress() noexcept {
        for (const auto& worker : workers) {
            worker->progress_changed.notify_all();
        }
    }

    [[nodiscard]] bool process_heartbeat_acknowledgement(
        Worker& worker,
        HeartbeatSchedule& schedule) noexcept {
        std::optional<Worker::PendingHeartbeatAcknowledgement> pending;
        {
            const std::lock_guard progress_lock{worker.progress_mutex};
            pending = worker.heartbeat_acknowledgement;
        }
        if (!pending) {
            return true;
        }

        if (!pending->schedule_accounted) {
            const auto acknowledged = schedule.acknowledge(
                pending->report.acknowledged_sequence,
                pending->received_at);
            if (!acknowledged) {
                const auto rejection =
                    acknowledged.error() == HeartbeatScheduleError::Expired
                    ? WorkerTermination::HeartbeatTimeout
                    : WorkerTermination::SessionFailure;
                set_termination(worker, rejection);
                {
                    const std::lock_guard progress_lock{worker.progress_mutex};
                    worker.heartbeat_acknowledgement.reset();
                    ++worker.progress_generation;
                }
                worker.progress_changed.notify_all();
                return false;
            }
            {
                const std::lock_guard progress_lock{worker.progress_mutex};
                if (!worker.heartbeat_acknowledgement
                    || worker.heartbeat_acknowledgement->report.sequence
                        != pending->report.sequence) {
                    set_termination(
                        worker, WorkerTermination::SessionFailure);
                    return false;
                }
                worker.heartbeat_acknowledgement->schedule_accounted = true;
                pending = worker.heartbeat_acknowledgement;
            }
        }

        if (!pending->observer_complete) {
            return true;
        }
        const auto rejection = pending->observer_accepted
            ? WorkerTermination::None
            : WorkerTermination::ObserverRejected;
        if (rejection != WorkerTermination::None) {
            set_termination(worker, rejection);
        }
        {
            const std::lock_guard progress_lock{worker.progress_mutex};
            worker.heartbeat_acknowledgement.reset();
            ++worker.progress_generation;
        }
        worker.progress_changed.notify_all();
        if (rejection != WorkerTermination::None) {
            return false;
        }
        counters.validated_heartbeat_acknowledgements.fetch_add(
            1U, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool service_heartbeat(
        Worker& worker,
        const std::shared_ptr<DirectorResumeSession>& session,
        HeartbeatSchedule& schedule) noexcept {
        if (!process_heartbeat_acknowledgement(
                worker, schedule)) {
            return false;
        }
        HeartbeatSchedulePoll status{};
        {
            // Receipt publication and expiry polling share one total order.
            // If the reader owns this lock first, its complete Ack is visible;
            // if the publisher owns it first at the deadline, expiry wins.
            const std::lock_guard progress_lock{worker.progress_mutex};
            if (worker.heartbeat_acknowledgement) {
                return true;
            }
            status = schedule.poll(std::chrono::steady_clock::now());
        }
        if (status == HeartbeatSchedulePoll::Expired) {
            set_termination(worker, WorkerTermination::HeartbeatTimeout);
            return false;
        }
        if (status != HeartbeatSchedulePoll::Due) {
            return true;
        }

        cmd::StreamCursor released_through{};
        {
            const std::lock_guard observer_lock{worker.observer_mutex};
            if (!worker.observer_registered || observer == nullptr
                || !observer->heartbeat_released_through(
                    released_through)) {
                set_termination(worker, WorkerTermination::ObserverRejected);
                return false;
            }
        }
        auto sent = session->send_heartbeat(
            released_through,
            config.heartbeat_response_timeout);
        if (!sent) {
            set_termination(worker, WorkerTermination::SessionFailure);
            return false;
        }
        // The response deadline starts only after the complete framed write.
        const auto scheduled = schedule.mark_sent(
            sent.value().sequence,
            sent.value().sent_at);
        if (!scheduled) {
            set_termination(worker, WorkerTermination::SessionFailure);
            return false;
        }
        counters.sent_heartbeats.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool drain_commit_permits(
        Worker& worker,
        Id128 session_id,
        const std::shared_ptr<DirectorResumeSession>& session) noexcept {
        for (;;) {
            FrameCommitPermitIntent intent{};
            DirectorSessionObserver::CommitPermitPoll poll{
                DirectorSessionObserver::CommitPermitPoll::Rejected};
            {
                const std::lock_guard observer_lock{worker.observer_mutex};
                if (!worker.observer_registered || observer == nullptr) {
                    set_termination(worker, WorkerTermination::ObserverRejected);
                    return false;
                }
                poll = observer->try_take_commit_permit(session_id, intent);
            }

            if (poll == DirectorSessionObserver::CommitPermitPoll::Empty) {
                return true;
            }
            if (poll != DirectorSessionObserver::CommitPermitPoll::Ready) {
                set_termination(worker, WorkerTermination::ObserverRejected);
                return false;
            }
            if (worker.termination.load(std::memory_order_acquire)
                != WorkerTermination::None) {
                return false;
            }

            auto sent = session->send_commit_permit(intent);
            if (!sent
                || sent.value().barrier_ordinal != intent.barrier_ordinal
                || sent.value().acknowledged_sequence
                    != intent.acknowledged_sequence
                || sent.value().before != intent.before
                || sent.value().after != intent.after) {
                set_termination(worker, WorkerTermination::SessionFailure);
                return false;
            }
        }
    }

    [[nodiscard]] std::optional<Id128> next_session_id() noexcept {
        if (session_ids_exhausted) {
            return std::nullopt;
        }
        const auto sequence = next_session_sequence;
        if (sequence == std::numeric_limits<core::u64>::max()) {
            session_ids_exhausted = true;
        } else {
            ++next_session_sequence;
        }
        return Id128{config.session_id_namespace, sequence};
    }

    [[nodiscard]] std::optional<std::size_t> available_worker() {
        for (std::size_t index = 0U; index < workers.size(); ++index) {
            auto& worker = *workers[index];
            if (worker.active.load(std::memory_order_acquire)) {
                continue;
            }
            if (worker.thread.joinable()) {
                worker.thread.join();
            }
            return index;
        }
        return std::nullopt;
    }

    void publish_resources(
        Worker& worker,
        const std::shared_ptr<DirectorResumeSession>& session,
        const std::shared_ptr<DirectorLiveSubscription>& live) noexcept {
        const std::lock_guard lock{worker.resources_mutex};
        worker.session = session;
        worker.live = live;
    }

    void clear_resources(Worker& worker) noexcept {
        const std::lock_guard lock{worker.resources_mutex};
        worker.session.reset();
        worker.live.reset();
    }

    void close_pre_live_resources(Worker& worker) noexcept {
        std::shared_ptr<DirectorLiveSubscription> live;
        std::shared_ptr<DirectorResumeSession> session;
        {
            const std::lock_guard lock{worker.resources_mutex};
            live = worker.live;
            session = worker.session;
        }
        if (live) {
            live->close();
        }
        if (session) {
            session->close();
        }
    }

    void finish_worker(
        Worker& worker,
        Id128 session_id,
        const std::shared_ptr<DirectorResumeSession>& session,
        const std::shared_ptr<DirectorLiveSubscription>& live,
        std::thread& acknowledgement_thread) noexcept {
        auto reason = worker.termination.load(std::memory_order_acquire);
        if (reason == WorkerTermination::None) {
            reason = WorkerTermination::SessionFailure;
            set_termination(worker, reason);
        }

        const auto graceful_stop = reason == WorkerTermination::ServiceStopped
            && session->state() == DirectorSessionState::Live;
        worker.phase.store(
            graceful_stop ? WorkerPhase::Draining : WorkerPhase::Cleaning,
            std::memory_order_release);
        cmd::StreamCursor goodbye_frontier{};
        bool have_goodbye_frontier = false;
        if (observer != nullptr) {
            const std::lock_guard observer_lock{worker.observer_mutex};
            if (worker.observer_registered) {
                observer->unregister(session_id, leave_reason(reason));
                worker.observer_registered = false;
            }
        }
        if (graceful_stop && observer != nullptr) {
            const std::lock_guard observer_lock{worker.observer_mutex};
            have_goodbye_frontier =
                observer->heartbeat_released_through(goodbye_frontier);
        }

        // Membership is removed before bounded socket I/O, so this stopping
        // worker cannot enter a later publication cut while its Goodbye is
        // backpressured.
        if (graceful_stop && have_goodbye_frontier) {
            auto goodbye = session->send_goodbye(
                GoodbyeReason::NormalShutdown,
                goodbye_frontier);
            if (goodbye) {
                counters.graceful_goodbyes.fetch_add(
                    1U, std::memory_order_relaxed);
            }
        }
        worker.phase.store(WorkerPhase::Cleaning, std::memory_order_release);
        live->close();
        session->close();
        worker.progress_changed.notify_all();
        if (acknowledgement_thread.joinable()) {
            acknowledgement_thread.join();
        }
        clear_resources(worker);
        count_termination(reason);
        active_workers.fetch_sub(1U, std::memory_order_acq_rel);
        worker.phase.store(WorkerPhase::Done, std::memory_order_release);
        worker.active.store(false, std::memory_order_release);
    }

    void run_worker(
        Worker& worker,
        asio::ip::tcp::socket socket,
        Id128 session_id) noexcept {
        std::shared_ptr<DirectorResumeSession> session;
        std::shared_ptr<DirectorLiveSubscription> live;
        std::thread acknowledgement_thread;
        try {
            worker.phase.store(WorkerPhase::Resuming, std::memory_order_release);
            auto subscription = hub->subscribe();
            if (!subscription) {
                counters.subscription_failures.fetch_add(
                    1U, std::memory_order_relaxed);
                close_socket(socket);
                set_termination(worker, WorkerTermination::SessionFailure);
                active_workers.fetch_sub(1U, std::memory_order_acq_rel);
                worker.phase.store(WorkerPhase::Done, std::memory_order_release);
                worker.active.store(false, std::memory_order_release);
                return;
            }

            auto session_config = config.session_defaults;
            session_config.session_id = session_id;
            session_config.io.admission_budget = admission_budget;
            live = std::make_shared<DirectorLiveSubscription>(
                std::move(subscription.value().live));
            session = std::make_shared<DirectorResumeSession>(
                std::move(socket), session_config, *authority, *leases);
            publish_resources(worker, session, live);

            if (worker.termination.load(std::memory_order_acquire)
                != WorkerTermination::None) {
                finish_worker(
                    worker, session_id, session, live, acknowledgement_thread);
                return;
            }

            std::atomic_bool resume_phase_complete{false};
            std::jthread hub_monitor{[this,
                                      &worker,
                                      &resume_phase_complete,
                                      session,
                                      live](std::stop_token stop_token) {
                while (!stop_token.stop_requested()
                       && !resume_phase_complete.load(
                           std::memory_order_acquire)) {
                    if (worker.termination.load(std::memory_order_acquire)
                        != WorkerTermination::None) {
                        session->close();
                        return;
                    }
                    const auto state = live->state();
                    if (state != DirectorSubscriptionState::Active) {
                        set_termination(
                            worker, subscription_termination(state));
                        session->close();
                        return;
                    }
                    std::unique_lock progress_lock{worker.progress_mutex};
                    static_cast<void>(worker.progress_changed.wait_for(
                        progress_lock,
                        std::chrono::milliseconds{10},
                        [&worker, &resume_phase_complete] {
                            return resume_phase_complete.load(
                                       std::memory_order_acquire)
                                || worker.termination.load(
                                       std::memory_order_acquire)
                                    != WorkerTermination::None;
                        }));
                }
            }};

            auto resumed = session->resume(
                std::move(subscription.value().source));
            if (!resumed) {
                counters.resume_failures.fetch_add(1U, std::memory_order_relaxed);
                set_termination(worker, WorkerTermination::SessionFailure);
            } else {
                // ResumeAck prepares replay frames as an aggregate. They are
                // not barrier-eligible until every historical frame commits.
                while (worker.termination.load(std::memory_order_acquire)
                           == WorkerTermination::None
                       && session->outstanding_frames() != 0U) {
                    auto acknowledgement =
                        session->receive_frame_acknowledgement();
                    if (!acknowledgement) {
                        set_termination(
                            worker, WorkerTermination::SessionFailure);
                        break;
                    }
                }
                if (worker.termination.load(std::memory_order_acquire)
                        == WorkerTermination::None
                    && !same_cursor(
                        session->committed_cursor(), resumed.value().target)) {
                    set_termination(worker, WorkerTermination::SessionFailure);
                }
            }

            resume_phase_complete.store(true, std::memory_order_release);
            hub_monitor.request_stop();
            worker.progress_changed.notify_all();
            hub_monitor.join();

            if (worker.termination.load(std::memory_order_acquire)
                != WorkerTermination::None) {
                finish_worker(
                    worker, session_id, session, live, acknowledgement_thread);
                return;
            }

            const auto subscription_state = live->state();
            if (subscription_state != DirectorSubscriptionState::Active) {
                set_termination(
                    worker, subscription_termination(subscription_state));
                finish_worker(
                    worker, session_id, session, live, acknowledgement_thread);
                return;
            }
            const auto peer = session->peer_identity();
            const auto peer_node_index = session->peer_node_index();
            if (!peer || !peer_node_index) {
                set_termination(worker, WorkerTermination::SessionFailure);
                finish_worker(
                    worker, session_id, session, live, acknowledgement_thread);
                return;
            }

            worker.phase.store(WorkerPhase::Registering, std::memory_order_release);
            bool accepted = true;
            if (observer != nullptr) {
                accepted = observer->register_live(DirectorLiveRegistration{
                    session_id,
                    *peer,
                    *peer_node_index,
                    session->committed_cursor(),
                    session->last_node_control_sequence(),
                });
                if (accepted) {
                    const std::lock_guard observer_lock{worker.observer_mutex};
                    worker.observer_registered = true;
                }
            }
            if (!accepted) {
                counters.observer_rejections.fetch_add(
                    1U, std::memory_order_relaxed);
                set_termination(worker, WorkerTermination::ObserverRejected);
                finish_worker(
                    worker, session_id, session, live, acknowledgement_thread);
                return;
            }
            counters.live_registrations.fetch_add(1U, std::memory_order_relaxed);
            worker.phase.store(WorkerPhase::Live, std::memory_order_release);

            auto created_heartbeat = HeartbeatSchedule::create(
                HeartbeatScheduleConfig{
                    config.heartbeat_interval,
                    config.heartbeat_response_timeout,
                },
                std::chrono::steady_clock::now());
            if (!created_heartbeat) {
                set_termination(worker, WorkerTermination::SessionFailure);
                finish_worker(
                    worker, session_id, session, live, acknowledgement_thread);
                return;
            }
            auto heartbeat_schedule =
                std::move(created_heartbeat).value();
            {
                const std::lock_guard progress_lock{worker.progress_mutex};
                worker.heartbeat_acknowledgement.reset();
            }

            acknowledgement_thread = std::thread{
                [this, &worker, session_id, session] {
                    while (worker.termination.load(std::memory_order_acquire)
                               == WorkerTermination::None
                           && session->state() == DirectorSessionState::Live) {
                        auto inbound = session->receive_live_message();
                        if (!inbound) {
                            set_termination(
                                worker, WorkerTermination::SessionFailure);
                            return;
                        }

                        if (const auto* acknowledgement =
                                std::get_if<FrameAcknowledgementReport>(
                                    &inbound.value())) {
                            bool observer_accepted = true;
                            if (observer != nullptr) {
                                const std::lock_guard observer_lock{
                                    worker.observer_mutex};
                                if (worker.observer_registered) {
                                    observer_accepted = observer->on_ack(
                                        session_id, *acknowledgement);
                                }
                            }
                            if (!observer_accepted) {
                                set_termination(
                                    worker,
                                    WorkerTermination::ObserverRejected);
                                return;
                            }
                            counters.validated_acknowledgements.fetch_add(
                                1U, std::memory_order_relaxed);
                        } else if (const auto* heartbeat =
                                       std::get_if<
                                           HeartbeatAcknowledgementReport>(
                                           &inbound.value())) {
                            {
                                const std::lock_guard progress_lock{
                                    worker.progress_mutex};
                                if (worker.heartbeat_acknowledgement) {
                                    set_termination(
                                        worker,
                                        WorkerTermination::SessionFailure);
                                    return;
                                }
                                worker.heartbeat_acknowledgement.emplace(
                                    Worker::PendingHeartbeatAcknowledgement{
                                        *heartbeat,
                                        std::chrono::steady_clock::now(),
                                        false,
                                        false,
                                        false,
                                    });
                                ++worker.progress_generation;
                            }
                            worker.progress_changed.notify_all();

                            bool observer_accepted = true;
                            if (observer != nullptr) {
                                const std::lock_guard observer_lock{
                                    worker.observer_mutex};
                                observer_accepted = worker.observer_registered
                                    && observer->on_heartbeat_ack(
                                        session_id, *heartbeat);
                            }
                            {
                                const std::lock_guard progress_lock{
                                    worker.progress_mutex};
                                if (worker.heartbeat_acknowledgement
                                    && worker.heartbeat_acknowledgement
                                            ->report.sequence
                                        == heartbeat->sequence) {
                                    worker.heartbeat_acknowledgement
                                        ->observer_complete = true;
                                    worker.heartbeat_acknowledgement
                                        ->observer_accepted =
                                        observer_accepted;
                                    ++worker.progress_generation;
                                }
                            }
                            if (!observer_accepted) {
                                set_termination(
                                    worker,
                                    WorkerTermination::ObserverRejected);
                                return;
                            }
                            worker.progress_changed.notify_all();
                            std::unique_lock progress_lock{
                                worker.progress_mutex};
                            worker.progress_changed.wait(
                                progress_lock,
                                [&worker] {
                                    return !worker.heartbeat_acknowledgement
                                        || worker.termination.load(
                                               std::memory_order_acquire)
                                            != WorkerTermination::None;
                                });
                            if (worker.termination.load(
                                    std::memory_order_acquire)
                                != WorkerTermination::None) {
                                return;
                            }
                            continue;
                        } else {
                            counters.graceful_goodbyes.fetch_add(
                                1U, std::memory_order_relaxed);
                            set_termination(
                                worker, WorkerTermination::PeerGoodbye);
                            return;
                        }
                        {
                            const std::lock_guard progress_lock{
                                worker.progress_mutex};
                            ++worker.progress_generation;
                        }
                        worker.progress_changed.notify_all();
                        notify_all_worker_progress();
                    }
                }};

            std::optional<PublishedDirectorFrame> pending;
            while (worker.termination.load(std::memory_order_acquire)
                   == WorkerTermination::None) {
                if (!drain_commit_permits(worker, session_id, session)) {
                    break;
                }
                if (!service_heartbeat(
                        worker,
                        session,
                        heartbeat_schedule)) {
                    break;
                }
                if (!pending) {
                    auto publication = live->wait_for(
                        std::chrono::milliseconds{5});
                    if (publication.state
                        != DirectorSubscriptionState::Active) {
                        set_termination(
                            worker,
                            subscription_termination(publication.state));
                        break;
                    }
                    if (!publication.frame) {
                        continue;
                    }
                    pending.emplace(std::move(*publication.frame));
                }

                const auto current_subscription_state = live->state();
                if (current_subscription_state
                    != DirectorSubscriptionState::Active) {
                    set_termination(
                        worker,
                        subscription_termination(current_subscription_state));
                    break;
                }

                auto sent = session->send_committed(pending->batch());
                if (sent) {
                    pending.reset();
                    continue;
                }
                if (!inflight_window_full(sent.error())) {
                    set_termination(worker, WorkerTermination::SessionFailure);
                    break;
                }

                std::unique_lock progress_lock{worker.progress_mutex};
                const auto observed_generation = worker.progress_generation;
                static_cast<void>(worker.progress_changed.wait_for(
                    progress_lock,
                    std::chrono::milliseconds{25},
                    [&worker, observed_generation] {
                        return worker.termination.load(
                                   std::memory_order_acquire)
                                != WorkerTermination::None
                            || worker.progress_generation
                                != observed_generation;
                    }));
            }

            finish_worker(
                worker, session_id, session, live, acknowledgement_thread);
        } catch (const std::bad_alloc&) {
            counters.worker_start_failures.fetch_add(1U, std::memory_order_relaxed);
            set_termination(worker, WorkerTermination::SessionFailure);
            if (live && session) {
                finish_worker(
                    worker, session_id, session, live, acknowledgement_thread);
            } else {
                close_socket(socket);
                active_workers.fetch_sub(1U, std::memory_order_acq_rel);
                worker.phase.store(WorkerPhase::Done, std::memory_order_release);
                worker.active.store(false, std::memory_order_release);
            }
        } catch (const std::length_error&) {
            counters.worker_start_failures.fetch_add(1U, std::memory_order_relaxed);
            set_termination(worker, WorkerTermination::SessionFailure);
            if (live && session) {
                finish_worker(
                    worker, session_id, session, live, acknowledgement_thread);
            } else {
                close_socket(socket);
                active_workers.fetch_sub(1U, std::memory_order_acq_rel);
                worker.phase.store(WorkerPhase::Done, std::memory_order_release);
                worker.active.store(false, std::memory_order_release);
            }
        } catch (...) {
            counters.worker_start_failures.fetch_add(1U, std::memory_order_relaxed);
            set_termination(worker, WorkerTermination::SessionFailure);
            if (live && session) {
                finish_worker(
                    worker, session_id, session, live, acknowledgement_thread);
            } else {
                close_socket(socket);
                active_workers.fetch_sub(1U, std::memory_order_acq_rel);
                worker.phase.store(WorkerPhase::Done, std::memory_order_release);
                worker.active.store(false, std::memory_order_release);
            }
        }
    }

    void dispatch(asio::ip::tcp::socket socket) noexcept {
        if (!running.load(std::memory_order_acquire)) {
            close_socket(socket);
            return;
        }
        const auto available = available_worker();
        if (!available) {
            counters.capacity_rejections.fetch_add(1U, std::memory_order_relaxed);
            close_socket(socket);
            return;
        }
        const auto session_id = next_session_id();
        if (!session_id) {
            counters.session_id_exhaustions.fetch_add(
                1U, std::memory_order_relaxed);
            close_socket(socket);
            return;
        }

        auto& worker = *workers[*available];
        worker.termination.store(WorkerTermination::None, std::memory_order_release);
        worker.phase.store(WorkerPhase::Idle, std::memory_order_release);
        worker.active.store(true, std::memory_order_release);
        active_workers.fetch_add(1U, std::memory_order_acq_rel);
        try {
            worker.thread = std::thread{
                [this,
                 &worker,
                 accepted = std::move(socket),
                 session_id = *session_id]() mutable {
                    run_worker(worker, std::move(accepted), session_id);
                }};
        } catch (const std::system_error&) {
            counters.worker_start_failures.fetch_add(1U, std::memory_order_relaxed);
            active_workers.fetch_sub(1U, std::memory_order_acq_rel);
            worker.phase.store(WorkerPhase::Done, std::memory_order_release);
            worker.active.store(false, std::memory_order_release);
            close_socket(socket);
        }
    }

    void accept_loop() noexcept {
        while (running.load(std::memory_order_acquire)) {
            asio::ip::tcp::socket socket{context};
            std::error_code error;
            acceptor->accept(socket, error);
            if (!error) {
                counters.accepted_connections.fetch_add(
                    1U, std::memory_order_relaxed);
                dispatch(std::move(socket));
                continue;
            }
            if (error != asio::error::would_block
                && error != asio::error::try_again
                && error != asio::error::connection_aborted) {
                if (!running.load(std::memory_order_acquire)) {
                    break;
                }
            }
            std::unique_lock wait_lock{accept_wait_mutex};
            static_cast<void>(accept_changed.wait_for(
                wait_lock,
                std::chrono::milliseconds{10},
                [this] { return !running.load(std::memory_order_acquire); }));
        }
    }

    DirectorSessionServiceConfig config;
    DirectorSessionHub* hub;
    const persist::DurableDirector* authority;
    NodeSessionLeaseAuthority* leases;
    DirectorSessionObserver* observer;
    asio::io_context context;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
    std::shared_ptr<FramedStreamAdmissionBudget> admission_budget;
    std::vector<std::unique_ptr<Worker>> workers;
    std::thread accept_thread;
    mutable std::mutex lifecycle_mutex;
    std::mutex accept_wait_mutex;
    std::condition_variable accept_changed;
    std::atomic_bool running{};
    std::optional<asio::ip::tcp::endpoint> local_endpoint;
    std::atomic_size_t active_workers{};
    core::u64 next_session_sequence{};
    bool session_ids_exhausted{};
    AtomicCounters counters;
};

} // namespace detail

DirectorSessionService::DirectorSessionService(
    DirectorSessionServiceConfig config,
    DirectorSessionHub& hub,
    const persist::DurableDirector& authority,
    NodeSessionLeaseAuthority& leases,
    DirectorSessionObserver* observer)
    : state_(std::make_unique<detail::DirectorSessionServiceState>(
          std::move(config), hub, authority, leases, observer)) {}

DirectorSessionService::~DirectorSessionService() {
    stop();
}

core::Result<asio::ip::tcp::endpoint, DirectorSessionServiceError>
DirectorSessionService::start() {
    const std::lock_guard lifecycle_lock{state_->lifecycle_mutex};
    if (state_->running.load(std::memory_order_acquire)) {
        return ServiceStartResult::failure(service_error(
            DirectorSessionServiceErrorCode::AlreadyRunning));
    }
    if (!valid_config(state_->config) || state_->observer == nullptr
        || !state_->observer->coordinates_commit_permits()) {
        return ServiceStartResult::failure(service_error(
            DirectorSessionServiceErrorCode::InvalidConfiguration));
    }

    try {
        if (state_->workers.empty()) {
            std::vector<std::unique_ptr<
                detail::DirectorSessionServiceState::Worker>> allocated;
            allocated.reserve(state_->config.max_workers);
            for (std::size_t index = 0U;
                 index < state_->config.max_workers;
                 ++index) {
                allocated.push_back(
                    std::make_unique<detail::DirectorSessionServiceState::Worker>());
            }
            state_->workers = std::move(allocated);
        }
        if (!state_->admission_budget) {
            state_->admission_budget =
                std::make_shared<FramedStreamAdmissionBudget>(
                    state_->config.max_receive_bytes);
        }
        state_->acceptor = std::make_unique<asio::ip::tcp::acceptor>(
            state_->context);
    } catch (const std::bad_alloc&) {
        return ServiceStartResult::failure(service_error(
            DirectorSessionServiceErrorCode::AllocationFailed));
    } catch (const std::length_error&) {
        return ServiceStartResult::failure(service_error(
            DirectorSessionServiceErrorCode::AllocationFailed));
    }

    std::error_code error;
    state_->acceptor->open(state_->config.bind_endpoint.protocol(), error);
    if (error) {
        state_->acceptor.reset();
        return ServiceStartResult::failure(service_error(
            DirectorSessionServiceErrorCode::OpenFailed, error));
    }
    state_->acceptor->set_option(asio::socket_base::reuse_address{true}, error);
    if (error) {
        std::error_code ignored;
        state_->acceptor->close(ignored);
        state_->acceptor.reset();
        return ServiceStartResult::failure(service_error(
            DirectorSessionServiceErrorCode::OpenFailed, error));
    }
    state_->acceptor->bind(state_->config.bind_endpoint, error);
    if (error) {
        std::error_code ignored;
        state_->acceptor->close(ignored);
        state_->acceptor.reset();
        return ServiceStartResult::failure(service_error(
            DirectorSessionServiceErrorCode::BindFailed, error));
    }
    state_->acceptor->listen(
        static_cast<int>(std::min<std::size_t>(
            state_->config.max_workers,
            static_cast<std::size_t>(
                asio::socket_base::max_listen_connections))),
        error);
    if (error) {
        std::error_code ignored;
        state_->acceptor->close(ignored);
        state_->acceptor.reset();
        return ServiceStartResult::failure(service_error(
            DirectorSessionServiceErrorCode::ListenFailed, error));
    }
    state_->acceptor->non_blocking(true, error);
    if (error) {
        std::error_code ignored;
        state_->acceptor->close(ignored);
        state_->acceptor.reset();
        return ServiceStartResult::failure(service_error(
            DirectorSessionServiceErrorCode::ListenFailed, error));
    }
    state_->local_endpoint = state_->acceptor->local_endpoint(error);
    if (error) {
        std::error_code ignored;
        state_->acceptor->close(ignored);
        state_->acceptor.reset();
        state_->local_endpoint.reset();
        return ServiceStartResult::failure(service_error(
            DirectorSessionServiceErrorCode::ListenFailed, error));
    }

    state_->running.store(true, std::memory_order_release);
    try {
        state_->accept_thread = std::thread{[state = state_.get()] {
            state->accept_loop();
        }};
    } catch (const std::system_error& exception) {
        state_->running.store(false, std::memory_order_release);
        std::error_code ignored;
        state_->acceptor->close(ignored);
        state_->acceptor.reset();
        state_->local_endpoint.reset();
        return ServiceStartResult::failure(service_error(
            DirectorSessionServiceErrorCode::ThreadStartFailed,
            exception.code()));
    }
    return ServiceStartResult::success(*state_->local_endpoint);
}

void DirectorSessionService::stop() noexcept {
    const std::lock_guard lifecycle_lock{state_->lifecycle_mutex};
    const auto was_running =
        state_->running.exchange(false, std::memory_order_acq_rel);
    if (!was_running && !state_->accept_thread.joinable()) {
        return;
    }
    state_->accept_changed.notify_all();
    if (state_->accept_thread.joinable()) {
        state_->accept_thread.join();
    }
    if (state_->acceptor) {
        std::error_code ignored;
        state_->acceptor->close(ignored);
        state_->acceptor.reset();
    }
    state_->local_endpoint.reset();

    for (auto& worker_ptr : state_->workers) {
        auto& worker = *worker_ptr;
        if (!worker.active.load(std::memory_order_acquire)) {
            continue;
        }
        state_->set_termination(worker, WorkerTermination::ServiceStopped);
        const auto phase = worker.phase.load(std::memory_order_acquire);
        if (phase == WorkerPhase::Idle || phase == WorkerPhase::Resuming) {
            state_->close_pre_live_resources(worker);
        }
    }
    for (auto& worker_ptr : state_->workers) {
        auto& worker = *worker_ptr;
        if (worker.thread.joinable()) {
            worker.thread.join();
        }
    }
}

bool DirectorSessionService::running() const noexcept {
    return state_->running.load(std::memory_order_acquire);
}

std::optional<asio::ip::tcp::endpoint>
DirectorSessionService::local_endpoint() const noexcept {
    const std::lock_guard lifecycle_lock{state_->lifecycle_mutex};
    return state_->local_endpoint;
}

std::size_t DirectorSessionService::active_workers() const noexcept {
    return state_->active_workers.load(std::memory_order_acquire);
}

DirectorSessionServiceCounters DirectorSessionService::counters() const noexcept {
    return DirectorSessionServiceCounters{
        state_->counters.accepted_connections.load(std::memory_order_relaxed),
        state_->counters.capacity_rejections.load(std::memory_order_relaxed),
        state_->counters.session_id_exhaustions.load(std::memory_order_relaxed),
        state_->counters.worker_start_failures.load(std::memory_order_relaxed),
        state_->counters.subscription_failures.load(std::memory_order_relaxed),
        state_->counters.resume_failures.load(std::memory_order_relaxed),
        state_->counters.live_registrations.load(std::memory_order_relaxed),
        state_->counters.observer_rejections.load(std::memory_order_relaxed),
        state_->counters.validated_acknowledgements.load(
            std::memory_order_relaxed),
        state_->counters.validated_heartbeat_acknowledgements.load(
            std::memory_order_relaxed),
        state_->counters.sent_heartbeats.load(std::memory_order_relaxed),
        state_->counters.heartbeat_timeouts.load(std::memory_order_relaxed),
        state_->counters.graceful_goodbyes.load(std::memory_order_relaxed),
        state_->counters.resync_disconnects.load(std::memory_order_relaxed),
        state_->counters.hub_closed_disconnects.load(std::memory_order_relaxed),
        state_->counters.session_failures.load(std::memory_order_relaxed),
    };
}

std::size_t DirectorSessionService::receive_capacity_bytes() const noexcept {
    const std::lock_guard lifecycle_lock{state_->lifecycle_mutex};
    return state_->admission_budget
        ? state_->admission_budget->capacity_bytes()
        : 0U;
}

std::size_t DirectorSessionService::reserved_receive_bytes() const noexcept {
    const std::lock_guard lifecycle_lock{state_->lifecycle_mutex};
    return state_->admission_budget
        ? state_->admission_budget->reserved_bytes()
        : 0U;
}

} // namespace mediaserver::net
