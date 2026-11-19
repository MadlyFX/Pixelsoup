#include "tracker_listener.hpp"

#include <mediaserver/track/freed.hpp>
#include <mediaserver/track/natnet.hpp>

#include <array>
#include <bit>
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

// A NatNet 4.1 server reply and a minimal frame of data: one rigid body and
// nothing else in the volume. Little endian throughout.
[[nodiscard]] std::vector<std::byte> natnet_server_info() {
    std::vector<std::byte> bytes;
    const auto u8 = [&bytes](core::u8 value) {
        bytes.push_back(static_cast<std::byte>(value));
    };
    const auto u16 = [&u8](core::u16 value) {
        u8(static_cast<core::u8>(value & 0xFFU));
        u8(static_cast<core::u8>(value >> 8U));
    };
    u16(track::natnet_server_info_message);
    u16(264U);
    const std::string_view name{"Motive"};
    for (std::size_t index = 0U; index < 256U; ++index) {
        u8(index < name.size() ? static_cast<core::u8>(name[index]) : 0U);
    }
    constexpr std::array<core::u8, 4U> application_version{3U, 1U, 0U, 0U};
    constexpr std::array<core::u8, 4U> natnet_version{4U, 1U, 0U, 0U};
    for (const core::u8 value : application_version) {
        u8(value);
    }
    for (const core::u8 value : natnet_version) {
        u8(value);
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> natnet_frame(
    core::u32 frame_number, core::i32 body_id, float x, bool tracked) {
    std::vector<std::byte> payload;
    const auto u32 = [&payload](core::u32 value) {
        for (unsigned shift = 0U; shift < 32U; shift += 8U) {
            payload.push_back(
                static_cast<std::byte>((value >> shift) & 0xFFU));
        }
    };
    const auto f32 = [&u32](float value) {
        u32(std::bit_cast<core::u32>(value));
    };
    u32(frame_number);
    u32(0U); u32(0U); // marker sets: none, zero bytes
    u32(0U); u32(0U); // unlabeled markers
    u32(1U);          // one rigid body
    u32(4U + 7U * 4U + 4U + 2U);
    u32(static_cast<core::u32>(body_id));
    f32(x); f32(1.5F); f32(-2.0F);
    f32(0.0F); f32(0.0F); f32(0.0F); f32(1.0F);
    f32(0.0001F); // mean error
    payload.push_back(static_cast<std::byte>(tracked ? 1U : 0U));
    payload.push_back(std::byte{0});
    u32(0U); u32(0U); // skeletons
    u32(0U); u32(0U); // assets
    u32(0U); u32(0U); // labeled markers
    u32(0U); u32(0U); // force plates
    u32(0U); u32(0U); // devices
    u32(0U); u32(0U); // timecode
    u32(0U); u32(0U); // timestamp (double)
    for (int stamp = 0; stamp < 6; ++stamp) {
        u32(0U);      // three 64-bit timestamps
    }
    u32(0U); u32(0U); // precision timestamp
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{0}); // frame params
    u32(0U);          // end of data

    std::vector<std::byte> datagram;
    datagram.push_back(static_cast<std::byte>(track::natnet_frame_message));
    datagram.push_back(std::byte{0});
    datagram.push_back(static_cast<std::byte>(payload.size() & 0xFFU));
    datagram.push_back(static_cast<std::byte>(payload.size() >> 8U));
    datagram.insert(datagram.end(), payload.begin(), payload.end());
    return datagram;
}

// A NatNet feed cannot decode a frame until the server has said which version
// it speaks, holds rather than guesses until then, and afterwards tracks the
// authored rigid body and only that one.
void a_natnet_feed_holds_until_the_server_answers_then_tracks_its_body() {
    scene::TrackingSourceSettings source{};
    source.enabled = false;
    source.protocol = scene::TrackerProtocolKind::NatNet;
    source.camera_id = 5U;
    server::TrackerListener listener;
    LISTENER_CHECK(listener.configure(source));
    LISTENER_CHECK(!listener.awaiting_handshake()); // not enabled: no socket

    // Frames before the reply are counted and held, never published.
    LISTENER_CHECK(!listener.deliver(natnet_frame(1U, 5, 1.0F, true)));
    LISTENER_CHECK(!listener.deliver(natnet_frame(2U, 5, 1.0F, true)));
    LISTENER_CHECK(listener.status().awaiting_handshake == 2U);
    LISTENER_CHECK(listener.status().accepted == 0U);
    LISTENER_CHECK(!listener.sample().has_value());
    LISTENER_CHECK(listener.status().server_description.empty());

    // The reply is not a pose, but it names the server and unlocks decoding.
    LISTENER_CHECK(!listener.deliver(natnet_server_info()));
    LISTENER_CHECK(listener.status().server_description
                   == "Motive 3.1.0, NatNet 4.1");
    LISTENER_CHECK(listener.status().unrecognized == 0U);

    LISTENER_CHECK(listener.deliver(natnet_frame(3U, 5, 1.25F, true)));
    const auto status = listener.status();
    LISTENER_CHECK(status.accepted == 1U);
    LISTENER_CHECK(status.last_pose.has_value());
    LISTENER_CHECK(status.last_pose->protocol == track::TrackerProtocol::NatNet);
    LISTENER_CHECK(status.last_pose->x == 1.25F);
    LISTENER_CHECK(status.last_pose->y == 1.5F);
    LISTENER_CHECK(status.last_pose->sequence == 3U);
    LISTENER_CHECK(status.last_pose->camera_id == 5U);
    // No lens on a rigid body: the authored fallback focal length applies.
    LISTENER_CHECK(status.field_of_view
                   == scene::tracked_field_of_view(status.source, 0.0F));
    LISTENER_CHECK(listener.sample() && listener.sample()->publications == 1U);

    // Another body in the volume is somebody else's camera.
    LISTENER_CHECK(!listener.deliver(natnet_frame(4U, 9, 7.0F, true)));
    LISTENER_CHECK(listener.status().foreign_camera == 1U);
    // An occluded body holds the last good pose.
    LISTENER_CHECK(!listener.deliver(natnet_frame(5U, 5, 7.0F, false)));
    LISTENER_CHECK(listener.status().untracked == 1U);
    LISTENER_CHECK(listener.sample()->pose.x == 1.25F);
    LISTENER_CHECK(listener.sample()->publications == 1U);

    // A feed for no particular body takes the first one in every frame.
    scene::TrackingSourceSettings any_body = source;
    any_body.camera_id = 0U;
    server::TrackerListener open;
    LISTENER_CHECK(open.configure(any_body));
    LISTENER_CHECK(!open.deliver(natnet_server_info()));
    LISTENER_CHECK(open.deliver(natnet_frame(1U, 9, 7.0F, true)));
    LISTENER_CHECK(open.status().accepted == 1U);
    LISTENER_CHECK(open.sample()->pose.camera_id == 9U);

    // Reconfiguring to another source forgets the version: it may be a
    // different server.
    any_body.camera_id = 2U;
    LISTENER_CHECK(open.configure(any_body));
    LISTENER_CHECK(!open.deliver(natnet_frame(1U, 2, 7.0F, true)));
    LISTENER_CHECK(open.status().awaiting_handshake == 1U);
}

// The real NatNet socket: joins the multicast group, asks the sender's command
// port for its version when a frame arrives it cannot decode, and tracks once
// the reply has come back. This is the one path `deliver` cannot cover - the
// join, the connect request's destination, and the reply arriving on the same
// socket that receives frames.
void a_natnet_socket_joins_the_group_and_completes_the_handshake() {
    const auto free_port = []() {
        asio::io_context probe_context;
        asio::ip::udp::socket probe{probe_context};
        std::error_code error;
        probe.open(asio::ip::udp::v4(), error);
        LISTENER_CHECK(!error);
        probe.bind(asio::ip::udp::endpoint{asio::ip::udp::v4(), 0U}, error);
        LISTENER_CHECK(!error);
        const unsigned short port = probe.local_endpoint().port();
        probe.close(error);
        return port;
    };
    const unsigned short data_port = free_port();

    // The stand-in server: a socket on an ephemeral command port, which is
    // also where its frames are sent from, so the listener's connect request
    // comes back to it.
    asio::io_context context;
    asio::ip::udp::socket server{context};
    std::error_code error;
    server.open(asio::ip::udp::v4(), error);
    LISTENER_CHECK(!error);
    server.bind(asio::ip::udp::endpoint{asio::ip::udp::v4(), 0U}, error);
    LISTENER_CHECK(!error);
    server.non_blocking(true, error);
    LISTENER_CHECK(!error);
    const unsigned short command_port = server.local_endpoint().port();

    scene::TrackingSourceSettings source{};
    source.enabled = true;
    source.protocol = scene::TrackerProtocolKind::NatNet;
    source.port = data_port;
    source.command_port = command_port;
    source.camera_id = 5U;
    server::TrackerListener listener;
    LISTENER_CHECK(listener.configure(source));
    LISTENER_CHECK(listener.status().state
                   == server::TrackerListenerState::Waiting);
    LISTENER_CHECK(listener.awaiting_handshake());

    const asio::ip::udp::endpoint group{
        asio::ip::make_address_v4("239.255.42.99"), data_port};
    const auto deadline_from_now = []() {
        return std::chrono::steady_clock::now() + std::chrono::seconds{5};
    };

    // Frames until a connect request arrives at the command port.
    std::array<std::byte, 16U> request{};
    bool asked = false;
    core::u32 frame_number = 1U;
    for (auto deadline = deadline_from_now();
         !asked && std::chrono::steady_clock::now() < deadline;) {
        const auto frame = natnet_frame(frame_number++, 5, 1.0F, true);
        server.send_to(asio::buffer(frame.data(), frame.size()), group, 0,
                       error);
        LISTENER_CHECK(!error);
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        asio::ip::udp::endpoint from;
        const std::size_t received = server.receive_from(
            asio::buffer(request.data(), request.size()), from, 0, error);
        if (!error && received == 4U) {
            asked = true;
            // From the listener's own command socket, not the data port: on
            // the Motive machine itself the server holds the data port too,
            // and a reply sent there would reach Motive, not the Director.
            LISTENER_CHECK(from.port() != data_port);
            LISTENER_CHECK(track::natnet_message_id(
                               std::span<const std::byte>{request.data(), 4U})
                           == track::natnet_connect_message);
            // The reply goes back to where the request came from.
            const auto info = natnet_server_info();
            server.send_to(asio::buffer(info.data(), info.size()), from, 0,
                           error);
            LISTENER_CHECK(!error);
        }
    }
    LISTENER_CHECK(asked);
    LISTENER_CHECK(listener.status().awaiting_handshake >= 1U);

    // Frames after the reply are decoded and published.
    bool tracked = false;
    for (auto deadline = deadline_from_now();
         !tracked && std::chrono::steady_clock::now() < deadline;) {
        const auto frame = natnet_frame(frame_number++, 5, 2.5F, true);
        server.send_to(asio::buffer(frame.data(), frame.size()), group, 0,
                       error);
        LISTENER_CHECK(!error);
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        tracked = listener.sample().has_value();
    }
    LISTENER_CHECK(tracked);
    const auto status = listener.status();
    LISTENER_CHECK(!listener.awaiting_handshake());
    LISTENER_CHECK(status.server_description == "Motive 3.1.0, NatNet 4.1");
    LISTENER_CHECK(status.state == server::TrackerListenerState::Receiving);
    LISTENER_CHECK(status.last_pose && status.last_pose->x == 2.5F);
    LISTENER_CHECK(status.last_pose->protocol == track::TrackerProtocol::NatNet);
    listener.stop();
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

// A 4.1 description of two rigid bodies, sized as Motive writes it.
[[nodiscard]] std::vector<std::byte> natnet_descriptions() {
    std::vector<std::byte> bytes;
    const auto u8 = [&bytes](core::u8 value) {
        bytes.push_back(static_cast<std::byte>(value));
    };
    const auto u32 = [&u8](core::u32 value) {
        for (core::u32 shift = 0U; shift < 32U; shift += 8U) {
            u8(static_cast<core::u8>((value >> shift) & 0xFFU));
        }
    };
    const auto body = [&](std::string_view name, core::i32 id) {
        u32(1U);
        u32(static_cast<core::u32>(name.size() + 1U + 4U + 4U + 12U + 4U));
        for (const char c : name) {
            u8(static_cast<core::u8>(c));
        }
        u8(0U);
        u32(static_cast<core::u32>(id));
        u32(0xFFFF'FFFFU);
        for (int index = 0; index < 12; ++index) {
            u8(0U);
        }
        u32(0U);
    };
    u8(static_cast<core::u8>(track::natnet_model_definitions_message));
    u8(0U);
    u8(0U);
    u8(0U);
    u32(2U);
    body("Camera A", 5);
    body("Witness", 11);
    return bytes;
}

void a_natnet_feed_lists_the_rigid_bodies_the_server_describes() {
    scene::TrackingSourceSettings source{};
    source.protocol = scene::TrackerProtocolKind::NatNet;
    source.port = 1511U;
    server::TrackerListener listener;
    LISTENER_CHECK(listener.configure(source));
    // A description before the version is not guessed at.
    LISTENER_CHECK(!listener.deliver(natnet_descriptions()));
    LISTENER_CHECK(listener.status().rigid_bodies.empty());
    LISTENER_CHECK(!listener.deliver(natnet_server_info()));
    LISTENER_CHECK(!listener.deliver(natnet_descriptions()));
    const auto bodies = listener.status().rigid_bodies;
    LISTENER_CHECK(bodies.size() == 2U);
    LISTENER_CHECK(bodies[0] == (server::TrackerRigidBody{5, "Camera A"}));
    LISTENER_CHECK(bodies[1] == (server::TrackerRigidBody{11, "Witness"}));
    // Neither is a pose.
    LISTENER_CHECK(listener.status().accepted == 0U);
    LISTENER_CHECK(!listener.sample());
}

void an_authored_server_is_asked_before_any_frame_arrives() {
    // Multicast may not be reaching this host at all, which is exactly when
    // an operator most needs to see that the server answers and what it has.
    asio::io_context context;
    asio::ip::udp::socket server{context};
    std::error_code error;
    server.open(asio::ip::udp::v4(), error);
    LISTENER_CHECK(!error);
    server.bind(asio::ip::udp::endpoint{
                    asio::ip::make_address_v4("127.0.0.1"), 0U}, error);
    LISTENER_CHECK(!error);
    server.non_blocking(true, error);
    LISTENER_CHECK(!error);

    asio::ip::udp::socket probe{context};
    probe.open(asio::ip::udp::v4(), error);
    probe.bind(asio::ip::udp::endpoint{asio::ip::udp::v4(), 0U}, error);
    const unsigned short data_port = probe.local_endpoint().port();
    probe.close(error);

    scene::TrackingSourceSettings source{};
    source.enabled = true;
    source.protocol = scene::TrackerProtocolKind::NatNet;
    source.port = data_port;
    source.command_port = server.local_endpoint().port();
    source.server_address = {127U, 0U, 0U, 1U};
    server::TrackerListener listener;
    LISTENER_CHECK(listener.configure(source));
    LISTENER_CHECK(listener.status().server_address == "127.0.0.1");

    // Answers each request as Motive would: the version to a connect, the
    // asset list to a description request.
    std::array<std::byte, 16U> request{};
    core::u64 connects = 0U;
    core::u64 descriptions = 0U;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (listener.status().rigid_bodies.empty()
           && std::chrono::steady_clock::now() < deadline) {
        asio::ip::udp::endpoint from;
        const std::size_t received = server.receive_from(
            asio::buffer(request.data(), request.size()), from, 0, error);
        if (error || received != 4U) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
            continue;
        }
        const auto message = track::natnet_message_id(
            std::span<const std::byte>{request.data(), 4U});
        const auto reply = message == track::natnet_connect_message
            ? natnet_server_info()
            : natnet_descriptions();
        if (message == track::natnet_connect_message) {
            ++connects;
        } else {
            LISTENER_CHECK(message
                           == track::natnet_request_model_definitions_message);
            ++descriptions;
        }
        server.send_to(asio::buffer(reply.data(), reply.size()), from, 0,
                       error);
        LISTENER_CHECK(!error);
    }
    LISTENER_CHECK(connects >= 1U);
    LISTENER_CHECK(descriptions >= 1U);
    const auto status = listener.status();
    LISTENER_CHECK(status.server_description == "Motive 3.1.0, NatNet 4.1");
    LISTENER_CHECK(status.rigid_bodies.size() == 2U);
    LISTENER_CHECK(status.rigid_bodies[1].name == "Witness");
    LISTENER_CHECK(status.datagrams >= 2U && status.accepted == 0U);
    listener.stop();
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 12U> cases{{
    {"a natnet feed lists the rigid bodies the server describes",
     &a_natnet_feed_lists_the_rigid_bodies_the_server_describes},
    {"an authored server is asked before any frame arrives",
     &an_authored_server_is_asked_before_any_frame_arrives},
    {"a natnet feed holds until the server answers, then tracks its body",
     &a_natnet_feed_holds_until_the_server_answers_then_tracks_its_body},
    {"a natnet socket joins the group and completes the handshake",
     &a_natnet_socket_joins_the_group_and_completes_the_handshake},
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
