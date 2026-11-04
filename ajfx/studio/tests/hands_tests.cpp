#include "hands.hpp"
#include "performance.hpp"
#include "stage.hpp"
#include "test_support.hpp"

// Hands: painting things in, erasing and pushing them with a brush,
// grabbing a body and throwing it, poking it, setting off a blast. Each is
// recorded as acts, replayed exactly, saved and reopened, and edited on the
// timeline. The two scenes the sandbox is for are here whole: logs painted
// in that tumble and smoulder, and a ball picked up and flung so a wall
// breaks. Expectations come from geometry worked out here (a dab's things lie
// on its disc, a carried body follows the hand, a push off-centre spins it
// the way the lever says), from counts, and from comparing two runs.
namespace {
using namespace studio_test;

Object& object(Document& document, Uid uid) { return std::get<Object>(document.scene->item(uid)->data); }

float length(ajfx::Vec3 v) { return std::sqrt(ajfx::dot(v, v)); }

Prepared prepared(const Document& document) {
    Prepared ready = prepare_scene(document);
    if (!ready.compiled) {
        throw std::runtime_error{ready.error};
    }
    return ready;
}

Played play(const Document& document, const Prepared& ready, const ajfx::RigidSolver& solver, std::uint64_t to) {
    Played played = play_scene(document, ready, &solver, 0U, to);
    if (!played.error.empty()) {
        throw std::runtime_error{played.error};
    }
    return played;
}

std::size_t pieces(const Played& played, const Prepared& ready) {
    std::size_t count = 0U;
    for (const WorldBody& body : world_bodies(played, ready)) {
        count += body.ref ? 0U : 1U;
    }
    return count;
}

// The scene saved and opened again.
Document reopened(const Document& document) {
    Loaded loaded = load_document(save_document(document));
    if (!loaded.document) {
        throw std::runtime_error{loaded.error};
    }
    return std::move(*loaded.document);
}

struct Logs final {
    Document document = new_scene();
    Uid log{};
    Uid smoke{};
};

// Logs that are only ever painted in, with smoke trailing them.
Logs smouldering_logs() {
    Logs logs;
    logs.log = add_object(logs.document, "slab");
    Object& log = object(logs.document, logs.log);
    log.distribution = Distribution::Placed;
    log.size = 0.6F;
    logs.smoke = add_bound_system(logs.document, RelationKind::TrailFrom, logs.log);
    CHECK(logs.smoke != 0U);
    return logs;
}

struct Wall final {
    Document document = new_scene();
    Uid wall{};
    Uid ball{};
};

// A wall that breaks when hit at 4 m/s or blasted that hard, and a heavy
// ball in front of it.
Wall wall_and_ball() {
    Wall scene;
    scene.wall = add_object(scene.document);
    Object& wall = object(scene.document, scene.wall);
    wall.size = 1.6F;
    wall.position = {0.0F, 0.8F, -2.0F};
    wall.fracture = true;
    wall.fragments = 8U;
    wall.fracture_speed = 4.0F;
    scene.ball = add_object(scene.document);
    Object& ball = object(scene.document, scene.ball);
    ball.size = 0.4F;
    ball.position = {0.0F, 0.2F, 2.0F};
    ball.density = 3000.0F;
    return scene;
}

// ------------------------------------------------------------ painting

void a_painted_stroke_lays_logs_that_smoulder_and_replay_exactly() {
    Logs logs = smouldering_logs();
    const Prepared ready = prepared(logs.document);
    const ActTargets& targets = ready.targets;
    CHECK(droppable(targets, logs.log));
    Brush brush;
    brush.radius = 0.8F;
    brush.count = 2U;
    brush.lift = 1.0F; // from a metre up, so they tumble into each other
    // Along the floor from x = -2 to 2, a dab every 6 steps from step 30.
    std::uint32_t stroke = 0U;
    std::size_t dabs = 0U;
    for (std::uint64_t tick = 30U; tick <= 90U; tick += 6U) {
        const float x = -2.0F + 4.0F * static_cast<float>(tick - 30U) / 60.0F;
        const BrushDab dab = brush_dab(logs.document, targets, brush, logs.log, {x, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
                                       {}, tick, stroke);
        CHECK(dab.refusal.empty() && dab.serials.size() == 1U && dab.tick == tick);
        CHECK(stroke == 0U || dab.stroke == stroke);
        stroke = dab.stroke;
        ++dabs;
    }
    const Performance& performance = logs.document.scene->performance;
    CHECK(dabs == 11U && performance.acts.size() == 11U);
    // Each dab holds a serial per log, one after another.
    CHECK(performance.next_serial == 1U + 22U && stroke == 1U);
    CHECK(dropped_count(performance) == 22U);
    for (const Act& act : performance.acts) {
        CHECK(act.kind == ActKind::Paint && act.stroke == stroke && act.count == 2U);
        // Its middle half a log and the lift above the floor.
        CHECK(near(act.position.y, 0.3F + 1.0F));
        for (const PaintedThing& thing : painted_things(act)) {
            // On the brush's disc, across the surface.
            CHECK(thing.serial >= act.serial && thing.serial < act.serial + 2U);
            CHECK(near(thing.position.y, act.position.y, 1.0e-5F));
            CHECK(length(thing.position - act.position) <= brush.radius + 1.0e-5F);
            // Realistic: stood up along the floor, turned about it only.
            const ajfx::Vec3 up = ajfx::rotate(thing.orientation, {0.0F, 1.0F, 0.0F});
            CHECK(near(up.y, 1.0F, 1.0e-4F));
        }
    }
    // The dabs' things are not all in one place.
    const std::vector<PaintedThing> first = painted_things(performance.acts.front());
    CHECK(!(first[0].position == first[1].position));

    auto solver = ajfx::rigid::make_jolt_solver();
    const Played played = play(logs.document, ready, *solver, 240U);
    CHECK(played.skipped == 0U && played.world.has_value());
    CHECK(world_bodies(played, ready).size() == 22U);
    // They fell and lie on the floor now.
    for (const WorldBody& body : world_bodies(played, ready)) {
        CHECK(body.position.y < 1.0F);
    }
    // And smoke trailed them as they fell.
    std::size_t smoke = 0U;
    for (std::size_t e = 0U; e < played.simulation->effect().emitters.size(); ++e) {
        if (e != *played.world) {
            smoke += played.simulation->emitter(e).spawned;
        }
    }
    CHECK(smoke > 0U);

    // Played again, and saved and opened again: the same, bit for bit.
    const std::uint64_t digest = state_digest(*played.simulation);
    CHECK(state_digest(*play(logs.document, ready, *solver, 240U).simulation) == digest);
    const Document again = reopened(logs.document);
    CHECK(again.scene->performance == performance);
    CHECK(state_digest(*play(again, prepared(again), *solver, 240U).simulation) == digest);

    // One stroke on the log's track, a bar from its first dab to its last.
    const std::vector<Track> tracks = collect_tracks(*logs.document.scene, 6.0F);
    const auto track = std::find_if(tracks.begin(), tracks.end(), [&](const Track& t) { return t.item == logs.log; });
    CHECK(track != tracks.end() && track->markers.size() == 1U);
    const TrackMarker bar = track->markers.front();
    CHECK(bar.kind == MarkerKind::Paint && bar.part == stroke && near(bar.seconds, 0.5F) && near(bar.duration, 1.0F));
    // Dragged half a second later, the whole stroke goes with it.
    CHECK(set_marker_time(logs.document, bar, 1.0F));
    CHECK(performance.acts.front().tick == 60U && performance.acts.back().tick == 120U);
    CHECK(painted_things(performance.acts.front())[0].position == first[0].position);

    // Taking one dab away leaves every other log as it would have been.
    Document fewer = logs.document;
    const std::uint32_t middle = fewer.scene->performance.acts[5].serial;
    CHECK(remove_act(fewer.scene->performance, middle));
    CHECK(fewer.scene->performance.acts.size() == 10U && fewer.scene->performance.next_serial == 23U);
    CHECK(painted_things(fewer.scene->performance.acts[6])[1].position ==
          painted_things(performance.acts[7])[1].position);
    // Taking the stroke away takes it all.
    CHECK(remove_marker_act(logs.document, collect_tracks(*logs.document.scene, 6.0F).front().markers.front()));
    CHECK(logs.document.scene->performance.acts.empty());
}

void a_brush_erases_and_pushes_what_is_under_it() {
    Logs logs = smouldering_logs();
    const Prepared ready = prepared(logs.document);
    const ActTargets& targets = ready.targets;
    Performance& performance = logs.document.scene->performance;
    // Four logs on the stage, two near x = -3 and two near x = 3.
    for (const float x : {-3.35F, -2.65F, 2.65F, 3.35F}) {
        CHECK(place(logs.document, targets, logs.log, {x, 0.3F, 0.0F}, {}).serial != 0U);
    }
    auto solver = ajfx::rigid::make_jolt_solver();
    const Played resting = play(logs.document, ready, *solver, 60U);
    const std::vector<StageBody> bodies = played_stage(resting, ready);
    CHECK(bodies.size() >= 4U);

    Brush eraser;
    eraser.mode = BrushMode::Erase;
    eraser.radius = 0.6F;
    // Nothing under it in the middle.
    const BrushDab missed = brush_dab(logs.document, targets, eraser, 0U, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
                                      bodies, 60U);
    CHECK(missed.serials.empty() && !missed.refusal.empty() && performance.acts.size() == 4U);
    // The two on the left go; the two on the right stay.
    const BrushDab erased = brush_dab(logs.document, targets, eraser, 0U, {-3.0F, 0.3F, 0.0F}, {0.0F, 1.0F, 0.0F},
                                      bodies, 60U);
    CHECK(erased.serials.size() == 2U && erased.refusal.empty());
    CHECK(performance.acts[4].kind == ActKind::Delete && performance.acts[4].target == BodyRef{.drop = 1U});
    // A second pass finds nothing more to take away there.
    CHECK(brush_dab(logs.document, targets, eraser, 0U, {-3.0F, 0.3F, 0.0F}, {0.0F, 1.0F, 0.0F}, bodies, 61U)
              .serials.empty());
    CHECK(dropped_count(performance) == 2U);

    // Pushed apart, out from the brush's middle along the floor.
    Brush pusher;
    pusher.mode = BrushMode::Push;
    pusher.radius = 0.6F;
    pusher.strength = 3.0F;
    const BrushDab pushed = brush_dab(logs.document, targets, pusher, 0U, {3.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
                                      bodies, 60U);
    CHECK(pushed.serials.size() == 2U);
    const Act* left = find_act(performance, pushed.serials[0]);
    const Act* right = find_act(performance, pushed.serials[1]);
    CHECK(left->kind == ActKind::Push && left->target == BodyRef{.drop = 3U});
    // Along the floor: no part of the push is up or down.
    CHECK(left->push.x < -2.9F && near(left->push.y, 0.0F, 1.0e-6F) && right->push.x > 2.9F);
    const Played after = play(logs.document, ready, *solver, 61U);
    CHECK(world_bodies(after, ready).size() == 2U);
    CHECK(find_body(after, ready, BodyRef{.drop = 3U})->velocity.x < -1.5F);
    CHECK(find_body(after, ready, BodyRef{.drop = 4U})->velocity.x > 1.5F);

    // Pull draws them in instead.
    Brush puller = pusher;
    puller.mode = BrushMode::Pull;
    const BrushDab pulled = brush_dab(logs.document, targets, puller, 0U, {3.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
                                      bodies, 90U);
    CHECK(find_act(performance, pulled.serials[0])->push.x > 2.9F);
}

void a_brush_keeps_to_the_budgets() {
    Logs logs = smouldering_logs();
    logs.document.scene->physics.max_dropped = 20U;
    const Prepared ready = prepared(logs.document);
    Brush brush;
    brush.count = 10U;
    // The world bears at most SolidWorld::drops_per_step (16) a step: a
    // second dab of 10 at step 40 goes in step 41.
    const BrushDab first = brush_dab(logs.document, ready.targets, brush, logs.log, {}, {0.0F, 1.0F, 0.0F}, {}, 40U);
    const BrushDab second =
        brush_dab(logs.document, ready.targets, brush, logs.log, {}, {0.0F, 1.0F, 0.0F}, {}, 40U, first.stroke);
    CHECK(first.tick == 40U && second.tick == 41U && second.stroke == first.stroke);
    // The scene holds at most 20 painted things: a third dab is refused.
    const BrushDab third = brush_dab(logs.document, ready.targets, brush, logs.log, {}, {0.0F, 1.0F, 0.0F}, {}, 50U);
    CHECK(third.serials.empty() && third.refusal.find("at most 20") != std::string::npos);
    CHECK(logs.document.scene->performance.acts.size() == 2U);
    // More in one dab than a step bears is refused outright.
    brush.count = 17U;
    CHECK(!brush_dab(logs.document, ready.targets, brush, logs.log, {}, {0.0F, 1.0F, 0.0F}, {}, 60U).refusal.empty());
    // As is painting what cannot be dropped: the ground.
    brush.count = 1U;
    const Uid ground = logs.document.scene->items.front().uid;
    CHECK(!brush_dab(logs.document, ready.targets, brush, ground, {}, {0.0F, 1.0F, 0.0F}, {}, 60U).refusal.empty());
    // Both dabs replay, nothing refused by the world.
    auto solver = ajfx::rigid::make_jolt_solver();
    const Played played = play(logs.document, ready, *solver, 45U);
    CHECK(played.simulation->emitter(*played.world).refused == 0U);
    CHECK(world_bodies(played, ready).size() == 20U);
}

// ------------------------------------------------------------ grabbing

// A hand carrying the held body to `aim` at `tick`, as the pointer and wheel
// do: the plane taken to the aim's depth, the ray from the camera through it.
bool carry_to(Document& document, Hold& hold, ajfx::Vec3 camera, ajfx::Vec3 aim, std::uint64_t tick) {
    push_deeper(hold, ajfx::dot(aim - hold.plane.point, hold.plane.normal));
    const ajfx::Vec3 ray = aim - camera;
    return carry(document, hold, camera, ray * (1.0F / length(ray)), tick);
}

void a_ball_grabbed_and_flung_breaks_the_wall() {
    Wall scene = wall_and_ball();
    const Prepared ready = prepared(scene.document);
    const ActTargets& targets = ready.targets;
    auto solver = ajfx::rigid::make_jolt_solver();
    const BodyRef ball{.object = scene.ball, .count = 0U};
    const Played before = play(scene.document, ready, *solver, 30U);
    const std::optional<WorldBody> resting = find_body(before, ready, ball);
    CHECK(resting.has_value());
    const ajfx::Vec3 camera{0.0F, 3.0F, 10.0F};
    // Pressed on the top of the ball.
    const StageHit hit{.point = resting->position + ajfx::Vec3{0.0F, 0.2F, 0.0F},
                       .center = resting->position,
                       .item = 0U,
                       .piece = false,
                       .on_body = true,
                       .spawn_index = resting->spawn_index};
    GrabStart start = begin_grab(scene.document, targets, hit, resting->orientation, resting->position - camera, 30U);
    CHECK(start.refusal.empty() && start.hold.serial != 0U);
    // Its middle is held a fifth of a metre below the pointer.
    CHECK(near(start.hold.offset.y, -0.2F, 1.0e-5F));
    // Lifted to a metre and a half over two thirds of a second, held there
    // still, then swung
    // hard at the wall and let go.
    std::uint64_t tick = 31U;
    const ajfx::Vec3 lifted{0.0F, 1.5F, 1.5F};
    const ajfx::Vec3 pointer_at_start = hit.point;
    for (; tick <= 90U; ++tick) {
        const float t = std::min(1.0F, static_cast<float>(tick - 30U) / 40.0F);
        CHECK(carry_to(scene.document, start.hold, camera,
                       pointer_at_start + (lifted - pointer_at_start) * t, tick));
    }
    const ajfx::Vec3 swung{0.0F, 1.0F, -0.4F};
    for (; tick <= 100U; ++tick) {
        const float t = static_cast<float>(tick - 90U) / 10.0F;
        CHECK(carry_to(scene.document, start.hold, camera, lifted + (swung - lifted) * t, tick));
    }
    const Act* grab = find_act(scene.document.scene->performance, start.hold.serial);
    CHECK(grab->kind == ActKind::Grab && grab->tick == 30U && act_end(*grab) == 100U && grab->path.size() == 71U);
    // Every sample is where the pointer was less the grip.
    CHECK(near(grab->path[60].point.y, lifted.y - 0.2F, 1.0e-4F) && near(grab->path[60].point.z, 1.5F, 1.0e-4F));

    // Held up, it hangs where the hand holds it.
    const Played held = play(scene.document, ready, *solver, 88U);
    const WorldBody up = *find_body(held, ready, ball);
    CHECK(length(up.position - (lifted - ajfx::Vec3{0.0F, 0.2F, 0.0F})) < 0.15F);
    // Let go, it flies on at the wall as fast as it was swung.
    const Played thrown = play(scene.document, ready, *solver, 101U);
    const WorldBody flying = *find_body(thrown, ready, ball);
    CHECK(flying.velocity.z < -6.0F);
    CHECK(pieces(thrown, ready) == 0U);
    // And the wall breaks.
    const Played after = play(scene.document, ready, *solver, 160U);
    CHECK(pieces(after, ready) > 0U);
    CHECK(!find_body(after, ready, BodyRef{.object = scene.wall, .count = 0U}));

    // Saved and opened again, it replays bit for bit.
    const std::uint64_t digest = state_digest(*after.simulation);
    const Document again = reopened(scene.document);
    CHECK(again.scene->performance == scene.document.scene->performance);
    CHECK(state_digest(*play(again, prepared(again), *solver, 160U).simulation) == digest);

    // On the ball's track: a bar over the steps it was held.
    const std::vector<Track> tracks = collect_tracks(*scene.document.scene, 6.0F);
    const auto track = std::find_if(tracks.begin(), tracks.end(), [&](const Track& t) { return t.item == scene.ball; });
    CHECK(track != tracks.end() && track->markers.size() == 1U);
    CHECK(track->markers[0].kind == MarkerKind::Grab && near(track->markers[0].duration, 70.0F / 60.0F));

    // Holding on a step longer replays only from the step after the last.
    Document longer = scene.document;
    CHECK(extend_grab(longer.scene->performance, start.hold.serial, 101U, swung));
    CHECK(first_difference(scene.document.scene->performance, longer.scene->performance) ==
          std::optional<std::uint64_t>{101U});
    // A sample before the last is refused.
    CHECK(!extend_grab(longer.scene->performance, start.hold.serial, 50U, swung));

    // Re-recorded from just before the swing: the hand lets go up there and
    // the ball drops short of the wall, which stands.
    Document again_from = scene.document;
    CHECK(rerecord_from(again_from.scene->performance, 90U) == 1U);
    CHECK(act_end(again_from.scene->performance.acts.front()) == 89U);
    const Played dropped = play(again_from, ready, *solver, 160U);
    CHECK(pieces(dropped, ready) == 0U);
    CHECK(find_body(dropped, ready, BodyRef{.object = scene.wall, .count = 0U}).has_value());
    // Before that step it played as it did.
    CHECK(state_digest(*play(again_from, ready, *solver, 90U).simulation) ==
          state_digest(*play(scene.document, ready, *solver, 90U).simulation));
}

void a_hand_carries_across_a_plane_facing_the_camera() {
    const GrabPlane plane{{0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, -1.0F}};
    // Straight through, and at a slant.
    const std::optional<ajfx::Vec3> ahead = on_plane(plane, {0.0F, 1.0F, 5.0F}, {0.0F, 0.0F, -1.0F});
    CHECK(ahead && near(ahead->z, 0.0F, 1.0e-6F) && near(ahead->y, 1.0F, 1.0e-6F));
    const ajfx::Vec3 slant = ajfx::Vec3{1.0F, 0.0F, -1.0F} * (1.0F / std::sqrt(2.0F));
    const std::optional<ajfx::Vec3> side = on_plane(plane, {0.0F, 1.0F, 5.0F}, slant);
    CHECK(side && near(side->x, 5.0F, 1.0e-4F) && near(side->z, 0.0F, 1.0e-4F));
    // Along the plane or away from it: nowhere.
    CHECK(!on_plane(plane, {0.0F, 1.0F, 5.0F}, {1.0F, 0.0F, 0.0F}));
    CHECK(!on_plane(plane, {0.0F, 1.0F, 5.0F}, {0.0F, 0.0F, 1.0F}));
    // The wheel takes it further off.
    Hold hold{.serial = 0U, .plane = plane, .offset = {}};
    push_deeper(hold, 2.0F);
    CHECK(near(hold.plane.point.z, -2.0F) && near(hold.plane.point.y, 1.0F));
    // A hand on nothing is refused, and acts on nothing.
    Wall scene = wall_and_ball();
    const Prepared ready = prepared(scene.document);
    const StageHit floor{.point = {}, .center = {}, .item = 0U, .piece = false, .on_body = false};
    CHECK(!begin_grab(scene.document, ready.targets, floor, {}, {0.0F, 0.0F, -1.0F}, 10U).refusal.empty());
    CHECK(!carry(scene.document, hold, {0.0F, 1.0F, 5.0F}, {0.0F, 0.0F, -1.0F}, 11U));
    CHECK(scene.document.scene->performance.acts.empty());
}

// ------------------------------------------------------------ poke and blast

void a_poke_pushes_and_spins_and_a_blast_breaks() {
    Wall scene = wall_and_ball();
    const Prepared ready = prepared(scene.document);
    const ActTargets& targets = ready.targets;
    auto solver = ajfx::rigid::make_jolt_solver();
    const BodyRef ball{.object = scene.ball, .count = 0U};
    const WorldBody resting = *find_body(play(scene.document, ready, *solver, 30U), ready, ball);
    // Poked at its top towards +x: it goes +x and, as a lever on its
    // middle, spins about -z.
    const float r = resting.size * 0.5F;
    const StageHit top{.point = resting.position + ajfx::Vec3{0.0F, r, 0.0F},
                       .center = resting.position,
                       .item = 0U,
                       .piece = false,
                       .on_body = true,
                       .spawn_index = resting.spawn_index};
    const StageAct poked = poke(scene.document, targets, top, {1.0F, 0.0F, 0.0F}, 4.0F, r, 30U);
    CHECK(poked.serial != 0U && poked.refusal.empty());
    const Act& push = *find_act(scene.document.scene->performance, poked.serial);
    CHECK(near(push.push.x, 4.0F) && near(push.twist.z, -4.0F * r / (0.4F * r * r), 1.0e-3F));
    const WorldBody moving = *find_body(play(scene.document, ready, *solver, 31U), ready, ball);
    CHECK(moving.velocity.x > 3.0F);
    // Poked on nothing: refused.
    StageHit air = top;
    air.on_body = false;
    CHECK(poke(scene.document, targets, air, {1.0F, 0.0F, 0.0F}, 4.0F, r, 30U).serial == 0U);

    // A blast in front of the wall, strong enough there to break it.
    Document weak = scene.document;
    const StageAct blasted = set_off_blast(scene.document, targets, {0.0F, 0.2F, -1.0F}, 3.0F, 12.0F, 60U);
    CHECK(blasted.serial != 0U);
    CHECK(pieces(play(scene.document, ready, *solver, 60U), ready) == 0U);
    const Played broken = play(scene.document, ready, *solver, 61U);
    CHECK(pieces(broken, ready) == 8U);
    // Its pieces are thrown back from it.
    for (const WorldBody& body : world_bodies(broken, ready)) {
        if (!body.ref) {
            CHECK(body.velocity.z < 0.0F);
        }
    }
    // Too weak there (2 m/s at the wall's middle against 4), it only
    // shoves the wall.
    CHECK(set_off_blast(weak, targets, {0.0F, 0.2F, -1.0F}, 3.0F, 2.0F, 60U).serial != 0U);
    const Played shoved = play(weak, ready, *solver, 61U);
    CHECK(pieces(shoved, ready) == 0U);
    CHECK(find_body(shoved, ready, BodyRef{.object = scene.wall, .count = 0U})->velocity.z < -0.05F);
    // A blast needs a reach and a strength.
    CHECK(!set_off_blast(weak, targets, {}, 0.0F, 2.0F, 60U).refusal.empty());

    // On the timeline, on a track of its own, and moved from there.
    const std::vector<Track> tracks = collect_tracks(*scene.document.scene, 6.0F);
    CHECK(tracks.back().item == 0U && tracks.back().name == blasts_track && tracks.back().markers.size() == 1U);
    const TrackMarker marker = tracks.back().markers.front();
    CHECK(marker.kind == MarkerKind::Blasted && near(marker.seconds, 1.0F));
    CHECK(set_marker_time(scene.document, marker, 2.0F));
    CHECK(find_act(scene.document.scene->performance, blasted.serial)->tick == 120U);
    CHECK(pieces(play(scene.document, ready, *solver, 61U), ready) == 0U);
}

// ------------------------------------------------------------ saving

void every_hand_is_saved_strictly() {
    Wall scene = wall_and_ball();
    Performance& performance = scene.document.scene->performance;
    // Paint needs a kind that can be dropped: the ball will do.
    const std::uint32_t paint = add_paint(performance, PaintDab{.tick = 5U,
                                                                .object = scene.ball,
                                                                .position = {0.25F, 1.5F, -0.125F},
                                                                .normal = {0.0F, 0.0F, 2.0F},
                                                                .turn = {0.0F, 30.0F, 0.0F},
                                                                .velocity = {0.0F, -1.0F, 0.0F},
                                                                .radius = 0.75F,
                                                                .count = 3U,
                                                                .falloff = 0.5F,
                                                                .paint_turn = PaintTurn::Random,
                                                                .stroke = 0U});
    CHECK(paint == 1U && performance.next_serial == 4U);
    // Its way out is kept unit length.
    CHECK(performance.acts[0].normal == ajfx::Vec3{0.0F, 0.0F, 1.0F} && performance.acts[0].stroke == 1U);
    const std::uint32_t push = add_push(performance, 10U, BodyRef{.drop = 2U}, {1.0F, 0.0F, 0.5F}, {0.0F, 3.0F, 0.0F});
    const std::uint32_t grab = add_grab(performance, 20U, BodyRef{.object = scene.ball, .count = 0U},
                                        {0.0F, 1.0F, 0.0F}, {0.0F, 15.0F, 0.0F}, 7.5F, 9.0F);
    CHECK(extend_grab(performance, grab, 24U, {0.5F, 1.25F, 0.0F}));
    CHECK(extend_grab(performance, grab, 24U, {0.5F, 1.5F, 0.0F})); // the same step: replaced
    CHECK(performance.acts.back().path.size() == 2U && performance.acts.back().path[1].point.y == 1.5F);
    const std::uint32_t blast = add_blast(performance, 40U, {0.0F, 0.0F, -1.0F}, 2.5F, 9.0F);
    add_delete(performance, 50U, BodyRef{.drop = 3U});
    CHECK(push == 4U && grab == 5U && blast == 6U && performance.next_serial == 8U);
    CHECK(validate_scene(scene.document, false).empty());
    CHECK(performance_summary(performance) == "1 paint stroke, 1 grab, 1 push, 1 blast, 1 deletion");

    const std::string saved = save_document(scene.document);
    const Loaded loaded = load_document(saved);
    CHECK(loaded.document.has_value() && loaded.document->scene->performance == performance);
    CHECK(save_document(*loaded.document) == saved);
    // The same actions come of it, so the same replay.
    const Prepared ready = prepared(scene.document);
    const auto a = performance_actions(performance, ready.targets);
    const auto b = performance_actions(loaded.document->scene->performance, ready.targets);
    CHECK(a->digest(ready.targets.emitter) == b->digest(ready.targets.emitter));
    // A grab steers at every step it holds: 20 to 24, along its path.
    std::size_t steers = 0U;
    for (const ajfx::Action& action : a->actions(ready.targets.emitter)) {
        steers += action.kind == ajfx::ActionKind::Steer ? 1U : 0U;
    }
    CHECK(steers == 5U);
    const std::span<const ajfx::Action> at_22 = a->actions_at(ready.targets.emitter, 22U);
    CHECK(at_22.size() == 1U && near(at_22[0].position.x, 0.25F) && near(at_22[0].position.y, 1.25F));
    CHECK(near(at_22[0].gain, 7.5F) && near(at_22[0].max_speed, 9.0F));

    // Strict: a key it does not know, an orientation it does not know.
    std::string unknown = saved;
    unknown.replace(unknown.find("\"falloff\""), 9U, "\"falloffX\"");
    CHECK(!load_document(unknown).document);
    std::string turned = saved;
    turned.replace(turned.find("\"random\""), 8U, "\"sideways\"");
    CHECK(!load_document(turned).document);
    // A grab that goes back in time, a paint of too many, serials that
    // overlap: refused.
    Document backwards = scene.document;
    backwards.scene->performance.acts[2].path[1].tick = 19U;
    CHECK(!validate_scene(backwards, false).empty());
    Document crowded = scene.document;
    crowded.scene->performance.acts[0].count = SolidWorld::drops_per_step + 1U;
    CHECK(!validate_scene(crowded, false).empty());
    Document overlapping = scene.document;
    overlapping.scene->performance.acts[1].serial = 2U; // inside the paint's 1..3
    CHECK(!validate_scene(overlapping, false).empty());

    // Taking the paint away takes the push and the deletion of what it made.
    CHECK(remove_act(performance, paint));
    CHECK(performance.acts.size() == 2U && performance.acts[0].kind == ActKind::Grab);
    // Removing the ball's Object takes the grab on it.
    CHECK(remove_item(scene.document, scene.ball));
    CHECK(performance.acts.size() == 1U && performance.acts[0].kind == ActKind::Blast);
}

void re_recording_keeps_what_came_before() {
    Wall scene = wall_and_ball();
    Performance& performance = scene.document.scene->performance;
    const std::uint32_t early = add_drop(performance, 10U, scene.ball, {1.0F, 2.0F, 0.0F}, {}, {});
    const std::uint32_t grab = add_grab(performance, 20U, BodyRef{.drop = early}, {1.0F, 1.0F, 0.0F}, {});
    for (std::uint64_t tick = 21U; tick <= 60U; ++tick) {
        CHECK(extend_grab(performance, grab, tick, {1.0F, 1.0F + 0.01F * static_cast<float>(tick), 0.0F}));
    }
    const std::uint32_t late = add_drop(performance, 50U, scene.ball, {-1.0F, 2.0F, 0.0F}, {}, {});
    add_push(performance, 55U, BodyRef{.drop = late}, {0.0F, 2.0F, 0.0F}, {});
    const Performance before = performance;
    // From step 40: the late drop and its push go, the grab lets go at 39.
    CHECK(rerecord_from(performance, 40U) == 3U);
    CHECK(performance.acts.size() == 2U && act_end(performance.acts[1]) == 39U);
    CHECK(performance.next_serial == before.next_serial);
    // The grab is the first act that changed: a simulation goes back to it.
    CHECK(first_difference(before, performance) == std::optional<std::uint64_t>{20U});
    // A new act after it takes a new serial: none is ever reused.
    CHECK(add_blast(performance, 45U, {}, 1.0F, 1.0F) == before.next_serial);
    // From the first step everything goes.
    CHECK(rerecord_from(performance, 0U) == 3U && performance.acts.empty());
}

} // namespace

void add_hands_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"a_painted_stroke_lays_logs_that_smoulder_and_replay_exactly",
                     a_painted_stroke_lays_logs_that_smoulder_and_replay_exactly});
    cases.push_back({"a_brush_erases_and_pushes_what_is_under_it", a_brush_erases_and_pushes_what_is_under_it});
    cases.push_back({"a_brush_keeps_to_the_budgets", a_brush_keeps_to_the_budgets});
    cases.push_back({"a_ball_grabbed_and_flung_breaks_the_wall", a_ball_grabbed_and_flung_breaks_the_wall});
    cases.push_back({"a_hand_carries_across_a_plane_facing_the_camera", a_hand_carries_across_a_plane_facing_the_camera});
    cases.push_back({"a_poke_pushes_and_spins_and_a_blast_breaks", a_poke_pushes_and_spins_and_a_blast_breaks});
    cases.push_back({"every_hand_is_saved_strictly", every_hand_is_saved_strictly});
    cases.push_back({"re_recording_keeps_what_came_before", re_recording_keeps_what_came_before});
}
