#include "performance.hpp"
#include "test_support.hpp"

// A scene's performance: drops and deletions done while it plays, kept with
// the scene, replayed exactly by the simulation, and never a new package.
namespace {
using namespace studio_test;

Object& object(Document& document, Uid uid) { return std::get<Object>(document.scene->item(uid)->data); }

Compiled ready(const Document& document) {
    Compiled compiled = compile(document);
    if (!compiled.error.empty()) {
        throw std::runtime_error{compiled.error};
    }
    CHECK(compiled.effect);
    std::string error;
    make_cuts(compiled, error);
    if (!error.empty()) {
        throw std::runtime_error{error};
    }
    return compiled;
}

struct Stage final {
    Document document = new_scene();
    Uid cube{};
};

// The Ground, and a loose cube standing on it.
Stage cube_on_the_ground() {
    Stage stage;
    stage.cube = add_object(stage.document);
    object(stage.document, stage.cube).position = {0.0F, 0.5F, 0.0F};
    return stage;
}

ajfx::Simulation played(const Compiled& compiled, const ajfx::RigidSolver& solver, const ajfx::ActionList& actions,
                        std::uint64_t to) {
    auto created = ajfx::Simulation::create(*compiled.effect, 71U,
                                            ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = &solver});
    if (!created) {
        throw std::runtime_error{created.error().message};
    }
    ajfx::Simulation simulation = std::move(created).value();
    simulation.reset(nullptr, &actions);
    simulation.advance_to(to, nullptr, &actions);
    return simulation;
}

std::optional<std::size_t> find_body(const ajfx::EmitterState& state, std::uint32_t spawn_index) {
    for (std::size_t i = 0U; i < state.alive(); ++i) {
        if (state.spawn_index[i] == spawn_index) {
            return i;
        }
    }
    return std::nullopt;
}

void dropped_copies_join_the_world_and_replay_exactly() {
    Stage stage = cube_on_the_ground();
    const Compiled compiled = ready(stage.document);
    const ActTargets targets = act_targets(compiled);
    CHECK(targets.drops.has_value() && targets.max_dropped == 64U);
    CHECK(droppable(targets, stage.cube));
    // The Ground stands still: it is no body, so nothing drops it.
    CHECK(!droppable(targets, stage.document.scene->items.front().uid));

    Performance& performance = stage.document.scene->performance;
    const std::uint32_t first = add_drop(performance, 30U, stage.cube, {-1.0F, 2.0F, 0.0F}, {}, {});
    const std::uint32_t second = add_drop(performance, 30U, stage.cube, {1.0F, 2.0F, 0.0F}, {0.0F, 45.0F, 0.0F}, {});
    const std::uint32_t third = add_drop(performance, 90U, stage.cube, {0.0F, 3.0F, 0.0F}, {}, {0.0F, -2.0F, 0.0F});
    CHECK(first == 1U && second == 2U && third == 3U && performance.next_serial == 4U);

    std::size_t skipped = 99U;
    const std::shared_ptr<const ajfx::ActionList> actions = performance_actions(performance, targets, &skipped);
    CHECK(skipped == 0U && actions->size() == 3U);
    const std::span<const ajfx::Action> at_30 = actions->actions_at(targets.emitter, 30U);
    CHECK(at_30.size() == 2U && at_30[0].serial == first && at_30[1].serial == second);
    CHECK(at_30[0].kind == ajfx::ActionKind::Spawn && at_30[0].body == targets.kinds.at(stage.cube));
    CHECK(at_30[1].orientation == ajfx::prop_orientation(ajfx::Vec3{0.0F, 45.0F, 0.0F}));

    auto solver = ajfx::rigid::make_jolt_solver();
    const ajfx::Simulation before = played(compiled, *solver, *actions, 30U);
    CHECK(before.emitter(0).alive() == 1U);
    const ajfx::Simulation run = played(compiled, *solver, *actions, 240U);
    CHECK(run.emitter(0).alive() == 4U);
    const std::optional<std::size_t> landed = find_body(run.emitter(0), *body_index(targets, BodyRef{.drop = third}));
    CHECK(landed.has_value());
    // It fell from 3 m and came to rest on the others or the floor.
    CHECK(run.emitter(0).position[*landed].y < 2.5F && std::fabs(run.emitter(0).velocity[*landed].y) < 0.5F);
    const ajfx::Simulation rerun = played(compiled, *solver, *actions, 240U);
    CHECK(rerun.emitter(0).position == run.emitter(0).position);
    CHECK(rerun.emitter(0).orientation == run.emitter(0).orientation);
}

void taking_a_drop_away_leaves_the_others_as_they_were() {
    Stage stage = cube_on_the_ground();
    const Compiled compiled = ready(stage.document);
    const ActTargets targets = act_targets(compiled);
    Performance& performance = stage.document.scene->performance;
    // Far apart, so none meets another.
    const std::uint32_t left = add_drop(performance, 20U, stage.cube, {-6.0F, 1.0F, 0.0F}, {}, {});
    const std::uint32_t right = add_drop(performance, 40U, stage.cube, {6.0F, 1.0F, 0.0F}, {}, {});
    auto solver = ajfx::rigid::make_jolt_solver();
    const ajfx::Simulation both = played(compiled, *solver, *performance_actions(performance, targets), 120U);

    Performance fewer = performance;
    CHECK(remove_act(fewer, left));
    CHECK(fewer.next_serial == performance.next_serial);
    CHECK(first_difference(performance, fewer) == std::optional<std::uint64_t>{20U});
    const ajfx::Simulation one = played(compiled, *solver, *performance_actions(fewer, targets), 120U);
    CHECK(one.emitter(0).alive() == both.emitter(0).alive() - 1U);
    // The right one keeps its spawn index, so its every random draw.
    const std::uint32_t index = *body_index(targets, BodyRef{.drop = right});
    const std::optional<std::size_t> a = find_body(both.emitter(0), index);
    const std::optional<std::size_t> b = find_body(one.emitter(0), index);
    CHECK(a && b);
    CHECK(both.emitter(0).position[*a] == one.emitter(0).position[*b]);
    CHECK(both.emitter(0).size[*a] == one.emitter(0).size[*b]);
    CHECK(both.emitter(0).color[*a] == one.emitter(0).color[*b]);
}

void deleted_bodies_leave_in_their_step() {
    Stage stage = cube_on_the_ground();
    const Compiled compiled = ready(stage.document);
    const ActTargets targets = act_targets(compiled);
    Performance& performance = stage.document.scene->performance;
    const std::uint32_t dropped = add_drop(performance, 10U, stage.cube, {3.0F, 1.0F, 0.0F}, {}, {});
    add_delete(performance, 60U, BodyRef{.object = stage.cube, .count = 0U});
    add_delete(performance, 90U, BodyRef{.drop = dropped});
    // A body named by where it came from, and back.
    const std::uint32_t own = *body_index(targets, BodyRef{.object = stage.cube, .count = 0U});
    CHECK(body_ref(targets, own) == std::optional<BodyRef>{BodyRef{.object = stage.cube, .count = 0U}});
    CHECK(body_ref(targets, *body_index(targets, BodyRef{.drop = dropped})) ==
          std::optional<BodyRef>{BodyRef{.drop = dropped}});
    CHECK(!body_ref(targets, ajfx::spawn_index_of(ajfx::piece_spawn_source, 0U)));

    auto solver = ajfx::rigid::make_jolt_solver();
    const std::shared_ptr<const ajfx::ActionList> actions = performance_actions(performance, targets);
    CHECK(played(compiled, *solver, *actions, 60U).emitter(0).alive() == 2U);
    const ajfx::Simulation after_first = played(compiled, *solver, *actions, 61U);
    CHECK(after_first.emitter(0).alive() == 1U && !find_body(after_first.emitter(0), own));
    CHECK(played(compiled, *solver, *actions, 91U).emitter(0).alive() == 0U);

    // A drop taken away takes its deletion with it.
    CHECK(remove_act(performance, dropped));
    CHECK(performance.acts.size() == 1U && performance.acts.front().kind == ActKind::Delete);
}

void acting_changes_no_package_and_no_drop_past_the_budget() {
    Stage stage = cube_on_the_ground();
    stage.document.scene->physics.max_dropped = 2U;
    const Compiled plain = ready(stage.document);
    const ActTargets targets = act_targets(plain);
    CHECK(targets.max_dropped == 2U);
    StageHit floor{.point = {0.5F, 0.0F, -0.5F}, .center = {}, .item = 0U, .piece = false, .on_body = false};
    const StageAct made = act_on_stage(stage.document, targets, StageTool::Drop, stage.cube, floor, 50U);
    CHECK(made.serial == 1U && made.refusal.empty());
    const Act& act = stage.document.scene->performance.acts.front();
    // Its bottom is a little above the floor where the click landed.
    CHECK(act.tick == 50U && act.object == stage.cube);
    CHECK(near(act.position.x, 0.5F) && near(act.position.z, -0.5F));
    CHECK(near(act.position.y, drop_height + object(stage.document, stage.cube).size * 0.5F));
    CHECK(act_on_stage(stage.document, targets, StageTool::Drop, stage.cube, floor, 51U).serial == 2U);
    const StageAct refused = act_on_stage(stage.document, targets, StageTool::Drop, stage.cube, floor, 52U);
    CHECK(refused.serial == 0U && refused.refusal.find("at most 2") != std::string::npos);
    CHECK(stage.document.scene->performance.acts.size() == 2U);
    // Nothing selected, or something that is no loose body: nothing is done.
    CHECK(!act_on_stage(stage.document, targets, StageTool::Drop, 0U, floor, 52U).refusal.empty());
    CHECK(!act_on_stage(stage.document, targets, StageTool::Delete, 0U, floor, 52U).refusal.empty());
    CHECK(stage.document.scene->performance.acts.size() == 2U);

    // The package is what it was: the performance is no part of it.
    const Compiled acted = ready(stage.document);
    CHECK(acted.effect == plain.effect);
    Document bare = stage.document;
    bare.scene->performance = Performance{};
    CHECK(without_performance(stage.document) == bare);
    // A scene that takes no drops has nowhere for them.
    stage.document.scene->physics.max_dropped = 0U;
    const Compiled none = ready(stage.document);
    CHECK(!none.solids.drops && !droppable(act_targets(none), stage.cube));
    CHECK(act_on_stage(stage.document, act_targets(none), StageTool::Drop, stage.cube, floor, 60U).serial == 0U);

    // Paused, an act is at the step shown; playing, just past what the
    // preview's physics may have worked out ahead.
    CHECK(act_tick(false, 120U, 2U) == 120U);
    CHECK(act_tick(true, 120U, 2U) == 123U);
}

void the_performance_is_saved_and_shown_on_the_timeline() {
    Stage stage = cube_on_the_ground();
    Performance& performance = stage.document.scene->performance;
    const std::uint32_t drop = add_drop(performance, 75U, stage.cube, {0.25F, 1.5F, -0.125F}, {0.0F, 30.0F, 0.0F},
                                        {0.5F, 0.0F, 0.0F});
    add_delete(performance, 150U, BodyRef{.drop = drop});
    add_delete(performance, 140U, BodyRef{.object = stage.cube, .count = 0U});
    stage.document.scene->physics.max_dropped = 12U;
    CHECK(performance.acts[1].tick == 140U && performance.acts[2].tick == 150U);

    const std::string saved = save_document(stage.document);
    const Loaded loaded = load_document(saved);
    CHECK(loaded.document.has_value());
    CHECK(loaded.document->scene->performance == performance);
    CHECK(loaded.document->scene->physics.max_dropped == 12U);
    CHECK(save_document(*loaded.document) == saved);
    // Strict, as the rest of a scene is.
    std::string unknown = saved;
    unknown.replace(unknown.find("\"velocity\""), 10U, "\"velocityX\"");
    CHECK(!load_document(unknown).document);
    // Acts out of order, or with a serial not yet handed out, are refused.
    Document disordered = stage.document;
    std::swap(disordered.scene->performance.acts[0], disordered.scene->performance.acts[2]);
    CHECK(!validate_scene(disordered, false).empty());
    CHECK(!load_document(save_document(disordered)).document);
    Document unserialed = stage.document;
    unserialed.scene->performance.next_serial = 2U;
    CHECK(!validate_scene(unserialed, false).empty());

    // On the cube's track: its drop and both deletions, at their times.
    const std::vector<Track> tracks = collect_tracks(*stage.document.scene, 6.0F);
    const auto cube = std::find_if(tracks.begin(), tracks.end(), [&](const Track& t) { return t.item == stage.cube; });
    CHECK(cube != tracks.end() && cube->markers.size() == 3U);
    CHECK(cube->markers[0].kind == MarkerKind::Drop && near(cube->markers[0].seconds, 1.25F));
    CHECK(cube->markers[0].part == drop);
    // Dragging the drop's marker moves the drop, keeping tick order.
    CHECK(set_marker_time(stage.document, cube->markers[0], 2.5F));
    CHECK(stage.document.scene->performance.acts[0].kind == ActKind::Delete);
    CHECK(find_act(stage.document.scene->performance, drop)->tick == 150U);
    CHECK(stage.document.scene->performance.acts.back().serial == drop);

    // Removing the cube removes what was done to it.
    CHECK(remove_item(stage.document, stage.cube));
    CHECK(stage.document.scene->performance.acts.empty());
}

void a_bake_holds_the_performance_it_was_taken_with() {
    Stage stage = cube_on_the_ground();
    stage.document.loop_seconds = 2.0F;
    add_drop(stage.document.scene->performance, 20U, stage.cube, {0.0F, 2.5F, 0.0F}, {}, {});
    const Compiled compiled = ready(stage.document);
    auto solver = ajfx::rigid::make_jolt_solver();
    const ajfx::Result<BakeRecord> baked = bake_scene(stage.document, compiled, 71U, solver.get(), {});
    CHECK(baked.ok());
    const BakeRecord& record = baked.value();
    CHECK(bake_status(stage.document, compiled, &record, nullptr).current);

    // The cache opens with the performance it was baked with, and only then.
    const std::shared_ptr<const ajfx::ActionList> actions =
        performance_actions(stage.document.scene->performance, act_targets(compiled));
    CHECK(ajfx::Cache::open(record.bytes, *compiled.effect, 71U, {}, compiled.meshes, {}, actions.get()).ok());
    CHECK(!ajfx::Cache::open(record.bytes, *compiled.effect, 71U, {}, compiled.meshes).ok());

    // Another drop: the bake no longer holds the scene.
    Document more = stage.document;
    add_drop(more.scene->performance, 40U, stage.cube, {0.5F, 2.5F, 0.0F}, {}, {});
    const BakeStatus stale = bake_status(more, compiled, &record, nullptr);
    CHECK(!stale.current);
    CHECK(std::any_of(stale.items.begin(), stale.items.end(), [](const ItemBake& layer) {
        return layer.state == BakeState::Stale && layer.why.find("dropped") != std::string::npos;
    }));
}

} // namespace

void add_performance_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"dropped_copies_join_the_world_and_replay_exactly", dropped_copies_join_the_world_and_replay_exactly});
    cases.push_back({"taking_a_drop_away_leaves_the_others_as_they_were", taking_a_drop_away_leaves_the_others_as_they_were});
    cases.push_back({"deleted_bodies_leave_in_their_step", deleted_bodies_leave_in_their_step});
    cases.push_back({"acting_changes_no_package_and_no_drop_past_the_budget", acting_changes_no_package_and_no_drop_past_the_budget});
    cases.push_back({"the_performance_is_saved_and_shown_on_the_timeline", the_performance_is_saved_and_shown_on_the_timeline});
    cases.push_back({"a_bake_holds_the_performance_it_was_taken_with", a_bake_holds_the_performance_it_was_taken_with});
}
