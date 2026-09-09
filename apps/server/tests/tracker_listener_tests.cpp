#include "tracker_listener.hpp"

#include <mediaserver/track/freed.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

namespace {

namespace core = mediaserver::core;
namespace scene = mediaserver::scene;
namespace server = mediaserver::server;
namespace track = mediaserver::track;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define LISTENER_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] scene::TrackingSourceSettings freed_source(
    core::u32 port, core::u32 camera = 0U) noexcept {
    scene::TrackingSourceSettings source{};
    source.enabled = true;
    source.protocol = scene::TrackerProtocolKind::FreeD;
    source.port = port;
    source.camera_id = camera;
    return source;
}

[[nodiscard]] std::vector<std::byte> freed_datagram(
    core::u8 camera, core::i32 pan, core::i32 x) {
    std::vector<std::byte> bytes;
    const auto write24 = [&bytes](core::i32 value) {
        const auto raw = static_cast<core::u32>(value) & 0x00FF'FFFFU;
        bytes.push_back(static_cast<std::byte>((raw >> 16U) & 0xFFU));
        bytes.push_back(static_cast<std::byte>((raw >> 8U) & 0xFFU));
        bytes.push_back(static_cast<std::byte>(raw & 0xFFU));
    };
    bytes.push_back(track::freed_d1_identifier);
    bytes.push_back(static_cast<std::byte>(camera));
    write24(pan);
    write24(0);
    write24(0);
    write24(x);
    write24(0);
    write24(0);
    write24(0);
    write24(0);
    bytes.push_back(std::byte{0});
    bytes.push_back(std::byte{0});
    bytes.push_back(track::freed_d1_checksum(
        std::span<const std::byte>{bytes.data(), bytes.size()}));
    return bytes;
}

void an_unconfigured_listener_is_idle_and_publishes_nothing() {
    server::TrackerListener listener;
    const auto initial = listener.status();
    LISTENER_CHECK(initial.state == server::TrackerListenerState::Idle);
    LISTENER_CHECK(!listener.sample().has_value());

    // Disabled, and enabled with no port, are both simply idle: a source that
    // cannot be listening says so here rather than leaving every reader to
    // guard the same thing.
    LISTENER_CHECK(listener.configure(scene::TrackingSourceSettings{}));
    LISTENER_CHECK(listener.status().state
                   == server::TrackerListenerState::Idle);

    scene::TrackingSourceSettings portless = freed_source(0U);
    LISTENER_CHECK(listener.configure(portless));
    LISTENER_CHECK(!listener.status().source.enabled);
    LISTENER_CHECK(listener.status().state
                   == server::TrackerListenerState::Idle);
}

void a_datagram_is_parsed_counted_and_published() {
    server::TrackerListener listener;
    // Port zero keeps the socket closed; deliver() is the same path the
    // receive loop runs, so the parsing and counting are tested without
    // binding anything.
    scene::TrackingSourceSettings source = freed_source(0U);
    source.enabled = false;
    LISTENER_CHECK(listener.configure(source));

    const auto datagram = freed_datagram(0U, 90 * 32'768, 64'000);
    LISTENER_CHECK(listener.deliver(datagram));

    const auto status = listener.status();
    LISTENER_CHECK(status.datagrams == 1U);
    LISTENER_CHECK(status.accepted == 1U);
    LISTENER_CHECK(status.last_pose.has_value());
    LISTENER_CHECK(status.last_pose->pan == 0.25F);
    LISTENER_CHECK(status.last_pose->x == 1.0F);
    // The lens is resolved on the Director, and with no zoom encoder that is
    // the authored fallback focal length on the authored sensor.
    const auto expected = scene::tracked_field_of_view(status.source, 0.0F);
    LISTENER_CHECK(status.field_of_view == expected);
    LISTENER_CHECK(status.field_of_view > 0.0F);

    const auto sample = listener.sample();
    LISTENER_CHECK(sample.has_value());
    LISTENER_CHECK(sample->pose == *status.last_pose);
    LISTENER_CHECK(sample->publications == 1U);
}

void refusals_are_counted_by_kind_and_publish_nothing() {
    server::TrackerListener listener;
    scene::TrackingSourceSettings source = freed_source(0U);
    source.enabled = false;
    LISTENER_CHECK(listener.configure(source));

    const auto good = freed_datagram(0U, 0, 0);
    LISTENER_CHECK(listener.deliver(good));

    // Short read.
    LISTENER_CHECK(!listener.deliver(
        std::span<const std::byte>{good.data(), good.size() - 4U}));
    // Wrong message type.
    auto foreign = good;
    foreign[0] = std::byte{0xD0};
    foreign.back() = track::freed_d1_checksum(
        std::span<const std::byte>{foreign.data(), foreign.size() - 1U});
    LISTENER_CHECK(!listener.deliver(foreign));
    // A flipped bit anywhere in the body.
    auto corrupt = good;
    corrupt[12] = static_cast<std::byte>(
        static_cast<core::u32>(corrupt[12]) ^ 0x01U);
    LISTENER_CHECK(!listener.deliver(corrupt));

    const auto status = listener.status();
    LISTENER_CHECK(status.datagrams == 4U);
    LISTENER_CHECK(status.accepted == 1U);
    LISTENER_CHECK(status.truncated == 1U);
    LISTENER_CHECK(status.unrecognized == 1U);
    LISTENER_CHECK(status.checksum_failures == 1U);
    // The one good pose is still the published one: a refused datagram never
    // partially replaces it.
    LISTENER_CHECK(listener.sample().has_value());
    LISTENER_CHECK(listener.sample()->publications == 1U);
}

// Two cameras can share a port. A datagram for the other one is an ordinary
// event, counted so an operator who set the wrong camera id can see why
// nothing is tracking rather than staring at a dead panel.
void a_datagram_for_another_camera_is_counted_not_tracked() {
    server::TrackerListener listener;
    scene::TrackingSourceSettings source = freed_source(0U, 3U);
    source.enabled = false;
    LISTENER_CHECK(listener.configure(source));

    LISTENER_CHECK(!listener.deliver(freed_datagram(9U, 0, 64'000)));
    LISTENER_CHECK(listener.status().foreign_camera == 1U);
    LISTENER_CHECK(!listener.sample().has_value());

    LISTENER_CHECK(listener.deliver(freed_datagram(3U, 0, 128'000)));
    LISTENER_CHECK(listener.status().accepted == 1U);
    LISTENER_CHECK(listener.sample().has_value());
    LISTENER_CHECK(listener.sample()->pose.x == 2.0F);
}

// An OpenTrackIO feed authored for a camera other than zero used to refuse
// every datagram it received, while the link showed as bound and the pose
// counters showed only refusals. The parser never set `camera_id`, so the
// listener compared an authored 3 against a hard zero and counted every sample
// as somebody else's camera.
void an_opentrackio_feed_matches_on_the_samples_source_number() {
    const auto sample = [](std::string_view source_number) {
        std::string text = "{\"protocol\": {\"name\": \"OpenTrackIO\", "
                           "\"version\": [1, 0, 0]},";
        text += source_number;
        text += "\"transforms\": [{\"id\": \"Camera\", "
                "\"translation\": {\"x\": 1.0, \"y\": 2.0, \"z\": 3.0}, "
                "\"rotation\": {\"pan\": 0.0, \"tilt\": 0.0, "
                "\"roll\": 0.0}}]}";
        return text;
    };
    const auto as_bytes = [](const std::string& text) {
        return std::vector<std::byte>{
            reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())};
    };

    scene::TrackingSourceSettings source{};
    source.enabled = false;
    source.protocol = scene::TrackerProtocolKind::OpenTrackIO;
    source.camera_id = 3U;
    server::TrackerListener listener;
    LISTENER_CHECK(listener.configure(source));

    // The camera this feed is authored for: tracked.
    const auto mine = as_bytes(sample("\"sourceNumber\": 3,"));
    LISTENER_CHECK(listener.deliver(mine));
    LISTENER_CHECK(listener.status().accepted == 1U);
    LISTENER_CHECK(listener.sample().has_value());

    // Somebody else's: counted, not tracked.
    const auto theirs = as_bytes(sample("\"sourceNumber\": 7,"));
    LISTENER_CHECK(!listener.deliver(theirs));
    LISTENER_CHECK(listener.status().foreign_camera == 1U);

    // A sample that names no camera cannot be matched against an authored id.
    // Counted as foreign rather than silently taken as camera zero.
    const auto anonymous = as_bytes(sample(""));
    LISTENER_CHECK(!listener.deliver(anonymous));
    LISTENER_CHECK(listener.status().foreign_camera == 2U);

    // But a feed that asks for no particular camera takes it, which is the
    // ordinary single-camera-on-a-port case.
    scene::TrackingSourceSettings any_camera = source;
    any_camera.camera_id = 0U;
    server::TrackerListener open;
    LISTENER_CHECK(open.configure(any_camera));
    LISTENER_CHECK(open.deliver(anonymous));
    LISTENER_CHECK(open.status().accepted == 1U);
}

// The real socket. Binding an ephemeral port and sending a datagram to it is
// the one thing a parser test cannot cover, and it is where a wrong address
// family or a missing bind would hide.
// The receive must wake on a datagram, not on a timer.
//
// The loop this replaced slept 100 ms whenever the socket was empty, so at any
// real tracker rate it woke to find a burst queued, drained it, and slept
// again: pose updates reached the slot in ~10 Hz bursts against a 24-60 Hz
// shoot, and the 0-100 ms of jitter that produced is not a fixed offset, so the
// authored latency compensation could not correct it.
//
// Ten datagrams sent one at a time, each only after the previous was observed,
// so a poll cannot amortise them into one wake: an event-driven receive costs
// microseconds per round and a 100 ms poll costs an expected 50 ms, which is
// half a second against the budget below. Deliberately amplified rather than
// measured once, so the failure is decisive instead of a coin toss.
void a_datagram_wakes_the_receive_rather_than_a_timer() {
    server::TrackerListener listener;
    unsigned short port = 0U;
    {
        asio::io_context probe_context;
        asio::ip::udp::socket probe{probe_context};
        std::error_code error;
        probe.open(asio::ip::udp::v4(), error);
        LISTENER_CHECK(!error);
        probe.bind(asio::ip::udp::endpoint{asio::ip::udp::v4(), 0U}, error);
        LISTENER_CHECK(!error);
        port = probe.local_endpoint().port();
        probe.close(error);
    }
    LISTENER_CHECK(listener.configure(freed_source(port)));

    asio::io_context context;
    asio::ip::udp::socket sender{context};
    std::error_code error;
    sender.open(asio::ip::udp::v4(), error);
    LISTENER_CHECK(!error);
    const asio::ip::udp::endpoint destination{
        asio::ip::make_address_v4("127.0.0.1"), port};

    constexpr int rounds = 10;
    const auto started = std::chrono::steady_clock::now();
    core::u64 observed = 0U;
    for (int round = 0; round < rounds; ++round) {
        // A distinct pan per round, so each datagram is a new pose rather than
        // a repeat the slot could coalesce.
        const auto datagram =
            freed_datagram(0U, (10 + round) * 32'768, 32'000);
        sender.send_to(asio::buffer(datagram.data(), datagram.size()),
                       destination, 0, error);
        LISTENER_CHECK(!error);
        // Spin rather than sleep: sleeping here would measure the sleep.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (listener.status().accepted <= observed
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        const auto accepted = listener.status().accepted;
        if (accepted <= observed) {
            // Loopback UDP may drop. A lost datagram is not what this is
            // testing, so it costs a round rather than the test.
            continue;
        }
        observed = accepted;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    LISTENER_CHECK(observed > 0U);
    // Generous against an event-driven receive by two orders of magnitude, and
    // far under what ten rounds of a 100 ms poll would cost.
    LISTENER_CHECK(elapsed < std::chrono::milliseconds{500});
    listener.stop();
}

void a_bound_socket_receives_a_real_datagram() {
    server::TrackerListener listener;
    // Port 0 asks the operating system for a free one, but the listener needs
    // a fixed authored port, so find one by binding and releasing first.
    unsigned short port = 0U;
    {
        asio::io_context probe_context;
        asio::ip::udp::socket probe{probe_context};
        std::error_code error;
        probe.open(asio::ip::udp::v4(), error);
        LISTENER_CHECK(!error);
        probe.bind(asio::ip::udp::endpoint{asio::ip::udp::v4(), 0U}, error);
        LISTENER_CHECK(!error);
        port = probe.local_endpoint().port();
        probe.close(error);
    }

    LISTENER_CHECK(listener.configure(freed_source(port)));
    LISTENER_CHECK(listener.status().state
                   == server::TrackerListenerState::Waiting);

    asio::io_context context;
    asio::ip::udp::socket sender{context};
    std::error_code error;
    sender.open(asio::ip::udp::v4(), error);
    LISTENER_CHECK(!error);
    const asio::ip::udp::endpoint destination{
        asio::ip::make_address_v4("127.0.0.1"), port};

    const auto datagram = freed_datagram(0U, 45 * 32'768, 32'000);
    // UDP on loopback is reliable in practice but not promised, and the
    // receive loop polls; resend until it lands or the bound is exhausted.
    bool received = false;
    for (int attempt = 0; attempt < 100 && !received; ++attempt) {
        sender.send_to(asio::buffer(datagram.data(), datagram.size()),
                       destination, 0, error);
        LISTENER_CHECK(!error);
        for (int wait = 0; wait < 20 && !received; ++wait) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            received = listener.sample().has_value();
        }
    }
    LISTENER_CHECK(received);

    const auto status = listener.status();
    LISTENER_CHECK(status.state == server::TrackerListenerState::Receiving);
    LISTENER_CHECK(status.accepted >= 1U);
    LISTENER_CHECK(status.last_pose.has_value());
    LISTENER_CHECK(std::abs(status.last_pose->pan - 0.125F) < 1.0e-6F);
    LISTENER_CHECK(status.last_pose->x == 0.5F);

    // Stopping is bounded and leaves the port free for the next configure.
    listener.stop();
    LISTENER_CHECK(listener.status().state
                   == server::TrackerListenerState::Idle);
    server::TrackerListener again;
    LISTENER_CHECK(again.configure(freed_source(port)));
    LISTENER_CHECK(again.status().listening());
}

// An operator adjusting the sensor height mid-take must not drop the socket
// underneath the show, but changing the port must.
void reconfiguring_is_idempotent_for_an_unchanged_source() {
    server::TrackerListener listener;
    scene::TrackingSourceSettings source = freed_source(0U);
    source.enabled = false;
    LISTENER_CHECK(listener.configure(source));
    LISTENER_CHECK(listener.deliver(freed_datagram(0U, 0, 64'000)));
    LISTENER_CHECK(listener.status().accepted == 1U);

    // The same source again keeps the counters, because nothing was reopened.
    LISTENER_CHECK(listener.configure(source));
    LISTENER_CHECK(listener.status().accepted == 1U);

    // A different source is a different socket, and its counters describe it
    // rather than the process.
    source.camera_id = 4U;
    LISTENER_CHECK(listener.configure(source));
    LISTENER_CHECK(listener.status().accepted == 0U);
    LISTENER_CHECK(listener.status().datagrams == 0U);
    LISTENER_CHECK(!listener.sample());
    LISTENER_CHECK(listener.deliver(freed_datagram(4U, 0, 128'000)));
    LISTENER_CHECK(listener.sample() && listener.sample()->pose.x == 2.0F);
    source.enabled = false;
    source.camera_id = 7U;
    LISTENER_CHECK(listener.configure(source));
    LISTENER_CHECK(!listener.sample());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 8U> cases{{
    {"an unconfigured listener is idle and publishes nothing",
     &an_unconfigured_listener_is_idle_and_publishes_nothing},
    {"a datagram is parsed, counted, and published",
     &a_datagram_is_parsed_counted_and_published},
    {"refusals are counted by kind and publish nothing",
     &refusals_are_counted_by_kind_and_publish_nothing},
    {"a datagram for another camera is counted, not tracked",
     &a_datagram_for_another_camera_is_counted_not_tracked},
    {"an opentrackio feed matches on the sample's source number",
     &an_opentrackio_feed_matches_on_the_samples_source_number},
    {"a bound socket receives a real datagram",
     &a_bound_socket_receives_a_real_datagram},
    {"a datagram wakes the receive rather than a timer",
     &a_datagram_wakes_the_receive_rather_than_a_timer},
    {"reconfiguring is idempotent for an unchanged source",
     &reconfiguring_is_idempotent_for_an_unchanged_source},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
