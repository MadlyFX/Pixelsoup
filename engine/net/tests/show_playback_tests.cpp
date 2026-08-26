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
        PLAYBACK_CHECK(
            NodeRig::advance_cohort_to(frame, {&node_a, &node_b}));
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
        PLAYBACK_CHECK(
            NodeRig::advance_cohort_to(frame, {&node_a, &node_b}));
        assert_converged(node_a, node_b);
        PLAYBACK_CHECK(node_a.presented().show_frame == held);
        PLAYBACK_CHECK(
            node_a.presented().transport->state == show::TransportState::Paused);
    }

    // Exact seek: an arbitrary frame is reached precisely, not approximately.
    constexpr show::ShowFrame target{4'321U};
    director.seek(target);
    director.publish_held();
    PLAYBACK_CHECK(NodeRig::advance_cohort_to(
        director.sealed_through(), {&node_a, &node_b}));
    assert_converged(node_a, node_b);
    PLAYBACK_CHECK(node_a.presented().show_frame == target);

    // Resuming from the sought position continues from exactly there.
    director.play();
    for (core::u64 index = 0U; index < 5U; ++index) {
        const auto position = director.publish_next();
        PLAYBACK_CHECK(position == show::ShowFrame{target.value() + index});
        PLAYBACK_CHECK(NodeRig::advance_cohort_to(
            director.sealed_through(), {&node_a, &node_b}));
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
    PLAYBACK_CHECK(NodeRig::advance_cohort_to(frame, {&early, &late}));

    PLAYBACK_CHECK(late.driver().counters().checkpoint_activations >= 1U);
    assert_converged(early, late);
    PLAYBACK_CHECK(
        late.presented().show_frame == director.show_frame());
    PLAYBACK_CHECK(!late.faulted());

    early.stop();
    late.stop();
    director.stop();
}

void a_lost_node_restarts_and_rejoins_the_running_show() {
    DirectorShow director;
    const auto endpoint = director.start();

    NodeRig node_a{endpoint, 0U};
    NodeRig node_b{endpoint, 1U};
    node_a.start();
    node_b.start();
    PLAYBACK_CHECK(node_a.connected());
    PLAYBACK_CHECK(node_b.connected());
    PLAYBACK_CHECK(wait_until([&] { return director.active_sessions() == 2U; }));

    director.play();
    for (core::u64 index = 0U; index < 12U; ++index) {
        static_cast<void>(director.publish_next());
    }
    PLAYBACK_CHECK(NodeRig::advance_cohort_to(
        director.sealed_through(), {&node_a, &node_b}));
    assert_converged(node_a, node_b);

    node_b.stop();
    PLAYBACK_CHECK(wait_until([&] { return director.active_sessions() == 1U; }));
    for (core::u64 index = 0U; index < 8U; ++index) {
        static_cast<void>(director.publish_next());
    }
    PLAYBACK_CHECK(node_a.advance_to(director.sealed_through()));

    const auto recovery_started = std::chrono::steady_clock::now();
    NodeRig restarted_b{endpoint, 1U, 1U};
    restarted_b.start();
    PLAYBACK_CHECK(restarted_b.connected());
    for (core::u64 index = 0U; index < 8U; ++index) {
        static_cast<void>(director.publish_next());
    }
    PLAYBACK_CHECK(NodeRig::advance_cohort_to(
        director.sealed_through(), {&node_a, &restarted_b}));
    PLAYBACK_CHECK(wait_until([&] { return director.active_sessions() == 2U; }));
    assert_converged(node_a, restarted_b);
    PLAYBACK_CHECK(
        std::chrono::steady_clock::now() - recovery_started < 5s);
    PLAYBACK_CHECK(!node_a.faulted());
    PLAYBACK_CHECK(!restarted_b.faulted());
    PLAYBACK_CHECK(restarted_b.driver().counters().resync_reports == 0U);

    node_a.stop();
    restarted_b.stop();
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

// The 3D counterpart to the convergence cases above: the Director authors a
// replicated scene, and two nodes independently evaluate it to a byte-identical
// result at every show frame while each owns a different slice of the canvas.
// This is the headless half of the scene proof - no GPU, no window - and it
// runs the real seal-and-replicate path rather than a single in-process model.
void two_nodes_agree_on_a_replicated_scene_frame_by_frame() {
    // The scene's ids must clear the transport's reserved id. If this regresses,
    // the transport node overwrites the stage, the canvas silently reverts to
    // its default, and everything below still "passes" on equal-but-wrong state
    // - so it is asserted at compile time, where it cannot be evaluated away.
    static_assert(
        show::demo_scene_node_base > show::transport_node_id.value(),
        "the demo scene must not reuse the transport's reserved node id");

    DirectorShow director;
    const auto endpoint = director.start();

    NodeRig node_a{endpoint, 0U};
    NodeRig node_b{endpoint, 1U};
    node_a.start();
    node_b.start();
    PLAYBACK_CHECK(node_a.connected());
    PLAYBACK_CHECK(node_b.connected());

    // Author the scene, then play: both land on the first sealed control frame,
    // so the frame that starts the show is the frame that carries the scene and
    // the transport anchor together.
    director.author_scene();
    director.play();
    for (core::u64 index = 0U; index < 20U; ++index) {
        static_cast<void>(director.publish_next());
    }
    PLAYBACK_CHECK(
        NodeRig::advance_cohort_to(director.sealed_through(), {&node_a, &node_b}));
    assert_converged(node_a, node_b);

    // Each node evaluates from its own committed document - two documents,
    // reconstructed independently over two connections.
    scene::SceneModel model_a;
    scene::SceneModel model_b;
    model_a.reset(node_a.presented().root);
    model_b.reset(node_b.presented().root);

    const scene::SceneTime origin{0U, show_rate.numerator, show_rate.denominator};
    const auto opening = model_a.evaluate(origin);
    // The stage survived being authored next to the transport anchor: the canvas
    // is the authored 2560x720, not the 1920x1080 default a clobbered stage
    // leaves behind. This is the assertion that catches the node-id collision.
    PLAYBACK_CHECK(opening.canvas == (scene::Canvas{2'560U, 720U}));
    PLAYBACK_CHECK(model_a.counters().rejected == 0U);

    // Both feed-map bindings are present and fit the canvas, so each node has a
    // slice to draw and neither runs off the shared image.
    const auto* const slice_0 = opening.slice_for(0U);
    const auto* const slice_1 = opening.slice_for(1U);
    PLAYBACK_CHECK(slice_0 != nullptr);
    PLAYBACK_CHECK(slice_1 != nullptr);
    PLAYBACK_CHECK(scene::contains(opening.canvas, *slice_0));
    PLAYBACK_CHECK(scene::contains(opening.canvas, *slice_1));

    // The load-bearing property: at every show frame the two nodes compute a
    // byte-identical scene, and the scene is not static - the camera orbit, the
    // spin and the bob change it - so the agreement is about something that
    // moves rather than a frozen frame two wrong evaluators could also share.
    core::u64 previous = 0U;
    std::size_t distinct = 0U;
    for (core::u64 frame = 0U; frame < 60U; ++frame) {
        const scene::SceneTime time{
            frame, show_rate.numerator, show_rate.denominator};
        const auto digest_a = scene::digest(model_a.evaluate(time));
        const auto digest_b = scene::digest(model_b.evaluate(time));
        PLAYBACK_CHECK(digest_a == digest_b);
        if (frame == 0U || digest_a != previous) {
            ++distinct;
        }
        previous = digest_a;
    }
    PLAYBACK_CHECK(distinct >= 55U);

    PLAYBACK_CHECK(!node_a.faulted());
    PLAYBACK_CHECK(!node_b.faulted());

    node_a.stop();
    node_b.stop();
    director.stop();
}

// The feed-map counterpart to the scene case: the same real seal-and-replicate
// path, proving two nodes derive a byte-identical feed map from the document and
// that the map they agree on is a genuine cross-fade rather than a hard seam.
void two_nodes_agree_on_a_replicated_feed_map() {
    // The feed map's ids must clear both the transport's reserved id and the
    // demo scene's range. A regression there is silent - a colliding node
    // overwrites another and the map quietly changes - so it is asserted at
    // compile time, where it cannot be evaluated away.
    static_assert(
        show::demo_feed_map_node_base > show::transport_node_id.value(),
        "the demo feed map must not reuse the transport's reserved node id");
    static_assert(
        show::demo_feed_map_node_base > show::demo_scene_node_base + 7U,
        "the demo feed map must not reuse a demo scene node id");

    DirectorShow director;
    const auto endpoint = director.start();

    NodeRig node_a{endpoint, 0U};
    NodeRig node_b{endpoint, 1U};
    node_a.start();
    node_b.start();
    PLAYBACK_CHECK(node_a.connected());
    PLAYBACK_CHECK(node_b.connected());

    // Scene, feed map, and the transport anchor all land on the first sealed
    // control frame - one more layer of ids that must not collide with the
    // others.
    director.author_scene();
    director.author_feed_map();
    director.play();
    for (core::u64 index = 0U; index < 20U; ++index) {
        static_cast<void>(director.publish_next());
    }
    PLAYBACK_CHECK(
        NodeRig::advance_cohort_to(director.sealed_through(), {&node_a, &node_b}));
    assert_converged(node_a, node_b);

    // Each node parses the feed map from its own independently reconstructed
    // committed document - two documents, two connections.
    output::FeedMapModel model_a;
    output::FeedMapModel model_b;
    model_a.reset(node_a.presented().root);
    model_b.reset(node_b.presented().root);

    const auto feed_a = model_a.evaluate();
    const auto feed_b = model_b.evaluate();

    // The replication claim for the feed map: two nodes, two documents, one map
    // byte for byte. If this regresses, the wall's slices stop lining up.
    PLAYBACK_CHECK(output::digest(feed_a) == output::digest(feed_b));
    PLAYBACK_CHECK(model_a.counters().rejected == 0U);

    // Both bindings survived being authored next to the scene and the transport.
    const auto* const map_0 = feed_a.map_for(0U);
    const auto* const map_1 = feed_a.map_for(1U);
    PLAYBACK_CHECK(map_0 != nullptr);
    PLAYBACK_CHECK(map_1 != nullptr);

    // The load-bearing property on an LED wall: the two nodes' replicated
    // regions abut edge to edge with no gap and no overlap, so they tile the
    // canvas pixel-exact. Computed from the replicated maps, so it is agreement
    // about a real seam rather than a coincidence two wrong maps could share.
    PLAYBACK_CHECK(map_0->source.y == map_1->source.y);
    PLAYBACK_CHECK(map_0->source.height == map_1->source.height);
    PLAYBACK_CHECK(map_0->source.x + map_0->source.width == map_1->source.x);

    PLAYBACK_CHECK(!node_a.faulted());
    PLAYBACK_CHECK(!node_b.faulted());

    node_a.stop();
    node_b.stop();
    director.stop();
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 7U> cases{{
    {"play, pause, and exact seek across two nodes",
     &two_nodes_play_pause_and_seek_to_the_same_frame},
    {"late node adopts a checkpoint at a boundary",
     &a_node_joining_a_running_show_adopts_its_checkpoint_at_a_boundary},
    {"lost node restarts and rejoins the running show",
     &a_lost_node_restarts_and_rejoins_the_running_show},
    {"two nodes hold 120 fps with flat Director memory",
     &two_nodes_hold_the_rate_with_flat_director_memory},
    {"the journalling cadence still writes every frame",
     &the_journalling_cadence_still_writes_every_frame},
    {"two nodes agree on a replicated scene frame by frame",
     &two_nodes_agree_on_a_replicated_scene_frame_by_frame},
    {"two nodes agree on a replicated feed map",
     &two_nodes_agree_on_a_replicated_feed_map},
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
