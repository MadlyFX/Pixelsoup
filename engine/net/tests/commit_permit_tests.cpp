#include <mediaserver/net/resume_session.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

namespace {

namespace command = mediaserver::cmd;
namespace core = mediaserver::core;
namespace document = mediaserver::doc;
namespace net = mediaserver::net;
namespace wire = mediaserver::wire;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define PERMIT_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TcpPair final {
public:
    TcpPair()
        : acceptor_(context_), node_(context_), director_(context_) {
        const asio::ip::tcp::endpoint endpoint{
            asio::ip::address_v4::loopback(), 0U};
        std::error_code error;
        acceptor_.open(endpoint.protocol(), error);
        PERMIT_CHECK(!error);
        acceptor_.set_option(asio::socket_base::reuse_address{true}, error);
        PERMIT_CHECK(!error);
        acceptor_.bind(endpoint, error);
        PERMIT_CHECK(!error);
        acceptor_.listen(asio::socket_base::max_listen_connections, error);
        PERMIT_CHECK(!error);
        node_.connect(acceptor_.local_endpoint(), error);
        PERMIT_CHECK(!error);
        acceptor_.accept(director_, error);
        PERMIT_CHECK(!error);
    }

    [[nodiscard]] asio::ip::tcp::socket take_node() {
        return std::move(node_);
    }

    [[nodiscard]] asio::ip::tcp::socket take_director() {
        return std::move(director_);
    }

private:
    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket node_;
    asio::ip::tcp::socket director_;
};

[[nodiscard]] net::Id128 id(std::uint64_t low) noexcept {
    return net::Id128{0x7065726d69747465ULL, low};
}

[[nodiscard]] net::Id128 show_id() noexcept {
    return id(1U);
}

[[nodiscard]] net::Id128 director_epoch() noexcept {
    return id(2U);
}

[[nodiscard]] net::Id128 session_id() noexcept {
    return id(3U);
}

[[nodiscard]] command::StreamCursor genesis_cursor() noexcept {
    return command::StreamCursor{
        command::FrameNumber{0U},
        document::Version{0U},
        command::CommandId{1U},
    };
}

[[nodiscard]] command::StreamCursor cursor_after(
    std::uint64_t frame) noexcept {
    return command::StreamCursor{
        command::FrameNumber{frame},
        document::Version{frame},
        command::CommandId{frame + 1U},
    };
}

[[nodiscard]] document::Node make_node(
    std::uint64_t node_id,
    std::int64_t value) {
    return document::Node{
        document::NodeId{node_id},
        "permit.test.node",
        {{"value", value}},
    };
}

[[nodiscard]] command::Command make_command(std::uint64_t frame) {
    return command::Command{
        command::CommandId{frame},
        document::Version{frame - 1U},
        document::Version{frame},
        command::FrameNumber{frame},
        command::TransactionId{frame},
        command::UndoContextId{1U},
        command::PutNode{make_node(frame, static_cast<std::int64_t>(frame))},
        command::EraseNode{document::NodeId{frame}},
    };
}

[[nodiscard]] command::FrameSeal seal_for(
    const command::Command& authored) {
    return command::FrameSeal{
        authored.apply_at,
        authored.base,
        authored.result,
        {{authored.id, command::fingerprint(authored)}},
    };
}

[[nodiscard]] std::vector<std::byte> encode_wire_message(
    const wire::Message& message) {
    auto encoded = wire::encode(message);
    PERMIT_CHECK(encoded);
    return std::move(encoded).value();
}

[[nodiscard]] net::FrameBatch frame_batch(
    std::uint64_t frame,
    core::u64 sequence = 0U) {
    const auto authored = make_command(frame);
    const auto seal = seal_for(authored);
    std::vector<net::Packet> packets;
    packets.push_back(net::Packet{
        encode_wire_message(wire::Message{authored})});
    packets.push_back(net::Packet{
        encode_wire_message(wire::Message{seal})});
    return net::FrameBatch{
        session_id(),
        sequence == 0U ? frame + 1U : sequence,
        frame == 1U ? genesis_cursor() : cursor_after(frame - 1U),
        cursor_after(frame),
        std::move(packets),
    };
}

[[nodiscard]] net::NodeSessionConfig node_config() {
    net::NodeSessionConfig config;
    config.show_id = show_id();
    config.node_id = id(10U);
    config.incarnation = id(11U);
    config.last_director_epoch = director_epoch();
    return config;
}

[[nodiscard]] std::uint32_t read_u32_le(
    std::span<const std::byte, 4U> bytes) noexcept {
    return static_cast<std::uint32_t>(
               std::to_integer<unsigned int>(bytes[0U]))
        | (static_cast<std::uint32_t>(
               std::to_integer<unsigned int>(bytes[1U]))
           << 8U)
        | (static_cast<std::uint32_t>(
               std::to_integer<unsigned int>(bytes[2U]))
           << 16U)
        | (static_cast<std::uint32_t>(
               std::to_integer<unsigned int>(bytes[3U]))
           << 24U);
}

[[nodiscard]] std::vector<std::byte> read_raw_packet(
    asio::ip::tcp::socket& socket) {
    std::array<std::byte, 4U> prefix{};
    std::error_code error;
    const auto prefix_read = asio::read(socket, asio::buffer(prefix), error);
    PERMIT_CHECK(!error);
    PERMIT_CHECK(prefix_read == prefix.size());
    const auto payload_size = static_cast<std::size_t>(read_u32_le(prefix));
    PERMIT_CHECK(payload_size != 0U);
    PERMIT_CHECK(payload_size <= 1024U * 1024U);

    std::vector<std::byte> packet(payload_size + prefix.size());
    std::memcpy(packet.data(), prefix.data(), prefix.size());
    const auto payload_read = asio::read(
        socket,
        asio::buffer(packet.data() + prefix.size(), payload_size),
        error);
    PERMIT_CHECK(!error);
    PERMIT_CHECK(payload_read == payload_size);
    return packet;
}

[[nodiscard]] net::SessionMessage read_message(
    asio::ip::tcp::socket& socket) {
    auto decoded = net::decode_session(read_raw_packet(socket));
    PERMIT_CHECK(decoded);
    return std::move(decoded).value();
}

void write_message(
    asio::ip::tcp::socket& socket,
    const net::SessionMessage& message) {
    auto encoded = net::encode_session(message);
    PERMIT_CHECK(encoded);
    std::error_code error;
    const auto written = asio::write(
        socket, asio::buffer(encoded.value()), error);
    PERMIT_CHECK(!error);
    PERMIT_CHECK(written == encoded.value().size());
}

void handshake(asio::ip::tcp::socket& socket) {
    const auto hello_message = read_message(socket);
    const auto* hello = std::get_if<net::ClientHello>(&hello_message);
    PERMIT_CHECK(hello != nullptr);
    PERMIT_CHECK(hello->show_id == show_id());

    write_message(
        socket,
        net::SessionMessage{net::ServerWelcome{
            net::current_session_format_version,
            show_id(),
            session_id(),
            director_epoch(),
            net::ResumeMode::UpToDate,
            genesis_cursor(),
            genesis_cursor(),
            net::default_session_limits,
        }});
    write_message(
        socket,
        net::SessionMessage{net::SyncComplete{
            session_id(), 1U, genesis_cursor()}});
    const auto acknowledgement = read_message(socket);
    const auto* resume = std::get_if<net::ResumeAck>(&acknowledgement);
    PERMIT_CHECK(resume != nullptr);
    PERMIT_CHECK(resume->session_id == session_id());
    PERMIT_CHECK(resume->sequence == 1U);
}

void send_frame_and_wait_prepared(
    asio::ip::tcp::socket& socket,
    std::uint64_t frame,
    core::u64 sequence = 0U) {
    const auto batch = frame_batch(frame, sequence);
    write_message(socket, net::SessionMessage{batch});
    const auto acknowledgement = read_message(socket);
    const auto* prepared = std::get_if<net::FramePrepared>(&acknowledgement);
    PERMIT_CHECK(prepared != nullptr);
    PERMIT_CHECK(prepared->session_id == session_id());
    PERMIT_CHECK(prepared->acknowledged_sequence == batch.sequence);
    PERMIT_CHECK(prepared->after == batch.after);
}

[[nodiscard]] net::FrameCommitPermit valid_permit(
    std::uint64_t frame,
    core::u64 sequence,
    core::u64 barrier_ordinal,
    core::u64 acknowledged_sequence = 0U) {
    return net::FrameCommitPermit{
        session_id(),
        director_epoch(),
        sequence,
        acknowledged_sequence == 0U
            ? frame + 1U
            : acknowledged_sequence,
        barrier_ordinal,
        frame == 1U ? genesis_cursor() : cursor_after(frame - 1U),
        cursor_after(frame),
    };
}

struct PeerCapture final {
    std::exception_ptr exception;
};

using PermitMutator = void (*)(net::FrameCommitPermit&);

[[nodiscard]] std::thread start_one_frame_peer(
    asio::ip::tcp::socket socket,
    PermitMutator mutate,
    PeerCapture& capture) {
    return std::thread{
        [peer = std::move(socket), mutate, &capture]() mutable {
            try {
                handshake(peer);
                send_frame_and_wait_prepared(peer, 1U);
                auto permit = valid_permit(1U, 3U, 9U);
                mutate(permit);
                write_message(peer, net::SessionMessage{permit});
            } catch (...) {
                capture.exception = std::current_exception();
            }
        }};
}

void finish_peer(std::thread& peer, PeerCapture& capture) {
    if (peer.joinable()) {
        peer.join();
    }
    if (capture.exception != nullptr) {
        std::rethrow_exception(capture.exception);
    }
}

template <typename T>
[[nodiscard]] const net::SessionProtocolFailure* protocol_failure(
    const core::Result<T, net::ResumeSessionError>& result) noexcept {
    if (result) {
        return nullptr;
    }
    return std::get_if<net::SessionProtocolFailure>(&result.error());
}

void leave_valid(net::FrameCommitPermit&) {}

void change_session(net::FrameCommitPermit& permit) {
    permit.session_id = id(99U);
}

void change_epoch(net::FrameCommitPermit& permit) {
    permit.director_epoch = id(98U);
}

void change_sequence(net::FrameCommitPermit& permit) {
    permit.sequence = 4U;
}

void change_batch_sequence(net::FrameCommitPermit& permit) {
    permit.acknowledged_sequence = 1U;
}

void change_cursors(net::FrameCommitPermit& permit) {
    permit.before = command::StreamCursor{
        command::FrameNumber{8U},
        document::Version{8U},
        command::CommandId{9U},
    };
    permit.after = command::StreamCursor{
        command::FrameNumber{9U},
        document::Version{9U},
        command::CommandId{10U},
    };
}

void exact_permit_unlocks_show_clock_commit() {
    TcpPair pair;
    command::NodeCommandBus bus;
    net::NodeResumeSession node{pair.take_node(), bus, node_config()};
    PeerCapture capture;
    auto peer = start_one_frame_peer(
        pair.take_director(), &leave_valid, capture);

    auto resumed = node.resume();
    PERMIT_CHECK(resumed);
    auto received = node.receive_live_frame();
    PERMIT_CHECK(received);
    PERMIT_CHECK(bus.is_prepared(command::FrameNumber{1U}));
    auto blocked = node.commit_prepared_frame(command::FrameNumber{1U});
    PERMIT_CHECK(!blocked);
    const auto* blocked_failure = protocol_failure(blocked);
    PERMIT_CHECK(blocked_failure != nullptr);
    PERMIT_CHECK(
        blocked_failure->code
        == net::SessionProtocolErrorCode::FrameNotPermitted);
    PERMIT_CHECK(node.state() == net::NodeSessionState::Live);
    PERMIT_CHECK(bus.cursor() == genesis_cursor());

    auto permit = node.receive_commit_permit();
    PERMIT_CHECK(permit);
    PERMIT_CHECK(permit.value().sequence == 3U);
    PERMIT_CHECK(permit.value().acknowledged_sequence == 2U);
    PERMIT_CHECK(permit.value().barrier_ordinal == 9U);
    PERMIT_CHECK(permit.value().before == genesis_cursor());
    PERMIT_CHECK(permit.value().after == cursor_after(1U));
    PERMIT_CHECK(permit.value().permitted_frames == 1U);
    PERMIT_CHECK(node.permitted_frames() == 1U);
    PERMIT_CHECK(node.last_commit_permit_ordinal() == 9U);

    auto committed = node.commit_prepared_frame(command::FrameNumber{1U});
    PERMIT_CHECK(committed);
    PERMIT_CHECK(
        committed.value().status == command::CommitStatus::Committed);
    PERMIT_CHECK(bus.cursor() == cursor_after(1U));
    finish_peer(peer, capture);
    node.close();
}

void malformed_permit_closes_session(
    PermitMutator mutate,
    net::SessionProtocolErrorCode expected) {
    TcpPair pair;
    command::NodeCommandBus bus;
    net::NodeResumeSession node{pair.take_node(), bus, node_config()};
    PeerCapture capture;
    auto peer = start_one_frame_peer(
        pair.take_director(), mutate, capture);

    PERMIT_CHECK(node.resume());
    PERMIT_CHECK(node.receive_live_frame());
    auto permit = node.receive_commit_permit();
    PERMIT_CHECK(!permit);
    const auto* failure = protocol_failure(permit);
    PERMIT_CHECK(failure != nullptr);
    PERMIT_CHECK(failure->code == expected);
    PERMIT_CHECK(node.state() == net::NodeSessionState::Closed);
    PERMIT_CHECK(bus.cursor() == genesis_cursor());
    PERMIT_CHECK(bus.is_prepared(command::FrameNumber{1U}));
    finish_peer(peer, capture);
}

void malformed_binding_is_rejected() {
    malformed_permit_closes_session(
        &change_session, net::SessionProtocolErrorCode::SessionMismatch);
    malformed_permit_closes_session(
        &change_epoch, net::SessionProtocolErrorCode::EpochMismatch);
    malformed_permit_closes_session(
        &change_sequence, net::SessionProtocolErrorCode::SequenceMismatch);
    malformed_permit_closes_session(
        &change_batch_sequence,
        net::SessionProtocolErrorCode::AcknowledgementMismatch);
    malformed_permit_closes_session(
        &change_cursors, net::SessionProtocolErrorCode::CursorMismatch);
}

void out_of_order_permit_is_rejected() {
    TcpPair pair;
    command::NodeCommandBus bus;
    net::NodeResumeSession node{pair.take_node(), bus, node_config()};
    PeerCapture capture;
    auto peer = std::thread{
        [socket = pair.take_director(), &capture]() mutable {
            try {
                handshake(socket);
                send_frame_and_wait_prepared(socket, 1U);
                send_frame_and_wait_prepared(socket, 2U);
                write_message(
                    socket,
                    net::SessionMessage{valid_permit(2U, 4U, 10U)});
            } catch (...) {
                capture.exception = std::current_exception();
            }
        }};

    PERMIT_CHECK(node.resume());
    PERMIT_CHECK(node.receive_live_frame());
    PERMIT_CHECK(node.receive_live_frame());
    auto permit = node.receive_commit_permit();
    PERMIT_CHECK(!permit);
    const auto* failure = protocol_failure(permit);
    PERMIT_CHECK(failure != nullptr);
    PERMIT_CHECK(
        failure->code
        == net::SessionProtocolErrorCode::AcknowledgementMismatch);
    PERMIT_CHECK(node.state() == net::NodeSessionState::Closed);
    PERMIT_CHECK(bus.cursor() == genesis_cursor());
    finish_peer(peer, capture);
}

void nonmonotonic_ordinal_is_rejected() {
    TcpPair pair;
    command::NodeCommandBus bus;
    net::NodeResumeSession node{pair.take_node(), bus, node_config()};
    PeerCapture capture;
    auto peer = std::thread{
        [socket = pair.take_director(), &capture]() mutable {
            try {
                handshake(socket);
                send_frame_and_wait_prepared(socket, 1U);
                write_message(
                    socket,
                    net::SessionMessage{valid_permit(1U, 3U, 10U)});
                send_frame_and_wait_prepared(socket, 2U, 4U);
                write_message(
                    socket,
                    net::SessionMessage{valid_permit(2U, 5U, 10U, 4U)});
            } catch (...) {
                capture.exception = std::current_exception();
            }
        }};

    PERMIT_CHECK(node.resume());
    PERMIT_CHECK(node.receive_live_frame());
    PERMIT_CHECK(node.receive_commit_permit());
    PERMIT_CHECK(node.receive_live_frame());
    auto duplicate = node.receive_commit_permit();
    PERMIT_CHECK(!duplicate);
    const auto* failure = protocol_failure(duplicate);
    PERMIT_CHECK(failure != nullptr);
    PERMIT_CHECK(
        failure->code == net::SessionProtocolErrorCode::InvalidCommitPermit);
    PERMIT_CHECK(node.state() == net::NodeSessionState::Closed);
    PERMIT_CHECK(bus.cursor() == genesis_cursor());
    finish_peer(peer, capture);
}

void heartbeat_interleaves_with_prepare_permit_and_commit() {
    TcpPair pair;
    command::NodeCommandBus bus;
    net::NodeResumeSession node{pair.take_node(), bus, node_config()};
    PeerCapture capture;
    auto peer = std::thread{
        [socket = pair.take_director(), &capture]() mutable {
            try {
                handshake(socket);
                send_frame_and_wait_prepared(socket, 1U, 2U);
                write_message(
                    socket,
                    net::SessionMessage{net::Heartbeat{
                        session_id(),
                        director_epoch(),
                        3U,
                        1'000U,
                        cursor_after(9U),
                    }});
                const auto heartbeat_message = read_message(socket);
                const auto* heartbeat_ack =
                    std::get_if<net::HeartbeatAck>(&heartbeat_message);
                PERMIT_CHECK(heartbeat_ack != nullptr);
                PERMIT_CHECK(heartbeat_ack->sequence == 3U);
                PERMIT_CHECK(heartbeat_ack->acknowledged_sequence == 3U);
                PERMIT_CHECK(
                    heartbeat_ack->visible_through == genesis_cursor());

                write_message(
                    socket,
                    net::SessionMessage{valid_permit(1U, 4U, 9U, 2U)});
                const auto committed_message = read_message(socket);
                const auto* committed =
                    std::get_if<net::FrameCommitted>(&committed_message);
                PERMIT_CHECK(committed != nullptr);
                PERMIT_CHECK(committed->sequence == 4U);
                PERMIT_CHECK(committed->acknowledged_sequence == 2U);
                PERMIT_CHECK(committed->after == cursor_after(1U));

                write_message(
                    socket,
                    net::SessionMessage{net::Goodbye{
                        session_id(),
                        director_epoch(),
                        5U,
                        net::GoodbyeReason::NormalShutdown,
                        cursor_after(9U),
                    }});
            } catch (...) {
                capture.exception = std::current_exception();
            }
        }};

    PERMIT_CHECK(node.resume());
    auto frame = node.receive_live_message();
    PERMIT_CHECK(frame);
    PERMIT_CHECK(
        std::get_if<net::ReceivedFrameReport>(&frame.value()) != nullptr);
    auto prepared = node.flush_frame_acknowledgements();
    PERMIT_CHECK(prepared && prepared.value().flushed == 1U);

    auto heartbeat = node.receive_live_message();
    PERMIT_CHECK(heartbeat);
    const auto* heartbeat_report =
        std::get_if<net::HeartbeatReceivedReport>(&heartbeat.value());
    PERMIT_CHECK(heartbeat_report != nullptr);
    PERMIT_CHECK(heartbeat_report->released_through == cursor_after(9U));
    PERMIT_CHECK(heartbeat_report->visible_through == genesis_cursor());
    auto heartbeat_flushed = node.flush_frame_acknowledgements();
    PERMIT_CHECK(heartbeat_flushed);
    PERMIT_CHECK(heartbeat_flushed.value().heartbeat_acknowledgements == 1U);

    auto permit = node.receive_live_message();
    PERMIT_CHECK(permit);
    PERMIT_CHECK(
        std::get_if<net::FrameCommitPermitReport>(&permit.value()) != nullptr);
    auto committed = node.commit_prepared_frame(command::FrameNumber{1U});
    PERMIT_CHECK(committed);
    auto committed_flushed = node.flush_frame_acknowledgements();
    PERMIT_CHECK(committed_flushed && committed_flushed.value().flushed == 1U);

    auto goodbye = node.receive_live_message();
    PERMIT_CHECK(goodbye);
    PERMIT_CHECK(std::get_if<net::GoodbyeReport>(&goodbye.value()) != nullptr);
    PERMIT_CHECK(node.state() == net::NodeSessionState::Draining);
    finish_peer(peer, capture);
    node.close();
}

void peer_goodbye_discards_queued_controls() {
    TcpPair pair;
    command::NodeCommandBus bus;
    net::NodeResumeSession node{pair.take_node(), bus, node_config()};
    PeerCapture capture;
    auto peer = std::thread{
        [socket = pair.take_director(), &capture]() mutable {
            try {
                handshake(socket);
                write_message(
                    socket,
                    net::SessionMessage{net::Heartbeat{
                        session_id(),
                        director_epoch(),
                        2U,
                        1'000U,
                        cursor_after(4U),
                    }});
                write_message(
                    socket,
                    net::SessionMessage{net::Goodbye{
                        session_id(),
                        director_epoch(),
                        3U,
                        net::GoodbyeReason::Maintenance,
                        cursor_after(4U),
                    }});
            } catch (...) {
                capture.exception = std::current_exception();
            }
        }};

    PERMIT_CHECK(node.resume());
    PERMIT_CHECK(node.receive_live_message());
    PERMIT_CHECK(node.pending_control_messages() == 1U);
    auto goodbye = node.receive_live_message();
    PERMIT_CHECK(goodbye);
    PERMIT_CHECK(node.state() == net::NodeSessionState::Draining);
    PERMIT_CHECK(node.pending_control_messages() == 0U);
    auto flushed = node.flush_frame_acknowledgements();
    PERMIT_CHECK(flushed);
    PERMIT_CHECK(flushed.value().flushed == 0U);
    finish_peer(peer, capture);
    node.close();
}

void heartbeat_release_regression_closes_session() {
    TcpPair pair;
    command::NodeCommandBus bus;
    net::NodeResumeSession node{pair.take_node(), bus, node_config()};
    PeerCapture capture;
    auto peer = std::thread{
        [socket = pair.take_director(), &capture]() mutable {
            try {
                handshake(socket);
                write_message(
                    socket,
                    net::SessionMessage{net::Heartbeat{
                        session_id(),
                        director_epoch(),
                        2U,
                        1'000U,
                        cursor_after(9U),
                    }});
                const auto ack_message = read_message(socket);
                PERMIT_CHECK(
                    std::get_if<net::HeartbeatAck>(&ack_message) != nullptr);
                write_message(
                    socket,
                    net::SessionMessage{net::Heartbeat{
                        session_id(),
                        director_epoch(),
                        3U,
                        1'000U,
                        cursor_after(8U),
                    }});
            } catch (...) {
                capture.exception = std::current_exception();
            }
        }};

    PERMIT_CHECK(node.resume());
    PERMIT_CHECK(node.receive_live_message());
    PERMIT_CHECK(node.flush_frame_acknowledgements());
    auto regressed = node.receive_live_message();
    PERMIT_CHECK(!regressed);
    const auto* failure = protocol_failure(regressed);
    PERMIT_CHECK(failure != nullptr);
    PERMIT_CHECK(
        failure->code == net::SessionProtocolErrorCode::CursorMismatch);
    PERMIT_CHECK(node.state() == net::NodeSessionState::Closed);
    finish_peer(peer, capture);
}

void local_goodbye_uses_next_node_control_sequence() {
    TcpPair pair;
    command::NodeCommandBus bus;
    net::NodeResumeSession node{pair.take_node(), bus, node_config()};
    PeerCapture capture;
    auto peer = std::thread{
        [socket = pair.take_director(), &capture]() mutable {
            try {
                handshake(socket);
                write_message(
                    socket,
                    net::SessionMessage{net::Heartbeat{
                        session_id(),
                        director_epoch(),
                        2U,
                        1'000U,
                        cursor_after(3U),
                    }});
                const auto ack_message = read_message(socket);
                const auto* ack = std::get_if<net::HeartbeatAck>(&ack_message);
                PERMIT_CHECK(ack != nullptr);
                PERMIT_CHECK(ack->sequence == 2U);
                PERMIT_CHECK(ack->acknowledged_sequence == 2U);
                const auto goodbye_message = read_message(socket);
                const auto* goodbye =
                    std::get_if<net::Goodbye>(&goodbye_message);
                PERMIT_CHECK(goodbye != nullptr);
                PERMIT_CHECK(goodbye->sequence == 3U);
                PERMIT_CHECK(goodbye->final_cursor == genesis_cursor());
            } catch (...) {
                capture.exception = std::current_exception();
            }
        }};

    PERMIT_CHECK(node.resume());
    PERMIT_CHECK(node.receive_live_message());
    auto requested = node.request_goodbye(net::GoodbyeReason::Restart);
    PERMIT_CHECK(requested && requested.value().sequence == 3U);
    PERMIT_CHECK(node.state() == net::NodeSessionState::Draining);
    auto flushed = node.flush_frame_acknowledgements();
    PERMIT_CHECK(flushed && flushed.value().goodbyes == 1U);
    PERMIT_CHECK(flushed.value().heartbeat_acknowledgements == 1U);
    PERMIT_CHECK(node.state() == net::NodeSessionState::Closed);
    finish_peer(peer, capture);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"exact permit unlocks show-clock commit",
         &exact_permit_unlocks_show_clock_commit},
        {"malformed permit binding closes session", &malformed_binding_is_rejected},
        {"out-of-order permit closes session", &out_of_order_permit_is_rejected},
        {"nonmonotonic permit closes session", &nonmonotonic_ordinal_is_rejected},
        {"heartbeat interleaves with frame control",
         &heartbeat_interleaves_with_prepare_permit_and_commit},
        {"peer Goodbye discards queued controls",
         &peer_goodbye_discards_queued_controls},
        {"heartbeat release regression closes session",
         &heartbeat_release_regression_closes_session},
        {"local Goodbye uses next control sequence",
         &local_goodbye_uses_next_node_control_sequence},
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
