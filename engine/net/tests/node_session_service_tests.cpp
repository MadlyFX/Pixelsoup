#include <mediaserver/net/director_frame_barrier.hpp>
#include <mediaserver/net/director_session_service.hpp>
#include <mediaserver/net/durable_director_frame_release_authority.hpp>
#include <mediaserver/net/node_session_service.hpp>
#include <mediaserver/persist/release_ledger.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <asio/ip/address_v4.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

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

#define NODE_SERVICE_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] net::Id128 id(std::uint64_t low) noexcept {
    return net::Id128{0x6e6f646573657276ULL, low};
}

[[nodiscard]] cmd::StreamCursor genesis_cursor() noexcept {
    return {
        cmd::FrameNumber{0U}, doc::Version{0U}, cmd::CommandId{1U}};
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

class TempWal final {
public:
    TempWal() {
        static std::atomic_uint64_t sequence{};
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("mediaserver-node-service-" + std::to_string(stamp) + "-"
               + std::to_string(sequence.fetch_add(1U)) + ".wal");
        ledger_path_ = path_;
        ledger_path_ += ".release";
    }

    ~TempWal() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(ledger_path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
    [[nodiscard]] const std::filesystem::path& ledger_path() const noexcept {
        return ledger_path_;
    }

private:
    std::filesystem::path path_;
    std::filesystem::path ledger_path_;
};

[[nodiscard]] persist::CommandLog open_log(
    const std::filesystem::path& path) {
    persist::OpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    auto opened = persist::CommandLog::open_or_create(
        path, genesis_cursor(), options);
    NODE_SERVICE_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] persist::ReleaseLedger open_ledger(
    const std::filesystem::path& path,
    const persist::CommandLog& log) {
    auto snapshot = log.capture_committed();
    NODE_SERVICE_CHECK(snapshot);
    persist::ReleaseLedgerOpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    auto opened = persist::ReleaseLedger::open_or_create(
        path, snapshot.value(), options);
    NODE_SERVICE_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] net::DirectorSessionServiceConfig director_config() {
    net::DirectorSessionServiceConfig config;
    config.bind_endpoint = {asio::ip::address_v4::loopback(), 0U};
    config.max_workers = 2U;
    config.session_id_namespace = 0x6469726563746f72ULL;
    config.max_receive_bytes = 144U * 1024U * 1024U;
    config.heartbeat_interval = std::chrono::milliseconds{5};
    config.heartbeat_response_timeout = std::chrono::seconds{1};
    config.session_defaults.show_id = id(1U);
    config.session_defaults.director_epoch = id(2U);
    config.session_defaults.io.handshake_timeout = std::chrono::seconds{2};
    config.session_defaults.io.read_timeout = std::chrono::seconds{5};
    config.session_defaults.io.write_timeout = std::chrono::seconds{2};
    return config;
}

[[nodiscard]] net::NodeSessionServiceConfig node_config(
    asio::ip::tcp::endpoint endpoint) {
    net::NodeSessionServiceConfig config;
    config.director_endpoint = std::move(endpoint);
    config.session_defaults.show_id = id(1U);
    config.session_defaults.node_id = id(10U);
    config.session_defaults.incarnation = id(11U);
    config.session_defaults.node_index = 10U;
    config.session_defaults.last_director_epoch = {};
    config.session_defaults.io.handshake_timeout = std::chrono::seconds{2};
    config.session_defaults.io.read_timeout = std::chrono::seconds{5};
    config.session_defaults.io.write_timeout = std::chrono::seconds{2};
    config.connect_timeout = std::chrono::seconds{2};
    config.reconnect_initial_delay = std::chrono::milliseconds{10};
    config.reconnect_max_delay = std::chrono::milliseconds{40};
    return config;
}

class PendingDialer final : public net::NodeSessionDialer {
public:
    [[nodiscard]] net::NodeSessionConnectResult connect(
        const asio::ip::tcp::endpoint&,
        std::chrono::milliseconds,
        const std::atomic_bool& stop_requested) override {
        entered_.store(true, std::memory_order_release);
        while (!cancelled_.load(std::memory_order_acquire)
               && !stop_requested.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return net::NodeSessionConnectResult::failure({
            net::NodeSessionConnectErrorCode::Cancelled, {}});
    }

    void cancel() noexcept override {
        cancelled_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool entered() const noexcept {
        return entered_.load(std::memory_order_acquire);
    }

private:
    std::atomic_bool entered_{};
    std::atomic_bool cancelled_{};
};

class FailingDialer final : public net::NodeSessionDialer {
public:
    [[nodiscard]] net::NodeSessionConnectResult connect(
        const asio::ip::tcp::endpoint&,
        std::chrono::milliseconds,
        const std::atomic_bool&) override {
        attempts_.fetch_add(1U, std::memory_order_acq_rel);
        return net::NodeSessionConnectResult::failure({
            net::NodeSessionConnectErrorCode::ConnectFailed,
            asio::error::connection_refused});
    }

    void cancel() noexcept override {}

    [[nodiscard]] std::uint64_t attempts() const noexcept {
        return attempts_.load(std::memory_order_acquire);
    }

private:
    std::atomic_uint64_t attempts_{};
};

class SilentHandshakeServer final {
public:
    SilentHandshakeServer()
        : acceptor_(
              context_,
              {asio::ip::address_v4::loopback(), 0U}) {
        std::error_code error;
        acceptor_.non_blocking(true, error);
        NODE_SERVICE_CHECK(!error);
        thread_ = std::thread{[this] { run(); }};
    }

    ~SilentHandshakeServer() {
        release_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] asio::ip::tcp::endpoint endpoint() const {
        return acceptor_.local_endpoint();
    }

    [[nodiscard]] bool accepted() const noexcept {
        return accepted_.load(std::memory_order_acquire);
    }

private:
    void run() noexcept {
        asio::ip::tcp::socket socket{context_};
        while (!release_.load(std::memory_order_acquire)) {
            std::error_code error;
            acceptor_.accept(socket, error);
            if (!error) {
                accepted_.store(true, std::memory_order_release);
                while (!release_.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                socket.close(error);
                return;
            }
            if (error != asio::error::would_block
                && error != asio::error::try_again) {
                return;
            }
            std::this_thread::yield();
        }
    }

    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
    std::atomic_bool accepted_{};
    std::atomic_bool release_{};
};

void pending_connect_is_cancelled_by_stop() {
    PendingDialer dialer;
    cmd::NodeCommandBus node;
    auto config = node_config({asio::ip::address_v4::loopback(), 9U});
    config.connect_timeout = std::chrono::seconds{30};
    net::NodeSessionService service{std::move(config), node, &dialer};
    NODE_SERVICE_CHECK(service.start());
    NODE_SERVICE_CHECK(wait_until([&] { return dialer.entered(); }));

    const auto started = std::chrono::steady_clock::now();
    service.stop();
    // Cancellation, not latency. The bound only has to sit far enough below
    // the 30s connect timeout to prove the pending dial was cancelled rather
    // than waited out; a tighter one measures how promptly a loaded machine
    // schedules a join, which is not a property of this code.
    NODE_SERVICE_CHECK(
        std::chrono::steady_clock::now() - started < std::chrono::seconds{5});
    NODE_SERVICE_CHECK(!service.running());
    NODE_SERVICE_CHECK(
        service.snapshot().state == net::NodeSessionServiceState::Stopped);
    NODE_SERVICE_CHECK(service.reserved_receive_bytes() == 0U);
}

void invalid_multicast_configuration_never_dials() {
    FailingDialer dialer;
    cmd::NodeCommandBus node;
    auto config = node_config({
        asio::ip::address_v4{
            asio::ip::address_v4::bytes_type{239U, 1U, 2U, 3U}},
        9U});
    NODE_SERVICE_CHECK(!net::NodeSessionService::valid_configuration(config));
    net::NodeSessionService service{std::move(config), node, &dialer};
    auto started = service.start();
    NODE_SERVICE_CHECK(!started);
    NODE_SERVICE_CHECK(
        started.error().code
        == net::NodeSessionServiceErrorCode::InvalidConfiguration);
    NODE_SERVICE_CHECK(dialer.attempts() == 0U);

    auto unbounded_write = node_config({
        asio::ip::address_v4::loopback(), 9U});
    unbounded_write.session_defaults.io.write_timeout =
        std::chrono::milliseconds::zero();
    NODE_SERVICE_CHECK(
        !net::NodeSessionService::valid_configuration(unbounded_write));

    auto invalid_index = node_config({
        asio::ip::address_v4::loopback(), 9U});
    invalid_index.session_defaults.node_index =
        net::max_session_node_index + 1U;
    NODE_SERVICE_CHECK(
        !net::NodeSessionService::valid_configuration(invalid_index));
}

void transient_connect_failure_backs_off_and_restarts() {
    FailingDialer dialer;
    cmd::NodeCommandBus node;
    auto config = node_config({asio::ip::address_v4::loopback(), 9U});
    config.reconnect_initial_delay = std::chrono::milliseconds{5};
    config.reconnect_max_delay = std::chrono::milliseconds{10};
    net::NodeSessionService service{std::move(config), node, &dialer};
    NODE_SERVICE_CHECK(service.start());
    // Waiting on the failure counter rather than the attempt counter is what
    // makes this observation stable. The supervisor counts an attempt before
    // it dials and the failure after the dial returns, so a sample taken while
    // an attempt is in flight legitimately sees one more attempt than failure -
    // and stopping mid-dial leaves that skew behind for good. Four recorded
    // failures with four recorded backoff waits is the same statement without
    // the in-flight window.
    NODE_SERVICE_CHECK(wait_until([&] {
        const auto counters = service.counters();
        return counters.connect_failures >= 4U
            && counters.reconnect_waits >= 4U;
    }));
    const auto first_run = service.counters();
    // Every attempt failed: a resume is the only other way an attempt can end,
    // and this counter can only leave zero when one succeeds.
    NODE_SERVICE_CHECK(first_run.successful_resumes == 0U);
    NODE_SERVICE_CHECK(
        service.snapshot().last_failure
        == net::NodeSessionServiceFailure::Connect);
    service.stop();

    NODE_SERVICE_CHECK(service.start());
    NODE_SERVICE_CHECK(wait_until([&] {
        return service.counters().connect_failures
            >= first_run.connect_failures + 2U;
    }));
    service.stop();
    NODE_SERVICE_CHECK(!service.running());
    NODE_SERVICE_CHECK(service.counters().successful_resumes == 0U);
}

void pending_handshake_is_interrupted_by_stop() {
    SilentHandshakeServer peer;
    cmd::NodeCommandBus node;
    auto config = node_config(peer.endpoint());
    config.session_defaults.io.handshake_timeout = std::chrono::seconds{30};
    net::NodeSessionService service{std::move(config), node};
    NODE_SERVICE_CHECK(service.start());
    NODE_SERVICE_CHECK(wait_until([&] {
        return peer.accepted()
            && service.snapshot().state
                == net::NodeSessionServiceState::Resuming;
    }));

    const auto started = std::chrono::steady_clock::now();
    service.stop();
    // As above: far enough below the 30s handshake timeout to prove the
    // pending handshake was interrupted rather than waited out.
    NODE_SERVICE_CHECK(
        std::chrono::steady_clock::now() - started < std::chrono::seconds{5});
    NODE_SERVICE_CHECK(!service.running());
    NODE_SERVICE_CHECK(
        service.snapshot().state == net::NodeSessionServiceState::Stopped);
}

void first_checkpoint_then_durable_permit_commit_end_to_end() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    auto ledger = open_ledger(temporary.ledger_path(), log);
    cmd::DirectorScheduler scheduler;
    persist::DurableDirector durable{scheduler, log, ledger};
    net::DirectorSessionHub hub{
        durable,
        net::DirectorSessionHubLimits{2U, 4U, 128U * 1024U * 1024U}};
    net::NodeSessionLeaseRegistry leases{2U};
    net::DurableDirectorFrameReleaseAuthority releases{durable};
    net::DirectorFrameBarrier barrier{
        genesis_cursor(),
        releases,
        net::DirectorFrameBarrierLimits{1U, 8U, 8U}};
    net::DirectorSessionService director{
        director_config(), hub, durable, leases, &barrier};
    auto director_started = director.start();
    NODE_SERVICE_CHECK(director_started);

    cmd::NodeCommandBus node;
    net::NodeSessionService service{
        node_config(director_started.value()), node};
    constexpr core::u64 manifest_digest = 0x1122334455667788ULL;
    const std::array<core::u32, 1U> required_node_indices{10U};
    service.publish_runtime_asset_adoption(
        net::RuntimeAssetAdoption{manifest_digest, true});
    NODE_SERVICE_CHECK(service.start());
    NODE_SERVICE_CHECK(wait_until([&] {
        return service.snapshot().state
            == net::NodeSessionServiceState::CheckpointStaged;
    }));
    NODE_SERVICE_CHECK(node.cursor() == genesis_cursor());
    NODE_SERVICE_CHECK(service.activate_checkpoint());
    NODE_SERVICE_CHECK(wait_until([&] {
        return service.snapshot().state == net::NodeSessionServiceState::Live
            && barrier.active_sessions() == 1U;
    }));
    NODE_SERVICE_CHECK(service.snapshot().director_epoch == id(2U));
    NODE_SERVICE_CHECK(wait_until([&] {
        return service.counters().received_heartbeats >= 10U
            && director.counters().sent_heartbeats >= 10U
            && director.counters().validated_heartbeat_acknowledgements
                >= 10U;
    }));

    NODE_SERVICE_CHECK(barrier.runtime_assets_ready(
        manifest_digest, required_node_indices));
    service.publish_runtime_asset_adoption(
        net::RuntimeAssetAdoption{manifest_digest, false});
    NODE_SERVICE_CHECK(wait_until([&] {
        return !barrier.runtime_assets_ready(
            manifest_digest, required_node_indices);
    }));
    service.publish_runtime_asset_adoption(
        net::RuntimeAssetAdoption{manifest_digest, true});
    NODE_SERVICE_CHECK(wait_until([&] {
        return barrier.runtime_assets_ready(
            manifest_digest, required_node_indices);
    }));

    // A second connection with the same stable logical node identity is a
    // terminal lease fault, not a reconnect storm.
    cmd::NodeCommandBus duplicate_node;
    net::NodeSessionService duplicate{
        node_config(director_started.value()), duplicate_node};
    NODE_SERVICE_CHECK(duplicate.start());
    NODE_SERVICE_CHECK(wait_until([&] {
        return duplicate.snapshot().state
            == net::NodeSessionServiceState::Faulted;
    }));
    NODE_SERVICE_CHECK(
        duplicate.snapshot().last_failure
        == net::NodeSessionServiceFailure::LeaseRejected);
    NODE_SERVICE_CHECK(duplicate.counters().connection_attempts == 1U);
    NODE_SERVICE_CHECK(duplicate.counters().reconnect_waits == 0U);
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    NODE_SERVICE_CHECK(duplicate.counters().connection_attempts == 1U);
    duplicate.stop();

    auto tracked = barrier.seal_and_track(hub, cmd::FrameNumber{1U});
    NODE_SERVICE_CHECK(tracked);
    NODE_SERVICE_CHECK(wait_until([&] {
        return service.counters().received_frames == 1U
            && service.snapshot().permitted_frames == 1U;
    }));
    const auto release_report = ledger.recovery_report();
    NODE_SERVICE_CHECK(release_report.decision_count == 1U);
    NODE_SERVICE_CHECK(release_report.unresolved_wal_frames == 0U);
    NODE_SERVICE_CHECK(release_report.frontier.through_frame == 1U);
    NODE_SERVICE_CHECK(ledger.next_release_ordinal() == 2U);
    NODE_SERVICE_CHECK(
        releases.release_ordinal(
            tracked.value().seal.frame.before_cursor(),
            tracked.value().seal.frame.after_cursor())
        == 1U);

    auto committed = service.commit_prepared_frame(cmd::FrameNumber{1U});
    NODE_SERVICE_CHECK(committed);
    NODE_SERVICE_CHECK(
        committed.value().status == cmd::CommitStatus::Committed);
    NODE_SERVICE_CHECK(wait_until([&] {
        auto summary = barrier.snapshot(tracked.value().barrier);
        return summary && summary.value().committed == 1U;
    }));
    NODE_SERVICE_CHECK(node.cursor().through_frame == cmd::FrameNumber{1U});
    NODE_SERVICE_CHECK(service.counters().received_commit_permits == 1U);
    NODE_SERVICE_CHECK(service.counters().flushed_acknowledgements == 2U);

    service.stop();
    NODE_SERVICE_CHECK(service.counters().graceful_goodbyes == 1U);
    // No polling: the Director counts the Goodbye when it reads it, and it
    // only closes the connection afterwards. The node's stop() completes its
    // own close on that peer close, so by the time it returns the Director has
    // already accounted for the record. Waiting for this would only hide a
    // Goodbye that never arrived, which is exactly the failure worth catching.
    NODE_SERVICE_CHECK(director.counters().graceful_goodbyes >= 1U);
    director.stop();
    hub.close();
    NODE_SERVICE_CHECK(leases.active_sessions() == 0U);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"invalid multicast config",
         &invalid_multicast_configuration_never_dials},
        {"transient connect backoff restart",
         &transient_connect_failure_backs_off_and_restarts},
        {"pending connect stop", &pending_connect_is_cancelled_by_stop},
        {"pending handshake stop", &pending_handshake_is_interrupted_by_stop},
        {"checkpoint durable permit E2E",
         &first_checkpoint_then_durable_permit_commit_end_to_end},
    };
    std::size_t passed = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            ++passed;
            std::cout << "[pass] " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << passed << '/' << tests.size() << " tests passed\n";
    return passed == tests.size() ? 0 : 1;
}
