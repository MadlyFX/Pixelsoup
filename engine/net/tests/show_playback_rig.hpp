#pragma once

// Shared two-node playback rig: a real Director service, a real barrier,
// and node connectors talking over loopback TCP. The behavioural cases and
// the sustained-rate soak drive the same rig, so a soak run cannot quietly
// diverge from what the behavioural suite proves.

#include <mediaserver/net/director_frame_barrier.hpp>
#include <mediaserver/net/director_session_service.hpp>
#include <mediaserver/net/frame_commit_sink.hpp>
#include <mediaserver/net/node_session_service.hpp>
#include <mediaserver/show/frame_boundary_driver.hpp>
#include <mediaserver/show/test_pattern.hpp>
#include <mediaserver/show/transport_author.hpp>
#include <mediaserver/show/transport_authority.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <initializer_list>
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
    // Edit-shaped durability is the product default, so it is this rig's
    // default too. The journalling cadence stays reachable because it is still
    // a supported run mode and still has to work end to end.
    explicit DirectorShow(
        persist::FrameDurability durability =
            persist::FrameDurability::EditsOnly,
        command::DirectorSchedulerLimits limits = {})
        : scheduler_(
              {},
              command::FrameNumber{0U},
              command::CommandId{1U},
              limits),
          log_(open_log(temporary_.path())),
          durable_(scheduler_, log_, durability),
          hub_(durable_, net::DirectorSessionHubLimits{4U, 64U, 64U * 1024U * 1024U}),
          leases_(4U),
          barrier_(
              genesis_cursor(),
              release_,
              net::DirectorFrameBarrierLimits{4U, 128U, 128U}),
          service_(config(), hub_, durable_, leases_, &barrier_),
          transport_(make_transport()) {}

    [[nodiscard]] asio::ip::tcp::endpoint start() {
        auto started = service_.start();
        PLAYBACK_CHECK(started);
        return started.value();
    }

    void stop() noexcept { service_.stop(); }

    // Operator actions re-anchor at the next control frame to be sealed, so
    // the frame that carries the change is also the frame it takes effect on.
    void play() { PLAYBACK_CHECK(transport_.play(next_frame())); }

    // Pausing at the frame just sealed, so the held position is the one the
    // operator was looking at rather than the next one.
    void pause() { PLAYBACK_CHECK(transport_.pause(next_frame())); }

    void seek(show::ShowFrame target) {
        PLAYBACK_CHECK(transport_.seek(target, next_frame()));
    }

    // Seals the next control frame and returns the show position it derives.
    // A playing show authors nothing here: the position advances because the
    // ordinal did.
    [[nodiscard]] show::ShowFrame publish_next() {
        const auto frame = next_frame();
        seal_one();
        return transport_.frame_at(frame);
    }

    // Same operation under a held transport. It exists as a separate name only
    // because the tests read better for it; a paused frame and a playing frame
    // are now byte-identical control frames.
    void publish_held() { seal_one(); }

    [[nodiscard]] command::FrameNumber sealed_through() const noexcept {
        return scheduler_.sealed_through();
    }
    [[nodiscard]] show::ShowFrame show_frame() const noexcept {
        return transport_.frame_at(scheduler_.sealed_through());
    }
    [[nodiscard]] show::TransportState transport() const noexcept {
        return transport_.state();
    }
    // Document edits authored across the whole run. Under an anchored
    // transport this counts operator actions, not frames.
    [[nodiscard]] core::u64 authored_anchors() const noexcept {
        return anchors_;
    }
    // Serialized WAL bytes. Under edit-shaped durability the live seal path
    // never writes, so this stays at whatever the log held when it opened.
    [[nodiscard]] std::uint64_t wal_bytes() const {
        return log_.recovery_report().valid_bytes;
    }
    [[nodiscard]] command::DirectorAuthoringFootprint authoring_footprint()
        const noexcept {
        return scheduler_.authoring_footprint();
    }
    [[nodiscard]] show::ShowFrame frame_at(
        command::FrameNumber control_frame) const noexcept {
        return transport_.frame_at(control_frame);
    }
    [[nodiscard]] std::size_t tracked_frames() const noexcept {
        return tracked_.size();
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

    static show::TransportAuthority make_transport() {
        auto created = show::TransportAuthority::create(
            show::ShowClockConfig{
                show_rate, show::ShowFrame{0U}, show::ShowFrame{99'999U}, false},
            command::FrameNumber{0U});
        check(created.has_value(), "director transport authority", __LINE__);
        return std::move(created).value();
    }

    [[nodiscard]] command::FrameNumber next_frame() const noexcept {
        return command::FrameNumber{scheduler_.sealed_through().value() + 1U};
    }

    void seal_one() {
        const auto frame = next_frame();
        static_cast<void>(transport_.settle(frame));
        if (!anchored_ || *anchored_ != transport_.anchor()) {
            const auto authored = show::schedule_transport_anchor(
                scheduler_,
                transport_.anchor(),
                frame,
                command::UndoContextId{1U},
                command::TransactionId{frame.value()});
            PLAYBACK_CHECK(authored);
            anchored_ = transport_.anchor();
            ++anchors_;
        }

        auto sealed = barrier_.seal_and_track(hub_, frame);
        PLAYBACK_CHECK(sealed);
        tracked_.push_back(sealed.value().barrier);
        retire_completed();
    }

    TempWal temporary_;
    command::DirectorScheduler scheduler_;
    persist::CommandLog log_;
    persist::DurableDirector durable_;
    net::DirectorSessionHub hub_;
    net::NodeSessionLeaseRegistry leases_;
    MemoryReleaseAuthority release_;
    net::DirectorFrameBarrier barrier_;
    net::DirectorSessionService service_;
    show::TransportAuthority transport_;
    std::optional<show::TransportAnchor> anchored_;
    core::u64 anchors_{};
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

    // Has this node published `target`? Non-mutating, so a cohort-wide
    // convergence loop can test every member without pacing it again.
    [[nodiscard]] bool published_through(
        command::FrameNumber target) const noexcept {
        return presented_.control_frame.value() >= target.value();
    }

    // Paces every member's boundaries until all of them have published
    // `target`.
    //
    // The cohort is coupled by its own flow control, so its members only
    // converge together. The Director permits a frame once every member has
    // received it, and a member stops receiving once max_inflight_frames of
    // them are uncommitted; committing is what a frame boundary does. Pacing
    // one node to the target while the rest sit idle therefore stops making
    // progress the moment an idle member's backlog reaches that window, and no
    // amount of further waiting on the paced node can clear it. Whether the
    // members end a paced run inside that window is a matter of scheduling, so
    // a per-node convergence loop is a race by construction rather than a
    // slow path.
    [[nodiscard]] static bool advance_cohort_to(
        command::FrameNumber target,
        std::initializer_list<NodeRig*> cohort) {
        const auto deadline =
            std::chrono::steady_clock::now() + convergence_timeout;
        for (;;) {
            bool converged = true;
            for (auto* node : cohort) {
                node->tick();
                converged = converged && node->published_through(target);
            }
            if (converged) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
    }

    // Single-node convenience. A one-member cohort has nothing to deadlock
    // against, so this is the same loop with nothing to interleave.
    [[nodiscard]] bool advance_to(command::FrameNumber target) {
        return advance_cohort_to(target, {this});
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
        // Three Director keepalive intervals, not one. Any inbound message
        // refreshes this deadline, so leaving it equal to the Director's 5s
        // heartbeat period makes an idle node race its peer's keepalive
        // against its own idle timeout and drop a healthy session whenever it
        // loses - which is a coin flip, decided by machine load.
        config.session_defaults.io.read_timeout = 15s;
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
        input.show_frame = frame.show_frame;
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
    PLAYBACK_CHECK(left.presented().show_frame == right.presented().show_frame);
    // The presented surfaces are identical once the node tint is removed,
    // which is the property an operator checks by looking at two outputs.
    PLAYBACK_CHECK(
        rendered_digest(left.presented()) == rendered_digest(right.presented()));
}

struct SustainedOptions final {
    core::u64 seconds{2U};
    show::FrameRate rate{120U, 1U};
    // Zero prints only the final line, which is what the short in-suite case
    // wants; a long run reports progress so a stall is visible while it runs.
    core::u64 report_seconds{};
};

// One paced Director iteration plus one boundary on each node, which is what a
// three-process run does per frame. The nodes are ticked from this thread on
// purpose: the wall-clock budget then includes their work, so a rate this loop
// sustains is one the whole cluster sustained.
void run_sustained(const SustainedOptions& options) {
    const auto per_second = static_cast<core::u64>(
        show::frames_per_second(options.rate));
    const auto target_frames = options.seconds * per_second;
    const auto period = std::chrono::nanoseconds{
        1'000'000'000LL * static_cast<long long>(options.rate.denominator)
        / static_cast<long long>(options.rate.numerator)};

    DirectorShow director;
    const auto endpoint = director.start();
    NodeRig node_a{endpoint, 0U};
    NodeRig node_b{endpoint, 1U};
    node_a.start();
    node_b.start();
    PLAYBACK_CHECK(node_a.connected());
    PLAYBACK_CHECK(node_b.connected());

    director.play();

    const auto footprint_after_start = director.authoring_footprint();
    const auto wal_bytes_at_start = director.wal_bytes();
    const auto started = std::chrono::steady_clock::now();
    auto next_report = started + std::chrono::seconds{options.report_seconds};
    core::u64 sealed = 0U;
    core::u64 skipped_positions = 0U;
    // The first sealed frame is the one that carries the anchor, and it
    // presents the anchor position rather than the one after it. It is the
    // baseline, not a step.
    std::optional<show::ShowFrame> previous_position;

    for (core::u64 frame = 1U; frame <= target_frames; ++frame) {
        const auto due = started
            + std::chrono::duration_cast<Clock::duration>(
                  period * static_cast<long long>(frame));
        const auto position = director.publish_next();
        ++sealed;

        // Under an anchored transport the show advances exactly one position
        // per control frame. Anything else is a skipped position, which is the
        // failure this gate is written against.
        if (previous_position
            && position.value() != previous_position->value() + 1U) {
            ++skipped_positions;
        }
        previous_position = position;

        node_a.tick();
        node_b.tick();

        const auto now = Clock::now();
        if (options.report_seconds != 0U && now >= next_report) {
            next_report += std::chrono::seconds{options.report_seconds};
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - started)
                    .count();
            std::cout << "soak sealed=" << sealed << " show-frame="
                      << position.value() << " node-a="
                      << node_a.presented().show_frame.value() << " node-b="
                      << node_b.presented().show_frame.value()
                      << " anchors=" << director.authored_anchors()
                      << " wal-bytes=" << director.wal_bytes() << " seals/s="
                      << (elapsed == 0
                              ? 0
                              : static_cast<long long>(sealed) * 1000
                                  / elapsed)
                      << '\n'
                      << std::flush;
        }
        if (now < due) {
            std::this_thread::sleep_for(due - now);
        }
    }

    // The rate gate closes with the paced loop. Everything after it is the
    // cohort draining a backlog the Director already sealed, which is a
    // convergence property with its own deadline, not part of how long the
    // run took to seal its frames.
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             Clock::now() - started)
                             .count();

    const auto target = director.sealed_through();
    PLAYBACK_CHECK(NodeRig::advance_cohort_to(target, {&node_a, &node_b}));

    const auto footprint = director.authoring_footprint();
    std::cout << "soak complete"
              << " frames=" << sealed << " elapsed-ms=" << elapsed
              << " skipped-positions=" << skipped_positions
              << " anchors=" << director.authored_anchors()
              << " wal-bytes=" << director.wal_bytes()
              << " seals=" << footprint.retained_seals
              << " commands=" << footprint.frames_with_commands
              << " undo=" << footprint.undo_transactions
              << " show-frame-a=" << node_a.presented().show_frame.value()
              << " show-frame-b=" << node_b.presented().show_frame.value()
              << '\n'
              << std::flush;

    // Zero skipped show positions, the same show frame on both nodes.
    PLAYBACK_CHECK(skipped_positions == 0U);
    PLAYBACK_CHECK(sealed == target_frames);
    assert_converged(node_a, node_b);
    PLAYBACK_CHECK(!node_a.faulted());
    PLAYBACK_CHECK(!node_b.faulted());
    PLAYBACK_CHECK(node_a.driver().counters().resync_reports == 0U);
    PLAYBACK_CHECK(node_b.driver().counters().resync_reports == 0U);

    // Flat Director memory: one anchor for the whole run, an authoring
    // footprint bounded by the retention window, and not one byte written.
    PLAYBACK_CHECK(director.authored_anchors() == 1U);
    PLAYBACK_CHECK(director.wal_bytes() == wal_bytes_at_start);
    PLAYBACK_CHECK(
        footprint.retained_seals
        <= director.authoring_footprint().retained_seals);
    PLAYBACK_CHECK(footprint.retained_seals <= 256U);
    PLAYBACK_CHECK(footprint.frames_with_commands <= 256U);
    PLAYBACK_CHECK(
        footprint.undo_transactions
        <= footprint_after_start.undo_transactions + 1U);

    // The rate was actually held rather than merely attempted: a paced loop
    // that could not keep up finishes late.
    const auto budget = static_cast<long long>(options.seconds) * 1000LL;
    PLAYBACK_CHECK(elapsed <= budget + budget / 10 + 2'000);

    node_a.stop();
    node_b.stop();
    director.stop();
}


} // namespace
