#include <mediaserver/net/cluster_lifecycle.hpp>

#include <asio/ip/address_v4.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace net = mediaserver::net;
using namespace std::chrono_literals;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line) +
            ": check failed: " + expression};
    }
}

#define LIFECYCLE_CHECK(expression) \
    check(static_cast<bool>(expression), #expression, __LINE__)

constexpr std::string_view shared_secret{
    "test-only lifecycle secret 4c8f0b9a"};

class FakeProcessHost final : public net::ClusterProcessHost {
public:
    [[nodiscard]] net::ClusterLifecycleReply apply(
        net::ClusterLifecycleCommand command) noexcept override {
        ++calls;
        switch (command) {
        case net::ClusterLifecycleCommand::Status:
            return reply(net::ClusterLifecycleOutcome::NoChange, "status");
        case net::ClusterLifecycleCommand::Start:
            if (state == net::ClusterProcessState::Running) {
                return reply(net::ClusterLifecycleOutcome::NoChange,
                             "already running");
            }
            state = net::ClusterProcessState::Running;
            ++generation;
            return reply(net::ClusterLifecycleOutcome::Applied, "started");
        case net::ClusterLifecycleCommand::Restart:
            state = net::ClusterProcessState::Running;
            ++generation;
            return reply(net::ClusterLifecycleOutcome::Applied, "restarted");
        case net::ClusterLifecycleCommand::Stop:
            if (state == net::ClusterProcessState::Stopped) {
                return reply(net::ClusterLifecycleOutcome::NoChange,
                             "already stopped");
            }
            state = net::ClusterProcessState::Stopped;
            return reply(net::ClusterLifecycleOutcome::Applied, "stopped");
        }
        return reply(net::ClusterLifecycleOutcome::Failed,
                     "invalid command");
    }

    std::size_t calls{};
    mediaserver::core::u64 generation{};
    net::ClusterProcessState state{net::ClusterProcessState::Stopped};

private:
    [[nodiscard]] net::ClusterLifecycleReply reply(
        net::ClusterLifecycleOutcome outcome, std::string message) const {
        return net::ClusterLifecycleReply{
            outcome, state, 0U, generation, std::move(message)};
    }
};

class SlowProcessHost final : public net::ClusterProcessHost {
public:
    [[nodiscard]] net::ClusterLifecycleReply apply(
        net::ClusterLifecycleCommand) noexcept override {
        std::this_thread::sleep_for(200ms);
        return net::ClusterLifecycleReply{
            net::ClusterLifecycleOutcome::Applied,
            net::ClusterProcessState::Stopped, 0U, 1U, "stopped"};
    }
};

class FakeMachineInfoProvider final : public net::ClusterMachineInfoProvider {
public:
    explicit FakeMachineInfoProvider(net::ClusterMachineInfo value)
        : value_(std::make_shared<const net::ClusterMachineInfo>(
              std::move(value))) {
    }

    [[nodiscard]] std::shared_ptr<const net::ClusterMachineInfo>
    snapshot() noexcept override {
        calls.fetch_add(1U, std::memory_order_relaxed);
        return value_;
    }

    std::atomic_size_t calls{};

private:
    std::shared_ptr<const net::ClusterMachineInfo> value_;
};

class FakeAssetReadinessProvider final
    : public net::ClusterAssetReadinessProvider {
public:
    [[nodiscard]] std::optional<net::ClusterAssetReadinessState>
    probe(const net::ClusterAssetIdentity& identity,
          std::stop_token) noexcept override {
        calls.fetch_add(1U, std::memory_order_relaxed);
        if (fail) {
            return std::nullopt;
        }
        for (const auto& [known, state] : values) {
            if (known == identity) {
                return state;
            }
        }
        return net::ClusterAssetReadinessState::Missing;
    }

    std::atomic_size_t calls{};
    bool fail{};
    std::vector<std::pair<net::ClusterAssetIdentity,
                          net::ClusterAssetReadinessState>> values;
};

class SlowAssetReadinessProvider final
    : public net::ClusterAssetReadinessProvider {
public:
    [[nodiscard]] std::optional<net::ClusterAssetReadinessState>
    probe(const net::ClusterAssetIdentity&,
          std::stop_token stop) noexcept override {
        calls.fetch_add(1U, std::memory_order_relaxed);
        entered.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!release.load(std::memory_order_acquire) &&
               !stop.stop_requested() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        if (stop.stop_requested()) {
            cancelled.store(true, std::memory_order_release);
            return std::nullopt;
        }
        return net::ClusterAssetReadinessState::Ready;
    }

    std::atomic_bool entered{};
    std::atomic_bool release{};
    std::atomic_bool cancelled{};
    std::atomic_size_t calls{};
};

[[nodiscard]] net::ClusterAssetIdentity asset_identity(
    mediaserver::core::u8 seed,
    mediaserver::core::u64 byte_size) {
    net::ClusterAssetIdentity identity{};
    for (std::size_t index = 0U; index < identity.sha256.size(); ++index) {
        identity.sha256[index] = static_cast<mediaserver::core::u8>(
            seed + static_cast<mediaserver::core::u8>(index));
    }
    identity.byte_size = byte_size;
    return identity;
}

[[nodiscard]] net::ClusterMachineInfo representative_machine_info() {
    net::ClusterMachineInfo value{};
    value.availability = net::ClusterMachineInfoAvailability::Available;
    value.display_state = net::ClusterDisplayEnumerationState::Available;
    value.node_index = 999U;
    value.logical_cpu_count = 32U;
    value.sample_generation = 17U;
    value.uptime_ms = 123'456U;
    value.physical_memory_bytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;
    value.available_memory_bytes = 40ULL * 1024ULL * 1024ULL * 1024ULL;
    value.host_name = "render-node-a";
    value.operating_system = "Windows x64";
    value.message = "fresh snapshot";
    value.adapters.push_back(net::ClusterGpuAdapterInfo{
        0x1234U, 0x10deU, 0x2684U, 0x0001U, 161U,
        24ULL * 1024ULL * 1024ULL * 1024ULL,
        32ULL * 1024ULL * 1024ULL * 1024ULL, false, false,
        "GPU 0"});
    value.display_targets.push_back(net::ClusterDisplayTargetInfo{
        0x1234U, 3U, 1U, 0x4c2dU, 0x1234U,
        net::ClusterVideoOutputTechnology::DisplayPort, true, true, 3840U,
        2160U, 60'000U, 1'001U, 0, 0, "LED processor"});
    return value;
}

[[nodiscard]] net::ClusterLifecycleClientConfig client_config(
    std::string secret = std::string{shared_secret}) {
    return net::ClusterLifecycleClientConfig{
        std::move(secret), 500ms, 500ms, 500ms};
}

struct RunningService final {
    FakeProcessHost host;
    net::ClusterLifecycleService service;
    asio::ip::tcp::endpoint endpoint;

    explicit RunningService(
        mediaserver::core::u32 node_index = 7U,
        net::ClusterMachineInfoProvider* machine_info = nullptr,
        net::ClusterAssetReadinessProvider* asset_readiness = nullptr)
        : service(net::ClusterLifecycleServiceConfig{
                      {asio::ip::address_v4::loopback(), 0U}, node_index,
                      std::string{shared_secret}, 500ms, 500ms},
                  host, machine_info, asset_readiness) {
        auto started = service.start();
        LIFECYCLE_CHECK(started);
        endpoint = started.value();
    }
};

void numeric_endpoint_validation_is_explicit() {
    const auto ipv4 = net::parse_lifecycle_endpoint("127.0.0.1:47840");
    LIFECYCLE_CHECK(ipv4.has_value());
    LIFECYCLE_CHECK(ipv4->port() == 47840U);
    LIFECYCLE_CHECK(ipv4->address().is_loopback());

    const auto ipv6 = net::parse_lifecycle_endpoint("[::1]:47841");
    LIFECYCLE_CHECK(ipv6.has_value());
    LIFECYCLE_CHECK(ipv6->port() == 47841U);

    for (const std::string_view invalid : {
             "", "localhost:47840", "127.0.0.1", "127.0.0.1:0",
             "0.0.0.0:47840", "239.1.2.3:47840", "::1:47840",
             "255.255.255.255:47840", "[::1]", "[::1]:70000",
             "127.0.0.1:port"}) {
        LIFECYCLE_CHECK(!net::parse_lifecycle_endpoint(invalid).has_value());
    }
}

[[nodiscard]] std::array<std::byte, 44U> read_challenge(
    asio::ip::tcp::socket& socket) {
    std::array<std::byte, 44U> challenge{};
    std::error_code error;
    const std::size_t read = asio::read(socket, asio::buffer(challenge), error);
    LIFECYCLE_CHECK(!error);
    LIFECYCLE_CHECK(read == challenge.size());
    return challenge;
}

void stale_nonce_replay_is_rejected_before_host_dispatch() {
    RunningService fixture;
    asio::io_context context;
    asio::ip::tcp::socket first{context};
    first.connect(fixture.endpoint);
    const auto first_challenge = read_challenge(first);
    std::error_code ignored;
    first.close(ignored);

    asio::ip::tcp::socket second{context};
    second.connect(fixture.endpoint);
    const auto second_challenge = read_challenge(second);
    LIFECYCLE_CHECK(!std::equal(first_challenge.begin() + 12,
                               first_challenge.end(),
                               second_challenge.begin() + 12));

    // Fixed v1 request layout, deliberately carrying the prior connection's
    // nonce. The zero tag is never considered because nonce freshness fails.
    std::array<std::byte, 88U> replay{};
    replay[0U] = std::byte{84U};
    replay[4U] = std::byte{'M'};
    replay[5U] = std::byte{'S'};
    replay[6U] = std::byte{'L'};
    replay[7U] = std::byte{'C'};
    replay[8U] = std::byte{1U};
    replay[9U] = std::byte{2U};
    replay[10U] = std::byte{0U};
    replay[12U] = std::byte{7U};
    replay[16U] = std::byte{1U};
    std::copy(first_challenge.begin() + 12, first_challenge.end(),
              replay.begin() + 24);
    LIFECYCLE_CHECK(asio::write(second, asio::buffer(replay)) == replay.size());
    second.close(ignored);

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (fixture.service.counters().malformed_requests == 0U &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    LIFECYCLE_CHECK(fixture.service.counters().malformed_requests == 1U);
    LIFECYCLE_CHECK(fixture.host.calls == 0U);
}

void hmac_sha256_matches_rfc_known_answer() {
    LIFECYCLE_CHECK(net::detail::cluster_lifecycle_crypto_known_answer());
}

void stalled_handshake_honours_the_client_deadline() {
    asio::io_context context;
    asio::ip::tcp::acceptor listener{
        context, {asio::ip::address_v4::loopback(), 0U}};
    const auto endpoint = listener.local_endpoint();
    std::thread stalled_peer{[&] {
        asio::ip::tcp::socket socket{context};
        std::error_code error;
        listener.accept(socket, error);
        if (!error) {
            std::this_thread::sleep_for(250ms);
        }
    }};
    const auto begin = std::chrono::steady_clock::now();
    auto result = net::request_cluster_lifecycle(
        endpoint, 0U, net::ClusterLifecycleCommand::Status,
        net::ClusterLifecycleClientConfig{
            std::string{shared_secret}, 100ms, 60ms, 60ms});
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    stalled_peer.join();
    LIFECYCLE_CHECK(!result);
    LIFECYCLE_CHECK(result.error().code ==
                    net::ClusterLifecycleClientErrorCode::TimedOut);
    LIFECYCLE_CHECK(elapsed < 500ms);
}

void authenticated_fixed_commands_round_trip() {
    RunningService fixture;

    auto status = net::request_cluster_lifecycle(
        fixture.endpoint, 7U, net::ClusterLifecycleCommand::Status,
        client_config());
    LIFECYCLE_CHECK(status);
    LIFECYCLE_CHECK(status.value().state == net::ClusterProcessState::Stopped);
    LIFECYCLE_CHECK(status.value().generation == 0U);

    auto started = net::request_cluster_lifecycle(
        fixture.endpoint, 7U, net::ClusterLifecycleCommand::Start,
        client_config());
    LIFECYCLE_CHECK(started);
    LIFECYCLE_CHECK(started.value().outcome ==
                    net::ClusterLifecycleOutcome::Applied);
    LIFECYCLE_CHECK(started.value().state == net::ClusterProcessState::Running);
    LIFECYCLE_CHECK(started.value().generation == 1U);

    auto duplicate_start = net::request_cluster_lifecycle(
        fixture.endpoint, 7U, net::ClusterLifecycleCommand::Start,
        client_config());
    LIFECYCLE_CHECK(duplicate_start);
    LIFECYCLE_CHECK(duplicate_start.value().outcome ==
                    net::ClusterLifecycleOutcome::NoChange);
    LIFECYCLE_CHECK(duplicate_start.value().generation == 1U);

    auto restarted = net::request_cluster_lifecycle(
        fixture.endpoint, 7U, net::ClusterLifecycleCommand::Restart,
        client_config());
    LIFECYCLE_CHECK(restarted);
    LIFECYCLE_CHECK(restarted.value().state ==
                    net::ClusterProcessState::Running);
    LIFECYCLE_CHECK(restarted.value().generation == 2U);

    auto stopped = net::request_cluster_lifecycle(
        fixture.endpoint, 7U, net::ClusterLifecycleCommand::Stop,
        client_config());
    LIFECYCLE_CHECK(stopped);
    LIFECYCLE_CHECK(stopped.value().state == net::ClusterProcessState::Stopped);
    LIFECYCLE_CHECK(fixture.host.calls == 5U);

    const auto counters = fixture.service.counters();
    LIFECYCLE_CHECK(counters.authenticated_requests == 5U);
    LIFECYCLE_CHECK(counters.completed_requests == 5U);
    LIFECYCLE_CHECK(counters.authentication_failures == 0U);
}

void authentication_and_node_identity_fail_closed() {
    RunningService fixture{12U};

    auto wrong_secret = net::request_cluster_lifecycle(
        fixture.endpoint, 12U, net::ClusterLifecycleCommand::Start,
        client_config("a different 32-byte shared secret value"));
    LIFECYCLE_CHECK(!wrong_secret);
    LIFECYCLE_CHECK(wrong_secret.error().code ==
                    net::ClusterLifecycleClientErrorCode::AuthenticationFailed);
    LIFECYCLE_CHECK(fixture.host.calls == 0U);

    auto wrong_node = net::request_cluster_lifecycle(
        fixture.endpoint, 11U, net::ClusterLifecycleCommand::Start,
        client_config());
    LIFECYCLE_CHECK(wrong_node);
    LIFECYCLE_CHECK(wrong_node.value().outcome ==
                    net::ClusterLifecycleOutcome::ConfigMismatch);
    LIFECYCLE_CHECK(wrong_node.value().node_index == 12U);
    LIFECYCLE_CHECK(fixture.host.calls == 0U);

    const auto counters = fixture.service.counters();
    LIFECYCLE_CHECK(counters.authentication_failures == 1U);
    LIFECYCLE_CHECK(counters.config_mismatches == 1U);
    LIFECYCLE_CHECK(counters.completed_requests == 0U);
}

void authenticated_machine_inventory_round_trips() {
    auto expected = representative_machine_info();
    FakeMachineInfoProvider provider{expected};
    RunningService fixture{7U, &provider};

    auto result = net::request_cluster_machine_info(
        fixture.endpoint, 7U, client_config());
    LIFECYCLE_CHECK(result);
    expected.node_index = 7U;
    LIFECYCLE_CHECK(result.value() == expected);
    LIFECYCLE_CHECK(provider.calls.load(std::memory_order_relaxed) == 1U);

    // The original lifecycle v1 path remains usable beside inventory.
    auto status = net::request_cluster_lifecycle(
        fixture.endpoint, 7U, net::ClusterLifecycleCommand::Status,
        client_config());
    LIFECYCLE_CHECK(status);
    LIFECYCLE_CHECK(fixture.host.calls == 1U);
}

void inventory_authentication_and_identity_precede_disclosure() {
    FakeMachineInfoProvider provider{representative_machine_info()};
    RunningService fixture{12U, &provider};

    auto wrong_secret = net::request_cluster_machine_info(
        fixture.endpoint, 12U,
        client_config("a different 32-byte shared secret value"));
    LIFECYCLE_CHECK(!wrong_secret);
    LIFECYCLE_CHECK(wrong_secret.error().code ==
                    net::ClusterLifecycleClientErrorCode::AuthenticationFailed);

    auto wrong_node = net::request_cluster_machine_info(
        fixture.endpoint, 11U, client_config());
    LIFECYCLE_CHECK(wrong_node);
    LIFECYCLE_CHECK(
        wrong_node.value().availability ==
        net::ClusterMachineInfoAvailability::ConfigurationMismatch);
    LIFECYCLE_CHECK(wrong_node.value().node_index == 12U);
    LIFECYCLE_CHECK(wrong_node.value().host_name.empty());
    LIFECYCLE_CHECK(wrong_node.value().operating_system.empty());
    LIFECYCLE_CHECK(wrong_node.value().adapters.empty());
    LIFECYCLE_CHECK(wrong_node.value().display_targets.empty());
    LIFECYCLE_CHECK(provider.calls.load(std::memory_order_relaxed) == 0U);
}

void inventory_is_bounded_and_invalid_text_fails_closed() {
    auto oversized = representative_machine_info();
    oversized.host_name.assign(net::max_cluster_machine_host_name_bytes + 20U,
                               'h');
    const auto adapter = oversized.adapters.front();
    oversized.adapters.assign(
        net::max_cluster_machine_gpu_adapters + 4U,
        adapter);
    const auto target = oversized.display_targets.front();
    oversized.display_targets.assign(
        net::max_cluster_machine_display_targets + 4U,
        target);
    oversized.adapters.front().name.assign(
        net::max_cluster_machine_adapter_name_bytes + 10U, 'g');
    FakeMachineInfoProvider bounded_provider{std::move(oversized)};
    RunningService bounded_fixture{3U, &bounded_provider};
    auto bounded = net::request_cluster_machine_info(
        bounded_fixture.endpoint, 3U, client_config());
    LIFECYCLE_CHECK(bounded);
    LIFECYCLE_CHECK(bounded.value().truncated);
    LIFECYCLE_CHECK(bounded.value().host_name.size() ==
                    net::max_cluster_machine_host_name_bytes);
    LIFECYCLE_CHECK(bounded.value().adapters.size() ==
                    net::max_cluster_machine_gpu_adapters);
    LIFECYCLE_CHECK(bounded.value().display_targets.size() ==
                    net::max_cluster_machine_display_targets);
    LIFECYCLE_CHECK(bounded.value().adapters.front().name.size() ==
                    net::max_cluster_machine_adapter_name_bytes);

    auto invalid = representative_machine_info();
    invalid.host_name = "unsafe\nname";
    FakeMachineInfoProvider invalid_provider{std::move(invalid)};
    RunningService invalid_fixture{4U, &invalid_provider};
    auto rejected = net::request_cluster_machine_info(
        invalid_fixture.endpoint, 4U, client_config());
    LIFECYCLE_CHECK(rejected);
    LIFECYCLE_CHECK(rejected.value().availability ==
                    net::ClusterMachineInfoAvailability::Failed);
    LIFECYCLE_CHECK(rejected.value().host_name.empty());
    LIFECYCLE_CHECK(rejected.value().adapters.empty());
}

void absent_inventory_provider_is_explicitly_unavailable() {
    RunningService fixture{5U};
    auto result = net::request_cluster_machine_info(
        fixture.endpoint, 5U, client_config());
    LIFECYCLE_CHECK(result);
    LIFECYCLE_CHECK(result.value().availability ==
                    net::ClusterMachineInfoAvailability::Unavailable);
    LIFECYCLE_CHECK(result.value().display_state ==
                    net::ClusterDisplayEnumerationState::Unsupported);
}

void authenticated_asset_readiness_round_trips_with_aggregates() {
    const auto ready = asset_identity(1U, 1'024U);
    const auto missing = asset_identity(2U, 2'048U);
    const auto corrupt = asset_identity(3U, 4'096U);
    FakeAssetReadinessProvider provider;
    provider.values = {
        {ready, net::ClusterAssetReadinessState::Ready},
        {corrupt, net::ClusterAssetReadinessState::Corrupt}};
    RunningService fixture{7U, nullptr, &provider};

    const std::array identities{ready, missing, corrupt};
    auto result = net::request_cluster_asset_readiness(
        fixture.endpoint, 7U, identities, client_config());
    LIFECYCLE_CHECK(result);
    LIFECYCLE_CHECK(
        result.value().availability ==
        net::ClusterAssetReadinessAvailability::Available);
    LIFECYCLE_CHECK(result.value().node_index == 7U);
    LIFECYCLE_CHECK(result.value().ready == 1U);
    LIFECYCLE_CHECK(result.value().missing == 1U);
    LIFECYCLE_CHECK(result.value().corrupt == 1U);
    const std::vector<net::ClusterAssetReadinessEntry> expected_entries{
        {ready, net::ClusterAssetReadinessState::Ready},
        {missing, net::ClusterAssetReadinessState::Missing},
        {corrupt, net::ClusterAssetReadinessState::Corrupt}};
    LIFECYCLE_CHECK(result.value().entries == expected_entries);
    LIFECYCLE_CHECK(provider.calls.load(std::memory_order_relaxed) == 3U);

    const std::span<const net::ClusterAssetIdentity> none;
    auto empty = net::request_cluster_asset_readiness(
        fixture.endpoint, 7U, none, client_config());
    LIFECYCLE_CHECK(empty);
    LIFECYCLE_CHECK(
        empty.value().availability ==
        net::ClusterAssetReadinessAvailability::Available);
    LIFECYCLE_CHECK(empty.value().entries.empty());
    LIFECYCLE_CHECK(empty.value().ready == 0U);

    // The original lifecycle command remains compatible beside readiness.
    auto status = net::request_cluster_lifecycle(
        fixture.endpoint, 7U, net::ClusterLifecycleCommand::Status,
        client_config());
    LIFECYCLE_CHECK(status);
    LIFECYCLE_CHECK(fixture.host.calls == 1U);
}

void readiness_auth_identity_and_provider_fail_closed() {
    const auto identity = asset_identity(8U, 8'192U);
    const std::array identities{identity};
    FakeAssetReadinessProvider provider;
    provider.values = {
        {identity, net::ClusterAssetReadinessState::Ready}};
    RunningService fixture{12U, nullptr, &provider};

    auto wrong_secret = net::request_cluster_asset_readiness(
        fixture.endpoint, 12U, identities,
        client_config("a different 32-byte shared secret value"));
    LIFECYCLE_CHECK(!wrong_secret);
    LIFECYCLE_CHECK(wrong_secret.error().code ==
                    net::ClusterLifecycleClientErrorCode::
                        AuthenticationFailed);
    LIFECYCLE_CHECK(provider.calls.load(std::memory_order_relaxed) == 0U);

    auto wrong_node = net::request_cluster_asset_readiness(
        fixture.endpoint, 11U, identities, client_config());
    LIFECYCLE_CHECK(wrong_node);
    LIFECYCLE_CHECK(
        wrong_node.value().availability ==
        net::ClusterAssetReadinessAvailability::ConfigurationMismatch);
    LIFECYCLE_CHECK(wrong_node.value().node_index == 12U);
    LIFECYCLE_CHECK(wrong_node.value().entries.empty());
    LIFECYCLE_CHECK(provider.calls.load(std::memory_order_relaxed) == 0U);

    RunningService unavailable{5U};
    auto absent = net::request_cluster_asset_readiness(
        unavailable.endpoint, 5U, identities, client_config());
    LIFECYCLE_CHECK(absent);
    LIFECYCLE_CHECK(
        absent.value().availability ==
        net::ClusterAssetReadinessAvailability::Unavailable);
    LIFECYCLE_CHECK(absent.value().entries.empty());

    FakeAssetReadinessProvider failing;
    failing.fail = true;
    RunningService failed{6U, nullptr, &failing};
    auto failure = net::request_cluster_asset_readiness(
        failed.endpoint, 6U, identities, client_config());
    LIFECYCLE_CHECK(failure);
    LIFECYCLE_CHECK(
        failure.value().availability ==
        net::ClusterAssetReadinessAvailability::Failed);
    LIFECYCLE_CHECK(failure.value().entries.empty());
    LIFECYCLE_CHECK(failing.calls.load(std::memory_order_relaxed) == 1U);
}

void readiness_limits_fail_locally_and_the_maximum_round_trips() {
    FakeAssetReadinessProvider provider;
    RunningService fixture{9U, nullptr, &provider};

    const auto valid = asset_identity(20U, 100U);
    const std::array valid_request{valid};
    auto invalid_deadline = client_config();
    invalid_deadline.asset_readiness_timeout = 0ms;
    auto rejected_deadline = net::request_cluster_asset_readiness(
        fixture.endpoint, 9U, valid_request, invalid_deadline);
    LIFECYCLE_CHECK(!rejected_deadline);
    invalid_deadline.asset_readiness_timeout =
        net::max_cluster_asset_readiness_timeout + 1ms;
    rejected_deadline = net::request_cluster_asset_readiness(
        fixture.endpoint, 9U, valid_request, invalid_deadline);
    LIFECYCLE_CHECK(!rejected_deadline);
    LIFECYCLE_CHECK(fixture.service.counters().accepted_connections == 0U);

    auto zero = valid;
    zero.byte_size = 0U;
    const std::array zero_request{zero};
    auto rejected_zero = net::request_cluster_asset_readiness(
        fixture.endpoint, 9U, zero_request, client_config());
    LIFECYCLE_CHECK(!rejected_zero);
    LIFECYCLE_CHECK(rejected_zero.error().code ==
                    net::ClusterLifecycleClientErrorCode::
                        InvalidConfiguration);

    auto huge = valid;
    huge.byte_size = net::max_cluster_asset_bytes + 1U;
    const std::array huge_request{huge};
    auto rejected_huge = net::request_cluster_asset_readiness(
        fixture.endpoint, 9U, huge_request, client_config());
    LIFECYCLE_CHECK(!rejected_huge);

    const std::array duplicate_request{valid, valid};
    auto rejected_duplicate = net::request_cluster_asset_readiness(
        fixture.endpoint, 9U, duplicate_request, client_config());
    LIFECYCLE_CHECK(!rejected_duplicate);

    std::vector<net::ClusterAssetIdentity> too_many(
        net::max_cluster_asset_readiness_entries + 1U);
    for (std::size_t index = 0U; index < too_many.size(); ++index) {
        too_many[index] = asset_identity(
            static_cast<mediaserver::core::u8>(index),
            static_cast<mediaserver::core::u64>(index) + 1U);
        // Keep all 257 digests unique even though the first byte wraps.
        too_many[index].sha256[30U] = static_cast<mediaserver::core::u8>(
            index >> 8U);
        too_many[index].sha256[31U] =
            static_cast<mediaserver::core::u8>(index & 0xffU);
    }
    auto rejected_count = net::request_cluster_asset_readiness(
        fixture.endpoint, 9U, too_many, client_config());
    LIFECYCLE_CHECK(!rejected_count);
    LIFECYCLE_CHECK(fixture.service.counters().accepted_connections == 0U);

    too_many.resize(net::max_cluster_asset_readiness_entries);
    auto maximum = net::request_cluster_asset_readiness(
        fixture.endpoint, 9U, too_many, client_config());
    LIFECYCLE_CHECK(maximum);
    LIFECYCLE_CHECK(maximum.value().entries.size() ==
                    net::max_cluster_asset_readiness_entries);
    LIFECYCLE_CHECK(maximum.value().missing ==
                    net::max_cluster_asset_readiness_entries);
    LIFECYCLE_CHECK(provider.calls.load(std::memory_order_relaxed) ==
                    net::max_cluster_asset_readiness_entries);
}

void slow_readiness_has_its_own_budget_and_does_not_block_status() {
    SlowAssetReadinessProvider provider;
    RunningService fixture{14U, nullptr, &provider};
    const std::array identities{asset_identity(42U, 4'096U)};

    auto readiness_config = client_config();
    readiness_config.handshake_timeout = 75ms;
    readiness_config.write_timeout = 75ms;
    readiness_config.asset_readiness_timeout = 800ms;
    auto readiness = std::async(std::launch::async, [&] {
        return net::request_cluster_asset_readiness(
            fixture.endpoint, 14U, identities, readiness_config);
    });

    const auto entered_deadline = std::chrono::steady_clock::now() + 1s;
    while (!provider.entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entered_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    LIFECYCLE_CHECK(provider.entered.load(std::memory_order_acquire));

    // Saturate the listener with more readiness requests than it has workers.
    // They must fail fast instead of waiting on the provider mutex and taking
    // every worker away from lifecycle commands.
    auto competing_config = readiness_config;
    competing_config.handshake_timeout = 250ms;
    std::vector<std::future<core::Result<
        net::ClusterAssetReadinessReply,
        net::ClusterLifecycleClientError>>> competing;
    for (std::size_t index = 0U; index < 8U; ++index) {
        competing.push_back(std::async(std::launch::async, [&] {
            return net::request_cluster_asset_readiness(
                fixture.endpoint, 14U, identities, competing_config);
        }));
    }

    // The first hash probe remains deliberately blocked. A serial listener,
    // or a pool whose workers wait on readiness, exceeds this handshake budget.
    auto status_config = client_config();
    status_config.handshake_timeout = 250ms;
    auto status = net::request_cluster_lifecycle(
        fixture.endpoint, 14U, net::ClusterLifecycleCommand::Status,
        status_config);
    LIFECYCLE_CHECK(status);
    LIFECYCLE_CHECK(status.value().state ==
                    net::ClusterProcessState::Stopped);

    for (auto& request : competing) {
        LIFECYCLE_CHECK(request.wait_for(1s) == std::future_status::ready);
        auto result = request.get();
        LIFECYCLE_CHECK(result);
        LIFECYCLE_CHECK(
            result.value().availability ==
            net::ClusterAssetReadinessAvailability::Failed);
    }
    LIFECYCLE_CHECK(provider.calls.load(std::memory_order_relaxed) == 1U);

    // Keep the provider active beyond the 75 ms handshake window. The first
    // request still succeeds because its response has the dedicated budget.
    std::this_thread::sleep_for(100ms);
    provider.release.store(true, std::memory_order_release);
    LIFECYCLE_CHECK(readiness.wait_for(2s) == std::future_status::ready);
    auto result = readiness.get();
    LIFECYCLE_CHECK(result);
    LIFECYCLE_CHECK(
        result.value().availability ==
        net::ClusterAssetReadinessAvailability::Available);
    LIFECYCLE_CHECK(result.value().ready == 1U);
}

void service_stop_cancels_an_active_readiness_probe() {
    SlowAssetReadinessProvider provider;
    RunningService fixture{15U, nullptr, &provider};
    const std::array identities{asset_identity(43U, 8'192U)};
    auto readiness_config = client_config();
    readiness_config.asset_readiness_timeout = 1s;
    auto readiness = std::async(std::launch::async, [&] {
        return net::request_cluster_asset_readiness(
            fixture.endpoint, 15U, identities, readiness_config);
    });
    const auto entered_deadline = std::chrono::steady_clock::now() + 1s;
    while (!provider.entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entered_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    LIFECYCLE_CHECK(provider.entered.load(std::memory_order_acquire));

    const auto stop_started = std::chrono::steady_clock::now();
    fixture.service.stop();
    const auto stop_elapsed =
        std::chrono::steady_clock::now() - stop_started;
    LIFECYCLE_CHECK(stop_elapsed < 250ms);
    LIFECYCLE_CHECK(!fixture.service.running());
    LIFECYCLE_CHECK(provider.cancelled.load(std::memory_order_acquire));
    LIFECYCLE_CHECK(readiness.wait_for(1s) == std::future_status::ready);
    LIFECYCLE_CHECK(!readiness.get());
    const auto counters = fixture.service.counters();
    LIFECYCLE_CHECK(counters.authenticated_requests == 1U);
    LIFECYCLE_CHECK(counters.completed_requests == 0U);
    LIFECYCLE_CHECK(counters.cancelled_requests == 1U);
}

void client_cancellation_interrupts_a_readiness_receive() {
    SlowAssetReadinessProvider provider;
    RunningService fixture{16U, nullptr, &provider};
    const std::array identities{asset_identity(44U, 8'192U)};
    auto readiness_config = client_config();
    readiness_config.asset_readiness_timeout = 10s;
    std::stop_source cancel;
    auto readiness = std::async(std::launch::async, [&] {
        return net::request_cluster_asset_readiness(
            fixture.endpoint, 16U, identities, readiness_config,
            cancel.get_token());
    });
    const auto entered_deadline = std::chrono::steady_clock::now() + 1s;
    while (!provider.entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entered_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    LIFECYCLE_CHECK(provider.entered.load(std::memory_order_acquire));

    const auto cancel_started = std::chrono::steady_clock::now();
    LIFECYCLE_CHECK(cancel.request_stop());
    LIFECYCLE_CHECK(readiness.wait_for(1s) == std::future_status::ready);
    const auto outcome = readiness.get();
    LIFECYCLE_CHECK(!outcome);
    LIFECYCLE_CHECK(outcome.error().code ==
                    net::ClusterLifecycleClientErrorCode::Cancelled);
    LIFECYCLE_CHECK(std::chrono::steady_clock::now() - cancel_started < 1s);

    // The client owns only its socket. Release the independent server-side
    // provider so fixture shutdown does not rely on its cancellation path.
    provider.release.store(true, std::memory_order_release);
}

void service_start_stop_state_is_bounded_and_truthful() {
    FakeProcessHost host;
    net::ClusterLifecycleService invalid{
        net::ClusterLifecycleServiceConfig{
            {asio::ip::address_v4::loopback(), 0U}, 1U, "too short",
            100ms, 100ms},
        host};
    auto rejected = invalid.start();
    LIFECYCLE_CHECK(!rejected);
    LIFECYCLE_CHECK(rejected.error().code ==
                    net::ClusterLifecycleServiceErrorCode::InvalidConfiguration);

    RunningService fixture;
    LIFECYCLE_CHECK(fixture.service.running());
    auto duplicate = fixture.service.start();
    LIFECYCLE_CHECK(!duplicate);
    LIFECYCLE_CHECK(duplicate.error().code ==
                    net::ClusterLifecycleServiceErrorCode::AlreadyRunning);
    fixture.service.stop();
    LIFECYCLE_CHECK(!fixture.service.running());
    LIFECYCLE_CHECK(!fixture.service.local_endpoint().has_value());
}

void response_window_covers_a_bounded_process_transition() {
    LIFECYCLE_CHECK(net::ClusterLifecycleClientConfig{}.handshake_timeout >
                    3s);
    SlowProcessHost host;
    net::ClusterLifecycleService service{
        net::ClusterLifecycleServiceConfig{
            {asio::ip::address_v4::loopback(), 0U}, 4U,
            std::string{shared_secret}, 500ms, 500ms},
        host};
    auto started = service.start();
    LIFECYCLE_CHECK(started);
    auto stopped = net::request_cluster_lifecycle(
        started.value(), 4U, net::ClusterLifecycleCommand::Stop,
        client_config());
    LIFECYCLE_CHECK(stopped);
    LIFECYCLE_CHECK(stopped.value().state ==
                    net::ClusterProcessState::Stopped);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"numeric endpoint validation", &numeric_endpoint_validation_is_explicit},
        {"RFC HMAC-SHA256 known answer",
         &hmac_sha256_matches_rfc_known_answer},
        {"stalled handshake deadline",
         &stalled_handshake_honours_the_client_deadline},
        {"stale nonce replay is rejected",
         &stale_nonce_replay_is_rejected_before_host_dispatch},
        {"authenticated fixed command round trip",
         &authenticated_fixed_commands_round_trip},
        {"authentication and node identity fail closed",
         &authentication_and_node_identity_fail_closed},
        {"authenticated machine inventory round trip",
         &authenticated_machine_inventory_round_trips},
        {"inventory auth and identity precede disclosure",
         &inventory_authentication_and_identity_precede_disclosure},
        {"inventory is bounded and text fails closed",
         &inventory_is_bounded_and_invalid_text_fails_closed},
        {"absent inventory provider is unavailable",
         &absent_inventory_provider_is_explicitly_unavailable},
        {"authenticated asset readiness round trip",
         &authenticated_asset_readiness_round_trips_with_aggregates},
        {"readiness auth identity and provider fail closed",
         &readiness_auth_identity_and_provider_fail_closed},
        {"readiness bounded maximum",
         &readiness_limits_fail_locally_and_the_maximum_round_trips},
        {"slow readiness is isolated and has a dedicated deadline",
         &slow_readiness_has_its_own_budget_and_does_not_block_status},
        {"service stop cancels an active readiness probe",
         &service_stop_cancels_an_active_readiness_probe},
        {"client cancellation interrupts readiness receive",
         &client_cancellation_interrupts_a_readiness_receive},
        {"bounded service lifecycle",
         &service_start_stop_state_is_bounded_and_truthful},
        {"response window covers process transition",
         &response_window_covers_a_bounded_process_transition},
    };
    std::size_t failures{};
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[pass] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
