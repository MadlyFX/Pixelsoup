#include <mediaserver/net/node_session_service.hpp>

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>

namespace mediaserver::net {
namespace {

using StartResult = core::Result<std::monostate, NodeSessionServiceError>;
constexpr auto connect_poll_interval = std::chrono::milliseconds{2};

[[nodiscard]] bool fits_size_t(core::u64 value) noexcept {
    if constexpr (sizeof(std::size_t) >= sizeof(core::u64)) {
        return true;
    } else {
        return value <= static_cast<core::u64>(
                            std::numeric_limits<std::size_t>::max());
    }
}

[[nodiscard]] bool valid_config(
    const NodeSessionServiceConfig& config) noexcept {
    const auto& session = config.session_defaults;
    const auto& limits = session.limits;
    const auto& codec = session.codec_limits;
    const auto& wire = session.wire_limits;
    const auto zero = std::chrono::milliseconds::zero();
    return config.director_endpoint.port() != 0U
        && !config.director_endpoint.address().is_unspecified()
        && !config.director_endpoint.address().is_multicast()
        && static_cast<bool>(session.show_id)
        && static_cast<bool>(session.node_id)
        && static_cast<bool>(session.incarnation)
        && session.node_index <= max_session_node_index
        && !session.io.admission_budget
        && config.max_receive_bytes >= codec.max_message_bytes
        && config.connect_timeout > zero
        && config.reconnect_initial_delay > zero
        && config.reconnect_max_delay >= config.reconnect_initial_delay
        && session.io.handshake_timeout >= zero
        && session.io.read_timeout >= zero
        && session.io.write_timeout > zero
        && limits.max_packet_bytes != 0U
        && limits.max_frame_bytes != 0U
        && limits.max_inflight_frames != 0U
        && limits.max_inflight_bytes != 0U
        && limits.max_checkpoint_bytes != 0U
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
        && limits.max_checkpoint_bytes <= wire.max_message_bytes;
}

struct FailureDecision final {
    bool retry{};
    NodeSessionServiceFailure failure{NodeSessionServiceFailure::Protocol};
};

[[nodiscard]] FailureDecision classify_session_error(
    const ResumeSessionError& error) noexcept {
    if (const auto* framed = std::get_if<FramedStreamError>(&error)) {
        switch (framed->code) {
        case FramedStreamErrorCode::Closed:
        case FramedStreamErrorCode::TimedOut:
        case FramedStreamErrorCode::ReadFailed:
        case FramedStreamErrorCode::WriteFailed:
            return {true, NodeSessionServiceFailure::Transport};
        case FramedStreamErrorCode::InvalidSizePrefix:
        case FramedStreamErrorCode::SizeLimitExceeded:
        case FramedStreamErrorCode::AdmissionLimitExceeded:
        case FramedStreamErrorCode::AllocationFailed:
            return {false, NodeSessionServiceFailure::Protocol};
        }
    }

    const auto* protocol = std::get_if<SessionProtocolFailure>(&error);
    if (protocol == nullptr) {
        return {false, NodeSessionServiceFailure::Protocol};
    }
    if (protocol->code == SessionProtocolErrorCode::NodeLeaseRejected
        || (protocol->code == SessionProtocolErrorCode::PeerRejected
            && protocol->reject_code == RejectCode::NodeLeaseRejected)) {
        return {false, NodeSessionServiceFailure::LeaseRejected};
    }
    if (protocol->code == SessionProtocolErrorCode::PeerRejected
        && protocol->reject_code == RejectCode::CapacityExceeded) {
        return {true, NodeSessionServiceFailure::PeerCapacity};
    }
    if (protocol->code == SessionProtocolErrorCode::PeerRejected
        && protocol->reject_code == RejectCode::ServerShutdown) {
        return {true, NodeSessionServiceFailure::PeerShutdown};
    }
    return {false, NodeSessionServiceFailure::Protocol};
}

[[nodiscard]] std::optional<SessionProtocolErrorCode> protocol_code(
    const ResumeSessionError& error) noexcept {
    const auto* protocol = std::get_if<SessionProtocolFailure>(&error);
    if (protocol == nullptr) {
        return std::nullopt;
    }
    return protocol->code;
}

[[nodiscard]] bool benign_boundary_rejection(
    const ResumeSessionError& error) noexcept {
    const auto code = protocol_code(error);
    return code == SessionProtocolErrorCode::WrongState
        || code == SessionProtocolErrorCode::FrameNotPrepared
        || code == SessionProtocolErrorCode::FrameNotPermitted;
}

[[nodiscard]] std::chrono::milliseconds grow_backoff(
    std::chrono::milliseconds current,
    std::chrono::milliseconds maximum) noexcept {
    if (current >= maximum / 2) {
        return maximum;
    }
    return std::min(current * 2, maximum);
}

class AsyncNodeSessionDialer final : public NodeSessionDialer {
public:
    [[nodiscard]] NodeSessionConnectResult connect(
        const asio::ip::tcp::endpoint& endpoint,
        std::chrono::milliseconds timeout,
        const std::atomic_bool& stop_requested) override {
        cancelled_.store(false, std::memory_order_release);
        context_.restart();
        asio::ip::tcp::socket socket{context_};
        std::error_code open_error;
        socket.open(endpoint.protocol(), open_error);
        if (open_error) {
            return NodeSessionConnectResult::failure({
                NodeSessionConnectErrorCode::OpenFailed, open_error});
        }

        bool complete = false;
        std::error_code connect_error;
        socket.async_connect(
            endpoint,
            [&complete, &connect_error](const std::error_code& error) {
                connect_error = error;
                complete = true;
            });

        const auto now = std::chrono::steady_clock::now();
        const auto maximum_timeout = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::time_point::max() - now);
        const auto deadline = timeout >= maximum_timeout
            ? std::chrono::steady_clock::time_point::max()
            : now + std::chrono::duration_cast<
                  std::chrono::steady_clock::duration>(timeout);
        auto cancelled = false;
        auto timed_out = false;
        while (!complete) {
            if (!cancelled && !timed_out
                && (stop_requested.load(std::memory_order_acquire)
                    || cancelled_.load(std::memory_order_acquire))) {
                cancelled = true;
                socket.close(open_error);
            } else if (!cancelled && !timed_out
                       && std::chrono::steady_clock::now() >= deadline) {
                timed_out = true;
                socket.close(open_error);
            }
            static_cast<void>(context_.run_for(connect_poll_interval));
            if (context_.stopped() && !complete) {
                context_.restart();
            }
        }

        if (cancelled) {
            return NodeSessionConnectResult::failure({
                NodeSessionConnectErrorCode::Cancelled, {}});
        }
        if (timed_out) {
            return NodeSessionConnectResult::failure({
                NodeSessionConnectErrorCode::TimedOut, {}});
        }
        if (connect_error) {
            return NodeSessionConnectResult::failure({
                NodeSessionConnectErrorCode::ConnectFailed, connect_error});
        }
        return NodeSessionConnectResult::success(std::move(socket));
    }

    void cancel() noexcept override {
        cancelled_.store(true, std::memory_order_release);
    }

private:
    asio::io_context context_;
    std::atomic_bool cancelled_{};
};

} // namespace

namespace detail {

struct AtomicNodeServiceCounters final {
    std::atomic_uint64_t connection_attempts{};
    std::atomic_uint64_t connect_failures{};
    std::atomic_uint64_t successful_resumes{};
    std::atomic_uint64_t reconnect_waits{};
    std::atomic_uint64_t received_frames{};
    std::atomic_uint64_t received_commit_permits{};
    std::atomic_uint64_t received_heartbeats{};
    std::atomic_uint64_t received_tracked_poses{};
    std::atomic_uint64_t received_goodbyes{};
    std::atomic_uint64_t flushed_acknowledgements{};
    std::atomic_uint64_t graceful_goodbyes{};
    std::atomic_uint64_t committed_frames{};
    std::atomic_uint64_t checkpoint_activations{};
    std::atomic_uint64_t terminal_failures{};
    std::atomic_uint64_t unsupported_live_controls{};
};

class NodeSessionServiceStateData final {
public:
    NodeSessionServiceStateData(
        NodeSessionServiceConfig service_config,
        cmd::NodeCommandBus& command_bus,
        NodeSessionDialer* borrowed_dialer)
        : config(std::move(service_config)),
          node(&command_bus),
          owned_dialer(
              borrowed_dialer == nullptr
                  ? std::make_unique<AsyncNodeSessionDialer>()
                  : nullptr),
          dialer(
              borrowed_dialer == nullptr
                  ? static_cast<NodeSessionDialer*>(owned_dialer.get())
                  : borrowed_dialer),
          observed_director_epoch(config.session_defaults.last_director_epoch) {}

    [[nodiscard]] bool stopping() const noexcept {
        return stop_requested.load(std::memory_order_acquire)
            || terminal_requested.load(std::memory_order_acquire);
    }

    void wake_writer() noexcept {
        writer_generation.fetch_add(1U, std::memory_order_acq_rel);
        writer_changed.notify_all();
    }

    void latch_terminal(NodeSessionServiceFailure failure) noexcept {
        auto expected = false;
        if (terminal_requested.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            last_failure.store(failure, std::memory_order_release);
            counters.terminal_failures.fetch_add(1U, std::memory_order_relaxed);
        }
        phase.store(NodeSessionServiceState::Faulted, std::memory_order_release);
        wake_writer();
        backoff_changed.notify_all();
    }

    void clear(const std::shared_ptr<NodeResumeSession>& session) noexcept {
        const std::lock_guard lock{resource_mutex};
        if (active_session == session) {
            active_session.reset();
        }
    }

    [[nodiscard]] bool wait_backoff(
        std::chrono::milliseconds delay) noexcept {
        phase.store(NodeSessionServiceState::Backoff, std::memory_order_release);
        counters.reconnect_waits.fetch_add(1U, std::memory_order_relaxed);
        std::unique_lock lock{backoff_mutex};
        static_cast<void>(backoff_changed.wait_for(
            lock, delay, [this] { return stopping(); }));
        return !stopping();
    }

    static void record_writer_failure(
        FailureDecision decision,
        std::atomic_bool& failed,
        std::atomic_bool& retry,
        std::atomic<NodeSessionServiceFailure>& failure,
        const std::shared_ptr<NodeResumeSession>& session) noexcept {
        failure.store(decision.failure, std::memory_order_release);
        retry.store(decision.retry, std::memory_order_release);
        failed.store(true, std::memory_order_release);
        session->interrupt_transport();
    }

    void writer_loop(
        const std::shared_ptr<NodeResumeSession>& session,
        std::atomic_bool& failed,
        std::atomic_bool& retry,
        std::atomic<NodeSessionServiceFailure>& failure) noexcept {
        auto observed = writer_generation.load(std::memory_order_acquire);
        try {
            while (true) {
                const auto session_state = session->state();
                if (session_state == NodeSessionState::Closed) {
                    return;
                }
                if (session_state == NodeSessionState::CheckpointActivated) {
                    auto completed = session->complete_checkpoint_activation();
                    if (!completed) {
                        record_writer_failure(
                            classify_session_error(completed.error()),
                            failed,
                            retry,
                            failure,
                            session);
                        wake_writer();
                        return;
                    }
                    phase.store(
                        NodeSessionServiceState::Live,
                        std::memory_order_release);
                    wake_writer();
                    continue;
                }
                if ((session_state == NodeSessionState::Live
                     || session_state == NodeSessionState::Draining)
                    && session->pending_control_messages() != 0U) {
                    auto flushed = session->flush_frame_acknowledgements();
                    if (!flushed) {
                        record_writer_failure(
                            classify_session_error(flushed.error()),
                            failed,
                            retry,
                            failure,
                            session);
                        wake_writer();
                        return;
                    }
                    counters.flushed_acknowledgements.fetch_add(
                        static_cast<std::uint64_t>(
                            flushed.value().flushed
                            - flushed.value().heartbeat_acknowledgements
                            - flushed.value().goodbyes),
                        std::memory_order_relaxed);
                    counters.graceful_goodbyes.fetch_add(
                        static_cast<std::uint64_t>(flushed.value().goodbyes),
                        std::memory_order_relaxed);
                    continue;
                }
                if (session_state == NodeSessionState::Draining
                    || stopping()) {
                    session->interrupt_transport();
                    return;
                }

                std::unique_lock lock{writer_mutex};
                writer_changed.wait(lock, [this, &session, &observed] {
                    const auto current = writer_generation.load(
                        std::memory_order_acquire);
                    if (current != observed) {
                        observed = current;
                        return true;
                    }
                    return stopping()
                        || session->state() == NodeSessionState::Closed;
                });
            }
        } catch (...) {
            record_writer_failure(
                {false, NodeSessionServiceFailure::Internal},
                failed,
                retry,
                failure,
                session);
            wake_writer();
        }
    }

    void run() noexcept {
        auto backoff = config.reconnect_initial_delay;
        try {
            while (!stopping()) {
                phase.store(
                    NodeSessionServiceState::Connecting,
                    std::memory_order_release);
                counters.connection_attempts.fetch_add(
                    1U, std::memory_order_relaxed);
                auto connected = dialer->connect(
                    config.director_endpoint,
                    config.connect_timeout,
                    stop_requested);
                if (stopping()) {
                    break;
                }
                if (!connected) {
                    counters.connect_failures.fetch_add(
                        1U, std::memory_order_relaxed);
                    last_failure.store(
                        NodeSessionServiceFailure::Connect,
                        std::memory_order_release);
                    if (!wait_backoff(backoff)) {
                        break;
                    }
                    backoff = grow_backoff(
                        backoff, config.reconnect_max_delay);
                    continue;
                }

                NodeSessionConfig session_config = config.session_defaults;
                {
                    const std::lock_guard lock{resource_mutex};
                    session_config.last_director_epoch = observed_director_epoch;
                }
                session_config.io.admission_budget = admission_budget;
                auto session = std::make_shared<NodeResumeSession>(
                    std::move(connected).value(), *node, std::move(session_config));
                {
                    const std::lock_guard lock{resource_mutex};
                    session->publish_runtime_asset_adoption(
                        runtime_asset_adoption);
                    session->publish_runtime_output_sync(runtime_output_sync);
                    session->publish_runtime_tracking(runtime_tracking);
                    session->publish_runtime_renderer_readiness(
                        runtime_renderer_readiness);
                    active_session = session;
                }
                // Complete the stop/publication handoff before entering a
                // potentially blocking handshake. Either stop() observes the
                // published session and interrupts its transport, or this
                // worker observes stop and owns complete cleanup here.
                if (stopping()) {
                    session->close();
                    clear(session);
                    break;
                }
                phase.store(
                    NodeSessionServiceState::Resuming,
                    std::memory_order_release);

                auto resumed = session->resume();
                if (!resumed) {
                    const auto decision = classify_session_error(resumed.error());
                    session->close();
                    clear(session);
                    last_failure.store(decision.failure, std::memory_order_release);
                    if (!decision.retry) {
                        latch_terminal(decision.failure);
                        break;
                    }
                    if (!wait_backoff(backoff)) {
                        break;
                    }
                    backoff = grow_backoff(
                        backoff, config.reconnect_max_delay);
                    continue;
                }

                {
                    const std::lock_guard lock{resource_mutex};
                    observed_director_epoch = session->director_epoch();
                }
                counters.successful_resumes.fetch_add(
                    1U, std::memory_order_relaxed);
                last_failure.store(
                    NodeSessionServiceFailure::None,
                    std::memory_order_release);
                backoff = config.reconnect_initial_delay;

                std::atomic_bool writer_failed{};
                std::atomic_bool writer_retry{};
                std::atomic<NodeSessionServiceFailure> writer_failure{
                    NodeSessionServiceFailure::None};
                std::thread writer;
                try {
                    writer = std::thread{
                        [this,
                         session,
                         &writer_failed,
                         &writer_retry,
                         &writer_failure] {
                            writer_loop(
                                session,
                                writer_failed,
                                writer_retry,
                                writer_failure);
                        }};
                } catch (const std::system_error&) {
                    session->close();
                    clear(session);
                    latch_terminal(NodeSessionServiceFailure::Internal);
                    break;
                }

                if (resumed.value().checkpoint_staged) {
                    phase.store(
                        NodeSessionServiceState::CheckpointStaged,
                        std::memory_order_release);
                    while (!stopping()
                           && !writer_failed.load(std::memory_order_acquire)
                           && session->state() != NodeSessionState::Live
                           && session->state() != NodeSessionState::Closed) {
                        std::unique_lock lock{writer_mutex};
                        static_cast<void>(writer_changed.wait_for(
                            lock,
                            std::chrono::milliseconds{25},
                            [this, &session, &writer_failed] {
                                return stopping()
                                    || writer_failed.load(
                                        std::memory_order_acquire)
                                    || session->state() == NodeSessionState::Live
                                    || session->state()
                                        == NodeSessionState::Closed;
                            }));
                    }
                } else {
                    phase.store(
                        NodeSessionServiceState::Live,
                        std::memory_order_release);
                }

                std::optional<FailureDecision> reader_failure;
                while (!writer_failed.load(std::memory_order_acquire)
                       && session->state() == NodeSessionState::Live) {
                    auto inbound = session->receive_live_message();
                    if (!inbound) {
                        reader_failure = classify_session_error(inbound.error());
                        break;
                    }
                    if (std::get_if<ReceivedFrameReport>(&inbound.value())
                        != nullptr) {
                        counters.received_frames.fetch_add(
                            1U, std::memory_order_relaxed);
                        wake_writer();
                    } else if (
                        std::get_if<FrameCommitPermitReport>(&inbound.value())
                        != nullptr) {
                        counters.received_commit_permits.fetch_add(
                            1U, std::memory_order_relaxed);
                    } else if (
                        std::get_if<HeartbeatReceivedReport>(&inbound.value())
                        != nullptr) {
                        conformance_request.store(std::get<HeartbeatReceivedReport>(inbound.value()).conformance_request, std::memory_order_release);
                        counters.received_heartbeats.fetch_add(
                            1U, std::memory_order_relaxed);
                        wake_writer();
                    } else if (
                        std::get_if<TrackedPoseReport>(&inbound.value())
                        != nullptr) {
                        // A pose is answered by drawing the frame it belongs
                        // to, not by a control record, so the writer is left
                        // alone.
                        counters.received_tracked_poses.fetch_add(
                            1U, std::memory_order_relaxed);
                    } else {
                        counters.received_goodbyes.fetch_add(
                            1U, std::memory_order_relaxed);
                        reader_failure = FailureDecision{
                            true,
                            NodeSessionServiceFailure::PeerShutdown};
                        wake_writer();
                        break;
                    }
                }

                wake_writer();
                if (writer.joinable()) {
                    writer.join();
                }
                session->close();
                clear(session);
                if (stopping()) {
                    break;
                }

                FailureDecision decision{
                    true, NodeSessionServiceFailure::Transport};
                if (writer_failed.load(std::memory_order_acquire)) {
                    decision = {
                        writer_retry.load(std::memory_order_acquire),
                        writer_failure.load(std::memory_order_acquire)};
                } else if (reader_failure) {
                    decision = *reader_failure;
                }
                last_failure.store(decision.failure, std::memory_order_release);
                if (!decision.retry) {
                    latch_terminal(decision.failure);
                    break;
                }
                if (!wait_backoff(backoff)) {
                    break;
                }
            }
        } catch (const std::bad_alloc&) {
            latch_terminal(NodeSessionServiceFailure::Internal);
        } catch (const std::length_error&) {
            latch_terminal(NodeSessionServiceFailure::Internal);
        } catch (...) {
            latch_terminal(NodeSessionServiceFailure::Internal);
        }

        running.store(false, std::memory_order_release);
        if (stop_requested.load(std::memory_order_acquire)) {
            phase.store(NodeSessionServiceState::Stopped, std::memory_order_release);
        } else if (terminal_requested.load(std::memory_order_acquire)) {
            phase.store(NodeSessionServiceState::Faulted, std::memory_order_release);
        }
    }

    NodeSessionServiceConfig config;
    cmd::NodeCommandBus* node;
    std::unique_ptr<NodeSessionDialer> owned_dialer;
    NodeSessionDialer* dialer;
    std::shared_ptr<FramedStreamAdmissionBudget> admission_budget;
    mutable std::mutex lifecycle_mutex;
    std::thread supervisor;
    std::atomic_bool running{};
    std::atomic_bool stop_requested{};
    std::atomic_bool terminal_requested{};
    std::atomic<NodeSessionServiceState> phase{NodeSessionServiceState::Stopped};
    std::atomic<NodeSessionServiceFailure> last_failure{
        NodeSessionServiceFailure::None};
    mutable std::mutex resource_mutex;
    std::shared_ptr<NodeResumeSession> active_session;
    Id128 observed_director_epoch{};
    RuntimeAssetAdoption runtime_asset_adoption{};
    RuntimeOutputSync runtime_output_sync{};
    RuntimeTracking runtime_tracking{};
    RuntimeRendererReadiness runtime_renderer_readiness{};
    std::atomic<core::u64> conformance_request{};
    std::mutex backoff_mutex;
    std::condition_variable backoff_changed;
    std::mutex writer_mutex;
    std::condition_variable writer_changed;
    std::atomic_uint64_t writer_generation{};
    AtomicNodeServiceCounters counters;
};

} // namespace detail

NodeSessionService::NodeSessionService(
    NodeSessionServiceConfig config,
    cmd::NodeCommandBus& node,
    NodeSessionDialer* dialer)
    : state_(std::make_unique<detail::NodeSessionServiceStateData>(
          std::move(config), node, dialer)) {}

NodeSessionService::~NodeSessionService() {
    stop();
}

bool NodeSessionService::valid_configuration(
    const NodeSessionServiceConfig& config) noexcept {
    return valid_config(config);
}

core::Result<std::monostate, NodeSessionServiceError>
NodeSessionService::start() {
    const std::lock_guard lock{state_->lifecycle_mutex};
    if (state_->supervisor.joinable()
        || state_->running.load(std::memory_order_acquire)) {
        return StartResult::failure({
            NodeSessionServiceErrorCode::AlreadyRunning});
    }
    if (!valid_config(state_->config)) {
        return StartResult::failure({
            NodeSessionServiceErrorCode::InvalidConfiguration});
    }
    try {
        auto budget = std::make_shared<FramedStreamAdmissionBudget>(
            state_->config.max_receive_bytes);
        const std::lock_guard resource_lock{state_->resource_mutex};
        state_->admission_budget = std::move(budget);
    } catch (const std::bad_alloc&) {
        return StartResult::failure({
            NodeSessionServiceErrorCode::AllocationFailed});
    }

    state_->stop_requested.store(false, std::memory_order_release);
    state_->terminal_requested.store(false, std::memory_order_release);
    state_->last_failure.store(
        NodeSessionServiceFailure::None, std::memory_order_release);
    state_->phase.store(
        NodeSessionServiceState::Connecting, std::memory_order_release);
    state_->running.store(true, std::memory_order_release);
    try {
        state_->supervisor = std::thread{[state = state_.get()] {
            state->run();
        }};
    } catch (const std::system_error&) {
        state_->running.store(false, std::memory_order_release);
        state_->phase.store(
            NodeSessionServiceState::Stopped, std::memory_order_release);
        {
            const std::lock_guard resource_lock{state_->resource_mutex};
            state_->admission_budget.reset();
        }
        return StartResult::failure({
            NodeSessionServiceErrorCode::ThreadStartFailed});
    }
    return StartResult::success(std::monostate{});
}

void NodeSessionService::stop() noexcept {
    const std::lock_guard lifecycle_lock{state_->lifecycle_mutex};
    std::shared_ptr<NodeResumeSession> session;
    bool graceful = false;
    {
        const std::lock_guard lock{state_->resource_mutex};
        session = state_->active_session;
        if (session
            && state_->phase.load(std::memory_order_acquire)
                == NodeSessionServiceState::Live
            && session->state() == NodeSessionState::Live) {
            auto goodbye = session->request_goodbye(
                GoodbyeReason::NormalShutdown);
            if (goodbye) {
                graceful = true;
                state_->phase.store(
                    NodeSessionServiceState::Draining,
                    std::memory_order_release);
            } else {
                session->interrupt_transport();
            }
        }
        // Publish cancellation only after a Live session's Goodbye occupies
        // its exact FIFO slot. The writer can now observe stopping without
        // ever seeing an empty Live queue first.
        state_->stop_requested.store(true, std::memory_order_release);
        if (session && !graceful) {
            session->interrupt_transport();
        }
    }
    state_->dialer->cancel();
    state_->wake_writer();
    state_->backoff_changed.notify_all();
    if (state_->supervisor.joinable()) {
        state_->supervisor.join();
    }
    {
        const std::lock_guard lock{state_->resource_mutex};
        state_->active_session.reset();
        state_->admission_budget.reset();
    }
    state_->running.store(false, std::memory_order_release);
    state_->phase.store(
        NodeSessionServiceState::Stopped, std::memory_order_release);
}

core::Result<std::monostate, NodeSessionBoundaryError>
NodeSessionService::activate_checkpoint() noexcept {
    using Result = core::Result<std::monostate, NodeSessionBoundaryError>;
    try {
        const std::lock_guard lock{state_->resource_mutex};
        if (state_->stop_requested.load(std::memory_order_acquire)) {
            return Result::failure({
                NodeSessionBoundaryErrorCode::ServiceStopped, std::nullopt});
        }
        if (!state_->active_session) {
            return Result::failure({
                NodeSessionBoundaryErrorCode::NoActiveSession, std::nullopt});
        }
        if (state_->phase.load(std::memory_order_acquire)
            != NodeSessionServiceState::CheckpointStaged) {
            return Result::failure({
                NodeSessionBoundaryErrorCode::WrongState, std::nullopt});
        }
        auto activated =
            state_->active_session->activate_checkpoint_boundary();
        if (!activated) {
            const auto code = protocol_code(activated.error());
            if (!benign_boundary_rejection(activated.error())) {
                state_->active_session->close();
                state_->latch_terminal(NodeSessionServiceFailure::Protocol);
                return Result::failure({
                    NodeSessionBoundaryErrorCode::SessionFaulted, code});
            }
            return Result::failure({
                NodeSessionBoundaryErrorCode::SessionRejected, code});
        }
        state_->counters.checkpoint_activations.fetch_add(
            1U, std::memory_order_relaxed);
        state_->wake_writer();
        return Result::success(std::monostate{});
    } catch (...) {
        state_->latch_terminal(NodeSessionServiceFailure::Internal);
        return Result::failure({
            NodeSessionBoundaryErrorCode::SessionFaulted, std::nullopt});
    }
}

core::Result<cmd::CommitResult, NodeSessionBoundaryError>
NodeSessionService::commit_prepared_frame(cmd::FrameNumber frame) noexcept {
    using Result = core::Result<cmd::CommitResult, NodeSessionBoundaryError>;
    try {
        const std::lock_guard lock{state_->resource_mutex};
        if (state_->stop_requested.load(std::memory_order_acquire)) {
            return Result::failure({
                NodeSessionBoundaryErrorCode::ServiceStopped, std::nullopt});
        }
        if (!state_->active_session) {
            return Result::failure({
                NodeSessionBoundaryErrorCode::NoActiveSession, std::nullopt});
        }
        if (state_->phase.load(std::memory_order_acquire)
            != NodeSessionServiceState::Live) {
            return Result::failure({
                NodeSessionBoundaryErrorCode::WrongState, std::nullopt});
        }
        auto committed = state_->active_session->commit_prepared_frame(frame);
        if (!committed) {
            const auto code = protocol_code(committed.error());
            if (!benign_boundary_rejection(committed.error())) {
                state_->active_session->close();
                state_->latch_terminal(NodeSessionServiceFailure::Protocol);
                return Result::failure({
                    NodeSessionBoundaryErrorCode::SessionFaulted, code});
            }
            return Result::failure({
                NodeSessionBoundaryErrorCode::SessionRejected, code});
        }
        state_->counters.committed_frames.fetch_add(
            1U, std::memory_order_relaxed);
        state_->wake_writer();
        return Result::success(committed.value());
    } catch (...) {
        state_->latch_terminal(NodeSessionServiceFailure::Internal);
        std::shared_ptr<NodeResumeSession> session;
        try {
            const std::lock_guard lock{state_->resource_mutex};
            session = state_->active_session;
        } catch (...) {
        }
        if (session) {
            session->close();
        }
        return Result::failure({
            NodeSessionBoundaryErrorCode::SessionFaulted, std::nullopt});
    }
}

std::optional<NodeBoundaryState> NodeSessionService::boundary_state()
    const noexcept {
    try {
        std::shared_ptr<NodeResumeSession> session;
        {
            const std::lock_guard lock{state_->resource_mutex};
            if (state_->stop_requested.load(std::memory_order_acquire)) {
                return std::nullopt;
            }
            session = state_->active_session;
        }
        if (!session) {
            return std::nullopt;
        }
        return session->boundary_state();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<TrackedCameraFrame>
NodeSessionService::take_tracked_camera_pose() noexcept {
    try {
        std::shared_ptr<NodeResumeSession> session;
        {
            const std::lock_guard lock{state_->resource_mutex};
            if (state_->stop_requested.load(std::memory_order_acquire)) {
                return std::nullopt;
            }
            session = state_->active_session;
        }
        if (!session) {
            return std::nullopt;
        }
        return session->take_tracked_camera_pose();
    } catch (...) {
        return std::nullopt;
    }
}

core::u64 NodeSessionService::tracked_poses_coalesced() const noexcept {
    std::shared_ptr<NodeResumeSession> session;
    {
        const std::lock_guard lock{state_->resource_mutex};
        session = state_->active_session;
    }
    return session ? session->tracked_poses_coalesced() : 0U;
}

void NodeSessionService::publish_runtime_asset_adoption(
    RuntimeAssetAdoption adoption) noexcept {
    const std::lock_guard lock{state_->resource_mutex};
    try {
        RuntimeAssetAdoption session_copy = adoption;
        state_->runtime_asset_adoption = std::move(adoption);
        if (state_->active_session) {
            state_->active_session->publish_runtime_asset_adoption(
                std::move(session_copy));
        }
    } catch (const std::bad_alloc&) {
        // Keep the last complete report; a partial adoption would be a lie.
    }
}

void NodeSessionService::publish_runtime_output_sync(
    RuntimeOutputSync sync) noexcept {
    const std::lock_guard lock{state_->resource_mutex};
    state_->runtime_output_sync = sync;
    if (state_->active_session) {
        state_->active_session->publish_runtime_output_sync(sync);
    }
}

void NodeSessionService::publish_runtime_tracking(
    RuntimeTracking tracking) noexcept {
    const std::lock_guard lock{state_->resource_mutex};
    // Held here as well as on the session, so a reconnect replays the last
    // report rather than telling the Director this node has never tracked.
    state_->runtime_tracking = tracking;
    if (state_->active_session) {
        state_->active_session->publish_runtime_tracking(tracking);
    }
}

void NodeSessionService::publish_runtime_renderer_readiness(
    RuntimeRendererReadiness readiness) noexcept {
    const std::lock_guard lock{state_->resource_mutex};
    state_->runtime_renderer_readiness = readiness;
    if (state_->active_session) {
        state_->active_session->publish_runtime_renderer_readiness(readiness);
    }
}

core::u64 NodeSessionService::conformance_request() const noexcept {
    return state_ ? state_->conformance_request.load(std::memory_order_acquire) : 0U;
}

bool NodeSessionService::running() const noexcept {
    return state_->running.load(std::memory_order_acquire);
}

NodeSessionServiceSnapshot NodeSessionService::snapshot() const noexcept {
    NodeSessionServiceSnapshot result{
        state_->phase.load(std::memory_order_acquire),
        state_->last_failure.load(std::memory_order_acquire),
        {},
        0U,
        0U,
        0U,
    };
    try {
        std::shared_ptr<NodeResumeSession> session;
        {
            const std::lock_guard lock{state_->resource_mutex};
            result.director_epoch = state_->observed_director_epoch;
            session = state_->active_session;
        }
        if (session) {
            result.inflight_frames = session->inflight_frames();
            result.permitted_frames = session->permitted_frames();
            result.pending_acknowledgements =
                session->pending_frame_acknowledgements();
        }
    } catch (...) {
        result.state = NodeSessionServiceState::Faulted;
        result.last_failure = NodeSessionServiceFailure::Internal;
    }
    return result;
}

NodeSessionServiceCounters NodeSessionService::counters() const noexcept {
    return {
        state_->counters.connection_attempts.load(std::memory_order_relaxed),
        state_->counters.connect_failures.load(std::memory_order_relaxed),
        state_->counters.successful_resumes.load(std::memory_order_relaxed),
        state_->counters.reconnect_waits.load(std::memory_order_relaxed),
        state_->counters.received_frames.load(std::memory_order_relaxed),
        state_->counters.received_commit_permits.load(std::memory_order_relaxed),
        state_->counters.received_heartbeats.load(std::memory_order_relaxed),
        state_->counters.received_tracked_poses.load(
            std::memory_order_relaxed),
        state_->counters.received_goodbyes.load(std::memory_order_relaxed),
        state_->counters.flushed_acknowledgements.load(std::memory_order_relaxed),
        state_->counters.graceful_goodbyes.load(std::memory_order_relaxed),
        state_->counters.committed_frames.load(std::memory_order_relaxed),
        state_->counters.checkpoint_activations.load(std::memory_order_relaxed),
        state_->counters.terminal_failures.load(std::memory_order_relaxed),
        state_->counters.unsupported_live_controls.load(
            std::memory_order_relaxed),
    };
}

std::size_t NodeSessionService::receive_capacity_bytes() const noexcept {
    try {
        const std::lock_guard lock{state_->resource_mutex};
        return state_->admission_budget
            ? state_->admission_budget->capacity_bytes()
            : 0U;
    } catch (...) {
        return 0U;
    }
}

std::size_t NodeSessionService::reserved_receive_bytes() const noexcept {
    try {
        const std::lock_guard lock{state_->resource_mutex};
        return state_->admission_budget
            ? state_->admission_budget->reserved_bytes()
            : 0U;
    } catch (...) {
        return 0U;
    }
}

} // namespace mediaserver::net
