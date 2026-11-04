#include "stage.hpp"
#include "test_support.hpp"

// The stage: kinds that have no bodies of their own, things put down by hand,
// and the setup tools (jitter, settle, reset) that work on them. Expectations
// are independent of the code under test: a placed thing stands where it was
// put, one dropped from a height comes to rest on the floor at its half
// height, a jitter stays within its bounds and repeats exactly, and a reset
// returns what was put.
namespace {
using namespace studio_test;

Object& object(Document& document, Uid uid) { return std::get<Object>(document.scene->item(uid)->data); }
const Object& object(const Document& document, Uid uid) { return std::get<Object>(document.scene->item(uid)->data); }

Compiled ready(const Document& document) {
    Compiled compiled = compile(document, Purpose::Export);
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

struct Scene3 final {
    Document document = new_scene();
    Uid crate{};
};

// The Ground, and a Crate kind that nothing stands as until it is placed.
Scene3 crate_kind() {
    Scene3 scene;
    scene.crate = add_object(scene.document);
    Object& crate = object(scene.document, scene.crate);
    crate.distribution = Distribution::Placed;
    crate.size = 1.0F;
    return scene;
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

std::size_t emitter_of(const Compiled& compiled, const ActTargets& targets) {
    for (std::size_t i = 0U; i < compiled.effect->emitters.size(); ++i) {
        if (compiled.effect->emitters[i].name == targets.emitter) {
            return i;
        }
    }
    throw std::runtime_error{"no world"};
}

std::optional<std::size_t> body_of(const ajfx::EmitterState& state, std::uint32_t spawn_index) {
    for (std::size_t i = 0U; i < state.alive(); ++i) {
        if (state.spawn_index[i] == spawn_index) {
            return i;
        }
    }
    return std::nullopt;
}

float length(ajfx::Vec3 v) { return std::sqrt(ajfx::dot(v, v)); }

void a_kind_has_no_bodies_until_it_is_placed() {
    Scene3 scene = crate_kind();
    const Compiled compiled = ready(scene.document);
    const ActTargets targets = act_targets(compiled);
    CHECK(droppable(targets, scene.crate));
    CHECK(targets.kinds.contains(scene.crate));

    auto solver = ajfx::rigid::make_jolt_solver();
    const std::size_t world = emitter_of(compiled, targets);
    // Nothing is placed: the world holds no body of the crate's.
    {
        const ajfx::ActionList none;
        ajfx::Simulation simulation = played(compiled, *solver, none, 5U);
        CHECK(simulation.emitter(world).alive() == 0U);
    }

    const StageAct first = place(scene.document, targets, scene.crate, {-2.0F, 3.0F, 0.0F}, {0.0F, 30.0F, 0.0F});
    const StageAct second = place(scene.document, targets, scene.crate, {2.0F, 4.0F, 1.0F}, {});
    CHECK(first.serial == 1U && second.serial == 2U && first.refusal.empty());
    CHECK(stage_count(scene.document.scene->performance) == 2U);
    // A placement is a drop at step 0, so the package does not change.
    CHECK(compile(without_performance(scene.document), Purpose::Export).effect->emitters[world].spawn.size() ==
          compiled.effect->emitters[world].spawn.size());

    const std::shared_ptr<const ajfx::ActionList> actions =
        performance_actions(scene.document.scene->performance, targets);
    ajfx::Simulation at_start = played(compiled, *solver, *actions, 0U);
    const ajfx::EmitterState& state = at_start.emitter(world);
    CHECK(state.alive() == 2U);
    const std::optional<std::size_t> a = body_of(state, ajfx::spawn_index_of(*targets.drops, first.serial));
    const std::optional<std::size_t> b = body_of(state, ajfx::spawn_index_of(*targets.drops, second.serial));
    CHECK(a && b);
    CHECK(near(state.position[*a].x, -2.0F) && near(state.position[*a].y, 3.0F));
    CHECK(near(state.position[*b].x, 2.0F) && near(state.position[*b].z, 1.0F));
    // The first was put turned 30 degrees about the vertical.
    const ajfx::Vec3 turned = turn_of(state.orientation[*a]);
    CHECK(near(turned.y, 30.0F, 0.01F) && near(turned.x, 0.0F, 0.01F) && near(turned.z, 0.0F, 0.01F));
}

void a_kind_that_stands_still_cannot_be_placed() {
    Scene3 scene = crate_kind();
    object(scene.document, scene.crate).motion = ObjectMotion::Static;
    // Compiles to an error that says so, rather than a silent nothing.
    const Compiled compiled = compile(scene.document, Purpose::Export);
    CHECK(!compiled.error.empty());
    CHECK(compiled.error.find("placed by hand") != std::string::npos);
}

void turns_survive_the_trip_through_a_pose() {
    const ajfx::Vec3 samples[] = {{0.0F, 0.0F, 0.0F},    {0.0F, 90.0F, 0.0F},   {30.0F, 0.0F, 0.0F},
                                  {0.0F, -45.0F, 20.0F}, {10.0F, 70.0F, -35.0F}, {-60.0F, 120.0F, 80.0F},
                                  {89.9F, 20.0F, 0.0F}};
    for (const ajfx::Vec3 turn : samples) {
        const ajfx::Quat q = ajfx::prop_orientation(turn);
        const ajfx::Quat back = ajfx::prop_orientation(turn_of(q));
        // The same rotation, whichever sign the quaternion has.
        const float same = std::abs(q.x * back.x + q.y * back.y + q.z * back.z + q.w * back.w);
        CHECK(same > 0.99999F);
    }
}

void jitter_is_bounded_exact_and_taken_back_by_reset() {
    Scene3 scene = crate_kind();
    const Compiled compiled = ready(scene.document);
    const ActTargets targets = act_targets(compiled);
    for (int i = 0; i < 4; ++i) {
        CHECK(place(scene.document, targets, scene.crate, {static_cast<float>(i), 0.5F, 0.0F}, {0.0F, 10.0F * static_cast<float>(i), 0.0F}).serial != 0U);
    }
    Performance& performance = scene.document.scene->performance;
    const Performance put = performance;

    CHECK(jitter_stage(performance, 7U, 0.1F, 5.0F) == 4U);
    bool moved = false;
    for (std::size_t i = 0U; i < performance.acts.size(); ++i) {
        const Act& was = put.acts[i];
        const Act& act = performance.acts[i];
        CHECK(act.touched && act.home == was.position && act.home_turn == was.turn);
        const ajfx::Vec3 d = act.position - was.position;
        CHECK(std::abs(d.x) <= 0.1F && std::abs(d.y) <= 0.1F && std::abs(d.z) <= 0.1F);
        moved = moved || length(d) > 0.0F;
        // Tilted by at most 5 degrees about each axis from how it stood.
        const ajfx::Quat a = ajfx::prop_orientation(was.turn);
        const ajfx::Quat b = ajfx::prop_orientation(act.turn);
        const float cosine = std::min(1.0F, std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w));
        CHECK(2.0F * std::acos(cosine) * 57.29578F < 5.0F * 1.7321F + 0.01F);
    }
    CHECK(moved);

    // The same seed does the same; another seed does not.
    Performance again = put;
    CHECK(jitter_stage(again, 7U, 0.1F, 5.0F) == 4U && again == performance);
    Performance other = put;
    jitter_stage(other, 8U, 0.1F, 5.0F);
    CHECK(!(other == performance));
    // One thing taken away leaves every other as it was jittered.
    Performance fewer = put;
    remove_act(fewer, 2U);
    jitter_stage(fewer, 7U, 0.1F, 5.0F);
    for (const Act& act : fewer.acts) {
        CHECK(act.position == find_act(performance, act.serial)->position);
    }

    // A second jitter keeps the first home, and reset returns to it exactly.
    jitter_stage(performance, 9U, 0.1F, 5.0F);
    CHECK(reset_stage(performance) == 4U);
    CHECK(performance == put);
    CHECK(reset_stage(performance) == 0U);
    // Putting one somewhere by hand makes that its home.
    jitter_stage(performance, 7U, 0.1F, 5.0F);
    CHECK(move_placement(performance, 3U, {5.0F, 0.5F, 5.0F}, {}));
    CHECK(reset_stage(performance) == 3U);
    CHECK(find_act(performance, 3U)->position == ajfx::Vec3(5.0F, 0.5F, 5.0F));
    // Only things on the stage can be moved.
    add_drop(performance, 30U, scene.crate, {}, {}, {});
    CHECK(!move_placement(performance, performance.next_serial - 1U, {}, {}));
}

void settling_keeps_where_things_came_to_rest() {
    Scene3 scene = crate_kind();
    const Compiled compiled = ready(scene.document);
    const ActTargets targets = act_targets(compiled);
    // Dropped from three metres up, turned a little: the Ground is at 0.
    const StageAct put = place(scene.document, targets, scene.crate, {0.0F, 3.0F, 0.0F}, {0.0F, 20.0F, 0.0F});
    CHECK(put.serial != 0U);

    auto solver = ajfx::rigid::make_jolt_solver();
    const SettleResult result = settle_stage(scene.document, compiled, solver.get(), SettleOptions{.instance_seed = 71U});
    CHECK(result.refusal.empty());
    CHECK(result.settled == 1U && result.moving == 0U);
    CHECK(result.steps > 30U);

    const Act& act = *find_act(scene.document.scene->performance, put.serial);
    CHECK(act.touched && near(act.home.y, 3.0F) && near(act.home_turn.y, 20.0F));
    // A one metre crate on a floor at 0 rests with its middle at half a metre.
    CHECK(near(act.position.y, 0.5F, 0.03F));
    CHECK(near(act.position.x, 0.0F, 0.05F) && near(act.position.z, 0.0F, 0.05F));
    // It landed flat: pitch and roll are nothing to speak of.
    CHECK(std::abs(act.turn.x) < 1.0F && std::abs(act.turn.z) < 1.0F);

    // Played from the settled stage, it stands there: nothing falls.
    const std::shared_ptr<const ajfx::ActionList> actions =
        performance_actions(scene.document.scene->performance, targets);
    const std::size_t world = emitter_of(compiled, targets);
    const std::uint32_t index = ajfx::spawn_index_of(*targets.drops, put.serial);
    ajfx::Simulation start = played(compiled, *solver, *actions, 0U);
    ajfx::Simulation later = played(compiled, *solver, *actions, 90U);
    const std::optional<std::size_t> s = body_of(start.emitter(world), index);
    const std::optional<std::size_t> l = body_of(later.emitter(world), index);
    CHECK(s && l);
    CHECK(length(start.emitter(world).position[*s] - later.emitter(world).position[*l]) < 0.02F);

    // And reset puts it back where it was put.
    CHECK(reset_stage(scene.document.scene->performance) == 1U);
    CHECK(near(find_act(scene.document.scene->performance, put.serial)->position.y, 3.0F));
}

void settling_leaves_later_acts_and_refuses_an_empty_stage() {
    Scene3 scene = crate_kind();
    const Compiled compiled = ready(scene.document);
    const ActTargets targets = act_targets(compiled);
    auto solver = ajfx::rigid::make_jolt_solver();
    CHECK(!settle_stage(scene.document, compiled, solver.get()).refusal.empty());

    CHECK(place(scene.document, targets, scene.crate, {0.0F, 2.0F, 0.0F}, {}).serial == 1U);
    // A drop part way through the performance is not the stage: it is not
    // moved, and does not push the stage about while it settles.
    const std::uint32_t later = add_drop(scene.document.scene->performance, 60U, scene.crate, {0.0F, 6.0F, 0.0F}, {}, {});
    const Act before = *find_act(scene.document.scene->performance, later);
    const SettleResult result = settle_stage(scene.document, compiled, solver.get(), SettleOptions{.instance_seed = 71U});
    CHECK(result.settled == 1U);
    CHECK(*find_act(scene.document.scene->performance, later) == before);
}

void dropped_model_files_become_kinds_placed_by_hand() {
    Document document = new_scene();
    const std::size_t before = document.scene->items.size();
    const std::vector<std::string> files{"C:/models/Helmet.GLB", "C:/models/notes.txt", "C:/models/rock.obj",
                                         "C:/models/arch.gltf", "C:/models/photo.png", "C:/models/noextension"};
    const std::vector<Uid> kinds = add_dropped_models(document, files);
    CHECK(kinds.size() == 3U && document.scene->items.size() == before + 3U);
    const char* meshes[] = {"C:/models/Helmet.GLB", "C:/models/rock.obj", "C:/models/arch.gltf"};
    for (std::size_t i = 0U; i < kinds.size(); ++i) {
        const Object& kind = object(document, kinds[i]);
        CHECK(kind.mesh == meshes[i]);
        CHECK(kind.distribution == Distribution::Placed && kind.motion == ObjectMotion::Dynamic);
    }
    CHECK(add_dropped_models(document, std::vector<std::string>{"a.txt"}).empty());
    Document none;
    CHECK(add_dropped_models(none, files).empty());
}

void placements_are_saved_with_their_home() {
    Scene3 scene = crate_kind();
    const Compiled compiled = ready(scene.document);
    const ActTargets targets = act_targets(compiled);
    CHECK(place(scene.document, targets, scene.crate, {1.0F, 2.0F, 3.0F}, {10.0F, 20.0F, 30.0F}).serial == 1U);
    CHECK(place(scene.document, targets, scene.crate, {-1.0F, 1.0F, 0.5F}, {}).serial == 2U);
    CHECK(jitter_stage(scene.document.scene->performance, 3U, 0.2F, 8.0F) == 2U);
    CHECK(move_placement(scene.document.scene->performance, 2U, {4.0F, 0.5F, 4.0F}, {0.0F, 5.0F, 0.0F}));

    const std::string saved = save_document(scene.document);
    const Loaded loaded = load_document(saved);
    CHECK(loaded.document.has_value());
    CHECK(loaded.document->scene->performance == scene.document.scene->performance);
    CHECK(loaded.document->scene->performance.acts[0].touched && !loaded.document->scene->performance.acts[1].touched);
    CHECK(save_document(*loaded.document) == saved);
    CHECK(object(*loaded.document, scene.crate).distribution == Distribution::Placed);
}

} // namespace

void add_stage_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"a_kind_has_no_bodies_until_it_is_placed", a_kind_has_no_bodies_until_it_is_placed});
    cases.push_back({"a_kind_that_stands_still_cannot_be_placed", a_kind_that_stands_still_cannot_be_placed});
    cases.push_back({"turns_survive_the_trip_through_a_pose", turns_survive_the_trip_through_a_pose});
    cases.push_back({"jitter_is_bounded_exact_and_taken_back_by_reset", jitter_is_bounded_exact_and_taken_back_by_reset});
    cases.push_back({"settling_keeps_where_things_came_to_rest", settling_keeps_where_things_came_to_rest});
    cases.push_back({"settling_leaves_later_acts_and_refuses_an_empty_stage", settling_leaves_later_acts_and_refuses_an_empty_stage});
    cases.push_back({"dropped_model_files_become_kinds_placed_by_hand", dropped_model_files_become_kinds_placed_by_hand});
    cases.push_back({"placements_are_saved_with_their_home", placements_are_saved_with_their_home});
}
