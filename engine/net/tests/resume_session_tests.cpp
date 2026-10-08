#include <mediaserver/net/resume_session.hpp>
#include <mediaserver/net/director_session_hub.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
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
namespace persist = mediaserver::persist;
namespace wire = mediaserver::wire;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define RESUME_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempWal final {
public:
    TempWal() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("mediaserver-resume-session-" + std::to_string(timestamp) + "-"
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

class TcpPair final {
public:
    TcpPair()
        : acceptor_(context_), node_(context_), director_(context_) {
        const asio::ip::tcp::endpoint endpoint{
            asio::ip::address_v4::loopback(), 0U};
        std::error_code error;
        acceptor_.open(endpoint.protocol(), error);
        RESUME_CHECK(!error);
        acceptor_.set_option(asio::socket_base::reuse_address{true}, error);
        RESUME_CHECK(!error);
        acceptor_.bind(endpoint, error);
        RESUME_CHECK(!error);
        acceptor_.listen(asio::socket_base::max_listen_connections, error);
        RESUME_CHECK(!error);
        node_.connect(acceptor_.local_endpoint(), error);
        RESUME_CHECK(!error);
        acceptor_.accept(director_, error);
        RESUME_CHECK(!error);
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

class TestLeaseAuthority final : public net::NodeSessionLeaseAuthority {
public:
    [[nodiscard]] bool claim(
        const net::NodeSessionIdentity& identity,
        net::Id128 session_id) noexcept override {
        ++claims;
        if (!allow || active_identity.has_value()) {
            return false;
        }
        active_identity = identity;
        active_session = session_id;
        return true;
    }

    void release(
        const net::NodeSessionIdentity& identity,
        net::Id128 session_id) noexcept override {
        ++releases;
        if (active_identity == identity && active_session == session_id) {
            active_identity.reset();
            active_session.reset();
        }
    }

    bool allow{true};
    std::size_t claims{};
    std::size_t releases{};
    std::optional<net::NodeSessionIdentity> active_identity;
    std::optional<net::Id128> active_session;
};

using ResumeResult = core::Result<net::ResumeReport, net::ResumeSessionError>;

struct ResumeCapture final {
    std::optional<ResumeResult> result;
    std::exception_ptr exception;
};

using ReceiveResult =
    core::Result<net::ReceivedFrameReport, net::ResumeSessionError>;

struct ReceiveCapture final {
    std::optional<ReceiveResult> result;
    std::exception_ptr exception;
};

struct PeerCapture final {
    std::exception_ptr exception;
};

[[nodiscard]] net::Id128 id(std::uint64_t low) noexcept {
    return net::Id128{0x6d65646961736572ULL, low};
}

[[nodiscard]] net::Id128 show_id() noexcept {
    return id(1U);
}

[[nodiscard]] net::Id128 director_epoch() noexcept {
    return id(2U);
}

[[nodiscard]] command::StreamCursor genesis_cursor() noexcept {
    return command::StreamCursor{
        command::FrameNumber{0U},
        document::Version{0U},
        command::CommandId{1U},
    };
}

[[nodiscard]] persist::CommandLog open_log(const std::filesystem::path& path) {
    persist::OpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    auto opened = persist::CommandLog::open_or_create(
        path, genesis_cursor(), options);
    RESUME_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] document::Node make_node(std::uint64_t node_id, std::int64_t value) {
    return document::Node{
        document::NodeId{node_id},
        "test.node",
        {{"value", value}},
    };
}

[[nodiscard]] command::Payload put(std::uint64_t node_id, std::int64_t value) {
    return command::PutNode{make_node(node_id, value)};
}

[[nodiscard]] command::Payload erase(std::uint64_t node_id) {
    return command::EraseNode{document::NodeId{node_id}};
}

void schedule_put(
    command::DirectorScheduler& scheduler,
    std::uint64_t node_id,
    std::int64_t value,
    std::uint64_t transaction,
    std::uint64_t frame) {
    auto scheduled = scheduler.schedule_edit(
        put(node_id, value),
        erase(node_id),
        command::UndoContextId{1U},
        command::TransactionId{transaction},
        command::FrameNumber{frame});
    RESUME_CHECK(scheduled);
}

[[nodiscard]] command::Command make_command() {
    return command::Command{
        command::CommandId{1U},
        document::Version{0U},
        document::Version{1U},
        command::FrameNumber{1U},
        command::TransactionId{1U},
        command::UndoContextId{1U},
        put(1U, 10),
        erase(1U),
    };
}

[[nodiscard]] command::FrameSeal seal_for(const command::Command& authored) {
    return command::FrameSeal{
        authored.apply_at,
        authored.base,
        authored.result,
        {{authored.id, command::fingerprint(authored)}},
    };
}

[[nodiscard]] net::DirectorSessionConfig director_config(
    std::uint64_t session_low) {
    net::DirectorSessionConfig config;
    config.show_id = show_id();
    config.director_epoch = director_epoch();
    config.session_id = id(session_low);
    return config;
}

[[nodiscard]] net::NodeSessionConfig node_config(
    net::Id128 requested_show = show_id(),
    net::Id128 last_epoch = director_epoch(),
    bool force_checkpoint = false) {
    net::NodeSessionConfig config;
    config.show_id = requested_show;
    config.node_id = id(10U);
    config.incarnation = id(11U);
    config.last_director_epoch = last_epoch;
    config.force_checkpoint = force_checkpoint;
    return config;
}

[[nodiscard]] std::thread start_director_resume(
    net::DirectorResumeSession& session,
    ResumeCapture& capture) {
    return std::thread{[&session, &capture] {
        try {
            capture.result.emplace(session.resume());
        } catch (...) {
            capture.exception = std::current_exception();
        }
    }};
}

[[nodiscard]] std::thread start_director_resume(
    net::DirectorResumeSession& session,
    persist::DurableResumeSource source,
    ResumeCapture& capture) {
    return std::thread{
        [&session, source = std::move(source), &capture]() mutable {
            try {
                capture.result.emplace(session.resume(std::move(source)));
            } catch (...) {
                capture.exception = std::current_exception();
            }
        }};
}

void finish_thread(std::thread& worker, ResumeCapture& capture) {
    if (worker.joinable()) {
        worker.join();
    }
    if (capture.exception != nullptr) {
        std::rethrow_exception(capture.exception);
    }
    RESUME_CHECK(capture.result.has_value());
}

[[nodiscard]] std::thread start_live_receive(
    net::NodeResumeSession& session,
    ReceiveCapture& capture) {
    return std::thread{[&session, &capture] {
        try {
            capture.result.emplace(session.receive_live_frame());
        } catch (...) {
            capture.exception = std::current_exception();
        }
    }};
}

void finish_receive(std::thread& worker, ReceiveCapture& capture) {
    if (worker.joinable()) {
        worker.join();
    }
    if (capture.exception != nullptr) {
        std::rethrow_exception(capture.exception);
    }
    RESUME_CHECK(capture.result.has_value());
}

[[nodiscard]] net::FrameCommitPermitReport permit_next_live_frame(
    net::DirectorResumeSession& director,
    net::NodeResumeSession& node,
    core::u64 barrier_ordinal,
    core::u64 acknowledged_sequence,
    const command::StreamCursor& before,
    const command::StreamCursor& after) {
    auto sent = director.send_commit_permit(net::FrameCommitPermitIntent{
        barrier_ordinal,
        acknowledged_sequence,
        before,
        after,
    });
    RESUME_CHECK(sent);
    auto received = node.receive_commit_permit();
    RESUME_CHECK(received);
    RESUME_CHECK(received.value().sequence == sent.value().sequence);
    RESUME_CHECK(
        received.value().acknowledged_sequence
        == sent.value().acknowledged_sequence);
    RESUME_CHECK(
        received.value().barrier_ordinal
        == sent.value().barrier_ordinal);
    RESUME_CHECK(received.value().before == sent.value().before);
    RESUME_CHECK(received.value().after == sent.value().after);
    return received.value();
}

[[nodiscard]] const net::SessionProtocolFailure* protocol_failure(
    const ResumeResult& result) noexcept {
    if (result) {
        return nullptr;
    }
    return std::get_if<net::SessionProtocolFailure>(&result.error());
}

[[nodiscard]] std::vector<std::byte> encode_session_message(
    const net::SessionMessage& message) {
    auto encoded = net::encode_session(message);
    RESUME_CHECK(encoded);
    return std::move(encoded).value();
}

[[nodiscard]] std::vector<std::byte> encode_wire_message(
    const wire::Message& message) {
    auto encoded = wire::encode(message);
    RESUME_CHECK(encoded);
    return std::move(encoded).value();
}

[[nodiscard]] std::uint32_t read_u32_le(
    std::span<const std::byte, 4U> bytes) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<unsigned int>(bytes[0U]))
        | (static_cast<std::uint32_t>(std::to_integer<unsigned int>(bytes[1U])) << 8U)
        | (static_cast<std::uint32_t>(std::to_integer<unsigned int>(bytes[2U])) << 16U)
        | (static_cast<std::uint32_t>(std::to_integer<unsigned int>(bytes[3U])) << 24U);
}

[[nodiscard]] std::vector<std::byte> read_raw_packet(
    asio::ip::tcp::socket& socket) {
    std::array<std::byte, 4U> prefix{};
    std::error_code error;
    const auto prefix_read = asio::read(socket, asio::buffer(prefix), error);
    if (error || prefix_read != prefix.size()) {
        throw std::runtime_error{"fake peer could not read ClientHello prefix"};
    }
    const auto payload_size = static_cast<std::size_t>(read_u32_le(prefix));
    if (payload_size == 0U || payload_size > 1024U * 1024U) {
        throw std::runtime_error{"fake peer received invalid ClientHello size"};
    }
    std::vector<std::byte> packet(payload_size + prefix.size());
    std::memcpy(packet.data(), prefix.data(), prefix.size());
    const auto payload_read = asio::read(
        socket,
        asio::buffer(packet.data() + prefix.size(), payload_size),
        error);
    if (error || payload_read != payload_size) {
        throw std::runtime_error{"fake peer could not read ClientHello body"};
    }
    return packet;
}

[[nodiscard]] std::thread start_fake_replay_peer(
    asio::ip::tcp::socket socket,
    std::vector<std::byte> welcome,
    std::vector<std::byte> frame,
    bool truncate_frame,
    PeerCapture& capture) {
    return std::thread{
        [peer = std::move(socket),
         welcome = std::move(welcome),
         frame = std::move(frame),
         truncate_frame,
         &capture]() mutable {
            try {
                const auto hello = read_raw_packet(peer);
                const auto decoded = net::decode_session(hello);
                if (!decoded || !std::holds_alternative<net::ClientHello>(decoded.value())) {
                    throw std::runtime_error{"fake peer received invalid ClientHello"};
                }
                static_cast<void>(asio::write(peer, asio::buffer(welcome)));
                const auto sent_size = truncate_frame ? frame.size() - 3U : frame.size();
                static_cast<void>(asio::write(
                    peer, asio::buffer(frame.data(), sent_size)));
                std::error_code ignored;
                peer.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
            } catch (...) {
                capture.exception = std::current_exception();
            }
        }};
}

void finish_peer(std::thread& worker, PeerCapture& capture) {
    if (worker.joinable()) {
        worker.join();
    }
    if (capture.exception != nullptr) {
        std::rethrow_exception(capture.exception);
    }
}

void exact_tail_resumes_up_to_date() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler scheduler;
    persist::DurableDirector durable{scheduler, log};
    command::NodeCommandBus node_bus;
    TcpPair pair;
    TestLeaseAuthority leases;
    net::DirectorResumeSession director_session{
        pair.take_director(), director_config(100U), durable, leases};
    net::NodeResumeSession node_session{
        pair.take_node(), node_bus, node_config()};

    ResumeCapture director_capture;
    auto worker = start_director_resume(director_session, director_capture);
    auto node_result = node_session.resume();
    finish_thread(worker, director_capture);

    RESUME_CHECK(node_result);
    RESUME_CHECK(director_capture.result->has_value());
    RESUME_CHECK(node_result.value().mode == net::ResumeMode::UpToDate);
    RESUME_CHECK(director_capture.result->value().mode == net::ResumeMode::UpToDate);
    RESUME_CHECK(node_result.value().requested == genesis_cursor());
    RESUME_CHECK(node_result.value().target == genesis_cursor());
    RESUME_CHECK(node_result.value().frames == 0U);
    RESUME_CHECK(node_result.value().commands == 0U);
    RESUME_CHECK(node_session.state() == net::NodeSessionState::Live);
    RESUME_CHECK(director_session.state() == net::DirectorSessionState::Live);
    RESUME_CHECK(node_bus.cursor() == genesis_cursor());
    RESUME_CHECK(node_session.director_epoch() == director_epoch());
    RESUME_CHECK(leases.claims == 1U);
    RESUME_CHECK(leases.releases == 0U);
    RESUME_CHECK(director_session.peer_identity().has_value());
    RESUME_CHECK(
        *director_session.peer_identity()
        == net::NodeSessionIdentity{show_id(), id(10U), id(11U)});

    node_session.close();
    director_session.close();
    RESUME_CHECK(leases.releases == 1U);
    RESUME_CHECK(!leases.active_identity.has_value());
}

void wal_replay_delivers_content_and_empty_frames_without_committing() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler authoring;
    persist::DurableDirector durable{authoring, log};
    schedule_put(authoring, 1U, 10, 1U, 1U);
    auto content = durable.seal_frame(command::FrameNumber{1U});
    RESUME_CHECK(content);
    auto empty = durable.seal_frame(command::FrameNumber{2U});
    RESUME_CHECK(empty);
    RESUME_CHECK(empty.value().commands().empty());

    const auto authored_checkpoint = authoring.checkpoint();
    command::DirectorScheduler fresh{
        authored_checkpoint.document,
        authored_checkpoint.cursor.through_frame,
        authored_checkpoint.cursor.next_command};
    const auto checkpoint = fresh.checkpoint();
    RESUME_CHECK(fresh.commands_for(command::FrameNumber{1U}).empty());
    RESUME_CHECK(fresh.seal_for(command::FrameNumber{1U}) == nullptr);
    RESUME_CHECK(checkpoint.cursor == log.tail_cursor());
    persist::DurableDirector recovered{fresh, log};

    command::NodeCommandBus node_bus;
    TcpPair pair;
    TestLeaseAuthority leases;
    net::DirectorResumeSession director_session{
        pair.take_director(), director_config(101U), recovered, leases};
    net::NodeResumeSession node_session{
        pair.take_node(), node_bus, node_config()};
    ResumeCapture director_capture;
    auto worker = start_director_resume(director_session, director_capture);
    auto node_result = node_session.resume();
    finish_thread(worker, director_capture);

    RESUME_CHECK(node_result);
    RESUME_CHECK(director_capture.result->has_value());
    RESUME_CHECK(node_result.value().mode == net::ResumeMode::Replay);
    RESUME_CHECK(node_result.value().frames == 2U);
    RESUME_CHECK(node_result.value().commands == 1U);
    RESUME_CHECK(director_capture.result->value().frames == 2U);
    RESUME_CHECK(director_capture.result->value().commands == 1U);
    RESUME_CHECK(node_bus.cursor() == genesis_cursor());
    RESUME_CHECK(node_bus.document().empty());
    RESUME_CHECK(node_bus.is_prepared(command::FrameNumber{1U}));
    RESUME_CHECK(node_bus.is_prepared(command::FrameNumber{2U}));
    RESUME_CHECK(node_session.delivered_cursor() == checkpoint.cursor);

    auto first_commit =
        node_session.commit_prepared_frame(command::FrameNumber{1U});
    RESUME_CHECK(first_commit);
    RESUME_CHECK(
        first_commit.value().status == command::CommitStatus::Committed);
    RESUME_CHECK(first_commit.value().command_count == 1U);
    RESUME_CHECK(node_session.inflight_frames() == 2U);
    RESUME_CHECK(node_session.pending_frame_acknowledgements() == 1U);
    const auto* applied = node_bus.document().get(document::NodeId{1U});
    RESUME_CHECK(applied != nullptr);
    RESUME_CHECK(*applied == make_node(1U, 10));
    auto empty_commit =
        node_session.commit_prepared_frame(command::FrameNumber{2U});
    RESUME_CHECK(empty_commit);
    RESUME_CHECK(
        empty_commit.value().status == command::CommitStatus::Committed);
    RESUME_CHECK(empty_commit.value().command_count == 0U);
    RESUME_CHECK(node_session.inflight_frames() == 2U);
    RESUME_CHECK(node_session.pending_frame_acknowledgements() == 2U);
    auto committed_flush = node_session.flush_frame_acknowledgements();
    RESUME_CHECK(committed_flush);
    RESUME_CHECK(committed_flush.value().flushed == 2U);
    RESUME_CHECK(committed_flush.value().remaining == 0U);
    RESUME_CHECK(node_session.inflight_frames() == 0U);
    auto first_ack = director_session.receive_frame_acknowledgement();
    RESUME_CHECK(first_ack);
    RESUME_CHECK(
        first_ack.value().kind == net::FrameAcknowledgementKind::Committed);
    RESUME_CHECK(first_ack.value().sequence == 2U);
    RESUME_CHECK(first_ack.value().acknowledged_sequence == 1U);
    RESUME_CHECK(first_ack.value().outstanding_frames == 1U);
    auto empty_ack = director_session.receive_frame_acknowledgement();
    RESUME_CHECK(empty_ack);
    RESUME_CHECK(
        empty_ack.value().kind == net::FrameAcknowledgementKind::Committed);
    RESUME_CHECK(empty_ack.value().sequence == 3U);
    RESUME_CHECK(empty_ack.value().acknowledged_sequence == 2U);
    RESUME_CHECK(empty_ack.value().outstanding_frames == 0U);
    RESUME_CHECK(node_bus.cursor() == checkpoint.cursor);

    node_session.close();
    director_session.close();
}

void checkpoint_case(
    bool force_checkpoint,
    net::Id128 last_epoch,
    std::uint64_t session_low,
    std::optional<std::uint32_t> max_inflight_frames = std::nullopt,
    bool fault_node = false) {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler scheduler;
    persist::DurableDirector durable{scheduler, log};
    schedule_put(scheduler, 1U, 10, 1U, 1U);
    auto content = durable.seal_frame(command::FrameNumber{1U});
    RESUME_CHECK(content);
    auto empty = durable.seal_frame(command::FrameNumber{2U});
    RESUME_CHECK(empty);
    const auto checkpoint = scheduler.checkpoint();

    command::NodeCommandBus node_bus;
    if (fault_node) {
        const auto original = make_command();
        RESUME_CHECK(
            node_bus.receive(original) == command::ReceiveStatus::Accepted);
        auto conflict = original;
        conflict.forward = put(1U, 99);
        RESUME_CHECK(
            node_bus.receive(conflict) == command::ReceiveStatus::NeedsResync);
        RESUME_CHECK(node_bus.needs_resync());
    }
    const auto original_cursor = node_bus.cursor();
    TcpPair pair;
    TestLeaseAuthority leases;
    net::DirectorResumeSession director_session{
        pair.take_director(), director_config(session_low), durable, leases};
    auto config = node_config(show_id(), last_epoch, force_checkpoint);
    if (max_inflight_frames.has_value()) {
        config.limits.max_inflight_frames = *max_inflight_frames;
    }
    net::NodeResumeSession node_session{
        pair.take_node(),
        node_bus,
        config};
    ResumeCapture director_capture;
    auto worker = start_director_resume(director_session, director_capture);
    auto node_result = node_session.resume();

    const auto staged_state = node_session.state();
    const auto staged_cursor = node_bus.cursor();
    const auto staged_document_empty = node_bus.document().empty();
    const auto* staged_checkpoint = node_bus.prepared_checkpoint();
    const auto staged_checkpoint_matches = staged_checkpoint != nullptr
        && staged_checkpoint->cursor == checkpoint.cursor;
    std::optional<command::ReceiveStatus> blocked_receive;
    std::optional<command::CommitResult> blocked_commit;
    using ActivationResult =
        core::Result<std::monostate, net::ResumeSessionError>;
    std::optional<ActivationResult> activated;
    if (node_result) {
        blocked_receive = node_bus.receive(content.value().commands().front());
        blocked_commit = node_bus.commit_frame(command::FrameNumber{1U});
        activated.emplace(node_session.activate_checkpoint());
        if (!activated->has_value()) {
            node_session.close();
        }
    } else {
        node_session.close();
    }
    finish_thread(worker, director_capture);

    RESUME_CHECK(node_result);
    RESUME_CHECK(node_result.value().mode == net::ResumeMode::Checkpoint);
    RESUME_CHECK(node_result.value().checkpoint_staged);
    RESUME_CHECK(node_result.value().frames == 0U);
    RESUME_CHECK(node_result.value().commands == 0U);
    RESUME_CHECK(staged_state == net::NodeSessionState::CheckpointStaged);
    RESUME_CHECK(staged_cursor == original_cursor);
    RESUME_CHECK(staged_document_empty);
    RESUME_CHECK(staged_checkpoint_matches);
    RESUME_CHECK(blocked_receive == command::ReceiveStatus::CheckpointPending);
    RESUME_CHECK(blocked_commit.has_value());
    RESUME_CHECK(blocked_commit->status == command::CommitStatus::CheckpointPending);
    RESUME_CHECK(activated.has_value());
    RESUME_CHECK(activated->has_value());
    RESUME_CHECK(node_session.state() == net::NodeSessionState::Live);
    RESUME_CHECK(node_bus.prepared_checkpoint() == nullptr);
    RESUME_CHECK(node_bus.cursor() == checkpoint.cursor);
    RESUME_CHECK(node_bus.document().contains(document::NodeId{1U}));
    RESUME_CHECK(director_capture.result->has_value());
    RESUME_CHECK(
        director_capture.result->value().mode == net::ResumeMode::Checkpoint);
    RESUME_CHECK(director_session.state() == net::DirectorSessionState::Live);

    node_session.close();
    director_session.close();
}

void forced_checkpoint_stays_staged_until_activation() {
    checkpoint_case(true, director_epoch(), 102U);
}

void old_epoch_checkpoint_stays_staged_until_activation() {
    checkpoint_case(false, id(999U), 103U);
}

void replay_window_overflow_falls_back_to_checkpoint() {
    checkpoint_case(false, director_epoch(), 107U, 1U);
}

void faulted_node_automatically_requests_checkpoint() {
    checkpoint_case(
        false, director_epoch(), 109U, std::nullopt, true);
}

void reject_case(
    command::NodeCommandBus& node_bus,
    net::Id128 requested_show,
    net::RejectCode reject_code,
    net::SessionProtocolErrorCode director_error,
    std::uint64_t session_low) {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler scheduler;
    persist::DurableDirector durable{scheduler, log};
    const auto original_cursor = node_bus.cursor();
    TcpPair pair;
    TestLeaseAuthority leases;
    net::DirectorResumeSession director_session{
        pair.take_director(), director_config(session_low), durable, leases};
    net::NodeResumeSession node_session{
        pair.take_node(), node_bus, node_config(requested_show)};
    ResumeCapture director_capture;
    auto worker = start_director_resume(director_session, director_capture);
    auto node_result = node_session.resume();
    finish_thread(worker, director_capture);

    RESUME_CHECK(!node_result);
    RESUME_CHECK(!director_capture.result->has_value());
    const auto* node_failure = protocol_failure(node_result);
    const auto* director_failure = protocol_failure(*director_capture.result);
    RESUME_CHECK(node_failure != nullptr);
    RESUME_CHECK(director_failure != nullptr);
    RESUME_CHECK(node_failure->code == net::SessionProtocolErrorCode::PeerRejected);
    RESUME_CHECK(node_failure->reject_code == reject_code);
    RESUME_CHECK(director_failure->code == director_error);
    RESUME_CHECK(director_failure->reject_code == reject_code);
    RESUME_CHECK(node_session.state() == net::NodeSessionState::Closed);
    RESUME_CHECK(director_session.state() == net::DirectorSessionState::Closed);
    RESUME_CHECK(node_bus.cursor() == original_cursor);
    RESUME_CHECK(leases.claims == 0U);
    RESUME_CHECK(leases.releases == 0U);
}

void duplicate_node_lease_is_rejected_before_welcome() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler scheduler;
    persist::DurableDirector durable{scheduler, log};
    command::NodeCommandBus node_bus;
    TcpPair pair;
    TestLeaseAuthority leases;
    leases.allow = false;
    net::DirectorResumeSession director_session{
        pair.take_director(), director_config(110U), durable, leases};
    net::NodeResumeSession node_session{
        pair.take_node(), node_bus, node_config()};
    ResumeCapture director_capture;
    auto worker = start_director_resume(director_session, director_capture);
    auto node_result = node_session.resume();
    finish_thread(worker, director_capture);

    RESUME_CHECK(!node_result);
    RESUME_CHECK(!director_capture.result->has_value());
    const auto* node_failure = protocol_failure(node_result);
    const auto* director_failure = protocol_failure(*director_capture.result);
    RESUME_CHECK(node_failure != nullptr);
    RESUME_CHECK(director_failure != nullptr);
    RESUME_CHECK(node_failure->code == net::SessionProtocolErrorCode::PeerRejected);
    RESUME_CHECK(
        node_failure->reject_code == net::RejectCode::NodeLeaseRejected);
    RESUME_CHECK(
        director_failure->code
        == net::SessionProtocolErrorCode::NodeLeaseRejected);
    RESUME_CHECK(
        director_failure->reject_code == net::RejectCode::NodeLeaseRejected);
    RESUME_CHECK(leases.claims == 1U);
    RESUME_CHECK(leases.releases == 0U);
    RESUME_CHECK(!director_session.peer_identity().has_value());
    RESUME_CHECK(node_bus.cursor() == genesis_cursor());
}

void bounded_lease_registry_fences_incarnations_and_stale_release() {
    net::NodeSessionLeaseRegistry registry{2U};
    const net::NodeSessionIdentity first{show_id(), id(10U), id(11U)};
    const net::NodeSessionIdentity replacement{show_id(), id(10U), id(12U)};
    const net::NodeSessionIdentity second{show_id(), id(20U), id(21U)};
    const net::NodeSessionIdentity third{show_id(), id(30U), id(31U)};

    RESUME_CHECK(registry.claim(first, id(100U)));
    RESUME_CHECK(registry.owns(first, id(100U)));
    RESUME_CHECK(registry.active_sessions() == 1U);
    RESUME_CHECK(!registry.claim(replacement, id(101U)));
    RESUME_CHECK(!registry.claim(second, id(100U)));
    RESUME_CHECK(registry.claim(second, id(102U)));
    RESUME_CHECK(!registry.claim(third, id(103U)));

    registry.release(replacement, id(101U));
    RESUME_CHECK(registry.owns(first, id(100U)));
    RESUME_CHECK(registry.active_sessions() == 2U);
    registry.release(first, id(100U));
    RESUME_CHECK(!registry.owns(first, id(100U)));
    RESUME_CHECK(registry.claim(replacement, id(101U)));
    RESUME_CHECK(registry.owns(replacement, id(101U)));
    RESUME_CHECK(registry.active_sessions() == 2U);
}

void show_mismatch_and_cursor_ahead_are_rejected() {
    command::NodeCommandBus wrong_show_node;
    reject_case(
        wrong_show_node,
        id(404U),
        net::RejectCode::ShowMismatch,
        net::SessionProtocolErrorCode::ShowMismatch,
        104U);

    command::NodeCommandBus ahead_node{
        document::Root{},
        command::FrameNumber{1U},
        command::CommandId{1U}};
    reject_case(
        ahead_node,
        show_id(),
        net::RejectCode::InvalidCursor,
        net::SessionProtocolErrorCode::CursorAhead,
        105U);
}

void live_durable_batch_requires_explicit_node_commit() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler scheduler;
    persist::DurableDirector durable{scheduler, log};
    command::NodeCommandBus node_bus;
    TcpPair pair;
    TestLeaseAuthority leases;
    net::DirectorResumeSession director_session{
        pair.take_director(), director_config(106U), durable, leases};
    net::NodeResumeSession node_session{
        pair.take_node(), node_bus, node_config()};
    ResumeCapture director_capture;
    auto worker = start_director_resume(director_session, director_capture);
    auto node_resume = node_session.resume();
    finish_thread(worker, director_capture);
    RESUME_CHECK(node_resume);
    RESUME_CHECK(director_capture.result->has_value());

    TempWal foreign_temporary;
    auto foreign_log = open_log(foreign_temporary.path());
    command::DirectorScheduler foreign_scheduler;
    persist::DurableDirector foreign{foreign_scheduler, foreign_log};
    schedule_put(foreign_scheduler, 9U, 90, 9U, 1U);
    auto foreign_batch = foreign.seal_frame(command::FrameNumber{1U});
    RESUME_CHECK(foreign_batch);
    auto unauthorized = director_session.send_committed(foreign_batch.value());
    RESUME_CHECK(!unauthorized);
    const auto* authorization = std::get_if<net::SessionProtocolFailure>(
        &unauthorized.error());
    RESUME_CHECK(authorization != nullptr);
    RESUME_CHECK(
        authorization->code == net::SessionProtocolErrorCode::UnauthorizedBatch);
    RESUME_CHECK(director_session.state() == net::DirectorSessionState::Live);
    RESUME_CHECK(director_session.sent_cursor() == genesis_cursor());

    schedule_put(scheduler, 2U, 20, 2U, 1U);
    auto committed = durable.seal_frame(command::FrameNumber{1U});
    RESUME_CHECK(committed);
    RESUME_CHECK(committed.value().receipt().durable);
    ReceiveCapture receive_capture;
    auto receive_worker = start_live_receive(node_session, receive_capture);
    auto sent = director_session.send_committed(committed.value());
    if (!sent) {
        node_session.close();
    }
    finish_receive(receive_worker, receive_capture);

    RESUME_CHECK(sent);
    RESUME_CHECK(receive_capture.result->has_value());
    auto prepared_ack = director_session.receive_frame_acknowledgement();
    RESUME_CHECK(prepared_ack);
    RESUME_CHECK(
        prepared_ack.value().kind
        == net::FrameAcknowledgementKind::Prepared);
    RESUME_CHECK(prepared_ack.value().sequence == 2U);
    RESUME_CHECK(prepared_ack.value().acknowledged_sequence == 2U);
    RESUME_CHECK(prepared_ack.value().before == genesis_cursor());
    RESUME_CHECK(prepared_ack.value().after == log.tail_cursor());
    RESUME_CHECK(prepared_ack.value().outstanding_frames == 1U);
    RESUME_CHECK(receive_capture.result->value().before == genesis_cursor());
    RESUME_CHECK(receive_capture.result->value().after == log.tail_cursor());
    RESUME_CHECK(receive_capture.result->value().commands == 1U);
    RESUME_CHECK(node_bus.cursor() == genesis_cursor());
    RESUME_CHECK(node_bus.document().empty());
    RESUME_CHECK(node_bus.is_prepared(command::FrameNumber{1U}));
    RESUME_CHECK(node_session.delivered_cursor() == log.tail_cursor());
    RESUME_CHECK(director_session.sent_cursor() == log.tail_cursor());

    auto blocked_before_permit =
        node_session.commit_prepared_frame(command::FrameNumber{1U});
    RESUME_CHECK(!blocked_before_permit);
    const auto* missing_permit = std::get_if<net::SessionProtocolFailure>(
        &blocked_before_permit.error());
    RESUME_CHECK(missing_permit != nullptr);
    RESUME_CHECK(
        missing_permit->code
        == net::SessionProtocolErrorCode::FrameNotPermitted);
    RESUME_CHECK(node_bus.cursor() == genesis_cursor());
    RESUME_CHECK(node_session.state() == net::NodeSessionState::Live);

    const auto permit = permit_next_live_frame(
        director_session,
        node_session,
        1U,
        2U,
        genesis_cursor(),
        log.tail_cursor());
    RESUME_CHECK(permit.sequence == 3U);
    RESUME_CHECK(permit.acknowledged_sequence == 2U);
    RESUME_CHECK(permit.before == genesis_cursor());
    RESUME_CHECK(permit.after == log.tail_cursor());
    RESUME_CHECK(director_session.permitted_frames() == 1U);
    RESUME_CHECK(node_session.permitted_frames() == 1U);
    RESUME_CHECK(director_session.last_commit_permit_ordinal() == 1U);
    RESUME_CHECK(node_session.last_commit_permit_ordinal() == 1U);

    auto committed_at_show_clock =
        node_session.commit_prepared_frame(command::FrameNumber{1U});
    RESUME_CHECK(committed_at_show_clock);
    RESUME_CHECK(
        committed_at_show_clock.value().status
        == command::CommitStatus::Committed);
    RESUME_CHECK(node_session.inflight_frames() == 1U);
    RESUME_CHECK(node_session.pending_frame_acknowledgements() == 1U);
    auto committed_flush = node_session.flush_frame_acknowledgements();
    RESUME_CHECK(committed_flush);
    RESUME_CHECK(committed_flush.value().flushed == 1U);
    RESUME_CHECK(committed_flush.value().remaining == 0U);
    RESUME_CHECK(node_session.inflight_frames() == 0U);
    auto committed_ack = director_session.receive_frame_acknowledgement();
    RESUME_CHECK(committed_ack);
    RESUME_CHECK(
        committed_ack.value().kind
        == net::FrameAcknowledgementKind::Committed);
    RESUME_CHECK(committed_ack.value().sequence == 3U);
    RESUME_CHECK(committed_ack.value().acknowledged_sequence == 2U);
    RESUME_CHECK(committed_ack.value().before == genesis_cursor());
    RESUME_CHECK(committed_ack.value().after == log.tail_cursor());
    RESUME_CHECK(committed_ack.value().outstanding_frames == 0U);
    RESUME_CHECK(node_bus.cursor() == log.tail_cursor());
    const auto* applied = node_bus.document().get(document::NodeId{2U});
    RESUME_CHECK(applied != nullptr);
    RESUME_CHECK(*applied == make_node(2U, 20));

    node_session.close();
    director_session.close();
}

void hub_snapshot_and_live_queue_form_one_session_handoff() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler scheduler;
    persist::DurableDirector durable{scheduler, log};
    net::DirectorSessionHub hub{durable};

    schedule_put(scheduler, 1U, 10, 1U, 1U);
    auto first = hub.seal_and_publish(command::FrameNumber{1U});
    RESUME_CHECK(first);
    RESUME_CHECK(first.value().publication.queued == 0U);

    auto subscribed = hub.subscribe();
    RESUME_CHECK(subscribed);
    auto subscription = std::move(subscribed).value();
    RESUME_CHECK(
        subscription.source.checkpoint().cursor
        == first.value().frame.after_cursor());

    schedule_put(scheduler, 2U, 20, 2U, 2U);
    auto second = hub.seal_and_publish(command::FrameNumber{2U});
    RESUME_CHECK(second);
    RESUME_CHECK(second.value().publication.queued == 1U);

    command::NodeCommandBus node_bus;
    TcpPair pair;
    TestLeaseAuthority leases;
    net::DirectorResumeSession director_session{
        pair.take_director(), director_config(109U), durable, leases};
    net::NodeResumeSession node_session{
        pair.take_node(), node_bus, node_config()};

    ResumeCapture director_capture;
    auto worker = start_director_resume(
        director_session, std::move(subscription.source), director_capture);
    auto node_resume = node_session.resume();
    finish_thread(worker, director_capture);
    RESUME_CHECK(node_resume);
    RESUME_CHECK(director_capture.result->has_value());
    RESUME_CHECK(node_resume.value().mode == net::ResumeMode::Replay);
    RESUME_CHECK(node_resume.value().target == first.value().frame.after_cursor());
    RESUME_CHECK(node_bus.is_prepared(command::FrameNumber{1U}));

    auto replay_committed =
        node_session.commit_prepared_frame(command::FrameNumber{1U});
    RESUME_CHECK(replay_committed);
    auto replay_flush = node_session.flush_frame_acknowledgements();
    RESUME_CHECK(replay_flush);
    RESUME_CHECK(replay_flush.value().flushed == 1U);
    auto replay_ack = director_session.receive_frame_acknowledgement();
    RESUME_CHECK(replay_ack);
    RESUME_CHECK(
        replay_ack.value().kind == net::FrameAcknowledgementKind::Committed);
    RESUME_CHECK(replay_ack.value().acknowledged_sequence == 1U);

    auto queued = subscription.live.try_pop();
    RESUME_CHECK(queued.state == net::DirectorSubscriptionState::Active);
    RESUME_CHECK(queued.frame.has_value());
    RESUME_CHECK(
        queued.frame->before_cursor() == first.value().frame.after_cursor());
    RESUME_CHECK(
        queued.frame->after_cursor() == second.value().frame.after_cursor());

    ReceiveCapture receive_capture;
    auto receive_worker = start_live_receive(node_session, receive_capture);
    auto sent = director_session.send_committed(queued.frame->batch());
    if (!sent) {
        node_session.close();
    }
    finish_receive(receive_worker, receive_capture);
    RESUME_CHECK(sent);
    RESUME_CHECK(receive_capture.result->has_value());
    auto prepared_ack = director_session.receive_frame_acknowledgement();
    RESUME_CHECK(prepared_ack);
    RESUME_CHECK(
        prepared_ack.value().kind
        == net::FrameAcknowledgementKind::Prepared);
    RESUME_CHECK(prepared_ack.value().acknowledged_sequence == 3U);

    const auto live_permit = permit_next_live_frame(
        director_session,
        node_session,
        1U,
        3U,
        first.value().frame.after_cursor(),
        second.value().frame.after_cursor());
    RESUME_CHECK(live_permit.sequence == 4U);
    RESUME_CHECK(live_permit.acknowledged_sequence == 3U);

    auto live_committed =
        node_session.commit_prepared_frame(command::FrameNumber{2U});
    RESUME_CHECK(live_committed);
    auto live_flush = node_session.flush_frame_acknowledgements();
    RESUME_CHECK(live_flush);
    RESUME_CHECK(live_flush.value().flushed == 1U);
    auto live_ack = director_session.receive_frame_acknowledgement();
    RESUME_CHECK(live_ack);
    RESUME_CHECK(
        live_ack.value().kind == net::FrameAcknowledgementKind::Committed);
    RESUME_CHECK(live_ack.value().acknowledged_sequence == 3U);
    RESUME_CHECK(node_bus.cursor() == second.value().frame.after_cursor());
    RESUME_CHECK(node_bus.document().contains(document::NodeId{1U}));
    RESUME_CHECK(node_bus.document().contains(document::NodeId{2U}));

    subscription.live.close();
    node_session.close();
    director_session.close();
}

void live_receive_window_reopens_after_show_clock_commit() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler scheduler;
    persist::DurableDirector durable{scheduler, log};
    command::NodeCommandBus node_bus;
    TcpPair pair;
    TestLeaseAuthority leases;
    net::DirectorResumeSession director_session{
        pair.take_director(), director_config(108U), durable, leases};
    auto config = node_config();
    config.limits.max_inflight_frames = 1U;
    net::NodeResumeSession node_session{
        pair.take_node(), node_bus, config};
    ResumeCapture director_capture;
    auto worker = start_director_resume(director_session, director_capture);
    auto resumed = node_session.resume();
    finish_thread(worker, director_capture);
    RESUME_CHECK(resumed);
    RESUME_CHECK(director_capture.result->has_value());

    schedule_put(scheduler, 1U, 10, 1U, 1U);
    auto first = durable.seal_frame(command::FrameNumber{1U});
    RESUME_CHECK(first);
    schedule_put(scheduler, 2U, 20, 2U, 2U);
    auto second = durable.seal_frame(command::FrameNumber{2U});
    RESUME_CHECK(second);
    RESUME_CHECK(director_session.send_committed(first.value()));
    auto blocked_send = director_session.send_committed(second.value());
    RESUME_CHECK(!blocked_send);
    const auto* director_capacity = std::get_if<net::SessionProtocolFailure>(
        &blocked_send.error());
    RESUME_CHECK(director_capacity != nullptr);
    RESUME_CHECK(
        director_capacity->code
        == net::SessionProtocolErrorCode::InflightWindowFull);

    auto first_received = node_session.receive_live_frame();
    RESUME_CHECK(first_received);
    RESUME_CHECK(node_bus.is_prepared(command::FrameNumber{1U}));
    auto first_prepared = director_session.receive_frame_acknowledgement();
    RESUME_CHECK(first_prepared);
    RESUME_CHECK(
        first_prepared.value().kind
        == net::FrameAcknowledgementKind::Prepared);
    RESUME_CHECK(first_prepared.value().outstanding_frames == 1U);
    auto still_blocked = director_session.send_committed(second.value());
    RESUME_CHECK(!still_blocked);
    const auto* prepared_capacity = std::get_if<net::SessionProtocolFailure>(
        &still_blocked.error());
    RESUME_CHECK(prepared_capacity != nullptr);
    RESUME_CHECK(
        prepared_capacity->code
        == net::SessionProtocolErrorCode::InflightWindowFull);
    auto wrong_boundary =
        node_session.commit_prepared_frame(command::FrameNumber{2U});
    RESUME_CHECK(!wrong_boundary);
    const auto* wrong_boundary_failure =
        std::get_if<net::SessionProtocolFailure>(&wrong_boundary.error());
    RESUME_CHECK(wrong_boundary_failure != nullptr);
    RESUME_CHECK(
        wrong_boundary_failure->code
        == net::SessionProtocolErrorCode::FrameNotPrepared);
    RESUME_CHECK(!node_bus.needs_resync());
    RESUME_CHECK(node_bus.is_prepared(command::FrameNumber{1U}));
    auto window_full = node_session.receive_live_frame();
    RESUME_CHECK(!window_full);
    const auto* capacity = std::get_if<net::SessionProtocolFailure>(
        &window_full.error());
    RESUME_CHECK(capacity != nullptr);
    RESUME_CHECK(
        capacity->code == net::SessionProtocolErrorCode::InflightWindowFull);
    RESUME_CHECK(node_session.state() == net::NodeSessionState::Live);
    RESUME_CHECK(!node_bus.is_prepared(command::FrameNumber{2U}));

    const auto first_permit = permit_next_live_frame(
        director_session,
        node_session,
        1U,
        2U,
        genesis_cursor(),
        first.value().after_cursor());
    RESUME_CHECK(first_permit.acknowledged_sequence == 2U);

    auto first_committed =
        node_session.commit_prepared_frame(command::FrameNumber{1U});
    RESUME_CHECK(first_committed);
    RESUME_CHECK(node_session.inflight_frames() == 1U);
    RESUME_CHECK(node_session.pending_frame_acknowledgements() == 1U);
    auto still_full_until_flush = node_session.receive_live_frame();
    RESUME_CHECK(!still_full_until_flush);
    const auto* queued_capacity = std::get_if<net::SessionProtocolFailure>(
        &still_full_until_flush.error());
    RESUME_CHECK(queued_capacity != nullptr);
    RESUME_CHECK(
        queued_capacity->code
        == net::SessionProtocolErrorCode::InflightWindowFull);
    auto first_commit_flush = node_session.flush_frame_acknowledgements();
    RESUME_CHECK(first_commit_flush);
    RESUME_CHECK(first_commit_flush.value().flushed == 1U);
    RESUME_CHECK(node_session.inflight_frames() == 0U);
    auto first_commit_ack = director_session.receive_frame_acknowledgement();
    RESUME_CHECK(first_commit_ack);
    RESUME_CHECK(
        first_commit_ack.value().kind
        == net::FrameAcknowledgementKind::Committed);
    RESUME_CHECK(first_commit_ack.value().outstanding_frames == 0U);
    RESUME_CHECK(director_session.send_committed(second.value()));
    auto second_received = node_session.receive_live_frame();
    RESUME_CHECK(second_received);
    auto second_prepared = director_session.receive_frame_acknowledgement();
    RESUME_CHECK(second_prepared);
    RESUME_CHECK(
        second_prepared.value().kind
        == net::FrameAcknowledgementKind::Prepared);
    RESUME_CHECK(node_bus.is_prepared(command::FrameNumber{2U}));
    const auto second_permit = permit_next_live_frame(
        director_session,
        node_session,
        2U,
        4U,
        first.value().after_cursor(),
        second.value().after_cursor());
    RESUME_CHECK(second_permit.acknowledged_sequence == 4U);
    auto second_committed =
        node_session.commit_prepared_frame(command::FrameNumber{2U});
    RESUME_CHECK(second_committed);
    auto second_commit_flush = node_session.flush_frame_acknowledgements();
    RESUME_CHECK(second_commit_flush);
    RESUME_CHECK(second_commit_flush.value().flushed == 1U);
    auto second_commit_ack = director_session.receive_frame_acknowledgement();
    RESUME_CHECK(second_commit_ack);
    RESUME_CHECK(
        second_commit_ack.value().kind
        == net::FrameAcknowledgementKind::Committed);
    RESUME_CHECK(second_commit_ack.value().outstanding_frames == 0U);
    RESUME_CHECK(node_bus.cursor() == log.tail_cursor());

    node_session.close();
    director_session.close();
}

void next_live_receive_flushes_queued_commit_before_its_prepared_ack() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler scheduler;
    persist::DurableDirector durable{scheduler, log};
    command::NodeCommandBus node_bus;
    TcpPair pair;
    TestLeaseAuthority leases;
    net::DirectorResumeSession director_session{
        pair.take_director(), director_config(111U), durable, leases};
    net::NodeResumeSession node_session{
        pair.take_node(), node_bus, node_config()};
    ResumeCapture director_capture;
    auto worker = start_director_resume(director_session, director_capture);
    auto resumed = node_session.resume();
    finish_thread(worker, director_capture);
    RESUME_CHECK(resumed);
    RESUME_CHECK(director_capture.result->has_value());

    schedule_put(scheduler, 1U, 10, 1U, 1U);
    auto first = durable.seal_frame(command::FrameNumber{1U});
    RESUME_CHECK(first);
    schedule_put(scheduler, 2U, 20, 2U, 2U);
    auto second = durable.seal_frame(command::FrameNumber{2U});
    RESUME_CHECK(second);

    RESUME_CHECK(director_session.send_committed(first.value()));
    auto first_received = node_session.receive_live_frame();
    RESUME_CHECK(first_received);
    auto first_prepared = director_session.receive_frame_acknowledgement();
    RESUME_CHECK(first_prepared);
    RESUME_CHECK(
        first_prepared.value().kind
        == net::FrameAcknowledgementKind::Prepared);
    RESUME_CHECK(first_prepared.value().sequence == 2U);

    const auto first_permit = permit_next_live_frame(
        director_session,
        node_session,
        1U,
        2U,
        genesis_cursor(),
        first.value().after_cursor());
    RESUME_CHECK(first_permit.sequence == 3U);
    RESUME_CHECK(first_permit.acknowledged_sequence == 2U);

    auto first_committed =
        node_session.commit_prepared_frame(command::FrameNumber{1U});
    RESUME_CHECK(first_committed);
    RESUME_CHECK(node_session.pending_frame_acknowledgements() == 1U);
    RESUME_CHECK(node_session.inflight_frames() == 1U);

    RESUME_CHECK(director_session.send_committed(second.value()));
    auto second_received = node_session.receive_live_frame();
    RESUME_CHECK(second_received);
    RESUME_CHECK(node_session.pending_frame_acknowledgements() == 0U);
    RESUME_CHECK(node_session.inflight_frames() == 1U);

    auto first_commit_ack = director_session.receive_frame_acknowledgement();
    RESUME_CHECK(first_commit_ack);
    RESUME_CHECK(
        first_commit_ack.value().kind
        == net::FrameAcknowledgementKind::Committed);
    RESUME_CHECK(first_commit_ack.value().sequence == 3U);
    RESUME_CHECK(first_commit_ack.value().acknowledged_sequence == 2U);
    RESUME_CHECK(first_commit_ack.value().outstanding_frames == 1U);

    auto second_prepared = director_session.receive_frame_acknowledgement();
    RESUME_CHECK(second_prepared);
    RESUME_CHECK(
        second_prepared.value().kind
        == net::FrameAcknowledgementKind::Prepared);
    RESUME_CHECK(second_prepared.value().sequence == 4U);
    RESUME_CHECK(second_prepared.value().acknowledged_sequence == 4U);

    const auto second_permit = permit_next_live_frame(
        director_session,
        node_session,
        2U,
        4U,
        first.value().after_cursor(),
        second.value().after_cursor());
    RESUME_CHECK(second_permit.sequence == 5U);
    RESUME_CHECK(second_permit.acknowledged_sequence == 4U);

    auto second_committed =
        node_session.commit_prepared_frame(command::FrameNumber{2U});
    RESUME_CHECK(second_committed);
    auto final_flush = node_session.flush_frame_acknowledgements();
    RESUME_CHECK(final_flush);
    RESUME_CHECK(final_flush.value().flushed == 1U);
    auto second_commit_ack = director_session.receive_frame_acknowledgement();
    RESUME_CHECK(second_commit_ack);
    RESUME_CHECK(
        second_commit_ack.value().kind
        == net::FrameAcknowledgementKind::Committed);
    RESUME_CHECK(second_commit_ack.value().sequence == 5U);
    RESUME_CHECK(second_commit_ack.value().acknowledged_sequence == 4U);
    RESUME_CHECK(second_commit_ack.value().outstanding_frames == 0U);
    RESUME_CHECK(node_session.inflight_frames() == 0U);
    RESUME_CHECK(node_bus.cursor() == log.tail_cursor());

    node_session.close();
    director_session.close();
}

[[nodiscard]] std::vector<std::byte> fake_welcome() {
    return encode_session_message(net::SessionMessage{net::ServerWelcome{
        net::current_session_format_version,
        show_id(),
        id(200U),
        director_epoch(),
        net::ResumeMode::Replay,
        genesis_cursor(),
        command::StreamCursor{
            command::FrameNumber{1U},
            document::Version{1U},
            command::CommandId{2U}},
        net::default_session_limits,
    }});
}

[[nodiscard]] std::vector<std::byte> fake_frame(bool malformed_seal) {
    const auto authored = make_command();
    const auto seal = seal_for(authored);
    auto command_packet = encode_wire_message(wire::Message{authored});
    auto seal_packet = malformed_seal
        ? std::vector<std::byte>{std::byte{0xa5U}}
        : encode_wire_message(wire::Message{seal});
    return encode_session_message(net::SessionMessage{net::FrameBatch{
        id(200U),
        1U,
        genesis_cursor(),
        command::StreamCursor{
            command::FrameNumber{1U},
            document::Version{1U},
            command::CommandId{2U}},
        {
            net::Packet{std::move(command_packet)},
            net::Packet{std::move(seal_packet)},
        },
    }});
}

void interrupted_frame_case(bool malformed_seal, bool truncate_frame) {
    command::NodeCommandBus node_bus;
    const auto original_cursor = node_bus.cursor();
    TcpPair pair;
    net::NodeResumeSession node_session{
        pair.take_node(), node_bus, node_config()};
    PeerCapture peer_capture;
    auto peer = start_fake_replay_peer(
        pair.take_director(),
        fake_welcome(),
        fake_frame(malformed_seal),
        truncate_frame,
        peer_capture);
    auto resumed = node_session.resume();
    finish_peer(peer, peer_capture);

    RESUME_CHECK(!resumed);
    RESUME_CHECK(node_session.state() == net::NodeSessionState::Closed);
    RESUME_CHECK(node_bus.cursor() == original_cursor);
    RESUME_CHECK(node_bus.document().empty());
    RESUME_CHECK(!node_bus.is_prepared(command::FrameNumber{1U}));
    RESUME_CHECK(!node_bus.needs_resync());
    if (truncate_frame) {
        const auto* failure = std::get_if<net::FramedStreamError>(&resumed.error());
        RESUME_CHECK(failure != nullptr);
        RESUME_CHECK(failure->code == net::FramedStreamErrorCode::ReadFailed);
    } else {
        const auto* failure = std::get_if<net::WireDecodeFailure>(&resumed.error());
        RESUME_CHECK(failure != nullptr);
        RESUME_CHECK(failure->packet_index == 1U);
    }
}

void malformed_and_truncated_frames_are_atomic() {
    interrupted_frame_case(true, false);
    interrupted_frame_case(false, true);
}

void transactional_node_rejection_preserves_buffered_traffic() {
    command::NodeCommandBus node_bus;
    auto buffered = make_command();
    buffered.forward = put(1U, 99);
    RESUME_CHECK(
        node_bus.receive(buffered) == command::ReceiveStatus::Accepted);
    const auto original_cursor = node_bus.cursor();

    TcpPair pair;
    net::NodeResumeSession node_session{
        pair.take_node(), node_bus, node_config()};
    PeerCapture peer_capture;
    auto peer = start_fake_replay_peer(
        pair.take_director(),
        fake_welcome(),
        fake_frame(false),
        false,
        peer_capture);
    auto resumed = node_session.resume();
    finish_peer(peer, peer_capture);

    RESUME_CHECK(!resumed);
    const auto* failure = std::get_if<net::NodeBatchFailure>(&resumed.error());
    RESUME_CHECK(failure != nullptr);
    RESUME_CHECK(
        failure->error.code == command::BatchReceiveErrorCode::WouldFault);
    RESUME_CHECK(failure->error.fault.has_value());
    RESUME_CHECK(
        failure->error.fault->code == command::FaultCode::ConflictingCommand);
    RESUME_CHECK(node_bus.cursor() == original_cursor);
    RESUME_CHECK(node_bus.document().empty());
    RESUME_CHECK(!node_bus.needs_resync());
    RESUME_CHECK(!node_bus.is_prepared(command::FrameNumber{1U}));
    RESUME_CHECK(
        node_bus.receive(buffered) == command::ReceiveStatus::Duplicate);
}

void silent_peer_hits_configured_handshake_deadline() {
    command::NodeCommandBus node_bus;
    TcpPair pair;
    auto config = node_config();
    config.io.handshake_timeout = std::chrono::milliseconds{75};
    config.io.read_timeout = std::chrono::milliseconds{75};
    config.io.write_timeout = std::chrono::milliseconds{75};
    net::NodeResumeSession node_session{
        pair.take_node(), node_bus, std::move(config)};
    auto silent_peer = pair.take_director();

    auto resumed = node_session.resume();

    RESUME_CHECK(!resumed);
    const auto* failure = std::get_if<net::FramedStreamError>(&resumed.error());
    RESUME_CHECK(failure != nullptr);
    RESUME_CHECK(failure->code == net::FramedStreamErrorCode::TimedOut);
    RESUME_CHECK(node_session.state() == net::NodeSessionState::Closed);
    RESUME_CHECK(node_bus.cursor() == genesis_cursor());
    RESUME_CHECK(node_bus.document().empty());
    RESUME_CHECK(silent_peer.is_open());
}

void concurrent_control_writer_accepts_fast_peer_replies() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler scheduler;
    persist::DurableDirector durable{scheduler, log};
    command::NodeCommandBus node_bus;
    TcpPair pair;
    TestLeaseAuthority leases;
    auto director_options = director_config(120U);
    auto node_options = node_config();
    // Reuse the only in-flight slot immediately after each Committed message.
    director_options.limits.max_inflight_frames = 1U;
    node_options.limits.max_inflight_frames = 1U;
    director_options.io.read_timeout = std::chrono::seconds{2};
    node_options.io.read_timeout = std::chrono::seconds{2};
    net::DirectorResumeSession director{
        pair.take_director(), std::move(director_options), durable, leases};
    net::NodeResumeSession node{
        pair.take_node(), node_bus, std::move(node_options)};
    ResumeCapture resume_capture;
    auto handshake = start_director_resume(director, resume_capture);
    const auto resumed = node.resume();
    finish_thread(handshake, resume_capture);
    RESUME_CHECK(resumed && resume_capture.result->has_value());

    constexpr core::u64 frame_count = 512U;
    std::exception_ptr reader_failure;
    std::exception_ptr writer_failure;
    std::exception_ptr director_failure;
    std::jthread writer{[&](std::stop_token stop) {
        try {
            while (!stop.stop_requested()) {
                const auto flushed = node.flush_frame_acknowledgements();
                RESUME_CHECK(flushed);
                std::this_thread::yield();
            }
        } catch (...) {
            writer_failure = std::current_exception();
            node.close();
        }
    }};
    std::jthread reader{[&] {
        try {
            for (core::u64 message = 0; message < frame_count * 2U; ++message) {
                const auto received = node.receive_live_message();
                if (!received) {
                    const auto* failure = std::get_if<net::SessionProtocolFailure>(
                        &received.error());
                    throw std::runtime_error{"fast peer reply failed, protocol code "
                        + std::to_string(failure ? static_cast<unsigned>(failure->code) : 255U)};
                }
                if (const auto* permit = std::get_if<net::FrameCommitPermitReport>(
                        &received.value())) {
                    RESUME_CHECK(node.commit_prepared_frame(permit->after.through_frame));
                } else {
                    RESUME_CHECK(std::holds_alternative<net::ReceivedFrameReport>(received.value()));
                }
            }
        } catch (...) {
            reader_failure = std::current_exception();
            node.close();
        }
    }};
    try {
        for (core::u64 number = 1U; number <= frame_count; ++number) {
            const auto frame = durable.seal_frame(command::FrameNumber{number});
            RESUME_CHECK(frame);
            RESUME_CHECK(director.send_committed(frame.value()));
            const auto prepared = director.receive_frame_acknowledgement();
            RESUME_CHECK(prepared && prepared.value().kind == net::FrameAcknowledgementKind::Prepared);
            RESUME_CHECK(director.send_commit_permit(net::FrameCommitPermitIntent{
                number, prepared.value().acknowledged_sequence,
                prepared.value().before, prepared.value().after}));
            const auto committed = director.receive_frame_acknowledgement();
            RESUME_CHECK(committed && committed.value().kind == net::FrameAcknowledgementKind::Committed);
        }
    } catch (...) {
        director_failure = std::current_exception();
        node.close();
        director.close();
    }
    writer.request_stop();
    writer.join();
    reader.join();
    if (reader_failure) std::rethrow_exception(reader_failure);
    if (writer_failure) std::rethrow_exception(writer_failure);
    if (director_failure) std::rethrow_exception(director_failure);
    RESUME_CHECK(node_bus.cursor().through_frame == command::FrameNumber{frame_count});
    RESUME_CHECK(node.inflight_frames() == 0U);
    RESUME_CHECK(node.state() == net::NodeSessionState::Live);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"exact tail is up to date", &exact_tail_resumes_up_to_date},
        {"WAL replay content and empty frame",
         &wal_replay_delivers_content_and_empty_frames_without_committing},
        {"forced checkpoint staging", &forced_checkpoint_stays_staged_until_activation},
        {"old epoch checkpoint staging", &old_epoch_checkpoint_stays_staged_until_activation},
        {"replay window checkpoint fallback",
         &replay_window_overflow_falls_back_to_checkpoint},
        {"faulted node checkpoint fallback",
         &faulted_node_automatically_requests_checkpoint},
        {"show and cursor rejects", &show_mismatch_and_cursor_ahead_are_rejected},
        {"duplicate node lease reject",
         &duplicate_node_lease_is_rejected_before_welcome},
        {"bounded node lease registry",
         &bounded_lease_registry_fences_incarnations_and_stale_release},
        {"live durable frame", &live_durable_batch_requires_explicit_node_commit},
        {"hub snapshot and live handoff",
         &hub_snapshot_and_live_queue_form_one_session_handoff},
        {"live receive window", &live_receive_window_reopens_after_show_clock_commit},
        {"queued commit precedes next prepared",
         &next_live_receive_flushes_queued_commit_before_its_prepared_ack},
        {"malformed and truncated atomicity", &malformed_and_truncated_frames_are_atomic},
        {"transactional node rejection",
         &transactional_node_rejection_preserves_buffered_traffic},
        {"session handshake deadline",
         &silent_peer_hits_configured_handshake_deadline},
        {"fast peer replies during control writes",
         &concurrent_control_writer_accepts_fast_peer_replies},
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
