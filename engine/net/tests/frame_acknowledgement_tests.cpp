#include <mediaserver/net/resume_session.hpp>

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

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define ACK_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempWal final {
public:
    TempWal() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("mediaserver-frame-ack-" + std::to_string(timestamp) + "-"
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
        ACK_CHECK(!error);
        acceptor_.set_option(asio::socket_base::reuse_address{true}, error);
        ACK_CHECK(!error);
        acceptor_.bind(endpoint, error);
        ACK_CHECK(!error);
        acceptor_.listen(asio::socket_base::max_listen_connections, error);
        ACK_CHECK(!error);
        node_.connect(acceptor_.local_endpoint(), error);
        ACK_CHECK(!error);
        acceptor_.accept(director_, error);
        ACK_CHECK(!error);
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
        if (active_identity.has_value()) {
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

    std::size_t claims{};
    std::size_t releases{};
    std::optional<net::NodeSessionIdentity> active_identity;
    std::optional<net::Id128> active_session;
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
    ACK_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] document::Node make_node(
    std::uint64_t node_id,
    std::int64_t value) {
    return document::Node{
        document::NodeId{node_id},
        "test.node",
        {{"value", value}},
    };
}

void schedule_put(
    command::DirectorScheduler& scheduler,
    std::uint64_t node_id,
    std::int64_t value,
    std::uint64_t transaction,
    std::uint64_t frame) {
    auto scheduled = scheduler.schedule_edit(
        command::PutNode{make_node(node_id, value)},
        command::EraseNode{document::NodeId{node_id}},
        command::UndoContextId{1U},
        command::TransactionId{transaction},
        command::FrameNumber{frame});
    ACK_CHECK(scheduled);
}

[[nodiscard]] net::DirectorSessionConfig director_config(
    std::uint64_t session_low) {
    net::DirectorSessionConfig config;
    config.show_id = show_id();
    config.director_epoch = director_epoch();
    config.session_id = id(session_low);
    return config;
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
    ACK_CHECK(!error);
    ACK_CHECK(prefix_read == prefix.size());
    const auto payload_size = static_cast<std::size_t>(read_u32_le(prefix));
    ACK_CHECK(payload_size != 0U);
    ACK_CHECK(payload_size <= 1024U * 1024U);

    std::vector<std::byte> packet(payload_size + prefix.size());
    std::memcpy(packet.data(), prefix.data(), prefix.size());
    const auto payload_read = asio::read(
        socket,
        asio::buffer(packet.data() + prefix.size(), payload_size),
        error);
    ACK_CHECK(!error);
    ACK_CHECK(payload_read == payload_size);
    return packet;
}

void write_session_message(
    asio::ip::tcp::socket& socket,
    const net::SessionMessage& message) {
    auto encoded = net::encode_session(message);
    ACK_CHECK(encoded);
    std::error_code error;
    const auto written = asio::write(
        socket, asio::buffer(encoded.value()), error);
    ACK_CHECK(!error);
    ACK_CHECK(written == encoded.value().size());
}

[[nodiscard]] net::SessionMessage read_session_message(
    asio::ip::tcp::socket& socket) {
    auto decoded = net::decode_session(read_raw_packet(socket));
    ACK_CHECK(decoded);
    return std::move(decoded).value();
}

using ResumeResult = core::Result<net::ResumeReport, net::ResumeSessionError>;

class RawDirectorFixture final {
public:
    explicit RawDirectorFixture(std::uint64_t session_low)
        : log_(open_log(temporary_.path())),
          durable_(scheduler_, log_),
          config_(director_config(session_low)),
          director_(pair_.take_director(), config_, durable_, leases_),
          peer_(pair_.take_node()) {
        handshake();
    }

    RawDirectorFixture(const RawDirectorFixture&) = delete;
    RawDirectorFixture& operator=(const RawDirectorFixture&) = delete;

    [[nodiscard]] net::FrameBatch publish_put(
        std::uint64_t node_id,
        std::int64_t value,
        std::uint64_t transaction,
        std::uint64_t frame) {
        schedule_put(scheduler_, node_id, value, transaction, frame);
        auto committed = durable_.seal_frame(command::FrameNumber{frame});
        ACK_CHECK(committed);
        auto sent = director_.send_committed(committed.value());
        ACK_CHECK(sent);
        auto message = read_session_message(peer_);
        auto* batch = std::get_if<net::FrameBatch>(&message);
        ACK_CHECK(batch != nullptr);
        return std::move(*batch);
    }

    void write(const net::SessionMessage& message) {
        write_session_message(peer_, message);
    }

    [[nodiscard]] net::SessionMessage read() {
        return read_session_message(peer_);
    }

    [[nodiscard]] net::DirectorResumeSession& director() noexcept {
        return director_;
    }

    [[nodiscard]] const TestLeaseAuthority& leases() const noexcept {
        return leases_;
    }

private:
    void handshake() {
        std::optional<ResumeResult> resumed;
        std::exception_ptr worker_exception;
        std::thread worker{[this, &resumed, &worker_exception] {
            try {
                resumed.emplace(director_.resume());
            } catch (...) {
                worker_exception = std::current_exception();
            }
        }};

        try {
            write_session_message(
                peer_,
                net::SessionMessage{net::ClientHello{
                    net::current_session_format_version,
                    net::current_session_format_version,
                    show_id(),
                    id(10U),
                    id(11U),
                    10U,
                    director_epoch(),
                    genesis_cursor(),
                    false,
                    net::default_session_limits,
                }});
            auto welcome_message = read_session_message(peer_);
            const auto* welcome = std::get_if<net::ServerWelcome>(
                &welcome_message);
            ACK_CHECK(welcome != nullptr);
            ACK_CHECK(welcome->mode == net::ResumeMode::UpToDate);
            ACK_CHECK(welcome->session_id == config_.session_id);

            auto sync_message = read_session_message(peer_);
            const auto* sync = std::get_if<net::SyncComplete>(&sync_message);
            ACK_CHECK(sync != nullptr);
            ACK_CHECK(sync->sequence == 1U);
            ACK_CHECK(sync->target == genesis_cursor());
            write_session_message(
                peer_,
                net::SessionMessage{net::ResumeAck{
                    config_.session_id,
                    1U,
                    genesis_cursor(),
                }});
        } catch (...) {
            director_.close();
            std::error_code ignored;
            peer_.close(ignored);
            worker.join();
            throw;
        }

        worker.join();
        if (worker_exception != nullptr) {
            std::rethrow_exception(worker_exception);
        }
        ACK_CHECK(resumed.has_value());
        ACK_CHECK(resumed->has_value());
        ACK_CHECK(director_.state() == net::DirectorSessionState::Live);
        ACK_CHECK(leases_.claims == 1U);
    }

    TempWal temporary_;
    persist::CommandLog log_;
    command::DirectorScheduler scheduler_;
    persist::DurableDirector durable_;
    TcpPair pair_;
    TestLeaseAuthority leases_;
    net::DirectorSessionConfig config_;
    net::DirectorResumeSession director_;
    asio::ip::tcp::socket peer_;
};

void expect_protocol_rejection(
    RawDirectorFixture& fixture,
    const net::SessionMessage& acknowledgement,
    net::SessionProtocolErrorCode expected) {
    fixture.write(acknowledgement);
    auto received = fixture.director().receive_frame_acknowledgement();
    ACK_CHECK(!received);
    const auto* failure = std::get_if<net::SessionProtocolFailure>(
        &received.error());
    ACK_CHECK(failure != nullptr);
    ACK_CHECK(failure->code == expected);
    ACK_CHECK(
        fixture.director().state() == net::DirectorSessionState::Closed);
    ACK_CHECK(fixture.leases().releases == 1U);
}

void wrong_session_is_rejected() {
    RawDirectorFixture fixture{120U};
    const auto frame = fixture.publish_put(1U, 10, 1U, 1U);
    expect_protocol_rejection(
        fixture,
        net::SessionMessage{net::FramePrepared{
            id(999U), 2U, frame.sequence, frame.after}},
        net::SessionProtocolErrorCode::SessionMismatch);
}

void wrong_control_sequence_is_rejected() {
    RawDirectorFixture fixture{121U};
    const auto frame = fixture.publish_put(1U, 10, 1U, 1U);
    expect_protocol_rejection(
        fixture,
        net::SessionMessage{net::FramePrepared{
            frame.session_id, 3U, frame.sequence, frame.after}},
        net::SessionProtocolErrorCode::SequenceMismatch);
}

void unknown_batch_sequence_is_rejected() {
    RawDirectorFixture fixture{122U};
    const auto frame = fixture.publish_put(1U, 10, 1U, 1U);
    expect_protocol_rejection(
        fixture,
        net::SessionMessage{net::FramePrepared{
            frame.session_id, 2U, 999U, frame.after}},
        net::SessionProtocolErrorCode::AcknowledgementMismatch);
}

void wrong_cursor_is_rejected() {
    RawDirectorFixture fixture{123U};
    const auto frame = fixture.publish_put(1U, 10, 1U, 1U);
    auto wrong_after = frame.after;
    wrong_after.revision = document::Version{wrong_after.revision.value() + 1U};
    expect_protocol_rejection(
        fixture,
        net::SessionMessage{net::FramePrepared{
            frame.session_id, 2U, frame.sequence, wrong_after}},
        net::SessionProtocolErrorCode::CursorMismatch);
}

void duplicate_prepared_is_rejected() {
    RawDirectorFixture fixture{124U};
    const auto frame = fixture.publish_put(1U, 10, 1U, 1U);
    fixture.write(net::SessionMessage{net::FramePrepared{
        frame.session_id, 2U, frame.sequence, frame.after}});
    auto first = fixture.director().receive_frame_acknowledgement();
    ACK_CHECK(first);
    ACK_CHECK(first.value().kind == net::FrameAcknowledgementKind::Prepared);
    ACK_CHECK(first.value().outstanding_frames == 1U);

    expect_protocol_rejection(
        fixture,
        net::SessionMessage{net::FramePrepared{
            frame.session_id, 3U, frame.sequence, frame.after}},
        net::SessionProtocolErrorCode::AcknowledgementMismatch);
}

void committed_before_prepared_is_rejected() {
    RawDirectorFixture fixture{125U};
    const auto frame = fixture.publish_put(1U, 10, 1U, 1U);
    expect_protocol_rejection(
        fixture,
        net::SessionMessage{net::FrameCommitted{
            frame.session_id, 2U, frame.sequence, frame.after}},
        net::SessionProtocolErrorCode::FrameNotPrepared);
}

void permit_requires_prepared_and_monotonic_ordinal() {
    RawDirectorFixture fixture{127U};
    const auto first = fixture.publish_put(1U, 10, 1U, 1U);
    const auto second = fixture.publish_put(2U, 20, 2U, 2U);

    auto zero_ordinal = fixture.director().send_commit_permit(
        net::FrameCommitPermitIntent{
            0U, first.sequence, first.before, first.after});
    ACK_CHECK(!zero_ordinal);
    const auto* zero_failure = std::get_if<net::SessionProtocolFailure>(
        &zero_ordinal.error());
    ACK_CHECK(zero_failure != nullptr);
    ACK_CHECK(
        zero_failure->code
        == net::SessionProtocolErrorCode::InvalidCommitPermit);

    auto too_early = fixture.director().send_commit_permit(
        net::FrameCommitPermitIntent{
            1U, first.sequence, first.before, first.after});
    ACK_CHECK(!too_early);
    const auto* early_failure = std::get_if<net::SessionProtocolFailure>(
        &too_early.error());
    ACK_CHECK(early_failure != nullptr);
    ACK_CHECK(
        early_failure->code
        == net::SessionProtocolErrorCode::FrameNotPrepared);
    ACK_CHECK(fixture.director().state() == net::DirectorSessionState::Live);

    fixture.write(net::SessionMessage{net::FramePrepared{
        first.session_id, 2U, first.sequence, first.after}});
    ACK_CHECK(fixture.director().receive_frame_acknowledgement());
    auto wrong_identity = fixture.director().send_commit_permit(
        net::FrameCommitPermitIntent{
            7U, second.sequence, second.before, second.after});
    ACK_CHECK(!wrong_identity);
    const auto* identity_failure = std::get_if<net::SessionProtocolFailure>(
        &wrong_identity.error());
    ACK_CHECK(identity_failure != nullptr);
    ACK_CHECK(
        identity_failure->code
        == net::SessionProtocolErrorCode::AcknowledgementMismatch);

    auto wrong_cursor = first.after;
    wrong_cursor.revision = document::Version{
        wrong_cursor.revision.value() + 1U};
    auto cursor_mismatch = fixture.director().send_commit_permit(
        net::FrameCommitPermitIntent{
            7U, first.sequence, first.before, wrong_cursor});
    ACK_CHECK(!cursor_mismatch);
    const auto* cursor_failure = std::get_if<net::SessionProtocolFailure>(
        &cursor_mismatch.error());
    ACK_CHECK(cursor_failure != nullptr);
    ACK_CHECK(
        cursor_failure->code
        == net::SessionProtocolErrorCode::CursorMismatch);

    auto first_permit = fixture.director().send_commit_permit(
        net::FrameCommitPermitIntent{
            7U, first.sequence, first.before, first.after});
    ACK_CHECK(first_permit);
    auto first_wire = fixture.read();
    const auto* first_message = std::get_if<net::FrameCommitPermit>(&first_wire);
    ACK_CHECK(first_message != nullptr);
    ACK_CHECK(first_message->session_id == first.session_id);
    ACK_CHECK(first_message->director_epoch == director_epoch());
    ACK_CHECK(first_message->sequence == 4U);
    ACK_CHECK(first_message->acknowledged_sequence == first.sequence);
    ACK_CHECK(first_message->barrier_ordinal == 7U);
    ACK_CHECK(first_message->before == first.before);
    ACK_CHECK(first_message->after == first.after);

    fixture.write(net::SessionMessage{net::FramePrepared{
        second.session_id, 3U, second.sequence, second.after}});
    ACK_CHECK(fixture.director().receive_frame_acknowledgement());
    auto duplicate_ordinal = fixture.director().send_commit_permit(
        net::FrameCommitPermitIntent{
            7U, second.sequence, second.before, second.after});
    ACK_CHECK(!duplicate_ordinal);
    const auto* ordinal_failure = std::get_if<net::SessionProtocolFailure>(
        &duplicate_ordinal.error());
    ACK_CHECK(ordinal_failure != nullptr);
    ACK_CHECK(
        ordinal_failure->code
        == net::SessionProtocolErrorCode::InvalidCommitPermit);
    ACK_CHECK(fixture.director().permitted_frames() == 1U);

    auto second_permit = fixture.director().send_commit_permit(
        net::FrameCommitPermitIntent{
            8U, second.sequence, second.before, second.after});
    ACK_CHECK(second_permit);
    auto second_wire = fixture.read();
    const auto* second_message =
        std::get_if<net::FrameCommitPermit>(&second_wire);
    ACK_CHECK(second_message != nullptr);
    ACK_CHECK(second_message->sequence == 5U);
    ACK_CHECK(second_message->acknowledged_sequence == second.sequence);
    ACK_CHECK(second_message->barrier_ordinal == 8U);
    ACK_CHECK(fixture.director().permitted_frames() == 2U);
    ACK_CHECK(fixture.director().last_commit_permit_ordinal() == 8U);
}

void committed_before_permit_is_rejected() {
    RawDirectorFixture fixture{128U};
    const auto frame = fixture.publish_put(1U, 10, 1U, 1U);
    fixture.write(net::SessionMessage{net::FramePrepared{
        frame.session_id, 2U, frame.sequence, frame.after}});
    auto prepared = fixture.director().receive_frame_acknowledgement();
    ACK_CHECK(prepared);

    expect_protocol_rejection(
        fixture,
        net::SessionMessage{net::FrameCommitted{
            frame.session_id, 3U, frame.sequence, frame.after}},
        net::SessionProtocolErrorCode::FrameNotPermitted);
}

void out_of_order_commit_is_rejected() {
    RawDirectorFixture fixture{126U};
    const auto first = fixture.publish_put(1U, 10, 1U, 1U);
    const auto second = fixture.publish_put(2U, 20, 2U, 2U);
    fixture.write(net::SessionMessage{net::FramePrepared{
        first.session_id, 2U, first.sequence, first.after}});
    auto first_prepared = fixture.director().receive_frame_acknowledgement();
    ACK_CHECK(first_prepared);
    fixture.write(net::SessionMessage{net::FramePrepared{
        second.session_id, 3U, second.sequence, second.after}});
    auto second_prepared = fixture.director().receive_frame_acknowledgement();
    ACK_CHECK(second_prepared);
    ACK_CHECK(second_prepared.value().outstanding_frames == 2U);

    expect_protocol_rejection(
        fixture,
        net::SessionMessage{net::FrameCommitted{
            second.session_id, 4U, second.sequence, second.after}},
        net::SessionProtocolErrorCode::AcknowledgementMismatch);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"wrong session", &wrong_session_is_rejected},
        {"wrong control sequence", &wrong_control_sequence_is_rejected},
        {"unknown batch sequence", &unknown_batch_sequence_is_rejected},
        {"wrong cursor", &wrong_cursor_is_rejected},
        {"duplicate Prepared", &duplicate_prepared_is_rejected},
        {"Committed before Prepared", &committed_before_prepared_is_rejected},
        {"permit requires Prepared and monotonic ordinal",
         &permit_requires_prepared_and_monotonic_ordinal},
        {"Committed before permit", &committed_before_permit_is_rejected},
        {"out-of-order commit", &out_of_order_commit_is_rejected},
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
