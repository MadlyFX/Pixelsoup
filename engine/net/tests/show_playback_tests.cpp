#include "show_playback_rig.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

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

    // Play: the frame that carries the anchor presents the position the
    // operator was looking at, and every frame after it derives the next one
    // from the ordinal alone. Both independently paced nodes land on it.
    for (core::u64 index = 0U; index < 20U; ++index) {
        const auto position = director.publish_next();
        PLAYBACK_CHECK(position == show::ShowFrame{index});
        const auto frame = director.sealed_through();
        PLAYBACK_CHECK(node_a.advance_to(frame));
        PLAYBACK_CHECK(node_b.advance_to(frame));
        assert_converged(node_a, node_b);
        PLAYBACK_CHECK(node_a.presented().show_frame == position);
        PLAYBACK_CHECK(
            node_a.presented().transport->state == show::TransportState::Playing);
    }

    const auto played_to = node_a.presented().show_frame;
    PLAYBACK_CHECK(played_to == show::ShowFrame{19U});
    // Twenty show frames advanced on one document edit: the anchor authored
    // when play was pressed. This is the property the durable path depends on.
    PLAYBACK_CHECK(director.authored_anchors() == 1U);

    // Pause: the show position stops advancing on both nodes even though
    // control frames keep flowing. The pause takes effect on the next control
    // frame, so it holds the position that frame derives rather than rewinding
    // to the one already on screen.
    director.pause();
    const show::ShowFrame held{played_to.value() + 1U};
    for (core::u64 index = 0U; index < 5U; ++index) {
        director.publish_held();
        const auto frame = director.sealed_through();
        PLAYBACK_CHECK(node_a.advance_to(frame));
        PLAYBACK_CHECK(node_b.advance_to(frame));
        assert_converged(node_a, node_b);
        PLAYBACK_CHECK(node_a.presented().show_frame == held);
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
    PLAYBACK_CHECK(node_a.presented().show_frame == target);

    // Resuming from the sought position continues from exactly there.
    director.play();
    for (core::u64 index = 0U; index < 5U; ++index) {
        const auto position = director.publish_next();
        PLAYBACK_CHECK(position == show::ShowFrame{target.value() + index});
        PLAYBACK_CHECK(node_a.advance_to(director.sealed_through()));
        PLAYBACK_CHECK(node_b.advance_to(director.sealed_through()));
        assert_converged(node_a, node_b);
    }
    PLAYBACK_CHECK(
        node_a.presented().show_frame
        == show::ShowFrame{target.value() + 4U});
    // Play, pause, seek and play again: four operator actions, four edits,
    // across every control frame the run sealed.
    PLAYBACK_CHECK(director.authored_anchors() == 4U);

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
        late.presented().show_frame == director.show_frame());
    PLAYBACK_CHECK(!late.faulted());

    early.stop();
    late.stop();
    director.stop();
}

// The V1 gate at a length the default suite can afford. The ten-minute run is
// the same code in mediaserver_show_soak; this one is here so a regression that
// puts a write, an allocation-per-frame, or a skipped position back on the seal
// path fails in CI rather than in the gate run.
void two_nodes_hold_the_rate_with_flat_director_memory() {
    SustainedOptions options{};
    options.seconds = 2U;
    options.rate = show::FrameRate{120U, 1U};
    run_sustained(options);
}

// Both durability cadences have to work end to end. The behavioural cases
// above run the edit-shaped default; this one runs the frame-cadence journal
// and checks the WAL actually grew by a frame per seal.
void the_journalling_cadence_still_writes_every_frame() {
    DirectorShow director{persist::FrameDurability::EveryFrame};
    const auto endpoint = director.start();
    NodeRig node{endpoint, 0U};
    node.start();
    PLAYBACK_CHECK(node.connected());

    const auto bytes_before = director.wal_bytes();
    director.play();
    for (core::u64 index = 0U; index < 8U; ++index) {
        static_cast<void>(director.publish_next());
    }
    PLAYBACK_CHECK(node.advance_to(director.sealed_through()));
    PLAYBACK_CHECK(director.wal_bytes() > bytes_before);
    PLAYBACK_CHECK(node.presented().show_frame == show::ShowFrame{7U});
    PLAYBACK_CHECK(!node.faulted());

    node.stop();
    director.stop();
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 4U> cases{{
    {"play, pause, and exact seek across two nodes",
     &two_nodes_play_pause_and_seek_to_the_same_frame},
    {"late node adopts a checkpoint at a boundary",
     &a_node_joining_a_running_show_adopts_its_checkpoint_at_a_boundary},
    {"two nodes hold 120 fps with flat Director memory",
     &two_nodes_hold_the_rate_with_flat_director_memory},
    {"the journalling cadence still writes every frame",
     &the_journalling_cadence_still_writes_every_frame},
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
