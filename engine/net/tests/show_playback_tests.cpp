#include <mediaserver/net/director_frame_barrier.hpp>
#include <mediaserver/net/director_session_service.hpp>
#include <mediaserver/net/frame_commit_sink.hpp>
#include <mediaserver/net/node_session_service.hpp>
#include <mediaserver/show/frame_boundary_driver.hpp>
#include <mediaserver/show/test_pattern.hpp>
#include <mediaserver/show/transport_author.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>

namespace {

namespace command = mediaserver::cmd;
namespace core = mediaserver::core;
namespace document = mediaserver::doc;
namespace net = mediaserver::net;
namespace persist = mediaserver::persist;
namespace show = mediaserver::show;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define PLAYBACK_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Clock = show::ShowClock::Clock;
using namespace std::chrono_literals;

constexpr show::FrameRate show_rate{50U, 1U};
constexpr auto frame_period = 20'000'000ns;
constexpr auto convergence_timeout = 10s;

[[nodiscard]] show::ShowClock::TimePoint at(std::chrono::nanoseconds value) {
    return show::ShowClock::TimePoint{
        std::chrono::duration_cast<Clock::duration>(value)};
}

[[nodiscard]] net::Id128 id(std::uint64_t low) noexcept {
    return net::Id128{0x73686f77706c6179ULL, low};
}

[[nodiscard]] command::StreamCursor genesis_cursor() noexcept {
    return command::StreamCursor{
        command::FrameNumber{0U},
        document::Version{0U},
        command::CommandId{1U},
    };
}

class TempWal final {
public:
    TempWal() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("mediaserver-show-playback-" + std::to_string(stamp) + "-"
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

// In-memory stand-in for the release ledger. The permit path under test is the
// barrier and the session protocol, not ledger durability, which has its own
// crash-window coverage.
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
        for (const auto& entry : entries_) {
            if (entry.before == before && entry.after == after) {
                return entry.ordinal;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool persist_release(
        const net::PublishedDirectorFrame& frame,
        core::u64 barrier_ordinal) noexcept override {
        const std::lock_guard lock{mutex_};
        if (barrier_ordinal != next_ordinal_
            || frame.before_cursor() != frontier_) {
            return false;
        }
        try {
            entries_.push_back(
                Entry{frame.before_cursor(), frame.after_cursor(), barrier_ordinal});
        } catch (...) {
            return false;
        }
        ++next_ordinal_;
        frontier_ = frame.after_cursor();
        return true;
    }

private:
    struct Entry final {
        command::StreamCursor before{};
        command::StreamCursor after{};
        core::u64 ordinal{};
    };

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    core::u64 next_ordinal_{1U};
    command::StreamCursor frontier_{genesis_cursor()};
};

[[nodiscard]] persist::CommandLog open_log(const std::filesystem::path& path) {
    persist::OpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    auto opened = persist::CommandLog::open_or_create(
        path, genesis_cursor(), options);
    PLAYBACK_CHECK(opened);
    return std::move(opened).value();
}

template <typename Predicate>
[[nodiscard]] bool wait_until(
    Predicate&& predicate,
    std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

// The Director half of the playback loop, driven by explicit time points so a
// test controls the transport exactly rather than racing a wall clock.
class DirectorShow final {
public:
    DirectorShow()
        : log_(open_log(temporary_.path())),
          durable_(scheduler_, log_),
          hub_(durable_, net::DirectorSessionHubLimits{4U, 64U, 64U * 1024U * 1024U}),
          leases_(4U),
          barrier_(
              genesis_cursor(),
              release_,
              net::DirectorFrameBarrierLimits{4U, 128U, 128U}),
          service_(config(), hub_, durable_, leases_, &barrier_),
          clock_(make_clock()) {}

    [[nodiscard]] asio::ip::tcp::endpoint start() {
        auto started = service_.start();
        PLAYBACK_CHECK(started);
        return started.value();
    }

    void stop() noexcept { service_.stop(); }

    void play() { PLAYBACK_CHECK(clock_.play(now_)); }

    // Pausing at the boundary just reached, so the held position is the frame
    // the operator was looking at rather than the next one.
    void pause() { PLAYBACK_CHECK(clock_.pause(now_)); }

    void seek(show::ShowFrame target) {
        PLAYBACK_CHECK(clock_.seek(target, now_));
    }

    // Advances the transport by one frame period and seals whatever it crossed,
    // exactly as apps/server does. Returns the show position now published.
    [[nodiscard]] show::ShowFrame publish_next() {
        advance();
        seal_one();
        return clock_.frame();
    }

    // Seals a control frame without advancing the transport, which is what a
    // paused show does every boundary.
    void publish_held() { seal_one(); }

    [[nodiscard]] command::FrameNumber sealed_through() const noexcept {
        return scheduler_.sealed_through();
    }
    [[nodiscard]] show::ShowFrame show_frame() const noexcept {
        return clock_.frame();
    }
    [[nodiscard]] show::TransportState transport() const noexcept {
        return clock_.state();
    }

    void retire_completed() {
        while (!tracked_.empty() && barrier_.retire(tracked_.front())) {
            tracked_.pop_front();
        }
    }

private:
    static net::DirectorSessionServiceConfig config() {
        net::DirectorSessionServiceConfig config;
        config.bind_endpoint =
            asio::ip::tcp::endpoint{asio::ip::address_v4::loopback(), 0U};
        config.max_workers = 4U;
        config.session_id_namespace = 0x73686f77636c6b73ULL;
        config.first_session_sequence = 1U;
        config.max_receive_bytes = 144U * 1024U * 1024U;
        config.session_defaults.show_id = id(1U);
        config.session_defaults.director_epoch = id(2U);
        config.session_defaults.io.handshake_timeout = 5s;
        config.session_defaults.io.read_timeout = std::chrono::milliseconds::zero();
        config.session_defaults.io.write_timeout = 5s;
        return config;
    }

    static show::ShowClock make_clock() {
        auto created = show::ShowClock::create(
            show::ShowClockConfig{
                show_rate, show::ShowFrame{0U}, show::ShowFrame{99'999U}, false},
            at(0ns));
        check(created.has_value(), "director show clock", __LINE__);
        return std::move(created).value();
    }

    void advance() {
        now_ += std::chrono::duration_cast<Clock::duration>(frame_period);
        PLAYBACK_CHECK(clock_.advance_to(now_));
    }

    void seal_one() {
        const command::FrameNumber frame{scheduler_.sealed_through().value() + 1U};
        const auto authored = show::schedule_transport_update(
            scheduler_,
            show::TransportSnapshot{clock_.frame(), clock_.state(), show_rate},
            frame,
            command::UndoContextId{1U},
            command::TransactionId{frame.value()});
        PLAYBACK_CHECK(authored);

        auto sealed = barrier_.seal_and_track(hub_, frame);
        PLAYBACK_CHECK(sealed);
        tracked_.push_back(sealed.value().barrier);
        retire_completed();
    }

    TempWal temporary_;
    persist::CommandLog log_;
    command::DirectorScheduler scheduler_;
    persist::DurableDirector durable_;
    net::DirectorSessionHub hub_;
    net::NodeSessionLeaseRegistry leases_;
    MemoryReleaseAuthority release_;
    net::DirectorFrameBarrier barrier_;
    net::DirectorSessionService service_;
    show::ShowClock clock_;
    show::ShowClock::TimePoint now_{at(0ns)};
    std::deque<net::DirectorFrameBarrierId> tracked_;
};

// One node process worth of state: its own connection, command bus, commit
// sink, and independently paced frame-boundary driver.
class NodeRig final {
public:
    NodeRig(const asio::ip::tcp::endpoint& director, core::u32 index)
        : index_(index),
          service_(config(director, index), bus_),
          sink_(service_, bus_),
          driver_(make_driver(sink_)) {}

    NodeRig(const NodeRig&) = delete;
    NodeRig& operator=(const NodeRig&) = delete;

    void start() {
        PLAYBACK_CHECK(service_.start());
        PLAYBACK_CHECK(driver_.play(now_));
    }

    void stop() noexcept { service_.stop(); }

    [[nodiscard]] bool connected() {
        return wait_until([this]() {
            const auto state = service_.snapshot().state;
            return state == net::NodeSessionServiceState::Live
                || state == net::NodeSessionServiceState::CheckpointStaged;
        });
    }

    // Crosses one local frame boundary and consumes whatever became visible.
    void tick() {
        now_ += std::chrono::duration_cast<Clock::duration>(frame_period);
        auto ticked = driver_.tick(now_);
        PLAYBACK_CHECK(ticked);
        last_ = std::move(ticked).value();

        show::PublishedFrame incoming{};
        std::size_t skipped = 0U;
        if (driver_.mailbox().consume_latest(incoming, skipped)) {
            presented_ = std::move(incoming);
        }
    }

    // Paces boundaries until the node has published `target`, which is what a
    // real node does while it waits for the Director.
    [[nodiscard]] bool advance_to(command::FrameNumber target) {
        const auto deadline =
            std::chrono::steady_clock::now() + convergence_timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            tick();
            if (presented_.control_frame.value() >= target.value()) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return presented_.control_frame.value() >= target.value();
    }

    [[nodiscard]] const show::PublishedFrame& presented() const noexcept {
        return presented_;
    }
    [[nodiscard]] const show::FrameBoundaryReport& last() const noexcept {
        return last_;
    }
    [[nodiscard]] const show::FrameBoundaryDriver& driver() const noexcept {
        return driver_;
    }
    [[nodiscard]] core::u32 index() const noexcept { return index_; }
    [[nodiscard]] bool faulted() const noexcept { return bus_.needs_resync(); }

private:
    static net::NodeSessionServiceConfig config(
        const asio::ip::tcp::endpoint& director,
        core::u32 index) {
        net::NodeSessionServiceConfig config{};
        config.director_endpoint = director;
        config.session_defaults.show_id = id(1U);
        config.session_defaults.node_id = id(100U + index);
        config.session_defaults.incarnation = id(200U + index);
        config.session_defaults.last_director_epoch = id(2U);
        config.session_defaults.io.handshake_timeout = 5s;
        config.session_defaults.io.read_timeout = 5s;
        config.session_defaults.io.write_timeout = 5s;
        config.max_receive_bytes = 144U * 1024U * 1024U;
        config.connect_timeout = 5s;
        return config;
    }

    static show::FrameBoundaryDriver make_driver(show::FrameCommitSink& sink) {
        show::FrameBoundaryDriverConfig config{};
        config.clock = show::ShowClockConfig{
            show_rate, show::ShowFrame{0U}, show::ShowFrame{99'999U}, false};
        config.mailbox_capacity = 16U;
        config.max_commits_per_tick = 8U;
        auto created = show::FrameBoundaryDriver::create(config, sink, at(0ns));
        check(created.has_value(), "node frame boundary driver", __LINE__);
        return std::move(created).value();
    }

    core::u32 index_;
    command::NodeCommandBus bus_;
    net::NodeSessionService service_;
    net::NodeServiceFrameCommitSink sink_;
    show::FrameBoundaryDriver driver_;
    show::ShowClock::TimePoint now_{at(0ns)};
    show::PublishedFrame presented_{};
    show::FrameBoundaryReport last_{};
};

[[nodiscard]] core::u64 rendered_digest(const show::PublishedFrame& frame) {
    std::vector<core::u8> pixels(64U * 64U * 4U, core::u8{0});
    const show::FrameBufferView view{
        pixels, 64U, 64U, 64U * 4U, show::PixelFormat::Rgba8888};

    show::TestPatternInput input{};
    if (frame.transport) {
        input.show_frame = frame.transport->show_frame;
        input.rate = frame.transport->rate;
        input.state = frame.transport->state;
    }
    input.control_frame = frame.control_frame;
    check(show::render_test_pattern(view, input), "render", __LINE__);
    return show::frame_buffer_digest(view);
}

void assert_converged(const NodeRig& left, const NodeRig& right) {
    PLAYBACK_CHECK(left.presented().control_frame == right.presented().control_frame);
    PLAYBACK_CHECK(left.presented().version == right.presented().version);
    PLAYBACK_CHECK(left.presented().transport.has_value());
    PLAYBACK_CHECK(left.presented().transport == right.presented().transport);
    // The presented surfaces are identical once the node tint is removed,
    // which is the property an operator checks by looking at two outputs.
    PLAYBACK_CHECK(
        rendered_digest(left.presented()) == rendered_digest(right.presented()));
}

void two_nodes_play_pause_and_seek_to_the_same_frame() {
    DirectorShow director;
    const auto endpoint = director.start();

    NodeRig node_a{endpoint, 0U};
    NodeRig node_b{endpoint, 1U};
    node_a.start();
    node_b.start();
    PLAYBACK_CHECK(node_a.connected());
    PLAYBACK_CHECK(node_b.connected());

    director.play();

    // Play: every sealed control frame carries the next show position, and
    // both independently paced nodes land on it.
    for (core::u64 index = 0U; index < 20U; ++index) {
        const auto position = director.publish_next();
        const auto frame = director.sealed_through();
        PLAYBACK_CHECK(node_a.advance_to(frame));
        PLAYBACK_CHECK(node_b.advance_to(frame));
        assert_converged(node_a, node_b);
        PLAYBACK_CHECK(node_a.presented().transport->show_frame == position);
        PLAYBACK_CHECK(
            node_a.presented().transport->state == show::TransportState::Playing);
    }

    const auto played_to = node_a.presented().transport->show_frame;
    PLAYBACK_CHECK(played_to == show::ShowFrame{20U});

    // Pause: the show position stops advancing on both nodes even though
    // control frames keep flowing.
    director.pause();
    for (core::u64 index = 0U; index < 5U; ++index) {
        director.publish_held();
        const auto frame = director.sealed_through();
        PLAYBACK_CHECK(node_a.advance_to(frame));
        PLAYBACK_CHECK(node_b.advance_to(frame));
        assert_converged(node_a, node_b);
        PLAYBACK_CHECK(node_a.presented().transport->show_frame == played_to);
        PLAYBACK_CHECK(
            node_a.presented().transport->state == show::TransportState::Paused);
    }

    // Exact seek: an arbitrary frame is reached precisely, not approximately.
    constexpr show::ShowFrame target{4'321U};
    director.seek(target);
    director.publish_held();
    PLAYBACK_CHECK(node_a.advance_to(director.sealed_through()));
    PLAYBACK_CHECK(node_b.advance_to(director.sealed_through()));
    assert_converged(node_a, node_b);
    PLAYBACK_CHECK(node_a.presented().transport->show_frame == target);

    // Resuming from the sought position continues from exactly there.
    director.play();
    for (core::u64 index = 1U; index <= 5U; ++index) {
        const auto position = director.publish_next();
        PLAYBACK_CHECK(position == show::ShowFrame{target.value() + index});
        PLAYBACK_CHECK(node_a.advance_to(director.sealed_through()));
        PLAYBACK_CHECK(node_b.advance_to(director.sealed_through()));
        assert_converged(node_a, node_b);
    }
    PLAYBACK_CHECK(
        node_a.presented().transport->show_frame
        == show::ShowFrame{target.value() + 5U});

    PLAYBACK_CHECK(!node_a.faulted());
    PLAYBACK_CHECK(!node_b.faulted());
    PLAYBACK_CHECK(node_a.driver().counters().resync_reports == 0U);
    PLAYBACK_CHECK(node_b.driver().counters().resync_reports == 0U);

    node_a.stop();
    node_b.stop();
    director.stop();
}

void a_node_joining_a_running_show_adopts_its_checkpoint_at_a_boundary() {
    DirectorShow director;
    const auto endpoint = director.start();

    NodeRig early{endpoint, 0U};
    early.start();
    PLAYBACK_CHECK(early.connected());

    director.play();
    for (core::u64 index = 0U; index < 12U; ++index) {
        static_cast<void>(director.publish_next());
    }
    PLAYBACK_CHECK(early.advance_to(director.sealed_through()));

    // The second node arrives with a stale cursor, so the Director stages a
    // full checkpoint rather than replaying from genesis.
    NodeRig late{endpoint, 1U};
    late.start();
    PLAYBACK_CHECK(late.connected());

    for (core::u64 index = 0U; index < 8U; ++index) {
        static_cast<void>(director.publish_next());
    }
    const auto frame = director.sealed_through();
    PLAYBACK_CHECK(early.advance_to(frame));
    PLAYBACK_CHECK(late.advance_to(frame));

    PLAYBACK_CHECK(late.driver().counters().checkpoint_activations >= 1U);
    assert_converged(early, late);
    PLAYBACK_CHECK(
        late.presented().transport->show_frame == director.show_frame());
    PLAYBACK_CHECK(!late.faulted());

    early.stop();
    late.stop();
    director.stop();
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 2U> cases{{
    {"play, pause, and exact seek across two nodes",
     &two_nodes_play_pause_and_seek_to_the_same_frame},
    {"late node adopts a checkpoint at a boundary",
     &a_node_joining_a_running_show_adopts_its_checkpoint_at_a_boundary},
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
