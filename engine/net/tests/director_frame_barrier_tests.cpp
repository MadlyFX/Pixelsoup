#include <mediaserver/net/director_frame_barrier.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

#define BARRIER_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempWal final {
public:
    TempWal() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("mediaserver-frame-barrier-" + std::to_string(timestamp) + "-"
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

[[nodiscard]] command::StreamCursor genesis_cursor() noexcept {
    return command::StreamCursor{
        command::FrameNumber{0U},
        document::Version{0U},
        command::CommandId{1U},
    };
}

class MemoryReleaseAuthority final : public net::DirectorFrameReleaseAuthority {
public:
    explicit MemoryReleaseAuthority(core::u64 next_ordinal = 1U) noexcept
        : next_ordinal_(next_ordinal) {}

    [[nodiscard]] core::u64 next_release_ordinal() const noexcept override {
        const std::lock_guard lock{mutex_};
        return next_ordinal_;
    }

    [[nodiscard]] std::optional<core::u64> release_ordinal(
        const command::StreamCursor& before,
        const command::StreamCursor& after) const noexcept override {
        const std::lock_guard lock{mutex_};
        for (std::size_t index = 0U; index < decision_count_; ++index) {
            if (decisions_[index].before == before
                && decisions_[index].after == after) {
                return decisions_[index].ordinal;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool persist_release(
        const net::PublishedDirectorFrame& frame,
        core::u64 barrier_ordinal) noexcept override {
        const std::lock_guard lock{mutex_};
        ++persist_calls_;
        if (fail_persistence_ || !frame
            || decision_count_ == decisions_.size()
            || barrier_ordinal == 0U || barrier_ordinal != next_ordinal_
            || frame.before_cursor() != frontier_) {
            return false;
        }
        decisions_[decision_count_] = Decision{
            frame.before_cursor(),
            frame.after_cursor(),
            barrier_ordinal,
        };
        ++decision_count_;
        ++next_ordinal_;
        frontier_ = frame.after_cursor();
        return true;
    }

    void fail_persistence(bool value = true) noexcept {
        const std::lock_guard lock{mutex_};
        fail_persistence_ = value;
    }

    [[nodiscard]] std::size_t decision_count() const noexcept {
        const std::lock_guard lock{mutex_};
        return decision_count_;
    }

    [[nodiscard]] std::size_t persist_calls() const noexcept {
        const std::lock_guard lock{mutex_};
        return persist_calls_;
    }

    [[nodiscard]] command::StreamCursor frontier() const noexcept {
        const std::lock_guard lock{mutex_};
        return frontier_;
    }

private:
    struct Decision final {
        command::StreamCursor before{};
        command::StreamCursor after{};
        core::u64 ordinal{};
    };

    mutable std::mutex mutex_;
    std::array<Decision, 64U> decisions_{};
    std::size_t decision_count_{};
    std::size_t persist_calls_{};
    core::u64 next_ordinal_{};
    command::StreamCursor frontier_{genesis_cursor()};
    bool fail_persistence_{};
};

[[nodiscard]] net::Id128 id(std::uint64_t low) noexcept {
    return net::Id128{0x6261727269657221ULL, low};
}

[[nodiscard]] persist::CommandLog open_log(
    const std::filesystem::path& path) {
    persist::OpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    auto opened = persist::CommandLog::open_or_create(
        path, genesis_cursor(), options);
    BARRIER_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] document::Node make_node(std::uint64_t value) {
    return document::Node{
        document::NodeId{value},
        "barrier.node",
        {{"value", static_cast<std::int64_t>(value)}},
    };
}

void schedule_put(command::DirectorScheduler& director, std::uint64_t frame) {
    auto scheduled = director.schedule_edit(
        command::PutNode{make_node(frame)},
        command::EraseNode{document::NodeId{frame}},
        command::UndoContextId{1U},
        command::TransactionId{frame},
        command::FrameNumber{frame});
    BARRIER_CHECK(scheduled);
}

[[nodiscard]] net::NodeSessionIdentity peer(
    std::uint64_t node,
    std::uint64_t incarnation) noexcept {
    return net::NodeSessionIdentity{id(1U), id(node), id(incarnation)};
}

[[nodiscard]] net::DirectorLiveRegistration registration(
    std::uint64_t session,
    std::uint64_t node,
    const command::StreamCursor& cursor,
    std::uint64_t incarnation = 1U,
    std::uint64_t last_control_sequence = 1U,
    std::optional<core::u32> node_index = std::nullopt) noexcept {
    return net::DirectorLiveRegistration{
        id(session),
        peer(node, incarnation),
        node_index.value_or(static_cast<core::u32>(node)),
        cursor,
        last_control_sequence,
    };
}

[[nodiscard]] net::FrameAcknowledgementReport acknowledgement(
    net::FrameAcknowledgementKind kind,
    std::uint64_t sequence,
    std::uint64_t batch,
    const command::StreamCursor& after) noexcept {
    const auto before = command::StreamCursor{
        command::FrameNumber{after.through_frame.value() - 1U},
        document::Version{after.revision.value() - 1U},
        command::CommandId{after.next_command.value() - 1U},
    };
    return net::FrameAcknowledgementReport{
        kind,
        sequence,
        batch,
        before,
        after,
        0U,
        0U,
    };
}

struct Fixture final {
    TempWal temporary;
    persist::CommandLog log;
    command::DirectorScheduler director;
    persist::DurableDirector durable;
    net::DirectorSessionHub hub;
    net::DirectorFrameBarrier barrier;

    explicit Fixture(net::DirectorFrameBarrierLimits limits = {})
        : log(open_log(temporary.path())),
          durable(director, log),
          hub(durable),
          barrier(genesis_cursor(), limits) {}

    [[nodiscard]] net::DirectorTrackedSealOutcome seal(std::uint64_t frame) {
        schedule_put(director, frame);
        auto tracked = barrier.seal_and_track(
            hub, command::FrameNumber{frame});
        BARRIER_CHECK(tracked);
        return std::move(tracked).value();
    }
};

struct GatedFixture final {
    TempWal temporary;
    persist::CommandLog log;
    command::DirectorScheduler director;
    persist::DurableDirector durable;
    net::DirectorSessionHub hub;
    MemoryReleaseAuthority release;
    net::DirectorFrameBarrier barrier;

    explicit GatedFixture(
        net::DirectorFrameBarrierLimits limits = {},
        core::u64 next_ordinal = 1U)
        : log(open_log(temporary.path())),
          durable(director, log),
          hub(durable),
          release(next_ordinal),
          barrier(genesis_cursor(), release, limits) {}

    [[nodiscard]] net::DirectorTrackedSealOutcome seal(std::uint64_t frame) {
        schedule_put(director, frame);
        auto tracked = barrier.seal_and_track(
            hub, command::FrameNumber{frame});
        BARRIER_CHECK(tracked);
        return std::move(tracked).value();
    }
};

// A pose that is replaced before its session drains it is a real loss, and one
// nothing could previously see. The outbound `sequence` cannot show it - that
// is assigned when a message is sent, so a pose coalesced away in the mailbox
// never consumes one and the stream a node receives stays gap-free however many
// were dropped. `pose_sequence` is assigned at publish instead, so the hole
// survives all the way to the node that missed it.
void a_coalesced_pose_is_counted_and_leaves_a_gap_a_node_can_see() {
    Fixture fixture;
    BARRIER_CHECK(fixture.barrier.register_live(
        registration(10U, 20U, genesis_cursor())));

    const auto pose = [](std::uint64_t camera) {
        net::TrackedCameraFrame frame{};
        frame.control_frame = camera;
        net::TrackedCameraSample sample{};
        sample.camera_node = camera;
        sample.aspect_ratio = 16.0F / 9.0F;
        sample.field_of_view_turns = 0.05F;
        frame.samples.push_back(sample);
        return frame;
    };

    BARRIER_CHECK(fixture.barrier.tracked_poses_coalesced() == 0U);

    // Drained between publishes: nothing is lost and the ordinals are
    // consecutive.
    BARRIER_CHECK(fixture.barrier.publish_tracked_pose(pose(1U)) == 1U);
    net::TrackedCameraFrame taken{};
    BARRIER_CHECK(fixture.barrier.try_take_tracked_pose(id(10U), taken)
                  == net::DirectorSessionObserver::TrackedPosePoll::Ready);
    BARRIER_CHECK(taken.pose_sequence == 1U);
    BARRIER_CHECK(fixture.barrier.tracked_poses_coalesced() == 0U);

    // Three published while the session never drains. Two are overwritten.
    BARRIER_CHECK(fixture.barrier.publish_tracked_pose(pose(2U)) == 1U);
    BARRIER_CHECK(fixture.barrier.publish_tracked_pose(pose(3U)) == 1U);
    BARRIER_CHECK(fixture.barrier.publish_tracked_pose(pose(4U)) == 1U);
    BARRIER_CHECK(fixture.barrier.tracked_poses_coalesced() == 2U);

    // The session gets the newest, which is the intended behaviour - a node
    // that fell behind wants the current pose, not a backlog of stale ones.
    BARRIER_CHECK(fixture.barrier.try_take_tracked_pose(id(10U), taken)
                  == net::DirectorSessionObserver::TrackedPosePoll::Ready);
    BARRIER_CHECK(taken.control_frame == 4U);
    // And the gap is visible: 1 then 4 says two were missed.
    BARRIER_CHECK(taken.pose_sequence == 4U);

    // Draining again finds nothing; a pose is sent once.
    BARRIER_CHECK(fixture.barrier.try_take_tracked_pose(id(10U), taken)
                  == net::DirectorSessionObserver::TrackedPosePoll::Empty);
}

void exact_cohort_retains_a_lost_generation() {
    Fixture fixture;
    BARRIER_CHECK(fixture.barrier.register_live(
        registration(10U, 20U, genesis_cursor())));

    auto first = fixture.seal(1U);
    BARRIER_CHECK(fixture.barrier.register_live(registration(
        11U, 21U, first.seal.frame.after_cursor())));
    auto before_leave = fixture.barrier.snapshot(first.barrier);
    BARRIER_CHECK(before_leave);
    BARRIER_CHECK(before_leave.value().members == 1U);

    fixture.barrier.unregister(
        id(10U), net::DirectorSessionLeaveReason::SessionFailure);
    auto after_leave = fixture.barrier.snapshot(first.barrier);
    BARRIER_CHECK(after_leave);
    BARRIER_CHECK(after_leave.value().members == 1U);
    BARRIER_CHECK(after_leave.value().lost == 1U);
    BARRIER_CHECK(after_leave.value().degraded());

    std::array<net::DirectorFrameBarrierLostSession, 1U> lost{};
    auto copied = fixture.barrier.copy_lost_sessions(first.barrier, lost);
    BARRIER_CHECK(copied);
    BARRIER_CHECK(copied.value().total == 1U);
    BARRIER_CHECK(copied.value().copied == 1U);
    BARRIER_CHECK(lost[0].session_id == id(10U));
    BARRIER_CHECK(
        lost[0].reason == net::DirectorSessionLeaveReason::SessionFailure);

    BARRIER_CHECK(fixture.barrier.register_live(registration(
        12U, 20U, first.seal.frame.after_cursor(), 2U)));
    BARRIER_CHECK(!fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed,
            2U,
            1U,
            first.seal.frame.after_cursor())));
    BARRIER_CHECK(fixture.barrier.snapshot(first.barrier).value().lost == 1U);
}

void prepared_and_committed_phases_are_distinct_and_ordered() {
    Fixture fixture;
    BARRIER_CHECK(fixture.barrier.register_live(
        registration(10U, 20U, genesis_cursor())));
    BARRIER_CHECK(fixture.barrier.register_live(
        registration(11U, 21U, genesis_cursor(), 1U, 16U)));
    auto first = fixture.seal(1U);
    const auto after = first.seal.frame.after_cursor();

    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared, 2U, 1U, after)));
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(11U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared, 17U, 1U, after)));
    auto prepared = fixture.barrier.wait_for(
        first.barrier,
        net::DirectorFrameBarrierPhase::Prepared,
        std::chrono::milliseconds{0});
    BARRIER_CHECK(prepared);
    BARRIER_CHECK(
        prepared.value().status
        == net::DirectorFrameBarrierWaitStatus::Reached);
    BARRIER_CHECK(prepared.value().summary.prepared == 2U);
    BARRIER_CHECK(
        prepared.value().summary.progress
        == net::DirectorFrameBarrierProgress::AwaitingCommitted);

    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed, 3U, 1U, after)));
    auto pending = fixture.barrier.wait_for(
        first.barrier,
        net::DirectorFrameBarrierPhase::Committed,
        std::chrono::milliseconds{0});
    BARRIER_CHECK(pending);
    BARRIER_CHECK(
        pending.value().status
        == net::DirectorFrameBarrierWaitStatus::TimedOut);

    BARRIER_CHECK(!fixture.barrier.on_ack(
        id(11U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed, 19U, 1U, after)));
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(11U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed, 18U, 1U, after)));
    auto complete = fixture.barrier.wait_for(
        first.barrier,
        net::DirectorFrameBarrierPhase::Committed,
        std::chrono::milliseconds{10});
    BARRIER_CHECK(complete);
    BARRIER_CHECK(
        complete.value().status
        == net::DirectorFrameBarrierWaitStatus::Reached);
    BARRIER_CHECK(complete.value().summary.committed == 2U);

    BARRIER_CHECK(!fixture.barrier.on_ack(
        id(11U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed, 19U, 1U, after)));
    auto retired = fixture.barrier.retire(first.barrier);
    BARRIER_CHECK(retired);
    BARRIER_CHECK(fixture.barrier.open_frames() == 0U);
    BARRIER_CHECK(!fixture.barrier.snapshot(first.barrier));
}

void replay_seeded_heartbeat_preserves_control_order_and_frame_progress() {
    Fixture fixture;
    BARRIER_CHECK(fixture.barrier.register_live(
        registration(10U, 20U, genesis_cursor(), 1U, 40U)));
    const auto first = fixture.seal(1U);
    const auto after = first.seal.frame.after_cursor();

    BARRIER_CHECK(fixture.barrier.on_heartbeat_ack(
        id(10U),
        net::HeartbeatAcknowledgementReport{
            41U,
            900U,
            genesis_cursor(),
        }));
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared,
            42U,
            101U,
            after)));

    BARRIER_CHECK(!fixture.barrier.on_heartbeat_ack(
        id(10U),
        net::HeartbeatAcknowledgementReport{
            44U,
            901U,
            genesis_cursor(),
        }));
    BARRIER_CHECK(!fixture.barrier.on_heartbeat_ack(
        id(10U),
        net::HeartbeatAcknowledgementReport{
            43U,
            901U,
            after,
        }));
    BARRIER_CHECK(fixture.barrier.on_heartbeat_ack(
        id(10U),
        net::HeartbeatAcknowledgementReport{
            43U,
            901U,
            genesis_cursor(),
        }));
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed,
            44U,
            101U,
            after)));
}

void runtime_asset_adoption_is_exact_live_and_fail_closed() {
    Fixture fixture;
    constexpr core::u64 manifest_digest = 0x1122334455667788ULL;
    constexpr core::u64 stale_digest = 0x8877665544332211ULL;
    constexpr core::u32 first_index = 3U;
    constexpr core::u32 second_index = 4U;

    const std::array<core::u32, 0U> no_nodes{};
    BARRIER_CHECK(fixture.barrier.runtime_assets_ready(
        manifest_digest, no_nodes));

    BARRIER_CHECK(fixture.barrier.register_live(registration(
        10U, 20U, genesis_cursor(), 1U, 1U, first_index)));
    BARRIER_CHECK(!fixture.barrier.register_live(registration(
        12U, 22U, genesis_cursor(), 1U, 1U, first_index)));
    BARRIER_CHECK(
        fixture.barrier.observer_diagnostics().last_error
        == net::DirectorFrameBarrierObserverError::DuplicateNodeIndex);
    BARRIER_CHECK(fixture.barrier.register_live(registration(
        11U, 21U, genesis_cursor(), 1U, 1U, second_index)));

    const std::array required{first_index, second_index};
    BARRIER_CHECK(!fixture.barrier.runtime_assets_ready(
        manifest_digest, required));
    BARRIER_CHECK(fixture.barrier.on_heartbeat_ack(
        id(10U),
        net::HeartbeatAcknowledgementReport{
            2U,
            900U,
            genesis_cursor(),
            net::RuntimeAssetAdoption{
                manifest_digest,
                true,
                {net::RuntimeMediaAssetStatus{
                    71U,
                    net::RuntimeMediaDecodeState::SoftwareOnly,
                    net::RuntimeProxyState::PreferFallback,
                    net::RuntimeProxyReason::CacheGenerating,
                }}},
        }));
    net::RuntimeAssetAdoption first_adoption{};
    BARRIER_CHECK(fixture.barrier.runtime_asset_adoption(
        first_index, manifest_digest, first_adoption));
    BARRIER_CHECK(first_adoption.media_assets.size() == 1U);
    BARRIER_CHECK(first_adoption.media_assets[0U].asset_id == 71U);
    BARRIER_CHECK(first_adoption.media_assets[0U].decode ==
                  net::RuntimeMediaDecodeState::SoftwareOnly);
    BARRIER_CHECK(!fixture.barrier.runtime_asset_adoption(
        first_index, stale_digest, first_adoption));
    BARRIER_CHECK(!fixture.barrier.runtime_assets_ready(
        manifest_digest, required));
    BARRIER_CHECK(fixture.barrier.on_heartbeat_ack(
        id(11U),
        net::HeartbeatAcknowledgementReport{
            2U,
            901U,
            genesis_cursor(),
            net::RuntimeAssetAdoption{manifest_digest, true},
        }));
    BARRIER_CHECK(fixture.barrier.runtime_assets_ready(
        manifest_digest, required));
    BARRIER_CHECK(!fixture.barrier.runtime_assets_ready(
        stale_digest, required));

    // Rejected control traffic cannot rewrite the last validated adoption.
    BARRIER_CHECK(!fixture.barrier.on_heartbeat_ack(
        id(10U),
        net::HeartbeatAcknowledgementReport{
            4U,
            902U,
            genesis_cursor(),
            net::RuntimeAssetAdoption{manifest_digest, false},
        }));
    BARRIER_CHECK(fixture.barrier.runtime_assets_ready(
        manifest_digest, required));

    const std::array duplicate{first_index, first_index};
    const std::array missing{first_index, core::u32{99U}};
    const std::array out_of_range{net::max_session_node_index + 1U};
    BARRIER_CHECK(!fixture.barrier.runtime_assets_ready(
        manifest_digest, duplicate));
    BARRIER_CHECK(!fixture.barrier.runtime_assets_ready(
        manifest_digest, missing));
    BARRIER_CHECK(!fixture.barrier.runtime_assets_ready(
        manifest_digest, out_of_range));

    fixture.barrier.unregister(
        id(11U), net::DirectorSessionLeaveReason::SessionFailure);
    BARRIER_CHECK(!fixture.barrier.runtime_assets_ready(
        manifest_digest, required));
    BARRIER_CHECK(fixture.barrier.register_live(registration(
        13U, 23U, genesis_cursor(), 1U, 1U, second_index)));
    BARRIER_CHECK(!fixture.barrier.runtime_assets_ready(
        manifest_digest, required));
}

void runtime_asset_adoption_requires_live_not_catching_up() {
    Fixture fixture;
    constexpr core::u64 manifest_digest = 0xAABBCCDDEEFF0011ULL;
    constexpr core::u32 node_index = 5U;
    const auto first = fixture.seal(1U);
    const auto after = first.seal.frame.after_cursor();
    BARRIER_CHECK(fixture.barrier.register_live(registration(
        10U, 20U, genesis_cursor(), 1U, 1U, node_index)));
    const auto catching_up = fixture.barrier.node_connection(node_index);
    BARRIER_CHECK(catching_up.state == net::DirectorNodeConnection::State::CatchingUp);
    BARRIER_CHECK(catching_up.committed_frame == 0U);
    BARRIER_CHECK(catching_up.target_frame == 1U);
    BARRIER_CHECK(fixture.barrier.on_heartbeat_ack(
        id(10U),
        net::HeartbeatAcknowledgementReport{
            2U,
            900U,
            genesis_cursor(),
            net::RuntimeAssetAdoption{manifest_digest, true},
        }));

    const std::array required{node_index};
    BARRIER_CHECK(!fixture.barrier.runtime_assets_ready(
        manifest_digest, required));
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared, 3U, 101U, after)));
    BARRIER_CHECK(!fixture.barrier.runtime_assets_ready(
        manifest_digest, required));
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed, 4U, 101U, after)));
    BARRIER_CHECK(fixture.barrier.runtime_assets_ready(
        manifest_digest, required));
    BARRIER_CHECK(fixture.barrier.node_connection(node_index).state ==
        net::DirectorNodeConnection::State::Live);
    BARRIER_CHECK(fixture.barrier.node_connection(node_index).committed_frame == 1U);
}

void runtime_renderer_readiness_is_validated_live_session_state() {
    Fixture fixture;
    constexpr core::u32 node_index = 7U;
    BARRIER_CHECK(fixture.barrier.register_live(registration(
        10U, 20U, genesis_cursor(), 1U, 1U, node_index)));

    net::RuntimeRendererReadiness reported{};
    BARRIER_CHECK(fixture.barrier.runtime_renderer_readiness(
        node_index, reported));
    BARRIER_CHECK(reported == net::RuntimeRendererReadiness{});

    const net::RuntimeRendererReadiness ready{
        0x52454E44504F4C31ULL,
        net::RuntimeRendererReadinessState::Ready,
        net::RuntimeRendererBackend::Advanced,
        net::RuntimeRendererBackend::Advanced,
        0U,
        true,
        false,
    };
    BARRIER_CHECK(fixture.barrier.on_heartbeat_ack(
        id(10U),
        net::HeartbeatAcknowledgementReport{
            2U,
            900U,
            genesis_cursor(),
            {},
            {},
            ready,
        }));
    BARRIER_CHECK(fixture.barrier.runtime_renderer_readiness(
        node_index, reported));
    BARRIER_CHECK(reported == ready);

    // An invalid control sequence cannot replace the last validated answer.
    auto unavailable = ready;
    unavailable.state =
        net::RuntimeRendererReadinessState::
            AdvancedImplementationUnavailable;
    unavailable.satisfied = false;
    BARRIER_CHECK(!fixture.barrier.on_heartbeat_ack(
        id(10U),
        net::HeartbeatAcknowledgementReport{
            4U,
            901U,
            genesis_cursor(),
            {},
            {},
            unavailable,
        }));
    BARRIER_CHECK(fixture.barrier.runtime_renderer_readiness(
        node_index, reported));
    BARRIER_CHECK(reported == ready);

    fixture.barrier.unregister(
        id(10U), net::DirectorSessionLeaveReason::SessionFailure);
    BARRIER_CHECK(!fixture.barrier.runtime_renderer_readiness(
        node_index, reported));
}

// Uses real session membership, validated heartbeat observations, the durable
// FramePrepared quorum and per-session FrameCommitPermits for both generations.
void asset_generations_reveal_together_after_the_durable_permit() {
    GatedFixture fixture;
    BARRIER_CHECK(fixture.barrier.register_live(registration(10,20,genesis_cursor())));
    BARRIER_CHECK(fixture.barrier.register_live(registration(11,21,genesis_cursor())));
    std::array<core::AssetPublicationGate,2> outputs;
    std::array<core::u64,2> sequence{1,1};
    auto cursor=genesis_cursor();
    const auto report=[&](unsigned node,core::u64 generation,bool ready) {
        net::RuntimeRendererReadiness value{0x52454e44504f4c31ULL,net::RuntimeRendererReadinessState::Ready,
            net::RuntimeRendererBackend::Advanced,net::RuntimeRendererBackend::Advanced,0,true,false};
        value.assets={generation,ready,0};
        BARRIER_CHECK(fixture.barrier.on_heartbeat_ack(id(10+node),net::HeartbeatAcknowledgementReport{
            ++sequence[node],900+sequence[node],cursor,{},{},value}));
    };
    const auto cohort=[&] {
        core::AssetCohort result;
        for(unsigned node=0;node<2;++node) {
            net::RuntimeRendererReadiness observed;
            const bool known=fixture.barrier.runtime_renderer_readiness(20+node,observed);
            result.observe(known?&observed.assets:nullptr);
        }
        return result;
    };
    for(core::u64 generation:{71ULL,72ULL}) {
        report(0,generation,true); report(1,generation,false);
        BARRIER_CHECK(!cohort().can_commit());
        BARRIER_CHECK(!outputs[0].present({generation,true,0},{},generation,1000+generation,true));
        BARRIER_CHECK(!outputs[1].present({generation,false,0},{},generation,1000+generation,true));
        report(1,generation,true); BARRIER_CHECK(cohort().can_commit());
        const auto frame=cursor.through_frame.value()+1;
        const core::AssetCommit decision{generation,frame+core::asset_commit_lead_frames,1000+generation};
        document::Node node{document::NodeId{frame},"asset.commit.test",{
            {"digest",static_cast<core::i64>(decision.digest)},
            {"control",static_cast<core::i64>(decision.control_frame)},
            {"show",static_cast<core::i64>(decision.show_frame)}}};
        BARRIER_CHECK(fixture.director.schedule_edit(command::PutNode{node},command::EraseNode{node.id},
            command::UndoContextId{1},command::TransactionId{frame},command::FrameNumber{frame}));
        const auto sealed=fixture.barrier.seal_and_track(fixture.hub,command::FrameNumber{frame}); BARRIER_CHECK(sealed);
        const auto after=sealed.value().seal.frame.after_cursor();
        net::FrameCommitPermitIntent permit;
        BARRIER_CHECK(fixture.barrier.on_ack(id(10),acknowledgement(net::FrameAcknowledgementKind::Prepared,++sequence[0],101+frame,after)));
        BARRIER_CHECK(fixture.barrier.try_take_commit_permit(id(10),permit)==net::DirectorSessionObserver::CommitPermitPoll::Empty);
        BARRIER_CHECK(fixture.barrier.on_ack(id(11),acknowledgement(net::FrameAcknowledgementKind::Prepared,++sequence[1],201+frame,after)));
        for(unsigned output=0;output<2;++output) {
            BARRIER_CHECK(fixture.barrier.try_take_commit_permit(id(10+output),permit)==net::DirectorSessionObserver::CommitPermitPoll::Ready);
            BARRIER_CHECK(permit.after==after);
            BARRIER_CHECK(fixture.barrier.on_ack(id(10+output),acknowledgement(net::FrameAcknowledgementKind::Committed,
                ++sequence[output],output==0?101+frame:201+frame,after)));
            BARRIER_CHECK(!outputs[output].present({generation,true,0},decision,decision.control_frame-1,decision.show_frame,true));
            BARRIER_CHECK(outputs[output].present({generation,true,0},decision,decision.control_frame,decision.show_frame,true));
            BARRIER_CHECK(outputs[output].visible_commit()==decision.control_frame);
        }
        core::AssetPublicationGate missed;
        BARRIER_CHECK(!missed.present({generation,false,0},decision,decision.control_frame,decision.show_frame,true));
        BARRIER_CHECK(!missed.present({generation,true,0},decision,decision.control_frame+1,decision.show_frame,true));
        BARRIER_CHECK(fixture.barrier.retire(sealed.value().barrier)); cursor=after;
    }
}

void a_lost_member_wakes_waiters_as_degraded_not_reached() {
    Fixture fixture;
    BARRIER_CHECK(fixture.barrier.register_live(
        registration(10U, 20U, genesis_cursor())));
    auto first = fixture.seal(1U);
    std::optional<core::Result<
        net::DirectorFrameBarrierWaitReport,
        net::DirectorFrameBarrierError>> waited;

    std::thread waiter{[&] {
        waited.emplace(fixture.barrier.wait_for(
            first.barrier,
            net::DirectorFrameBarrierPhase::Committed,
            std::chrono::seconds{5}));
    }};
    fixture.barrier.unregister(
        id(10U), net::DirectorSessionLeaveReason::ResyncRequired);
    waiter.join();
    BARRIER_CHECK(waited.has_value() && *waited);
    BARRIER_CHECK(
        waited->value().status
        == net::DirectorFrameBarrierWaitStatus::Degraded);
    BARRIER_CHECK(waited->value().summary.lost == 1U);
    BARRIER_CHECK(waited->value().summary.committed == 0U);
}

void catching_up_sessions_are_excluded_until_the_exact_tail() {
    Fixture fixture;
    auto first = fixture.seal(1U);
    BARRIER_CHECK(
        fixture.barrier.snapshot(first.barrier).value().progress
        == net::DirectorFrameBarrierProgress::EmptyCohort);

    BARRIER_CHECK(fixture.barrier.register_live(
        registration(10U, 20U, genesis_cursor(), 1U, 40U)));
    auto diagnostics = fixture.barrier.observer_diagnostics();
    BARRIER_CHECK(diagnostics.live_sessions == 0U);
    BARRIER_CHECK(diagnostics.catching_up_sessions == 1U);

    const auto after = first.seal.frame.after_cursor();
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared, 41U, 1U, after)));
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed, 42U, 1U, after)));
    diagnostics = fixture.barrier.observer_diagnostics();
    BARRIER_CHECK(diagnostics.live_sessions == 1U);
    BARRIER_CHECK(diagnostics.catching_up_sessions == 0U);

    auto second = fixture.seal(2U);
    auto second_summary = fixture.barrier.snapshot(second.barrier);
    BARRIER_CHECK(second_summary);
    BARRIER_CHECK(second_summary.value().members == 1U);
    BARRIER_CHECK(
        second_summary.value().progress
        == net::DirectorFrameBarrierProgress::AwaitingPrepared);
}

void an_ahead_or_foreign_acknowledgement_is_rejected() {
    Fixture fixture;
    BARRIER_CHECK(fixture.barrier.register_live(
        registration(10U, 20U, genesis_cursor())));
    auto first = fixture.seal(1U);
    auto foreign = first.seal.frame.after_cursor();
    foreign.revision = document::Version{foreign.revision.value() + 1U};
    foreign.next_command =
        command::CommandId{foreign.next_command.value() + 1U};

    BARRIER_CHECK(!fixture.barrier.on_ack(
        id(99U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared,
            2U,
            1U,
            first.seal.frame.after_cursor())));
    BARRIER_CHECK(!fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared, 2U, 1U, foreign)));
    BARRIER_CHECK(!fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed,
            2U,
            1U,
            first.seal.frame.after_cursor())));
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared,
            2U,
            1U,
            first.seal.frame.after_cursor())));
}

void publication_cut_linearizes_a_concurrent_leave() {
    for (std::uint64_t iteration = 1U; iteration <= 12U; ++iteration) {
        (void)iteration;
        Fixture fixture;
        BARRIER_CHECK(fixture.barrier.register_live(
            registration(10U, 20U, genesis_cursor())));
        schedule_put(fixture.director, 1U);
        std::barrier start{3};
        std::optional<core::Result<
            net::DirectorTrackedSealOutcome,
            net::DirectorFrameBarrierError>> sealed;
        std::exception_ptr seal_exception;

        std::thread sealer{[&] {
            try {
                start.arrive_and_wait();
                sealed.emplace(fixture.barrier.seal_and_track(
                    fixture.hub, command::FrameNumber{1U}));
            } catch (...) {
                seal_exception = std::current_exception();
            }
        }};
        std::thread leaver{[&] {
            start.arrive_and_wait();
            fixture.barrier.unregister(
                id(10U), net::DirectorSessionLeaveReason::SessionFailure);
        }};
        start.arrive_and_wait();
        sealer.join();
        leaver.join();
        if (seal_exception) {
            std::rethrow_exception(seal_exception);
        }
        BARRIER_CHECK(sealed.has_value() && *sealed);
        auto summary = fixture.barrier.snapshot(sealed->value().barrier);
        BARRIER_CHECK(summary);
        BARRIER_CHECK(
            (summary.value().members == 0U && summary.value().lost == 0U)
            || (summary.value().members == 1U && summary.value().lost == 1U));
    }
}

void publication_cut_linearizes_a_concurrent_join() {
    for (std::uint64_t iteration = 1U; iteration <= 12U; ++iteration) {
        (void)iteration;
        Fixture fixture;
        schedule_put(fixture.director, 1U);
        std::barrier start{3};
        std::optional<core::Result<
            net::DirectorTrackedSealOutcome,
            net::DirectorFrameBarrierError>> sealed;
        bool registered{};
        std::exception_ptr seal_exception;

        std::thread sealer{[&] {
            try {
                start.arrive_and_wait();
                sealed.emplace(fixture.barrier.seal_and_track(
                    fixture.hub, command::FrameNumber{1U}));
            } catch (...) {
                seal_exception = std::current_exception();
            }
        }};
        std::thread joiner{[&] {
            start.arrive_and_wait();
            registered = fixture.barrier.register_live(
                registration(10U, 20U, genesis_cursor()));
        }};
        start.arrive_and_wait();
        sealer.join();
        joiner.join();
        if (seal_exception) {
            std::rethrow_exception(seal_exception);
        }
        BARRIER_CHECK(sealed.has_value() && *sealed);
        BARRIER_CHECK(registered);
        auto summary = fixture.barrier.snapshot(sealed->value().barrier);
        BARRIER_CHECK(summary);
        const auto diagnostics = fixture.barrier.observer_diagnostics();
        BARRIER_CHECK(
            (summary.value().members == 1U
             && diagnostics.live_sessions == 1U
             && diagnostics.catching_up_sessions == 0U)
            || (summary.value().members == 0U
                && diagnostics.live_sessions == 0U
                && diagnostics.catching_up_sessions == 1U));
    }
}

void full_frame_capacity_rejects_before_the_wal_advances() {
    Fixture fixture{net::DirectorFrameBarrierLimits{4U, 1U}};
    auto first = fixture.seal(1U);
    const auto durable_tail = first.seal.frame.after_cursor();
    BARRIER_CHECK(fixture.barrier.open_frames() == 1U);

    schedule_put(fixture.director, 2U);
    auto rejected = fixture.barrier.seal_and_track(
        fixture.hub, command::FrameNumber{2U});
    BARRIER_CHECK(!rejected);
    BARRIER_CHECK(
        rejected.error().code
        == net::DirectorFrameBarrierErrorCode::FrameCapacity);
    auto source = fixture.durable.capture_resume_source();
    BARRIER_CHECK(source);
    BARRIER_CHECK(source.value().checkpoint().cursor == durable_tail);
    BARRIER_CHECK(fixture.barrier.tracked_tail() == durable_tail);
    BARRIER_CHECK(fixture.barrier.open_frames() == 1U);
}

void failed_hub_seal_aborts_the_reserved_record() {
    Fixture fixture;
    schedule_put(fixture.director, 2U);
    auto rejected = fixture.barrier.seal_and_track(
        fixture.hub, command::FrameNumber{2U});
    BARRIER_CHECK(!rejected);
    BARRIER_CHECK(
        rejected.error().code
        == net::DirectorFrameBarrierErrorCode::HubFailure);
    BARRIER_CHECK(fixture.barrier.open_frames() == 0U);
    BARRIER_CHECK(fixture.barrier.tracked_tail() == genesis_cursor());

    auto recovered = fixture.barrier.seal_and_track(
        fixture.hub, command::FrameNumber{1U});
    BARRIER_CHECK(recovered);
    BARRIER_CHECK(fixture.barrier.open_frames() == 1U);
}

void postcommit_lineage_fault_preserves_the_durable_outcome() {
    Fixture fixture;
    schedule_put(fixture.director, 1U);
    auto bypassed = fixture.hub.seal_and_publish(command::FrameNumber{1U});
    BARRIER_CHECK(bypassed);
    schedule_put(fixture.director, 2U);

    auto tracked = fixture.barrier.seal_and_track(
        fixture.hub, command::FrameNumber{2U});
    BARRIER_CHECK(tracked);
    BARRIER_CHECK(
        tracked.value().tracking_status
        == net::DirectorFrameBarrierTrackingStatus::InvariantFault);
    BARRIER_CHECK(
        tracked.value().seal.frame.after_cursor().through_frame
        == command::FrameNumber{2U});
    BARRIER_CHECK(
        fixture.barrier.tracked_tail()
        == tracked.value().seal.frame.after_cursor());
    const auto terminal = fixture.barrier.snapshot(tracked.value().barrier);
    BARRIER_CHECK(terminal);
    BARRIER_CHECK(terminal.value().tracking_fault);
    BARRIER_CHECK(terminal.value().degraded());
    const auto waited = fixture.barrier.wait_for(
        tracked.value().barrier,
        net::DirectorFrameBarrierPhase::Committed,
        std::chrono::milliseconds{0});
    BARRIER_CHECK(waited);
    BARRIER_CHECK(
        waited.value().status
        == net::DirectorFrameBarrierWaitStatus::Degraded);

    auto later = fixture.barrier.seal_and_track(
        fixture.hub, command::FrameNumber{3U});
    BARRIER_CHECK(!later);
    BARRIER_CHECK(
        later.error().code
        == net::DirectorFrameBarrierErrorCode::BarrierFaulted);
}

void durable_two_node_quorum_mints_only_exact_claimed_permits() {
    GatedFixture fixture{{4U, 8U, 8U}, 37U};
    BARRIER_CHECK(fixture.barrier.coordinates_commit_permits());
    BARRIER_CHECK(fixture.barrier.register_live(
        registration(10U, 20U, genesis_cursor())));
    BARRIER_CHECK(fixture.barrier.register_live(
        registration(11U, 21U, genesis_cursor(), 1U, 16U)));
    const auto first = fixture.seal(1U);
    const auto after = first.seal.frame.after_cursor();
    BARRIER_CHECK(first.barrier.ordinal == 37U);

    net::FrameCommitPermitIntent permit{};
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared, 2U, 101U, after)));
    BARRIER_CHECK(fixture.release.persist_calls() == 0U);
    BARRIER_CHECK(
        fixture.barrier.try_take_commit_permit(id(10U), permit)
        == net::DirectorSessionObserver::CommitPermitPoll::Empty);

    BARRIER_CHECK(fixture.barrier.on_ack(
        id(11U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared, 17U, 202U, after)));
    BARRIER_CHECK(fixture.release.persist_calls() == 1U);
    BARRIER_CHECK(fixture.release.decision_count() == 1U);
    BARRIER_CHECK(fixture.release.frontier() == after);
    BARRIER_CHECK(fixture.barrier.released_through() == after);

    // Even a valid Committed acknowledgement cannot bypass permit delivery.
    BARRIER_CHECK(!fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed, 3U, 101U, after)));
    BARRIER_CHECK(
        fixture.barrier.observer_diagnostics().last_error
        == net::DirectorFrameBarrierObserverError::PermitMismatch);

    BARRIER_CHECK(
        fixture.barrier.try_take_commit_permit(id(10U), permit)
        == net::DirectorSessionObserver::CommitPermitPoll::Ready);
    BARRIER_CHECK(permit.barrier_ordinal == 37U);
    BARRIER_CHECK(permit.acknowledged_sequence == 101U);
    BARRIER_CHECK(permit.before == genesis_cursor());
    BARRIER_CHECK(permit.after == after);
    BARRIER_CHECK(
        fixture.barrier.try_take_commit_permit(id(10U), permit)
        == net::DirectorSessionObserver::CommitPermitPoll::Empty);

    BARRIER_CHECK(
        fixture.barrier.try_take_commit_permit(id(11U), permit)
        == net::DirectorSessionObserver::CommitPermitPoll::Ready);
    BARRIER_CHECK(permit.barrier_ordinal == 37U);
    BARRIER_CHECK(permit.acknowledged_sequence == 202U);
    BARRIER_CHECK(permit.before == genesis_cursor());
    BARRIER_CHECK(permit.after == after);

    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed, 3U, 101U, after)));
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(11U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed, 18U, 202U, after)));
    BARRIER_CHECK(
        fixture.barrier.snapshot(first.barrier).value().progress
        == net::DirectorFrameBarrierProgress::Complete);
}

void degraded_predecessor_freezes_a_later_prepared_quorum() {
    GatedFixture fixture{{4U, 8U, 8U}};
    BARRIER_CHECK(fixture.barrier.register_live(
        registration(10U, 20U, genesis_cursor())));
    BARRIER_CHECK(fixture.barrier.register_live(
        registration(11U, 21U, genesis_cursor())));
    const auto first = fixture.seal(1U);
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared,
            2U,
            101U,
            first.seal.frame.after_cursor())));

    fixture.barrier.unregister(
        id(11U), net::DirectorSessionLeaveReason::SessionFailure);
    BARRIER_CHECK(fixture.barrier.snapshot(first.barrier).value().degraded());

    const auto second = fixture.seal(2U);
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared,
            3U,
            102U,
            second.seal.frame.after_cursor())));
    BARRIER_CHECK(
        fixture.barrier.snapshot(second.barrier).value().progress
        == net::DirectorFrameBarrierProgress::AwaitingCommitted);

    // F2 reached Prepared first, but the unresolved F1 lineage prevents any
    // release decision or later permit from becoming visible.
    BARRIER_CHECK(fixture.release.persist_calls() == 0U);
    BARRIER_CHECK(fixture.release.decision_count() == 0U);
    BARRIER_CHECK(fixture.barrier.released_through() == genesis_cursor());
    net::FrameCommitPermitIntent permit{};
    BARRIER_CHECK(
        fixture.barrier.try_take_commit_permit(id(10U), permit)
        == net::DirectorSessionObserver::CommitPermitPoll::Empty);
    auto retired = fixture.barrier.retire(first.barrier);
    BARRIER_CHECK(!retired);
    BARRIER_CHECK(
        retired.error().code
        == net::DirectorFrameBarrierErrorCode::FrameIncomplete);

    // A reconnect cannot claim state beyond the durable release frontier.
    BARRIER_CHECK(!fixture.barrier.register_live(registration(
        12U, 22U, second.seal.frame.after_cursor())));
    BARRIER_CHECK(
        fixture.barrier.observer_diagnostics().last_error
        == net::DirectorFrameBarrierObserverError::CursorAhead);
}

void catch_up_permit_is_held_then_reuses_the_durable_historical_ordinal() {
    GatedFixture fixture{{6U, 8U, 8U}, 71U};
    BARRIER_CHECK(fixture.barrier.register_live(
        registration(10U, 20U, genesis_cursor())));
    const auto first = fixture.seal(1U);
    const auto after = first.seal.frame.after_cursor();

    BARRIER_CHECK(fixture.barrier.register_live(
        registration(11U, 21U, genesis_cursor())));
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(11U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared, 2U, 202U, after)));
    net::FrameCommitPermitIntent permit{};
    BARRIER_CHECK(
        fixture.barrier.try_take_commit_permit(id(11U), permit)
        == net::DirectorSessionObserver::CommitPermitPoll::Empty);
    BARRIER_CHECK(fixture.release.decision_count() == 0U);

    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared, 2U, 101U, after)));
    BARRIER_CHECK(fixture.release.decision_count() == 1U);
    BARRIER_CHECK(
        fixture.barrier.try_take_commit_permit(id(11U), permit)
        == net::DirectorSessionObserver::CommitPermitPoll::Ready);
    BARRIER_CHECK(permit.barrier_ordinal == 71U);
    BARRIER_CHECK(permit.acknowledged_sequence == 202U);
    BARRIER_CHECK(
        fixture.barrier.try_take_commit_permit(id(10U), permit)
        == net::DirectorSessionObserver::CommitPermitPoll::Ready);
    BARRIER_CHECK(permit.barrier_ordinal == 71U);
    BARRIER_CHECK(permit.acknowledged_sequence == 101U);

    BARRIER_CHECK(fixture.barrier.on_ack(
        id(11U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed, 3U, 202U, after)));
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed, 3U, 101U, after)));
    BARRIER_CHECK(fixture.barrier.retire(first.barrier));

    BARRIER_CHECK(fixture.barrier.register_live(
        registration(12U, 22U, genesis_cursor())));
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(12U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared, 2U, 303U, after)));
    BARRIER_CHECK(
        fixture.barrier.try_take_commit_permit(id(12U), permit)
        == net::DirectorSessionObserver::CommitPermitPoll::Ready);
    BARRIER_CHECK(permit.barrier_ordinal == 71U);
    BARRIER_CHECK(permit.acknowledged_sequence == 303U);
    BARRIER_CHECK(permit.before == genesis_cursor());
    BARRIER_CHECK(permit.after == after);
}

void permit_fifo_drains_in_frame_order_and_commits_in_frame_order() {
    GatedFixture fixture{{4U, 8U, 4U}};
    BARRIER_CHECK(fixture.barrier.register_live(
        registration(10U, 20U, genesis_cursor())));
    const auto first = fixture.seal(1U);
    const auto second = fixture.seal(2U);
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared,
            2U,
            101U,
            first.seal.frame.after_cursor())));
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared,
            3U,
            102U,
            second.seal.frame.after_cursor())));
    BARRIER_CHECK(fixture.release.decision_count() == 2U);

    net::FrameCommitPermitIntent first_permit{};
    net::FrameCommitPermitIntent second_permit{};
    BARRIER_CHECK(
        fixture.barrier.try_take_commit_permit(id(10U), first_permit)
        == net::DirectorSessionObserver::CommitPermitPoll::Ready);
    BARRIER_CHECK(
        fixture.barrier.try_take_commit_permit(id(10U), second_permit)
        == net::DirectorSessionObserver::CommitPermitPoll::Ready);
    BARRIER_CHECK(first_permit.barrier_ordinal == 1U);
    BARRIER_CHECK(second_permit.barrier_ordinal == 2U);
    BARRIER_CHECK(first_permit.acknowledged_sequence == 101U);
    BARRIER_CHECK(second_permit.acknowledged_sequence == 102U);
    BARRIER_CHECK(
        fixture.barrier.try_take_commit_permit(id(10U), second_permit)
        == net::DirectorSessionObserver::CommitPermitPoll::Empty);

    BARRIER_CHECK(!fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed,
            4U,
            102U,
            second.seal.frame.after_cursor())));
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed,
            4U,
            101U,
            first.seal.frame.after_cursor())));
    BARRIER_CHECK(fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Committed,
            5U,
            102U,
            second.seal.frame.after_cursor())));
    BARRIER_CHECK(
        fixture.barrier.snapshot(first.barrier).value().progress
        == net::DirectorFrameBarrierProgress::Complete);
    BARRIER_CHECK(
        fixture.barrier.snapshot(second.barrier).value().progress
        == net::DirectorFrameBarrierProgress::Complete);
}

void failed_release_persistence_faults_without_exposing_a_permit() {
    GatedFixture fixture{{2U, 4U, 4U}};
    BARRIER_CHECK(fixture.barrier.register_live(
        registration(10U, 20U, genesis_cursor())));
    const auto first = fixture.seal(1U);
    fixture.release.fail_persistence();
    BARRIER_CHECK(!fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared,
            2U,
            101U,
            first.seal.frame.after_cursor())));
    BARRIER_CHECK(fixture.release.persist_calls() == 1U);
    BARRIER_CHECK(fixture.release.decision_count() == 0U);
    BARRIER_CHECK(fixture.barrier.released_through() == genesis_cursor());
    const auto terminal = fixture.barrier.snapshot(first.barrier);
    BARRIER_CHECK(terminal);
    BARRIER_CHECK(terminal.value().tracking_fault);
    BARRIER_CHECK(terminal.value().degraded());
    BARRIER_CHECK(!fixture.barrier.coordinates_commit_permits());
    net::FrameCommitPermitIntent permit{};
    BARRIER_CHECK(
        fixture.barrier.try_take_commit_permit(id(10U), permit)
        == net::DirectorSessionObserver::CommitPermitPoll::Rejected);

    schedule_put(fixture.director, 2U);
    auto later = fixture.barrier.seal_and_track(
        fixture.hub, command::FrameNumber{2U});
    BARRIER_CHECK(!later);
    BARRIER_CHECK(
        later.error().code
        == net::DirectorFrameBarrierErrorCode::BarrierFaulted);
}

void release_failure_wakes_committed_waiter_as_terminally_degraded() {
    GatedFixture fixture{{2U, 4U, 4U}};
    BARRIER_CHECK(fixture.barrier.register_live(
        registration(10U, 20U, genesis_cursor())));
    const auto first = fixture.seal(1U);
    fixture.release.fail_persistence();

    using WaitResult = core::Result<
        net::DirectorFrameBarrierWaitReport,
        net::DirectorFrameBarrierError>;
    std::barrier started{2};
    std::optional<WaitResult> waited;
    std::thread waiter{[&] {
        started.arrive_and_wait();
        waited.emplace(fixture.barrier.wait_for(
            first.barrier,
            net::DirectorFrameBarrierPhase::Committed,
            std::chrono::milliseconds{500}));
    }};
    started.arrive_and_wait();

    const auto rejected = !fixture.barrier.on_ack(
        id(10U),
        acknowledgement(
            net::FrameAcknowledgementKind::Prepared,
            2U,
            101U,
            first.seal.frame.after_cursor()));
    waiter.join();

    BARRIER_CHECK(rejected);
    BARRIER_CHECK(waited.has_value());
    BARRIER_CHECK(*waited);
    BARRIER_CHECK(
        waited->value().status
        == net::DirectorFrameBarrierWaitStatus::Degraded);
    BARRIER_CHECK(waited->value().summary.tracking_fault);
    BARRIER_CHECK(waited->value().summary.degraded());
}

// The Director never stops sealing for a rejoining node, so a Committed
// acknowledgement can never be expected to land exactly on the tail. The
// publication cut instead admits a session whose Prepared cursor is within
// the configured lag, and it is then an ordinary member whose earlier,
// non-member frames still commit.
void a_catching_up_session_joins_the_cut_within_the_prepared_lag() {
    constexpr std::uint64_t join_lag = 3U;
    constexpr core::u32 node_index = 6U;
    Fixture fixture{net::DirectorFrameBarrierLimits{4U, 16U, 16U, join_lag}};
    std::vector<command::StreamCursor> after{genesis_cursor()};
    const auto seal = [&](std::uint64_t frame) {
        auto sealed = fixture.seal(frame);
        after.push_back(sealed.seal.frame.after_cursor());
        return sealed;
    };
    for (std::uint64_t frame = 1U; frame <= 5U; ++frame) {
        static_cast<void>(seal(frame));
    }
    BARRIER_CHECK(fixture.barrier.register_live(registration(
        10U, 20U, genesis_cursor(), 1U, 40U, node_index)));

    std::uint64_t sequence = 40U;
    const auto prepare_through = [&](std::uint64_t first, std::uint64_t last) {
        for (auto frame = first; frame <= last; ++frame) {
            BARRIER_CHECK(fixture.barrier.on_ack(
                id(10U),
                acknowledgement(
                    net::FrameAcknowledgementKind::Prepared,
                    ++sequence,
                    100U + frame,
                    after[frame])));
        }
    };

    // Prepared through 3 with the tail at 7 is a lag of 4: still history, so
    // the cut leaves it out and the session stays catching up.
    static_cast<void>(seal(6U));
    static_cast<void>(seal(7U));
    prepare_through(1U, 3U);
    auto excluded = seal(8U);
    BARRIER_CHECK(
        fixture.barrier.snapshot(excluded.barrier).value().members == 0U);
    BARRIER_CHECK(fixture.barrier.node_connection(node_index).state
        == net::DirectorNodeConnection::State::CatchingUp);
    BARRIER_CHECK(fixture.barrier.observer_diagnostics().catching_up_sessions
        == 1U);

    // Prepared through 5 with the tail at 8 is exactly the configured lag.
    prepare_through(4U, 5U);
    auto joined = seal(9U);
    auto summary = fixture.barrier.snapshot(joined.barrier);
    BARRIER_CHECK(summary);
    BARRIER_CHECK(summary.value().members == 1U);
    BARRIER_CHECK(
        summary.value().progress
        == net::DirectorFrameBarrierProgress::AwaitingPrepared);
    const auto connection = fixture.barrier.node_connection(node_index);
    BARRIER_CHECK(connection.state == net::DirectorNodeConnection::State::Live);
    BARRIER_CHECK(connection.committed_frame == 0U);
    BARRIER_CHECK(connection.target_frame == 9U);
    const auto diagnostics = fixture.barrier.observer_diagnostics();
    BARRIER_CHECK(diagnostics.live_sessions == 1U);
    BARRIER_CHECK(diagnostics.catching_up_sessions == 0U);

    // Its in-flight frames finish in order: the non-member frames commit on
    // the permits they were queued with, and the member frame waits for it.
    prepare_through(6U, 9U);
    BARRIER_CHECK(
        fixture.barrier.snapshot(joined.barrier).value().prepared == 1U);
    for (std::uint64_t frame = 1U; frame <= 9U; ++frame) {
        BARRIER_CHECK(fixture.barrier.on_ack(
            id(10U),
            acknowledgement(
                net::FrameAcknowledgementKind::Committed,
                ++sequence,
                100U + frame,
                after[frame])));
    }
    BARRIER_CHECK(
        fixture.barrier.snapshot(joined.barrier).value().progress
        == net::DirectorFrameBarrierProgress::Complete);
    BARRIER_CHECK(
        fixture.barrier.node_connection(node_index).committed_frame == 9U);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"exact cohort retains lost generation",
         &exact_cohort_retains_a_lost_generation},
        {"prepared and committed phases",
         &prepared_and_committed_phases_are_distinct_and_ordered},
        {"replay-seeded heartbeat control order",
         &replay_seeded_heartbeat_preserves_control_order_and_frame_progress},
        {"runtime asset adoption exact live set",
         &runtime_asset_adoption_is_exact_live_and_fail_closed},
        {"runtime asset adoption excludes catch-up",
         &runtime_asset_adoption_requires_live_not_catching_up},
        {"asset generations reveal on the same permitted frame", &asset_generations_reveal_together_after_the_durable_permit},
        {"runtime renderer readiness is live session state",
         &runtime_renderer_readiness_is_validated_live_session_state},
        {"lost member wakes degraded",
         &a_lost_member_wakes_waiters_as_degraded_not_reached},
        {"catch-up promotion at exact tail",
         &catching_up_sessions_are_excluded_until_the_exact_tail},
        {"catch-up joins the cut within the prepared lag",
         &a_catching_up_session_joins_the_cut_within_the_prepared_lag},
        {"foreign acknowledgement rejection",
         &an_ahead_or_foreign_acknowledgement_is_rejected},
        {"publication cut concurrent leave",
         &publication_cut_linearizes_a_concurrent_leave},
        {"publication cut concurrent join",
         &publication_cut_linearizes_a_concurrent_join},
        {"capacity rejects before WAL",
         &full_frame_capacity_rejects_before_the_wal_advances},
        {"hub failure aborts reservation",
         &failed_hub_seal_aborts_the_reserved_record},
        {"postcommit lineage fault preserves outcome",
         &postcommit_lineage_fault_preserves_the_durable_outcome},
        {"durable exact two-node permit quorum",
         &durable_two_node_quorum_mints_only_exact_claimed_permits},
        {"degraded predecessor freezes later quorum",
         &degraded_predecessor_freezes_a_later_prepared_quorum},
        {"catch-up held then historical permit",
         &catch_up_permit_is_held_then_reuses_the_durable_historical_ordinal},
        {"permit FIFO drain and commit order",
         &permit_fifo_drains_in_frame_order_and_commits_in_frame_order},
        {"release failure exposes no permit",
         &failed_release_persistence_faults_without_exposing_a_permit},
        {"a coalesced pose is counted and leaves a visible gap",
         &a_coalesced_pose_is_counted_and_leaves_a_gap_a_node_can_see},
        {"release failure wakes committed waiter",
         &release_failure_wakes_committed_waiter_as_terminally_degraded},
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
