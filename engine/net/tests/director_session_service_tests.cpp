#include <mediaserver/net/director_frame_barrier.hpp>
#include <mediaserver/net/director_session_service.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

namespace {

namespace command = mediaserver::cmd;
namespace core = mediaserver::core;
namespace document = mediaserver::doc;
namespace net = mediaserver::net;
namespace persist = mediaserver::persist;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define SERVICE_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempWal final {
public:
    TempWal() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("mediaserver-session-service-" + std::to_string(timestamp) + "-"
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

class MemoryReleaseAuthority final : public net::DirectorFrameReleaseAuthority {
public:
    [[nodiscard]] core::u64 next_release_ordinal() const noexcept override {
        const std::lock_guard lock{mutex_};
        return next_ordinal_;
    }

    [[nodiscard]] std::optional<core::u64> release_ordinal(
        const command::StreamCursor& before,
        const command::StreamCursor& after) const noexcept override {
        const std::lock_guard lock{mutex_};
        for (std::size_t index = 0U; index < count_; ++index) {
            if (entries_[index].before == before
                && entries_[index].after == after) {
                return entries_[index].ordinal;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool persist_release(
        const net::PublishedDirectorFrame& frame,
        core::u64 barrier_ordinal) noexcept override {
        const std::lock_guard lock{mutex_};
        if (count_ == entries_.size() || barrier_ordinal != next_ordinal_
            || frame.before_cursor() != frontier_) {
            return false;
        }
        entries_[count_] = Entry{
            frame.before_cursor(),
            frame.after_cursor(),
            barrier_ordinal,
        };
        ++count_;
        ++next_ordinal_;
        frontier_ = frame.after_cursor();
        return true;
    }

    [[nodiscard]] std::size_t decision_count() const noexcept {
        const std::lock_guard lock{mutex_};
        return count_;
    }

    [[nodiscard]] command::StreamCursor frontier() const noexcept {
        const std::lock_guard lock{mutex_};
        return frontier_;
    }

private:
    struct Entry final {
        command::StreamCursor before{};
        command::StreamCursor after{};
        core::u64 ordinal{};
    };

    mutable std::mutex mutex_;
    std::array<Entry, 32U> entries_{};
    std::size_t count_{};
    core::u64 next_ordinal_{1U};
    command::StreamCursor frontier_{
        command::FrameNumber{0U},
        document::Version{0U},
        command::CommandId{1U},
    };
};

[[nodiscard]] net::Id128 id(std::uint64_t low) noexcept {
    return net::Id128{0x6d65646961736572ULL, low};
}

[[nodiscard]] net::Id128 show_id() noexcept { return id(1U); }
[[nodiscard]] net::Id128 director_epoch() noexcept { return id(2U); }

[[nodiscard]] command::StreamCursor genesis_cursor() noexcept {
    return command::StreamCursor{
        command::FrameNumber{0U},
        document::Version{0U},
        command::CommandId{1U},
    };
}

[[nodiscard]] bool same_cursor(
    const command::StreamCursor& left,
    const command::StreamCursor& right) noexcept {
    return left.through_frame == right.through_frame && left.revision == right.revision
        && left.next_command == right.next_command;
}

[[nodiscard]] persist::CommandLog open_log(
    const std::filesystem::path& path) {
    persist::OpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    auto opened = persist::CommandLog::open_or_create(
        path, genesis_cursor(), options);
    SERVICE_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] document::Node make_node(
    std::uint64_t node_id,
    std::int64_t value) {
    return document::Node{
        document::NodeId{node_id},
        "service.node",
        {{"value", value}},
    };
}

void schedule_put(
    command::DirectorScheduler& scheduler,
    std::uint64_t frame) {
    auto scheduled = scheduler.schedule_edit(
        command::PutNode{
            make_node(frame, static_cast<std::int64_t>(frame))},
        command::EraseNode{document::NodeId{frame}},
        command::UndoContextId{1U},
        command::TransactionId{frame},
        command::FrameNumber{frame});
    SERVICE_CHECK(scheduled);
}

template <typename Predicate>
[[nodiscard]] bool wait_until(
    Predicate&& predicate,
    std::chrono::milliseconds timeout = std::chrono::seconds{3}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return predicate();
}

class RecordingObserver final : public net::DirectorSessionObserver {
public:
    [[nodiscard]] bool register_live(
        const net::DirectorLiveRegistration& registration) noexcept override {
        const std::lock_guard lock{mutex_};
        if (registration_count_ >= registrations_.size()) {
            return false;
        }
        registrations_[registration_count_] = registration;
        ++registration_count_;
        return true;
    }

    [[nodiscard]] bool on_ack(
        net::Id128 session_id,
        const net::FrameAcknowledgementReport& acknowledgement) noexcept override {
        const std::lock_guard lock{mutex_};
        if (acknowledgement_count_ >= acks_.size()) {
            return false;
        }
        ack_sessions_[acknowledgement_count_] = session_id;
        acks_[acknowledgement_count_] = acknowledgement;
        ++acknowledgement_count_;
        if (acknowledgement.kind == net::FrameAcknowledgementKind::Prepared) {
            if (permit_count_ >= permits_.size()) {
                return false;
            }
            permit_sessions_[permit_count_] = session_id;
            permits_[permit_count_] = net::FrameCommitPermitIntent{
                acknowledgement.after.through_frame.value(),
                acknowledgement.acknowledged_sequence,
                acknowledgement.before,
                acknowledgement.after,
            };
            ++permit_count_;
        }
        return true;
    }

    void unregister(
        net::Id128 session_id,
        net::DirectorSessionLeaveReason reason) noexcept override {
        const std::lock_guard lock{mutex_};
        if (leave_count_ < leaves_.size()) {
            leave_sessions_[leave_count_] = session_id;
            leaves_[leave_count_] = reason;
            ++leave_count_;
        }
    }

    [[nodiscard]] bool coordinates_commit_permits() const noexcept override {
        return true;
    }

    [[nodiscard]] bool heartbeat_released_through(
        command::StreamCursor& output) const noexcept override {
        const std::lock_guard lock{mutex_};
        frontier_observed_after_leave_ = leave_count_ != 0U;
        output = genesis_cursor();
        return true;
    }

    [[nodiscard]] bool frontier_observed_after_leave() const noexcept {
        const std::lock_guard lock{mutex_};
        return frontier_observed_after_leave_;
    }

    [[nodiscard]] CommitPermitPoll try_take_commit_permit(
        net::Id128 session_id,
        net::FrameCommitPermitIntent& output) noexcept override {
        const std::lock_guard lock{mutex_};
        for (std::size_t index = 0U; index < permit_count_; ++index) {
            if (!permit_claimed_[index]
                && permit_sessions_[index] == session_id) {
                permit_claimed_[index] = true;
                output = permits_[index];
                return CommitPermitPoll::Ready;
            }
        }
        return CommitPermitPoll::Empty;
    }

    [[nodiscard]] std::size_t registration_count() const noexcept {
        const std::lock_guard lock{mutex_};
        return registration_count_;
    }

    [[nodiscard]] net::DirectorLiveRegistration registration(
        std::size_t index) const noexcept {
        const std::lock_guard lock{mutex_};
        return registrations_[index];
    }

    [[nodiscard]] std::size_t acknowledgement_count() const noexcept {
        const std::lock_guard lock{mutex_};
        return acknowledgement_count_;
    }

    [[nodiscard]] net::FrameAcknowledgementReport acknowledgement(
        std::size_t index) const noexcept {
        const std::lock_guard lock{mutex_};
        return acks_[index];
    }

    [[nodiscard]] net::Id128 acknowledgement_session(
        std::size_t index) const noexcept {
        const std::lock_guard lock{mutex_};
        return ack_sessions_[index];
    }

    [[nodiscard]] std::size_t leave_count() const noexcept {
        const std::lock_guard lock{mutex_};
        return leave_count_;
    }

    [[nodiscard]] net::DirectorSessionLeaveReason leave(
        std::size_t index) const noexcept {
        const std::lock_guard lock{mutex_};
        return leaves_[index];
    }

private:
    mutable std::mutex mutex_;
    std::array<net::DirectorLiveRegistration, 8U> registrations_{};
    std::array<net::Id128, 32U> ack_sessions_{};
    std::array<net::FrameAcknowledgementReport, 32U> acks_{};
    std::array<net::Id128, 8U> leave_sessions_{};
    std::array<net::DirectorSessionLeaveReason, 8U> leaves_{};
    std::size_t registration_count_{};
    std::size_t acknowledgement_count_{};
    std::size_t leave_count_{};
    std::array<net::Id128, 32U> permit_sessions_{};
    std::array<net::FrameCommitPermitIntent, 32U> permits_{};
    std::array<bool, 32U> permit_claimed_{};
    std::size_t permit_count_{};
    mutable bool frontier_observed_after_leave_{};
};

[[nodiscard]] net::DirectorSessionServiceConfig service_config(
    std::size_t max_workers = 4U,
    std::size_t receive_bytes = 144U * 1024U * 1024U) {
    net::DirectorSessionServiceConfig config;
    config.bind_endpoint = asio::ip::tcp::endpoint{
        asio::ip::address_v4::loopback(), 0U};
    config.max_workers = max_workers;
    config.session_id_namespace = 0x73657373696f6e73ULL;
    config.first_session_sequence = 1U;
    config.max_receive_bytes = receive_bytes;
    config.session_defaults.show_id = show_id();
    config.session_defaults.director_epoch = director_epoch();
    config.session_defaults.io.handshake_timeout = std::chrono::seconds{2};
    config.session_defaults.io.read_timeout = std::chrono::milliseconds::zero();
    config.session_defaults.io.write_timeout = std::chrono::seconds{2};
    return config;
}

[[nodiscard]] net::NodeSessionConfig node_config(
    net::Id128 node_id = id(10U),
    net::Id128 incarnation = id(11U)) {
    net::NodeSessionConfig config;
    config.show_id = show_id();
    config.node_id = node_id;
    config.incarnation = incarnation;
    config.last_director_epoch = director_epoch();
    config.io.handshake_timeout = std::chrono::seconds{2};
    config.io.read_timeout = std::chrono::seconds{2};
    config.io.write_timeout = std::chrono::seconds{2};
    return config;
}

class ServiceFixture final {
public:
    ServiceFixture(
        net::DirectorSessionHubLimits hub_limits = {},
        net::DirectorSessionServiceConfig config = service_config())
        : log_(open_log(temporary_.path())),
          durable_(scheduler_, log_),
          hub_(durable_, hub_limits),
          leases_(config.max_workers),
          service_(
              std::move(config), hub_, durable_, leases_, &observer_) {}

    [[nodiscard]] asio::ip::tcp::endpoint start() {
        auto started = service_.start();
        SERVICE_CHECK(started);
        return started.value();
    }

    [[nodiscard]] net::PublishedDirectorFrame publish(std::uint64_t frame) {
        schedule_put(scheduler_, frame);
        auto sealed = hub_.seal_and_publish(command::FrameNumber{frame});
        SERVICE_CHECK(sealed);
        return sealed.value().frame;
    }

    TempWal temporary_;
    persist::CommandLog log_;
    command::DirectorScheduler scheduler_;
    persist::DurableDirector durable_;
    net::DirectorSessionHub hub_;
    net::NodeSessionLeaseRegistry leases_;
    RecordingObserver observer_;
    net::DirectorSessionService service_;
};

class BarrierServiceFixture final {
public:
    BarrierServiceFixture()
        : log_(open_log(temporary_.path())),
          durable_(scheduler_, log_),
          hub_(
              durable_,
              net::DirectorSessionHubLimits{
                  2U,
                  1U,
                  128U * 1024U * 1024U,
              }),
          leases_(2U),
          release_authority_(),
          barrier_(
              genesis_cursor(),
              release_authority_,
              net::DirectorFrameBarrierLimits{2U, 16U}),
          config_(one_frame_config()),
          service_(config_, hub_, durable_, leases_, &barrier_) {}

    static net::DirectorSessionServiceConfig one_frame_config() {
        auto config = service_config(2U);
        config.session_defaults.limits.max_inflight_frames = 1U;
        return config;
    }

    TempWal temporary_;
    persist::CommandLog log_;
    command::DirectorScheduler scheduler_;
    persist::DurableDirector durable_;
    net::DirectorSessionHub hub_;
    net::NodeSessionLeaseRegistry leases_;
    MemoryReleaseAuthority release_authority_;
    net::DirectorFrameBarrier barrier_;
    net::DirectorSessionServiceConfig config_;
    net::DirectorSessionService service_;
};

[[nodiscard]] asio::ip::tcp::socket connect_socket(
    asio::io_context& context,
    const asio::ip::tcp::endpoint& endpoint) {
    asio::ip::tcp::socket socket{context};
    std::error_code error;
    socket.connect(endpoint, error);
    SERVICE_CHECK(!error);
    return socket;
}

void send_session_message(
    net::TcpFramedStream& stream,
    const net::SessionMessage& message) {
    auto encoded = net::encode_session(message);
    SERVICE_CHECK(encoded);
    SERVICE_CHECK(stream.send(encoded.value()));
}

[[nodiscard]] net::SessionMessage receive_session_message(
    net::TcpFramedStream& stream) {
    auto received = stream.receive();
    SERVICE_CHECK(received);
    auto decoded = net::decode_session(received.value());
    SERVICE_CHECK(decoded);
    return std::move(decoded).value();
}

[[nodiscard]] net::ServerWelcome complete_raw_up_to_date_resume(
    net::TcpFramedStream& stream,
    net::Id128 node_id = id(90U)) {
    send_session_message(
        stream,
        net::SessionMessage{net::ClientHello{
            net::current_session_format_version,
            net::current_session_format_version,
            show_id(),
            node_id,
            id(91U),
            director_epoch(),
            genesis_cursor(),
            false,
            net::default_session_limits,
        }});
    const auto welcome_message = receive_session_message(stream);
    const auto* welcome =
        std::get_if<net::ServerWelcome>(&welcome_message);
    SERVICE_CHECK(welcome != nullptr);
    const auto sync_message = receive_session_message(stream);
    const auto* sync = std::get_if<net::SyncComplete>(&sync_message);
    SERVICE_CHECK(sync != nullptr);
    SERVICE_CHECK(sync->sequence == 1U);
    send_session_message(
        stream,
        net::SessionMessage{net::ResumeAck{
            welcome->session_id,
            1U,
            welcome->target,
        }});
    return *welcome;
}

[[nodiscard]] bool socket_rejected(asio::ip::tcp::socket& socket) {
    std::error_code error;
    socket.non_blocking(true, error);
    SERVICE_CHECK(!error);
    std::array<std::byte, 1U> byte{};
    return wait_until(
        [&] {
            const auto read = socket.read_some(asio::buffer(byte), error);
            static_cast<void>(read);
            if (!error || error == asio::error::would_block
                || error == asio::error::try_again) {
                error.clear();
                return false;
            }
            return true;
        },
        std::chrono::seconds{2});
}

void put_u32_le(
    std::span<std::byte, 4U> bytes,
    std::uint32_t value) noexcept {
    bytes[0U] = static_cast<std::byte>(value & 0xFFU);
    bytes[1U] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    bytes[2U] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    bytes[3U] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

void accept_resume_and_live_delivery_flow() {
    ServiceFixture fixture;
    const auto endpoint = fixture.start();
    SERVICE_CHECK(fixture.service_.running());
    SERVICE_CHECK(fixture.service_.receive_capacity_bytes()
                  == 144U * 1024U * 1024U);

    asio::io_context context;
    command::NodeCommandBus node_bus;
    net::NodeResumeSession node{
        connect_socket(context, endpoint), node_bus, node_config()};
    auto resumed = node.resume();
    SERVICE_CHECK(resumed);
    SERVICE_CHECK(resumed.value().mode == net::ResumeMode::UpToDate);
    SERVICE_CHECK(wait_until([&] {
        return fixture.observer_.registration_count() == 1U;
    }));
    const auto registration = fixture.observer_.registration(0U);
    SERVICE_CHECK(registration.session_id.high == 0x73657373696f6e73ULL);
    SERVICE_CHECK(registration.session_id.low == 1U);
    SERVICE_CHECK(registration.peer.node_id == id(10U));
    SERVICE_CHECK(same_cursor(registration.committed_cursor, genesis_cursor()));

    static_cast<void>(fixture.publish(1U));
    auto received = node.receive_live_frame();
    SERVICE_CHECK(received);
    SERVICE_CHECK(received.value().after.through_frame
                  == command::FrameNumber{1U});
    SERVICE_CHECK(wait_until([&] {
        return fixture.observer_.acknowledgement_count() >= 1U;
    }));
    SERVICE_CHECK(
        fixture.observer_.acknowledgement(0U).kind
        == net::FrameAcknowledgementKind::Prepared);
    SERVICE_CHECK(
        fixture.observer_.acknowledgement_session(0U)
        == registration.session_id);

    SERVICE_CHECK(node.receive_commit_permit());
    SERVICE_CHECK(
        node.commit_prepared_frame(command::FrameNumber{1U}));
    SERVICE_CHECK(node.flush_frame_acknowledgements());
    SERVICE_CHECK(wait_until([&] {
        return fixture.observer_.acknowledgement_count() == 2U;
    }));
    SERVICE_CHECK(
        fixture.observer_.acknowledgement(1U).kind
        == net::FrameAcknowledgementKind::Committed);
    SERVICE_CHECK(same_cursor(node_bus.cursor(), received.value().after));

    node.close();
    SERVICE_CHECK(wait_until([&] {
        return fixture.observer_.leave_count() == 1U
            && fixture.service_.active_workers() == 0U;
    }));
    SERVICE_CHECK(
        fixture.observer_.leave(0U)
        == net::DirectorSessionLeaveReason::SessionFailure);
    fixture.service_.stop();
    SERVICE_CHECK(fixture.service_.reserved_receive_bytes() == 0U);
}

void worker_capacity_accepts_then_rejects_without_growth() {
    ServiceFixture fixture{{}, service_config(1U)};
    const auto endpoint = fixture.start();
    asio::io_context first_context;
    asio::io_context second_context;
    auto first = connect_socket(first_context, endpoint);
    SERVICE_CHECK(wait_until([&] {
        return fixture.service_.active_workers() == 1U
            && fixture.hub_.active_subscriptions() == 1U;
    }));
    auto second = connect_socket(second_context, endpoint);
    SERVICE_CHECK(wait_until([&] {
        return fixture.service_.counters().capacity_rejections == 1U;
    }));
    SERVICE_CHECK(socket_rejected(second));
    SERVICE_CHECK(fixture.service_.active_workers() == 1U);
    fixture.service_.stop();
    SERVICE_CHECK(fixture.service_.active_workers() == 0U);
    SERVICE_CHECK(fixture.hub_.active_subscriptions() == 0U);
}

void aggregate_receive_budget_is_validated_and_shared() {
    auto unbounded_write = service_config();
    unbounded_write.session_defaults.io.write_timeout =
        std::chrono::milliseconds::zero();
    ServiceFixture unsafe_stop{{}, std::move(unbounded_write)};
    auto unsafe_started = unsafe_stop.service_.start();
    SERVICE_CHECK(!unsafe_started);
    SERVICE_CHECK(
        unsafe_started.error().code
        == net::DirectorSessionServiceErrorCode::InvalidConfiguration);

    auto invalid = service_config(2U, 1U);
    ServiceFixture fixture{{}, std::move(invalid)};
    auto started = fixture.service_.start();
    SERVICE_CHECK(!started);
    SERVICE_CHECK(
        started.error().code
        == net::DirectorSessionServiceErrorCode::InvalidConfiguration);
    SERVICE_CHECK(fixture.service_.receive_capacity_bytes() == 0U);
    SERVICE_CHECK(!fixture.service_.local_endpoint().has_value());

    constexpr auto aggregate_capacity = 72U * 1024U * 1024U;
    constexpr auto claimed_body = 40U * 1024U * 1024U;
    ServiceFixture shared{{}, service_config(2U, aggregate_capacity)};
    const auto endpoint = shared.start();
    asio::io_context first_context;
    asio::io_context second_context;
    auto first = connect_socket(first_context, endpoint);
    auto second = connect_socket(second_context, endpoint);
    SERVICE_CHECK(wait_until([&] {
        return shared.service_.active_workers() == 2U;
    }));

    std::array<std::byte, 4U> prefix{};
    put_u32_le(prefix, claimed_body);
    std::error_code error;
    SERVICE_CHECK(asio::write(first, asio::buffer(prefix), error)
                  == prefix.size());
    SERVICE_CHECK(!error);
    SERVICE_CHECK(wait_until([&] {
        return shared.service_.reserved_receive_bytes()
            == claimed_body + prefix.size();
    }));
    SERVICE_CHECK(asio::write(second, asio::buffer(prefix), error)
                  == prefix.size());
    SERVICE_CHECK(!error);
    SERVICE_CHECK(socket_rejected(second));
    SERVICE_CHECK(wait_until([&] {
        return shared.service_.active_workers() == 1U;
    }));
    SERVICE_CHECK(shared.service_.reserved_receive_bytes()
                  == claimed_body + prefix.size());
    shared.service_.stop();
    SERVICE_CHECK(shared.service_.reserved_receive_bytes() == 0U);
}

void a_held_frame_retries_after_the_commit_window_reopens() {
    auto config = service_config(2U);
    config.session_defaults.limits.max_inflight_frames = 1U;
    ServiceFixture fixture{{}, std::move(config)};
    const auto endpoint = fixture.start();
    asio::io_context context;
    command::NodeCommandBus node_bus;
    net::NodeResumeSession node{
        connect_socket(context, endpoint), node_bus, node_config()};
    SERVICE_CHECK(node.resume());
    SERVICE_CHECK(wait_until([&] {
        return fixture.observer_.registration_count() == 1U;
    }));

    static_cast<void>(fixture.publish(1U));
    SERVICE_CHECK(node.receive_live_frame());
    SERVICE_CHECK(wait_until([&] {
        return fixture.observer_.acknowledgement_count() == 1U;
    }));
    static_cast<void>(fixture.publish(2U));

    auto locally_full = node.receive_live_frame();
    SERVICE_CHECK(!locally_full);
    const auto* capacity = std::get_if<net::SessionProtocolFailure>(
        &locally_full.error());
    SERVICE_CHECK(capacity != nullptr);
    SERVICE_CHECK(
        capacity->code == net::SessionProtocolErrorCode::InflightWindowFull);

    SERVICE_CHECK(node.receive_commit_permit());
    SERVICE_CHECK(
        node.commit_prepared_frame(command::FrameNumber{1U}));
    SERVICE_CHECK(node.flush_frame_acknowledgements());
    SERVICE_CHECK(node.receive_live_frame());
    SERVICE_CHECK(node.receive_commit_permit());
    SERVICE_CHECK(
        node.commit_prepared_frame(command::FrameNumber{2U}));
    SERVICE_CHECK(node.flush_frame_acknowledgements());
    SERVICE_CHECK(wait_until([&] {
        return fixture.observer_.acknowledgement_count() == 4U;
    }));
    SERVICE_CHECK(node_bus.cursor().through_frame == command::FrameNumber{2U});
    SERVICE_CHECK(fixture.service_.counters().resync_disconnects == 0U);
    node.close();
    fixture.service_.stop();
}

void backlog_overflow_releases_lease_and_reconnects_at_new_session() {
    ServiceFixture fixture{
        net::DirectorSessionHubLimits{
            2U,
            1U,
            128U * 1024U * 1024U,
        },
        service_config(2U),
    };
    const auto endpoint = fixture.start();

    asio::io_context stalled_context;
    net::TcpFramedStream stalled{
        connect_socket(stalled_context, endpoint),
        net::FramedStreamLimits{
            72U * 1024U * 1024U,
            std::chrono::seconds{2},
            std::chrono::seconds{2},
            std::chrono::seconds{2},
        }};
    send_session_message(
        stalled,
        net::SessionMessage{net::ClientHello{
            net::current_session_format_version,
            net::current_session_format_version,
            show_id(),
            id(10U),
            id(11U),
            director_epoch(),
            genesis_cursor(),
            false,
            net::default_session_limits,
        }});
    auto welcome_message = receive_session_message(stalled);
    const auto* welcome = std::get_if<net::ServerWelcome>(&welcome_message);
    SERVICE_CHECK(welcome != nullptr);
    SERVICE_CHECK(welcome->session_id.low == 1U);
    auto sync_message = receive_session_message(stalled);
    SERVICE_CHECK(std::get_if<net::SyncComplete>(&sync_message) != nullptr);
    SERVICE_CHECK(wait_until([&] {
        return fixture.leases_.active_sessions() == 1U;
    }));

    static_cast<void>(fixture.publish(1U));
    schedule_put(fixture.scheduler_, 2U);
    auto overflow = fixture.hub_.seal_and_publish(command::FrameNumber{2U});
    SERVICE_CHECK(overflow);
    SERVICE_CHECK(overflow.value().publication.resync_required == 1U);
    SERVICE_CHECK(wait_until([&] {
        return fixture.service_.counters().resync_disconnects == 1U
            && fixture.service_.active_workers() == 0U
            && fixture.leases_.active_sessions() == 0U;
    }));

    asio::io_context reconnect_context;
    command::NodeCommandBus node_bus;
    net::NodeResumeSession reconnected{
        connect_socket(reconnect_context, endpoint), node_bus, node_config()};
    auto resumed = reconnected.resume();
    SERVICE_CHECK(resumed);
    SERVICE_CHECK(resumed.value().mode == net::ResumeMode::Replay);
    SERVICE_CHECK(resumed.value().frames == 2U);
    SERVICE_CHECK(
        reconnected.commit_prepared_frame(command::FrameNumber{1U}));
    SERVICE_CHECK(reconnected.flush_frame_acknowledgements());
    SERVICE_CHECK(
        reconnected.commit_prepared_frame(command::FrameNumber{2U}));
    SERVICE_CHECK(reconnected.flush_frame_acknowledgements());
    SERVICE_CHECK(wait_until([&] {
        return fixture.observer_.registration_count() == 1U;
    }));
    SERVICE_CHECK(fixture.observer_.registration(0U).session_id.low == 2U);
    SERVICE_CHECK(fixture.observer_.acknowledgement_count() == 0U);
    SERVICE_CHECK(fixture.leases_.active_sessions() == 1U);

    static_cast<void>(fixture.publish(3U));
    SERVICE_CHECK(reconnected.receive_live_frame());
    SERVICE_CHECK(reconnected.receive_commit_permit());
    SERVICE_CHECK(
        reconnected.commit_prepared_frame(command::FrameNumber{3U}));
    SERVICE_CHECK(reconnected.flush_frame_acknowledgements());
    SERVICE_CHECK(wait_until([&] {
        return fixture.observer_.acknowledgement_count() == 2U;
    }));
    reconnected.close();
    fixture.service_.stop();
    SERVICE_CHECK(fixture.leases_.active_sessions() == 0U);
}

void tracked_live_member_overflow_degrades_the_existing_barrier() {
    BarrierServiceFixture fixture;
    auto started = fixture.service_.start();
    SERVICE_CHECK(started);
    asio::io_context context;
    command::NodeCommandBus node_bus;
    net::NodeResumeSession node{
        connect_socket(context, started.value()), node_bus, node_config()};
    SERVICE_CHECK(node.resume());
    SERVICE_CHECK(wait_until([&] {
        return fixture.barrier_.active_sessions() == 1U;
    }));

    std::optional<net::DirectorFrameBarrierId> first_barrier;
    bool overflowed = false;
    for (std::uint64_t frame = 1U; frame <= 8U; ++frame) {
        schedule_put(fixture.scheduler_, frame);
        auto tracked = fixture.barrier_.seal_and_track(
            fixture.hub_, command::FrameNumber{frame});
        SERVICE_CHECK(tracked);
        if (!first_barrier) {
            first_barrier = tracked.value().barrier;
        }
        if (tracked.value().seal.publication.resync_required == 1U) {
            overflowed = true;
            break;
        }
    }
    SERVICE_CHECK(overflowed);
    SERVICE_CHECK(first_barrier.has_value());
    SERVICE_CHECK(wait_until([&] {
        return fixture.service_.counters().resync_disconnects == 1U
            && fixture.service_.active_workers() == 0U;
    }));

    auto waited = fixture.barrier_.wait_for(
        *first_barrier,
        net::DirectorFrameBarrierPhase::Committed,
        std::chrono::seconds{1});
    SERVICE_CHECK(waited);
    SERVICE_CHECK(
        waited.value().status
        == net::DirectorFrameBarrierWaitStatus::Degraded);
    SERVICE_CHECK(waited.value().summary.members == 1U);
    SERVICE_CHECK(waited.value().summary.lost == 1U);

    std::array<net::DirectorFrameBarrierLostSession, 1U> lost{};
    auto copied = fixture.barrier_.copy_lost_sessions(*first_barrier, lost);
    SERVICE_CHECK(copied);
    SERVICE_CHECK(copied.value().total == 1U);
    SERVICE_CHECK(
        lost[0U].reason == net::DirectorSessionLeaveReason::ResyncRequired);
    node.close();
    fixture.service_.stop();
}

void two_node_service_drains_permits_only_after_durable_quorum() {
    BarrierServiceFixture fixture;
    auto started = fixture.service_.start();
    SERVICE_CHECK(started);

    asio::io_context first_context;
    asio::io_context second_context;
    command::NodeCommandBus first_bus;
    command::NodeCommandBus second_bus;
    net::NodeResumeSession first{
        connect_socket(first_context, started.value()),
        first_bus,
        node_config(id(10U), id(11U))};
    net::NodeResumeSession second{
        connect_socket(second_context, started.value()),
        second_bus,
        node_config(id(12U), id(13U))};
    SERVICE_CHECK(first.resume());
    SERVICE_CHECK(second.resume());
    SERVICE_CHECK(wait_until([&] {
        return fixture.barrier_.active_sessions() == 2U;
    }));

    schedule_put(fixture.scheduler_, 1U);
    auto tracked = fixture.barrier_.seal_and_track(
        fixture.hub_, command::FrameNumber{1U});
    SERVICE_CHECK(tracked);
    const auto barrier_id = tracked.value().barrier;

    SERVICE_CHECK(first.receive_live_frame());
    SERVICE_CHECK(wait_until([&] {
        auto summary = fixture.barrier_.snapshot(barrier_id);
        return summary && summary.value().prepared == 1U;
    }));
    SERVICE_CHECK(fixture.release_authority_.decision_count() == 0U);
    SERVICE_CHECK(
        fixture.barrier_.released_through() == genesis_cursor());

    SERVICE_CHECK(second.receive_live_frame());
    SERVICE_CHECK(wait_until([&] {
        return fixture.release_authority_.decision_count() == 1U;
    }));
    SERVICE_CHECK(
        fixture.release_authority_.frontier()
        == tracked.value().seal.frame.after_cursor());

    auto first_permit = first.receive_commit_permit();
    auto second_permit = second.receive_commit_permit();
    SERVICE_CHECK(first_permit);
    SERVICE_CHECK(second_permit);
    SERVICE_CHECK(
        first_permit.value().barrier_ordinal == barrier_id.ordinal);
    SERVICE_CHECK(
        second_permit.value().barrier_ordinal == barrier_id.ordinal);
    SERVICE_CHECK(first.commit_prepared_frame(command::FrameNumber{1U}));
    SERVICE_CHECK(second.commit_prepared_frame(command::FrameNumber{1U}));
    SERVICE_CHECK(first.flush_frame_acknowledgements());
    SERVICE_CHECK(second.flush_frame_acknowledgements());
    SERVICE_CHECK(wait_until([&] {
        auto summary = fixture.barrier_.snapshot(barrier_id);
        return summary && summary.value().committed == 2U;
    }));
    SERVICE_CHECK(first_bus.cursor().through_frame == command::FrameNumber{1U});
    SERVICE_CHECK(second_bus.cursor().through_frame == command::FrameNumber{1U});

    first.close();
    second.close();
    fixture.service_.stop();
}

void stop_wakes_workers_with_a_commit_permit_held_at_quorum() {
    BarrierServiceFixture fixture;
    auto started = fixture.service_.start();
    SERVICE_CHECK(started);

    asio::io_context first_context;
    asio::io_context second_context;
    command::NodeCommandBus first_bus;
    command::NodeCommandBus second_bus;
    net::NodeResumeSession first{
        connect_socket(first_context, started.value()),
        first_bus,
        node_config(id(10U), id(11U))};
    net::NodeResumeSession second{
        connect_socket(second_context, started.value()),
        second_bus,
        node_config(id(12U), id(13U))};
    SERVICE_CHECK(first.resume());
    SERVICE_CHECK(second.resume());
    SERVICE_CHECK(wait_until([&] {
        return fixture.barrier_.active_sessions() == 2U;
    }));

    schedule_put(fixture.scheduler_, 1U);
    auto tracked = fixture.barrier_.seal_and_track(
        fixture.hub_, command::FrameNumber{1U});
    SERVICE_CHECK(tracked);
    SERVICE_CHECK(first.receive_live_frame());
    SERVICE_CHECK(wait_until([&] {
        auto summary = fixture.barrier_.snapshot(tracked.value().barrier);
        return summary && summary.value().prepared == 1U;
    }));
    SERVICE_CHECK(fixture.release_authority_.decision_count() == 0U);

    const auto stop_start = std::chrono::steady_clock::now();
    fixture.service_.stop();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_start;
    SERVICE_CHECK(stop_elapsed < std::chrono::milliseconds{500});
    SERVICE_CHECK(fixture.service_.active_workers() == 0U);
    SERVICE_CHECK(fixture.release_authority_.decision_count() == 0U);
    auto summary = fixture.barrier_.snapshot(tracked.value().barrier);
    SERVICE_CHECK(summary);
    SERVICE_CHECK(summary.value().degraded());
    SERVICE_CHECK(summary.value().lost == 2U);
    first.close();
    second.close();
}

void service_rejects_a_missing_commit_permit_coordinator() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler scheduler;
    persist::DurableDirector durable{scheduler, log};
    net::DirectorSessionHub hub{durable};
    net::NodeSessionLeaseRegistry leases{1U};
    net::DirectorSessionService service{
        service_config(1U), hub, durable, leases, nullptr};
    auto started = service.start();
    SERVICE_CHECK(!started);
    SERVICE_CHECK(
        started.error().code
        == net::DirectorSessionServiceErrorCode::InvalidConfiguration);
    SERVICE_CHECK(!service.running());
    SERVICE_CHECK(!service.local_endpoint().has_value());
}

void hub_shutdown_unregisters_and_wakes_a_live_worker() {
    ServiceFixture fixture;
    const auto endpoint = fixture.start();
    asio::io_context context;
    command::NodeCommandBus node_bus;
    net::NodeResumeSession node{
        connect_socket(context, endpoint), node_bus, node_config()};
    SERVICE_CHECK(node.resume());
    SERVICE_CHECK(wait_until([&] {
        return fixture.observer_.registration_count() == 1U;
    }));

    fixture.hub_.close();
    SERVICE_CHECK(wait_until([&] {
        return fixture.service_.active_workers() == 0U
            && fixture.observer_.leave_count() == 1U;
    }));
    SERVICE_CHECK(
        fixture.observer_.leave(0U)
        == net::DirectorSessionLeaveReason::HubClosed);
    SERVICE_CHECK(
        fixture.service_.counters().hub_closed_disconnects == 1U);
    SERVICE_CHECK(fixture.leases_.active_sessions() == 0U);
    node.close();
    fixture.service_.stop();
}

void missing_heartbeat_ack_times_out_despite_frame_traffic() {
    auto config = service_config(1U);
    config.heartbeat_interval = std::chrono::milliseconds{500};
    config.heartbeat_response_timeout = std::chrono::seconds{1};
    ServiceFixture fixture{{}, std::move(config)};
    const auto endpoint = fixture.start();
    asio::io_context context;
    command::NodeCommandBus node_bus;
    net::NodeResumeSession node{
        connect_socket(context, endpoint), node_bus, node_config()};
    SERVICE_CHECK(node.resume());
    static_cast<void>(fixture.publish(1U));
    SERVICE_CHECK(node.receive_live_frame());
    SERVICE_CHECK(node.receive_commit_permit());
    SERVICE_CHECK(wait_until([&] {
        return fixture.service_.counters().sent_heartbeats == 1U;
    }));

    SERVICE_CHECK(
        node.commit_prepared_frame(command::FrameNumber{1U}));
    auto committed = node.flush_frame_acknowledgements();
    SERVICE_CHECK(committed && committed.value().flushed == 1U);
    SERVICE_CHECK(wait_until([&] {
        return fixture.observer_.acknowledgement_count() == 2U;
    }));

    auto heartbeat = node.receive_live_message();
    SERVICE_CHECK(heartbeat);
    SERVICE_CHECK(
        std::get_if<net::HeartbeatReceivedReport>(&heartbeat.value())
        != nullptr);
    SERVICE_CHECK(node.pending_control_messages() == 1U);

    SERVICE_CHECK(wait_until([&] {
        return fixture.service_.counters().heartbeat_timeouts == 1U
            && fixture.service_.active_workers() == 0U;
    }));
    SERVICE_CHECK(fixture.observer_.acknowledgement_count() == 2U);
    SERVICE_CHECK(fixture.observer_.leave_count() == 1U);
    node.close();
    fixture.service_.stop();
}

void wrong_heartbeat_ack_closes_the_session() {
    auto config = service_config(1U);
    config.heartbeat_interval = std::chrono::milliseconds{20};
    config.heartbeat_response_timeout = std::chrono::milliseconds{500};
    ServiceFixture fixture{{}, std::move(config)};
    const auto endpoint = fixture.start();
    asio::io_context context;
    net::TcpFramedStream stream{connect_socket(context, endpoint)};
    const auto welcome = complete_raw_up_to_date_resume(stream);
    const auto heartbeat_message = receive_session_message(stream);
    const auto* heartbeat = std::get_if<net::Heartbeat>(&heartbeat_message);
    SERVICE_CHECK(heartbeat != nullptr);
    send_session_message(
        stream,
        net::SessionMessage{net::HeartbeatAck{
            welcome.session_id,
            welcome.director_epoch,
            2U,
            heartbeat->sequence + 1U,
            genesis_cursor(),
        }});

    SERVICE_CHECK(wait_until([&] {
        return fixture.service_.counters().session_failures == 1U
            && fixture.service_.active_workers() == 0U;
    }));
    SERVICE_CHECK(
        fixture.service_.counters().validated_heartbeat_acknowledgements
        == 0U);
    SERVICE_CHECK(fixture.service_.counters().heartbeat_timeouts == 0U);
    stream.close();
    fixture.service_.stop();
}

void stop_wakes_idle_accept_and_live_worker() {
    ServiceFixture fixture;
    static_cast<void>(fixture.start());
    const auto idle_start = std::chrono::steady_clock::now();
    fixture.service_.stop();
    const auto idle_elapsed = std::chrono::steady_clock::now() - idle_start;
    SERVICE_CHECK(idle_elapsed < std::chrono::milliseconds{500});
    SERVICE_CHECK(!fixture.service_.local_endpoint().has_value());

    const auto endpoint = fixture.start();
    asio::io_context context;
    command::NodeCommandBus node_bus;
    net::NodeResumeSession node{
        connect_socket(context, endpoint), node_bus, node_config()};
    SERVICE_CHECK(node.resume());
    SERVICE_CHECK(wait_until([&] {
        return fixture.observer_.registration_count() == 1U;
    }));

    const auto worker_start = std::chrono::steady_clock::now();
    fixture.service_.stop();
    const auto worker_elapsed = std::chrono::steady_clock::now() - worker_start;
    SERVICE_CHECK(worker_elapsed < std::chrono::milliseconds{500});
    SERVICE_CHECK(!fixture.service_.running());
    SERVICE_CHECK(fixture.service_.active_workers() == 0U);
    SERVICE_CHECK(fixture.observer_.leave_count() == 1U);
    SERVICE_CHECK(
        fixture.observer_.leave(0U)
        == net::DirectorSessionLeaveReason::ServiceStopped);
    SERVICE_CHECK(fixture.leases_.active_sessions() == 0U);
    SERVICE_CHECK(fixture.service_.reserved_receive_bytes() == 0U);
    SERVICE_CHECK(fixture.observer_.frontier_observed_after_leave());
    SERVICE_CHECK(fixture.service_.counters().graceful_goodbyes == 1U);
    auto goodbye = node.receive_live_message();
    SERVICE_CHECK(goodbye);
    SERVICE_CHECK(
        std::get_if<net::GoodbyeReport>(&goodbye.value()) != nullptr);
    SERVICE_CHECK(node.state() == net::NodeSessionState::Draining);
}

void session_id_exhaustion_is_explicit() {
    auto config = service_config(1U);
    config.first_session_sequence = std::numeric_limits<core::u64>::max();
    ServiceFixture fixture{{}, std::move(config)};
    const auto endpoint = fixture.start();

    asio::io_context first_context;
    auto first = connect_socket(first_context, endpoint);
    SERVICE_CHECK(wait_until([&] {
        return fixture.service_.active_workers() == 1U;
    }));
    fixture.service_.stop();

    const auto second_endpoint = fixture.start();
    asio::io_context second_context;
    auto second = connect_socket(second_context, second_endpoint);
    SERVICE_CHECK(wait_until([&] {
        return fixture.service_.counters().session_id_exhaustions == 1U;
    }));
    SERVICE_CHECK(socket_rejected(second));
    SERVICE_CHECK(fixture.service_.active_workers() == 0U);
    fixture.service_.stop();
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"accept resume live delivery", &accept_resume_and_live_delivery_flow},
        {"bounded worker capacity",
         &worker_capacity_accepts_then_rejects_without_growth},
        {"aggregate receive budget", &aggregate_receive_budget_is_validated_and_shared},
        {"held frame window retry",
         &a_held_frame_retries_after_the_commit_window_reopens},
        {"overflow reconnect fencing",
         &backlog_overflow_releases_lease_and_reconnects_at_new_session},
        {"tracked overflow degrades barrier",
         &tracked_live_member_overflow_degrades_the_existing_barrier},
        {"two-node durable permit drain",
         &two_node_service_drains_permits_only_after_durable_quorum},
        {"stop with held commit permit",
         &stop_wakes_workers_with_a_commit_permit_held_at_quorum},
        {"missing coordinator rejected",
         &service_rejects_a_missing_commit_permit_coordinator},
        {"hub shutdown wakes worker",
         &hub_shutdown_unregisters_and_wakes_a_live_worker},
        {"missing heartbeat Ack times out despite traffic",
         &missing_heartbeat_ack_times_out_despite_frame_traffic},
        {"wrong heartbeat Ack closes session",
         &wrong_heartbeat_ack_closes_the_session},
        {"stop wakes accept and worker", &stop_wakes_idle_accept_and_live_worker},
        {"session id exhaustion", &session_id_exhaustion_is_explicit},
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
