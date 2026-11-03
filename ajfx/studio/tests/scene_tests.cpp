#include "scene_properties.hpp"
#include "test_support.hpp"

namespace {
using namespace studio_test;

Object &object(Document &document, Uid uid) { return std::get<Object>(document.scene->item(uid)->data); }
Compiled ready(const Document &document) {
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
ajfx::Simulation simulation(const Compiled &compiled, const ajfx::RigidSolver *solver = nullptr) {
    auto created = ajfx::Simulation::create(*compiled.effect, 71U,
                                            ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = solver});
    if (!created) {
        throw std::runtime_error{created.error().message};
    }
    return std::move(created).value();
}

void new_scene_ground_catches_objects_and_can_be_removed() {
    Document document = new_scene();
    CHECK(document.scene && document.scene->items.size() == 1U);
    const Uid ground = document.scene->items.front().uid;
    CHECK(document.scene->item(ground)->name == "Ground");
    CHECK(object(document, ground).motion == ObjectMotion::Static);
    const auto* mesh = library_mesh("floor");
    CHECK(mesh != nullptr);
    float top = -1.0F;
    for (const auto& vertex : mesh->vertices) {
        top = std::max(top, vertex.position.y * object(document, ground).size + object(document, ground).position.y);
    }
    CHECK(near(top, 0.0F));

    const Uid falling = add_object(document);
    object(document, falling).position = {0.0F, 2.0F, 0.0F};
    const Compiled compiled = ready(document);
    CHECK(compiled.requests.empty());
    CHECK(compiled.effect->emitters.size() == 1U);
    CHECK(compiled.meshes.contains("floor"));
    const auto* physics = ajfx::rigid_physics(compiled.effect->emitters.front());
    CHECK(physics && physics->props.size() == 1U);
    CHECK(physics->props.front().mesh == "floor" && physics->props.front().motion == ajfx::PropMotion::Fixed);
    auto solver = ajfx::rigid::make_jolt_solver();
    auto run = simulation(compiled, solver.get());
    run.advance_to(180U);
    CHECK(run.emitter(0).alive() == 1U);
    CHECK(near(run.emitter(0).position[0].y, 0.5F, 0.03F));
    CHECK(std::fabs(run.emitter(0).velocity[0].y) < 0.05F);
    auto rerun = simulation(compiled, solver.get());
    rerun.advance_to(180U);
    CHECK(run.state() == rerun.state());
    const auto bytes = ajfx::write_package(*compiled.effect, asset_entries(compiled));
    CHECK(bytes && ajfx::read_package(bytes.value()));

    const Loaded restored = load_document(save_document(document));
    CHECK(restored.document && *restored.document == document);
    CHECK(remove_item(document, ground));
    const Loaded without_ground = load_document(save_document(document));
    CHECK(without_ground.document && *without_ground.document == document);
    auto unsupported = simulation(ready(*without_ground.document), solver.get());
    unsupported.advance_to(180U);
    CHECK(unsupported.emitter(0).position[0].y < -5.0F);
    CHECK(add_floor(document) != 0U);
    CHECK(ready(document).requests.empty());
}

void typed_scene_roundtrip_and_strict_fields() {
    Document document = empty_scene();
    const Uid a = add_object(document, "artist/model.obj");
    const Uid b = add_particle_system(document);
    const Uid group = add_group(document);
    CHECK(group_item(document, a, group));
    object(document, a).tint = {0.17F, 0.63F, 0.91F, 0.78F};
    object(document, a).velocity = {1.25F, -2.5F, 3.75F};
    object(document, a).fracture = true;
    object(document, a).fragments = 13U;
    auto &system = std::get<ParticleSystem>(document.scene->item(b)->data);
    system.sources.front().mode = SourceMode::Initial;
    system.persistent = true;
    CHECK(add_behavior(document, b, BehaviorKind::Vortex) != 0U);
    CHECK(add_behavior(document, b, BehaviorKind::Turbulence) != 0U);
    system.motion[0].strength = -4.5F;
    system.motion[0].axis = VortexAxis::Horizontal;
    system.motion[0].center = {0.5F, 1.5F, -2.5F};
    system.motion[1].enabled = false;
    system.motion[1].size = 1.3F;
    system.size_driver = Driver::Age;
    system.size_over_age = {{0.0F, 0.25F}, {0.4F, 2.0F}, {1.0F, 0.0F}};
    system.tint_driver = Driver::Age;
    system.tint_over_age = {{0.0F, {1.0F, 0.5F, 0.25F, 1.0F}}, {1.0F, {0.1F, 0.2F, 0.3F, 0.0F}}};
    document.scene->physics.gravity = 0.37F;
    document.scene->physics.angular_damping = 0.19F;
    document.loop_seconds = 11.5F;
    const std::string saved = save_document(document);
    const Loaded loaded = load_document(saved);
    if (!loaded.document) {
        throw std::runtime_error{loaded.error};
    }
    CHECK(*loaded.document == document);
    CHECK(save_document(*loaded.document) == saved);
    CHECK(saved.find("\"layers\"") == std::string::npos);
    std::string bad = saved;
    bad.insert(bad.find("\"position\""), "\"invented\": 1, ");
    CHECK(!load_document(bad).document);
    bad = saved;
    bad.replace(bad.find("\"version\": 2"), 12U, "\"version\": 3");
    CHECK(!load_document(bad).document);
    bad = saved;
    bad.insert(bad.find("\"churn\""), "\"invented\": 1, ");
    CHECK(!load_document(bad).document);
    bad = saved;
    bad.replace(bad.find("\"size_driver\": 1"), 16U, "\"size_driver\": 7");
    CHECK(!load_document(bad).document);
    object(document, a).motion = ObjectMotion::Static;
    const Loaded unfinished = load_document(save_document(document));
    CHECK(unfinished.document && *unfinished.document == document);
    CHECK(!compile(*unfinished.document, Purpose::Export).effect);
    document.scene->items.back().parent = group;
    CHECK(!validate_scene(document).empty());
}

void independent_surfaces_share_one_colliding_world() {
    Document document = empty_scene();
    document.loop_seconds = 1.0F;
    const Uid floor = add_object(document);
    object(document, floor).motion = ObjectMotion::Static;
    object(document, floor).position = {0, -5, 0};
    object(document, floor).size = 10;
    const Uid a = add_object(document);
    const Uid b = add_object(document);
    object(document, a).position = {0, 0, 0};
    object(document, b).position = {0, 2, 0};
    object(document, a).tint = {1, 0.1F, 0.2F, 1};
    object(document, b).tint = {0.1F, 0.3F, 1, 1};
    const Compiled compiled = ready(document);
    CHECK(compiled.effect->emitters.size() == 1U);
    const auto &emitter = compiled.effect->emitters.front();
    const auto *physics = ajfx::rigid_physics(emitter);
    CHECK(physics && physics->bodies.size() >= 2U);
    const auto *ar = compiled.layer(a);
    const auto *br = compiled.layer(b);
    CHECK(ar && br && ar->emitter == br->emitter);
    const auto has_tint = [&](ajfx::Color tint) {
        return std::any_of(physics->bodies.begin(), physics->bodies.end(),
                           [&](const auto &kind) { return kind.draw_tint == tint; });
    };
    CHECK(has_tint(object(document, a).tint));
    CHECK(has_tint(object(document, b).tint));
    const auto bytes = ajfx::write_package(*compiled.effect, asset_entries(compiled));
    CHECK(bytes);
    const auto package = ajfx::read_package(bytes.value());
    CHECK(package && package.value() == *compiled.effect);
    auto solver = ajfx::rigid::make_jolt_solver();
    auto run = simulation(compiled, solver.get());
    run.advance_to(60U);
    CHECK(run.emitter(0).alive() == 2U);
    // The upper object rests on the held lower one rather than falling through.
    CHECK(run.emitter(0).position[1].y > 1.0F);
    CHECK(run.emitter(0).position[1].y < 2.1F);
    auto baked = bake_page(document, compiled, 71U, solver.get());
    CHECK(baked);
    const BakeRecord &record = baked.value();
    CHECK(bake_status(document, compile(document, Purpose::Export), &record).current);
    object(document, b).tint = {0.2F, 0.9F, 0.4F, 1};
    const Compiled recolored = ready(document);
    CHECK(same_scene_motion(record.page, document));
    CHECK(bake_status(document, compile(document, Purpose::Export), &record).current);
    CHECK(ajfx::Cache::open(record.bytes, *recolored.effect, 71U, {}, recolored.meshes));
    object(document, b).collider = ObjectCollider::Sphere;
    CHECK(!same_scene_motion(record.page, document));
    CHECK(!bake_status(document, compile(document, Purpose::Export), &record).current);
}

void organization_and_rename_do_not_change_simulation() {
    Document document = empty_scene();
    const Uid a = add_object(document);
    const Uid b = add_object(document, "slab");
    object(document, b).position = {0, 3, 0};
    object(document, b).velocity = {0.5F, 0.25F, -0.75F};
    object(document, b).rotation = {13, 29, 41};
    const Uid particles = add_particle_system(document);
    CHECK(add_source(document, particles, SourceMode::Burst) != 0U);
    const Compiled before = ready(document);
    const Uid group = add_group(document);
    CHECK(group_item(document, a, group));
    document.scene->item(a)->name = "Renamed freely";
    std::reverse(document.scene->items.begin(), document.scene->items.end());
    auto &sources = std::get<ParticleSystem>(document.scene->item(particles)->data).sources;
    std::reverse(sources.begin(), sources.end());
    const Compiled after = ready(document);
    CHECK(before.effect == after.effect);
    auto solver = ajfx::rigid::make_jolt_solver();
    auto one = simulation(before, solver.get());
    auto two = simulation(after, solver.get());
    CHECK(one.emitter(0).velocity[1] == ajfx::Vec3{0.5F, 0.25F, -0.75F});
    CHECK(one.emitter(0).orientation[1] == ajfx::prop_orientation({13, 29, 41}));
    one.advance_to(120U);
    two.advance_to(120U);
    CHECK(one.state() == two.state());
    ajfx::CheckpointRing checkpoints{15U, 16U};
    checkpoints.seek(two, 37U);
    two.advance_to(120U);
    CHECK(one.state() == two.state());
    CHECK(remove_item(document, group));
    CHECK(document.scene->item(a)->parent == 0U);
    CHECK(ready(document).effect == before.effect);
}

void initial_population_is_bounded_and_persistent() {
    Document document = empty_scene();
    const Uid uid = add_particle_system(document);
    auto &system = std::get<ParticleSystem>(document.scene->item(uid)->data);
    system.sources.front().mode = SourceMode::Initial;
    system.sources.front().count = 37;
    system.persistent = true;
    system.velocity = {};
    const Compiled compiled = ready(document);
    CHECK(compiled.effect->emitters.front().capacity == 64U);
    auto run = simulation(compiled);
    run.advance_to(601U);
    CHECK(run.emitter(0).alive() == 37U);
    CHECK(run.emitter(0).spawned == 37U);
    system.sources.front().mode = SourceMode::Continuous;
    CHECK(!compile(document, Purpose::Export).effect);
    system.sources.front().mode = SourceMode::Initial;
    document.loop_seconds = 200.0F;
    CHECK(bake_frames(document, compile(document, Purpose::Export)) == 12000U);
}

void ten_distinct_models_fracture_without_losing_owners() {
    Document document = empty_scene();
    const Uid floor = add_object(document);
    object(document, floor).motion = ObjectMotion::Static;
    object(document, floor).size = 10;
    object(document, floor).position = {0, -5, 0};
    for (unsigned i = 0; i < 10U; ++i) {
        const Uid uid = add_object(document, "model_" + std::to_string(i) + ".obj");
        auto &o = object(document, uid);
        o.size = 0.4F;
        o.position = {static_cast<float>(i % 5U) - 2.0F, 2.0F, static_cast<float>(i / 5U) - 0.5F};
        o.fracture = true;
        o.fragments = 8;
        o.fracture_speed = 0.5F;
        o.tint = {static_cast<float>(i + 1U) / 11.0F, 0.4F, 0.8F, 1};
    }
    Compiled compiled = compile(document, Purpose::Export);
    CHECK(compiled.effect && compiled.error.empty());
    CHECK(compiled.requests.size() == 10U);
    for (std::size_t i = 0U; i < compiled.requests.size(); ++i) {
        const auto &request = compiled.requests[i];
        auto mesh = *library_mesh("cube");
        for (auto &vertex : mesh.vertices) {
            vertex.position.x *= 0.5F + static_cast<float>(i) * 0.05F;
        }
        compiled.meshes[request.entry] = std::move(mesh);
    }
    std::string error;
    make_cuts(compiled, error);
    if (!error.empty()) {
        throw std::runtime_error{error};
    }
    CHECK(compiled.effect && compiled.effect->emitters.size() == 1U);
    const auto *physics = ajfx::rigid_physics(compiled.effect->emitters.front());
    CHECK(physics && physics->bodies.size() > 64U);
    auto solver = ajfx::rigid::make_jolt_solver();
    auto run = simulation(compiled, solver.get());
    run.advance_to(180U);
    CHECK(run.emitter(0).alive() > 10U);
    CHECK(run.emitter(0).refused == 0U);
    for (const auto &item : document.scene->items) {
        if (item.uid == floor) {
            continue;
        }
        const auto *report = compiled.layer(item.uid);
        CHECK(report && report->included && report->scope.kinds.size() >= 9U);
        for (auto kind : report->scope.kinds) {
            if (kind) {
                CHECK(physics->bodies[kind - 1U].draw_tint == std::get<Object>(item.data).tint);
            }
        }
    }
    for (auto &item : document.scene->items) {
        if (auto *o = std::get_if<Object>(&item.data); o && o->fracture) {
            o->fragments = 64U;
        }
    }
    const Compiled overflow = compile(document, Purpose::Export);
    CHECK(!overflow.effect && !overflow.error.empty());
}
} // namespace

// The Motion stack is typed behaviors in an authored order; size and colour
// over age reach the runtime as the keys the artist wrote.
void motion_stack_and_age_drivers_reach_the_runtime() {
    Document document = empty_scene();
    const Uid uid = add_particle_system(document);
    auto &system = std::get<ParticleSystem>(document.scene->item(uid)->data);
    const Uid vortex = add_behavior(document, uid, BehaviorKind::Vortex);
    const Uid drag = add_behavior(document, uid, BehaviorKind::Drag);
    const Uid turbulence = add_behavior(document, uid, BehaviorKind::Turbulence);
    CHECK(vortex != 0U && drag != 0U && turbulence != 0U && system.motion.size() == 3U);
    CHECK(system.motion[0].strength == 3.0F && system.motion[1].strength == 1.0F && system.motion[2].strength == 1.5F);
    system.motion[0].strength = -7.5F;
    system.motion[0].axis = VortexAxis::Horizontal;
    system.motion[0].center = {1.5F, 2.5F, 3.5F};
    system.motion[1].strength = 2.25F;
    system.motion[2].strength = 4.5F;
    system.motion[2].size = 1.3F;
    system.motion[2].churn = 0.9F;
    // Moving turbulence up puts it before drag: stack order, not identity order.
    CHECK(move_behavior(document, uid, turbulence, -1));
    CHECK(!move_behavior(document, uid, vortex, -1));
    CHECK(system.motion[1].uid == turbulence && system.motion[2].uid == drag);
    CHECK(validate_scene(document).empty());

    const auto kinds = [](const Document &lowered) {
        std::vector<std::string> out;
        for (const Block &block : lowered.layers.front().blocks) {
            if (block.kind == "swirl" || block.kind == "turbulence" || block.kind == "drag") {
                out.push_back(block.kind);
            }
        }
        return out;
    };
    Document lowered = lower_scene(document);
    CHECK(lowered.layers.size() == 1U);
    CHECK((kinds(lowered) == std::vector<std::string>{"swirl", "turbulence", "drag"}));
    const Block *swirl = lowered.block(vortex);
    const Block *noise = lowered.block(turbulence);
    CHECK(swirl && swirl->kind == "swirl" && get(*swirl, "strength").x == -7.5F &&
          get(*swirl, "axis").text == "horizontal" && get(*swirl, "center").z == 3.5F);
    CHECK(noise && get(*noise, "size").x == 1.3F && get(*noise, "churn").x == 0.9F);
    system.motion[2].enabled = false;
    lowered = lower_scene(document);
    CHECK((kinds(lowered) == std::vector<std::string>{"swirl", "turbulence"}));
    system.motion[2].enabled = true;

    // Age drivers: the compiled effect carries the authored keys.
    system.size_driver = Driver::Age;
    system.size_over_age = {{0.0F, 0.25F}, {0.5F, 2.0F}, {1.0F, 0.5F}};
    system.tint_driver = Driver::Age;
    system.tint_over_age = {{0.0F, {1.0F, 0.5F, 0.25F, 1.0F}}, {1.0F, {0.1F, 0.2F, 0.3F, 0.0F}}};
    const Compiled compiled = ready(document);
    const ajfx::SizeOverLife *size = nullptr;
    const ajfx::ColorOverLife *tint = nullptr;
    unsigned forces = 0U;
    for (const ajfx::UpdateModule &module : compiled.effect->emitters.front().update) {
        if (const auto *sized = std::get_if<ajfx::SizeOverLife>(&module)) {
            size = sized;
        } else if (const auto *coloured = std::get_if<ajfx::ColorOverLife>(&module)) {
            tint = coloured;
        } else if (std::holds_alternative<ajfx::Vortex>(module) || std::holds_alternative<ajfx::CurlNoise>(module) ||
                   std::holds_alternative<ajfx::Drag>(module)) {
            ++forces;
        }
    }
    CHECK(forces == 3U);
    CHECK(size && size->keys == system.size_over_age);
    CHECK(tint && tint->keys == system.tint_over_age);
    auto run = simulation(compiled);
    run.advance_to(30U);
    CHECK(run.emitter(0).alive() > 0U);

    // A motion or size-over-age edit stales the motion cache; colour does not.
    Document other = document;
    CHECK(same_scene_motion(document, other));
    std::get<ParticleSystem>(other.scene->item(uid)->data).tint_over_age.front().value.g = 0.9F;
    CHECK(same_scene_motion(document, other));
    std::get<ParticleSystem>(other.scene->item(uid)->data).size_over_age.front().value = 0.75F;
    CHECK(!same_scene_motion(document, other));
    other = document;
    std::get<ParticleSystem>(other.scene->item(uid)->data).motion[1].strength = 0.3F;
    CHECK(!same_scene_motion(document, other));
    other = document;
    CHECK(move_behavior(other, uid, drag, -1));
    CHECK(!same_scene_motion(document, other));

    // A duplicate owns fresh behavior identities.
    const Uid copy = duplicate_item(document, uid);
    CHECK(copy != 0U);
    const auto &twin = std::get<ParticleSystem>(document.scene->item(copy)->data);
    CHECK(twin.motion.size() == 3U && twin.motion[0].uid != vortex && twin.motion[0].strength == -7.5F);
    CHECK(validate_scene(document).empty());

    // Keys out of age order, and too many behaviors, are refused. The
    // duplicate grew the item list, so the system is looked up again.
    auto &again = std::get<ParticleSystem>(document.scene->item(uid)->data);
    again.size_over_age = {{0.6F, 1.0F}, {0.4F, 1.0F}};
    CHECK(!validate_scene(document).empty());
    again.size_over_age = {{0.0F, 1.0F}, {1.0F, 1.0F}};
    while (add_behavior(document, uid, BehaviorKind::Drag) != 0U) {
    }
    CHECK(again.motion.size() == max_behaviors);
    CHECK(validate_scene(document).empty());
}

void add_scene_tests(std::vector<studio_test::Case> &cases) {
    cases.push_back({"motion_stack_and_age_drivers_reach_the_runtime", motion_stack_and_age_drivers_reach_the_runtime});
    cases.push_back({"new_scene_ground_catches_objects_and_can_be_removed", new_scene_ground_catches_objects_and_can_be_removed});
    cases.push_back({"typed_scene_roundtrip_and_strict_fields", typed_scene_roundtrip_and_strict_fields});
    cases.push_back({"independent_surfaces_share_one_colliding_world", independent_surfaces_share_one_colliding_world});
    cases.push_back(
        {"organization_and_rename_do_not_change_simulation", organization_and_rename_do_not_change_simulation});
    cases.push_back({"initial_population_is_bounded_and_persistent", initial_population_is_bounded_and_persistent});
    cases.push_back(
        {"ten_distinct_models_fracture_without_losing_owners", ten_distinct_models_fracture_without_losing_owners});
}
