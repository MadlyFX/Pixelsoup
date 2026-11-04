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

// A Dynamic object starts with its middle where the inspector and its handle
// put it, whatever it collides as, and falls from there. It used to start
// half a pile spacing higher (most of its own size) and drop into place.
void a_dynamic_object_starts_where_it_is_placed() {
    Document document = new_scene();
    const Uid hull = add_object(document);
    object(document, hull).position = {0.6F, 2.3F, -0.9F};
    object(document, hull).size = 1.4F;
    const Uid ball = add_object(document);
    object(document, ball).position = {-1.7F, 1.1F, 0.4F};
    object(document, ball).size = 0.6F;
    object(document, ball).collider = ObjectCollider::Sphere;
    const Compiled compiled = ready(document);
    auto solver = ajfx::rigid::make_jolt_solver();
    auto run = simulation(compiled, solver.get());
    const ajfx::EmitterState &start = run.emitter(0);
    CHECK(start.alive() == 2U);
    const auto starts_at = [&start](ajfx::Vec3 at) {
        for (std::size_t i = 0U; i < start.alive(); ++i) {
            if (start.position[i] == at) {
                return true;
            }
        }
        return false;
    };
    CHECK(starts_at({0.6F, 2.3F, -0.9F}));
    CHECK(starts_at({-1.7F, 1.1F, 0.4F}));
    // Each comes to rest on the floor, its middle half its size up.
    run.advance_to(240U);
    const float a = run.emitter(0).position[0].y;
    const float b = run.emitter(0).position[1].y;
    CHECK(near(std::min(a, b), 0.3F, 0.03F) && near(std::max(a, b), 0.7F, 0.03F));
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
    system.motion[0].strength = {-4.5F, -1.5F};
    system.motion[0].axis = VortexAxis::Horizontal;
    system.motion[0].center = {0.5F, 1.5F, -2.5F};
    system.motion[0].seed = 17U;
    system.motion[1].enabled = false;
    system.motion[1].size = 1.3F;
    CHECK(add_behavior(document, b, BehaviorKind::Wind) != 0U);
    system.motion[2].heading = 135.0F;
    system.lifetime = {1.5F, 4.5F};
    system.size = {0.02F, 0.09F};
    system.speed = {0.5F, 2.5F};
    system.radius = {0.1F, 0.4F};
    system.brightness = {1.0F, 3.0F};
    system.spin = {30.0F, 120.0F};
    system.size_driver = Driver::Age;
    system.size_over_age = {{0.0F, 0.25F}, {0.4F, 2.0F}, {1.0F, 0.0F}};
    system.tint_driver = Driver::Age;
    system.tint_over_age = {{0.0F, {1.0F, 0.5F, 0.25F, 1.0F}}, {1.0F, {0.1F, 0.2F, 0.3F, 0.0F}}};
    system.lighting = ParticleLighting::Lit;
    system.curvature = 0.35F;
    system.wrap = 0.65F;
    system.ambient = 1.75F;
    system.shadowed = false;
    document.scene->physics.gravity = 0.37F;
    document.scene->physics.angular_damping = 0.19F;
    document.loop_seconds = 11.5F;
    document.backdrop = Backdrop::Night;
    const std::string saved = save_document(document);
    const Loaded loaded = load_document(saved);
    if (!loaded.document) {
        throw std::runtime_error{loaded.error};
    }
    CHECK(*loaded.document == document);
    CHECK(loaded.document->backdrop == Backdrop::Night);
    CHECK(save_document(*loaded.document) == saved);
    CHECK(saved.find("\"layers\"") == std::string::npos);
    // The light settings reach the renderer through the Appearance block.
    {
        Document lit_scene = empty_scene();
        const Uid puff = add_particle_system(lit_scene);
        auto &lit = std::get<ParticleSystem>(lit_scene.scene->item(puff)->data);
        lit.look = ParticleLook::Smoke;
        lit.lighting = ParticleLighting::Lit;
        lit.curvature = 0.35F;
        lit.wrap = 0.65F;
        lit.ambient = 1.75F;
        lit.shadowed = false;
        const Compiled compiled = compile(lit_scene, Purpose::Export);
        CHECK(compiled.effect && compiled.effect->emitters.size() == 1U);
        const ajfx::SpriteRenderer &r = compiled.effect->emitters[0].renderer;
        CHECK(r.lighting == ajfx::Lighting::Lit && r.curvature == 0.35F && r.wrap == 0.65F && r.ambient == 1.75F &&
              !r.shadowed);
        lit.lighting = ParticleLighting::Glows;
        CHECK(compile(lit_scene, Purpose::Export).effect->emitters[0].renderer.lighting == ajfx::Lighting::Unlit);
    }
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
    system.speed = {};
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
    CHECK(system.motion[0].strength == Range{3.0F, 3.0F} && system.motion[1].strength == Range{1.0F, 1.0F} &&
          system.motion[2].strength == Range{1.5F, 1.5F});
    system.motion[0].strength = {-7.5F, -7.5F};
    system.motion[0].axis = VortexAxis::Horizontal;
    system.motion[0].center = {1.5F, 2.5F, 3.5F};
    system.motion[1].strength = {0.75F, 2.25F};
    system.motion[2].strength = {4.5F, 4.5F};
    system.motion[2].size = 1.3F;
    system.motion[2].churn = 0.9F;
    system.motion[2].seed = 5U;
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
    CHECK(noise && get(*noise, "size").x == 1.3F && get(*noise, "churn").x == 0.9F && get(*noise, "seed").x == 5.0F);
    CHECK(lowered.block(drag) && get(*lowered.block(drag), "amount") == range(0.75F, 2.25F));
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

    // Ranges reach the runtime as two values, and a behavior's seed changes
    // its noise field and its per-particle shares.
    system.lifetime = {1.5F, 4.5F};
    system.size = {0.02F, 0.09F};
    system.speed = {0.5F, 2.5F};
    system.spin = {30.0F, 120.0F};
    system.brightness = {1.0F, 3.0F};
    const Compiled ranged = ready(document);
    const ajfx::Emitter &emitter = ranged.effect->emitters.front();
    const ajfx::Lifetime *life = nullptr;
    const ajfx::InitSize *init_size = nullptr;
    const ajfx::InitSpin *init_spin = nullptr;
    const ajfx::Velocity *velocity = nullptr;
    for (const ajfx::InitModule &module : emitter.init) {
        if (const auto *m = std::get_if<ajfx::Lifetime>(&module)) { life = m; }
        if (const auto *m = std::get_if<ajfx::InitSize>(&module)) { init_size = m; }
        if (const auto *m = std::get_if<ajfx::InitSpin>(&module)) { init_spin = m; }
        if (const auto *m = std::get_if<ajfx::Velocity>(&module)) { velocity = m; }
    }
    CHECK(life && life->min.value == 1.5F && life->max.value == 4.5F);
    CHECK(init_size && init_size->min.value == 0.02F && init_size->max.value == 0.09F);
    CHECK(init_spin && init_spin->speed_min.value == 30.0F && init_spin->speed_max.value == 120.0F);
    CHECK(velocity && velocity->speed_min.value == 0.5F && velocity->speed_max.value == 2.5F);
    const ajfx::Drag *drag_module = nullptr;
    const ajfx::CurlNoise *noise_module = nullptr;
    for (const ajfx::UpdateModule &module : emitter.update) {
        if (const auto *m = std::get_if<ajfx::Drag>(&module)) { drag_module = m; }
        if (const auto *m = std::get_if<ajfx::CurlNoise>(&module)) { noise_module = m; }
    }
    CHECK(drag_module && drag_module->scale_min.value == 0.75F && drag_module->scale_max.value == 2.25F);
    CHECK(noise_module);
    const std::uint32_t seeded = noise_module->seed;
    system.motion[1].seed = 6U;
    const Compiled reseeded = ready(document);
    for (const ajfx::UpdateModule &module : reseeded.effect->emitters.front().update) {
        if (const auto *m = std::get_if<ajfx::CurlNoise>(&module)) {
            CHECK(m->seed != seeded);
        }
    }
    Document brighter = document;
    CHECK(same_scene_motion(document, brighter));
    std::get<ParticleSystem>(brighter.scene->item(uid)->data).brightness = {2.0F, 2.0F};
    CHECK(!same_scene_motion(document, brighter));
    system.lifetime = {3.0F, 3.0F};
    system.size = {0.05F, 0.05F};
    system.speed = {1.0F, 1.0F};
    system.spin = {};
    system.brightness = {1.0F, 1.0F};

    // A motion or size-over-age edit stales the motion cache; colour does not.
    Document other = document;
    CHECK(same_scene_motion(document, other));
    std::get<ParticleSystem>(other.scene->item(uid)->data).tint_over_age.front().value.g = 0.9F;
    CHECK(same_scene_motion(document, other));
    std::get<ParticleSystem>(other.scene->item(uid)->data).size_over_age.front().value = 0.75F;
    CHECK(!same_scene_motion(document, other));
    other = document;
    std::get<ParticleSystem>(other.scene->item(uid)->data).motion[1].strength = {0.3F, 0.3F};
    CHECK(!same_scene_motion(document, other));
    other = document;
    CHECK(move_behavior(other, uid, drag, -1));
    CHECK(!same_scene_motion(document, other));

    // A duplicate owns fresh behavior identities.
    const Uid copy = duplicate_item(document, uid);
    CHECK(copy != 0U);
    const auto &twin = std::get<ParticleSystem>(document.scene->item(copy)->data);
    CHECK(twin.motion.size() == 3U && twin.motion[0].uid != vortex && twin.motion[0].strength == Range{-7.5F, -7.5F});
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

// A force is a scene item: it lowers into the systems it acts on, by their
// identities and never tree order; a blast throws everything from one host.
void forces_are_scene_items_that_act_on_their_targets() {
    Document document = empty_scene();
    const Uid a = add_particle_system(document);
    const Uid b = add_particle_system(document);
    const Uid vortex = add_force(document, ForceKind::Vortex);
    const Uid wind = add_force(document, ForceKind::Wind);
    const Uid blast = add_force(document, ForceKind::Blast);
    CHECK(vortex != 0U && wind != 0U && blast != 0U);
    auto &v = std::get<Force>(document.scene->item(vortex)->data);
    auto &w = std::get<Force>(document.scene->item(wind)->data);
    auto &x = std::get<Force>(document.scene->item(blast)->data);
    CHECK(document.scene->item(vortex)->name == "Vortex" && v.strength == Range{3.0F, 3.0F});
    v.position = {1.0F, 2.0F, 3.0F};
    v.strength = {-2.0F, -5.0F};
    v.axis = VortexAxis::Horizontal;
    w.all_systems = false;
    w.targets = {b};
    w.heading = 90.0F;
    x.position = {0.0F, 0.7F, 0.0F};
    x.power = 9.0F;
    x.radius = 2.5F;
    x.time = 1.25F;
    CHECK(validate_scene(document).empty());

    Document lowered = lower_scene(document);
    CHECK(lowered.layers.size() == 2U);
    const Block *swirl_a = lowered.block(a + 16U);
    const Block *swirl_b = lowered.block(b + 16U);
    const Block *wind_b = lowered.block(b + 17U);
    CHECK(swirl_a && swirl_a->kind == "swirl" && get(*swirl_a, "center").z == 3.0F &&
          get(*swirl_a, "strength") == range(-2.0F, -5.0F) && get(*swirl_a, "axis").text == "horizontal");
    CHECK(swirl_b && swirl_b->kind == "swirl");
    CHECK(wind_b && wind_b->kind == "wind" && get(*wind_b, "heading").x == 90.0F);
    CHECK(lowered.block(a + 17U) == nullptr);
    const Block *explosion = lowered.block(blast + 1U);
    CHECK(explosion && explosion->kind == "explosion" && get(*explosion, "power").x == 9.0F);
    CHECK(std::find_if(lowered.layers.front().blocks.begin(), lowered.layers.front().blocks.end(),
                       [blast](const Block &block) { return block.uid == blast + 1U; }) !=
          lowered.layers.front().blocks.end());
    CHECK(handle_block(document, vortex) == a + 16U);
    CHECK(handle_block(document, wind) == b + 17U);
    CHECK(handle_block(document, blast) == blast + 1U);

    // The compiled effect carries one blast and the forces in each emitter.
    const Compiled compiled = ready(document);
    CHECK(compiled.effect->blasts.size() == 1U && compiled.effect->blasts.front().time == 1.25F);
    unsigned vortices = 0U;
    unsigned winds = 0U;
    for (const ajfx::Emitter &emitter : compiled.effect->emitters) {
        for (const ajfx::UpdateModule &module : emitter.update) {
            vortices += std::holds_alternative<ajfx::Vortex>(module) ? 1U : 0U;
            winds += std::holds_alternative<ajfx::Gravity>(module) ? 1U : 0U;
        }
    }
    CHECK(vortices == 2U);
    CHECK(winds >= 1U);

    // Tree order and names never change where a force lowers to.
    std::swap(document.scene->items[0], document.scene->items[4]);
    document.scene->item(vortex)->name = "Whirl";
    const Document reordered = lower_scene(document);
    CHECK(reordered.block(a + 16U) && get(*reordered.block(a + 16U), "center").z == 3.0F);
    CHECK(reordered.block(b + 17U) && reordered.block(b + 17U)->kind == "wind");

    // A dragged handle writes back to the force, never to the compiler's copy.
    Document dragged = lowered;
    dragged.block(a + 16U)->set("center", vec3(4.0F, 2.0F, 6.0F));
    CHECK(adopt_handle_edit(document, dragged, vortex));
    CHECK(v.position == ajfx::Vec3{4.0F, 2.0F, 6.0F});
    dragged.block(blast + 1U)->set("radius", number(0.75F));
    CHECK(adopt_handle_edit(document, dragged, blast));
    CHECK(std::get<Force>(document.scene->item(blast)->data).radius == 0.75F); // the swap moved the items

    // Removing a named system drops it from the force; a missing one is refused.
    CHECK(remove_item(document, b));
    auto &wind_again = std::get<Force>(document.scene->item(wind)->data); // the item list shrank
    CHECK(wind_again.targets.empty());
    CHECK(validate_scene(document).empty());
    wind_again.targets = {b};
    CHECK(!validate_scene(document).empty());
    wind_again.targets.clear();

    // Save and load keep forces and their targets exactly.
    wind_again.targets = {a};
    const Loaded loaded = load_document(save_document(document));
    if (!loaded.document) {
        throw std::runtime_error{loaded.error};
    }
    CHECK(*loaded.document == document);

    // More than 16 forces on one system is refused: the vortex and the wind
    // already act on it, so fourteen more fill it and one more is too many.
    for (int k = 0; k < 14; ++k) {
        CHECK(add_force(document, ForceKind::Attract) != 0U);
    }
    CHECK(validate_scene(document).empty());
    CHECK(add_force(document, ForceKind::Attract) != 0U);
    CHECK(!validate_scene(document).empty());
}

// A relationship is one rule on the scene with two explicit endpoints: it
// lowers into the system as a birth from the object's hits, feeds the same
// runtime module the old blocks did, and goes when either endpoint goes.
void relationships_bind_an_object_to_a_system() {
    Document document = new_scene();
    const Uid cube = add_object(document);
    object(document, cube).position = {0.0F, 1.5F, 0.0F};
    const Uid dust = add_particle_system(document);
    auto &system = std::get<ParticleSystem>(document.scene->item(dust)->data);
    system.sources.clear();
    system.lifetime = {4.0F, 4.0F};
    CHECK(!validate_scene(document).empty()); // no source and no rule
    const Uid rule = add_relationship(document, RelationKind::EmitOnHit, cube, dust);
    CHECK(rule != 0U);
    CHECK(add_relationship(document, RelationKind::EmitOnHit, dust, dust) == 0U); // a system is not an object
    CHECK(add_relationship(document, RelationKind::TrailFrom, cube, cube) == 0U);
    auto &r = document.scene->relationships.front();
    r.hardness = 0.3F;
    r.per_hit = 7.0F;
    r.per_speed = 0.0F;
    r.spread = 45.0F;
    r.seed = 9U;
    CHECK(validate_scene(document).empty());

    const Document lowered = lower_scene(document);
    const Block *hits = lowered.block(rule);
    CHECK(hits && hits->kind == "hit_puffs" && get(*hits, "solid").x == static_cast<float>(cube) &&
          get(*hits, "per_hit").x == 7.0F && get(*hits, "spread").x == 45.0F && get(*hits, "seed").x == 9.0F);
    CHECK(std::any_of(lowered.layers.begin(), lowered.layers.end(), [&](const Layer &layer) {
        return layer.uid == dust && std::any_of(layer.blocks.begin(), layer.blocks.end(),
                                                [rule](const Block &b) { return b.uid == rule; });
    }));

    // The cube lands on the ground and dust is born where it hits.
    const Compiled compiled = ready(document);
    bool contacts = false;
    for (const ajfx::Emitter &emitter : compiled.effect->emitters) {
        for (const ajfx::SpawnModule &module : emitter.spawn) {
            if (const auto *found = std::get_if<ajfx::SpawnFromContacts>(&module)) {
                contacts = true;
                CHECK(found->per_hit == 7.0F && !found->bodies.empty());
            }
        }
    }
    CHECK(contacts);
    auto solver = ajfx::rigid::make_jolt_solver();
    auto run = simulation(compiled, solver.get());
    run.advance_to(120U);
    std::size_t born = 0U;
    for (std::size_t e = 0U; e < compiled.effect->emitters.size(); ++e) {
        if (compiled.effect->emitters[e].spawn.size() == 1U &&
            std::holds_alternative<ajfx::SpawnFromContacts>(compiled.effect->emitters[e].spawn.front())) {
            born = run.emitter(e).spawned;
        }
    }
    CHECK(born >= 7U);

    // A rule edit stales the motion cache; a rule shows from either endpoint.
    Document other = document;
    other.scene->relationships.front().per_hit = 3.0F;
    CHECK(!same_scene_motion(document, other));
    CHECK(same_scene_motion(document, document));

    // Save and load keep the rule exactly; a missing endpoint is refused.
    const Loaded loaded = load_document(save_document(document));
    if (!loaded.document) {
        throw std::runtime_error{loaded.error};
    }
    CHECK(*loaded.document == document);
    std::string bad = save_document(document);
    bad.replace(bad.find("\"object\": " + std::to_string(cube)), 10U + std::to_string(cube).size(), "\"object\": 7");
    CHECK(!load_document(bad).document);

    // A persistent population cannot be born of hits; a trail from every object can.
    const Uid trail = add_relationship(document, RelationKind::TrailFrom, 0U, dust);
    CHECK(trail != 0U && document.scene->relationships.back().per_speed == 10.0F);
    CHECK(lower_scene(document).block(trail) && lower_scene(document).block(trail)->kind == "trail");
    std::get<ParticleSystem>(document.scene->item(dust)->data).persistent = true;
    CHECK(!validate_scene(document).empty());
    std::get<ParticleSystem>(document.scene->item(dust)->data).persistent = false;

    // Removing an endpoint removes its rules; the trail from every object stays.
    CHECK(remove_item(document, cube));
    CHECK(document.scene->relationships.size() == 1U && document.scene->relationships.front().uid == trail);
    CHECK(remove_relationship(document, trail));
    CHECK(document.scene->relationships.empty());
    CHECK(!remove_relationship(document, trail));
}

// A particle system can trail another's particles: embers thrown up and
// slowed by drag, and smoke fed only by them. The rule lowers to the
// runtime's spawn_from_particles module naming the embers' emitter, nothing
// is born before the embers exist, smoke follows while they move, and a
// system can neither trail itself nor a ring of systems.
void a_system_trails_another_systems_particles() {
    Document document = new_scene();
    const Uid embers = add_particle_system(document);
    {
        auto &system = std::get<ParticleSystem>(document.scene->item(embers)->data);
        system.sources.clear();
        document.scene->item(embers)->name = "Embers";
        const Uid burst = add_source(document, embers, SourceMode::Burst);
        CHECK(burst != 0U);
        auto &source = std::get<ParticleSystem>(document.scene->item(embers)->data).sources.back();
        source.count = 24U;
        source.time = 1.0F; // nothing to trail for the first second
        auto &again = std::get<ParticleSystem>(document.scene->item(embers)->data);
        again.lifetime = {6.0F, 6.0F};
        again.speed = {3.0F, 4.0F};
        again.spread = 30.0F;
        again.collide = false;
        CHECK(add_behavior(document, embers, BehaviorKind::Drag) != 0U);
        std::get<ParticleSystem>(document.scene->item(embers)->data).motion.back().strength = {2.0F, 2.0F};
    }
    const Uid smoke = add_particle_system(document);
    {
        auto &system = std::get<ParticleSystem>(document.scene->item(smoke)->data);
        system.sources.clear();
        system.lifetime = {2.0F, 2.0F};
        system.speed = {0.0F, 0.0F};
        system.radius = {0.0F, 0.0F};
        system.collide = false;
    }
    CHECK(!validate_scene(document).empty()); // no source and no rule
    // Only another system feeds a trail from a system: not itself, not an object.
    CHECK(add_relationship(document, RelationKind::TrailFromSystem, smoke, smoke) == 0U);
    const Uid ground = document.scene->items.front().uid;
    CHECK(std::holds_alternative<Object>(document.scene->item(ground)->data));
    CHECK(add_relationship(document, RelationKind::TrailFromSystem, ground, smoke) == 0U);
    CHECK(add_relationship(document, RelationKind::TrailFrom, embers, smoke) == 0U);
    const Uid rule = add_relationship(document, RelationKind::TrailFromSystem, embers, smoke);
    CHECK(rule != 0U);
    {
        auto &r = document.scene->relationships.back();
        CHECK(r.per_speed == 10.0F);
        r.per_speed = 20.0F;
        r.min_speed = 0.5F;
        r.seed = 4U;
    }
    CHECK(validate_scene(document).empty());
    CHECK(relationship_active(*document.scene, document.scene->relationships.back()));

    // Lowered: the runtime module itself, in the smoke's layer, naming the
    // embers' layer emitter.
    const Document lowered = lower_scene(document);
    const Block *shed = lowered.block(rule);
    CHECK(shed && shed->kind == "module" && shed->raw);
    const auto *spawn = std::get_if<ajfx::SpawnModule>(&*shed->raw);
    const auto *trail = spawn ? std::get_if<ajfx::SpawnFromParticles>(spawn) : nullptr;
    CHECK(trail && trail->emitter == "item_" + std::to_string(embers) && trail->per_speed == 20.0F &&
          trail->min_speed == 0.5F);
    CHECK(get(*shed, "seed").x == 4.0F);
    CHECK(std::any_of(lowered.layers.begin(), lowered.layers.end(), [&](const Layer &layer) {
        return layer.uid == smoke &&
               std::any_of(layer.blocks.begin(), layer.blocks.end(), [rule](const Block &b) { return b.uid == rule; });
    }));

    // Nothing before the embers are thrown; smoke while they fly; none once
    // drag has stopped them.
    const Compiled compiled = ready(document);
    std::size_t embers_at = compiled.effect->emitters.size();
    std::size_t smoke_at = compiled.effect->emitters.size();
    for (std::size_t e = 0U; e < compiled.effect->emitters.size(); ++e) {
        const ajfx::Emitter &emitter = compiled.effect->emitters[e];
        if (emitter.name == "item_" + std::to_string(embers)) {
            embers_at = e;
        }
        for (const ajfx::SpawnModule &module : emitter.spawn) {
            if (const auto *found = std::get_if<ajfx::SpawnFromParticles>(&module)) {
                smoke_at = e;
                CHECK(found->emitter == "item_" + std::to_string(embers) && found->per_speed == 20.0F);
            }
        }
    }
    CHECK(embers_at < compiled.effect->emitters.size() && smoke_at < compiled.effect->emitters.size());
    auto solver = ajfx::rigid::make_jolt_solver();
    auto run = simulation(compiled, solver.get());
    run.advance_to(59U);
    CHECK(run.emitter(embers_at).spawned == 0U && run.emitter(smoke_at).spawned == 0U);
    run.advance_to(120U);
    CHECK(run.emitter(embers_at).alive() == 24U);
    const std::uint64_t flying = run.emitter(smoke_at).spawned;
    CHECK(flying > 50U);
    run.advance_to(300U);
    const std::uint64_t stopped = run.emitter(smoke_at).spawned;
    run.advance_to(360U);
    CHECK(run.emitter(smoke_at).spawned == stopped);
    CHECK(stopped > flying);

    // A rule edit stales the motion cache; save and load keep it exactly.
    Document other = document;
    other.scene->relationships.back().per_speed = 3.0F;
    CHECK(!same_scene_motion(document, other));
    const Loaded loaded = load_document(save_document(document));
    if (!loaded.document) {
        throw std::runtime_error{loaded.error};
    }
    CHECK(*loaded.document == document);
    std::string bad = save_document(document);
    const std::string kind = "\"kind\": 2";
    CHECK(bad.find(kind) != std::string::npos);
    bad.replace(bad.find(kind), kind.size(), "\"kind\": 3");
    CHECK(!load_document(bad).document);

    // A system feeding itself, or a ring of systems, is refused.
    document.scene->relationships.back().object = smoke;
    CHECK(validate_scene(document).find("cannot trail itself") != std::string::npos);
    document.scene->relationships.back().object = embers;
    CHECK(validate_scene(document).empty());
    const Uid ring = add_relationship(document, RelationKind::TrailFromSystem, smoke, embers);
    CHECK(ring != 0U);
    CHECK(validate_scene(document).find("round in a ring") != std::string::npos);
    CHECK(remove_relationship(document, ring));
    CHECK(validate_scene(document).empty());

    // One edit makes a trailing system from a system, named after it, and
    // removing the source removes the rule.
    const Uid made = add_bound_system(document, RelationKind::TrailFromSystem, embers);
    CHECK(made != 0U && document.scene->item(made)->name == "Embers trail");
    CHECK(std::get<ParticleSystem>(document.scene->item(made)->data).sources.empty());
    CHECK(document.scene->relationships.size() == 2U && document.scene->relationships.back().system == made &&
          document.scene->relationships.back().object == embers);
    CHECK(validate_scene(document).empty());
    CHECK(remove_item(document, embers));
    CHECK(document.scene->relationships.empty());
}

// The timeline is a view of the authored moments: a track for every item
// with time in it, and a dragged marker writes the one value it stands for.
void timeline_tracks_are_the_scene_moments() {
    Document document = empty_scene();
    const Uid still = add_object(document);
    const Uid released = add_object(document);
    object(document, released).release = 1.5F;
    const Uid moved = add_object(document);
    object(document, moved).motion = ObjectMotion::Animated;
    object(document, moved).move_at = 2.0F;
    object(document, moved).move_over = 0.75F;
    const Uid system = add_particle_system(document);
    const Uid burst = add_source(document, system, SourceMode::Burst);
    std::get<ParticleSystem>(document.scene->item(system)->data).sources.back().time = 3.25F;
    const Uid group = add_group(document);
    const Uid wind = add_force(document, ForceKind::Wind);
    const Uid blast = add_force(document, ForceKind::Blast);
    std::get<Force>(document.scene->item(blast)->data).time = 0.4F;
    static_cast<void>(still);
    static_cast<void>(group);
    static_cast<void>(wind);

    const std::vector<Track> tracks = collect_tracks(*document.scene, 6.0F);
    CHECK(tracks.size() == 4U);
    CHECK(tracks[0].item == released && tracks[0].markers.size() == 1U &&
          tracks[0].markers[0].kind == MarkerKind::Release && tracks[0].markers[0].seconds == 1.5F);
    CHECK(tracks[1].item == moved && tracks[1].markers[0].kind == MarkerKind::Move &&
          tracks[1].markers[0].seconds == 2.0F && tracks[1].markers[0].duration == 0.75F);
    CHECK(tracks[2].item == system && tracks[2].markers.size() == 2U);
    CHECK(tracks[2].markers[0].kind == MarkerKind::Continuous && tracks[2].markers[0].duration == 6.0F &&
          !tracks[2].markers[0].draggable);
    CHECK(tracks[2].markers[1].kind == MarkerKind::Burst && tracks[2].markers[1].part == burst &&
          tracks[2].markers[1].seconds == 3.25F);
    CHECK(tracks[3].item == blast && tracks[3].markers[0].kind == MarkerKind::Blast &&
          tracks[3].markers[0].seconds == 0.4F);

    CHECK(set_marker_time(document, tracks[0].markers[0], 2.25F) && object(document, released).release == 2.25F);
    CHECK(set_marker_time(document, tracks[0].markers[0], -1.0F) && object(document, released).release > 0.0F);
    CHECK(set_marker_time(document, tracks[1].markers[0], 1.0F) && object(document, moved).move_at == 1.0F);
    CHECK(set_marker_duration(document, tracks[1].markers[0], 2.5F) && object(document, moved).move_over == 2.5F);
    CHECK(!set_marker_duration(document, tracks[0].markers[0], 2.5F));
    CHECK(!set_marker_time(document, tracks[2].markers[0], 1.0F));
    CHECK(set_marker_time(document, tracks[2].markers[1], 0.5F) &&
          std::get<ParticleSystem>(document.scene->item(system)->data).sources.back().time == 0.5F);
    CHECK(set_marker_time(document, tracks[3].markers[0], 2.0F) &&
          std::get<Force>(document.scene->item(blast)->data).time == 2.0F);
    CHECK(!set_marker_time(document, tracks[3].markers[0], 2.0F)); // unchanged
    CHECK(validate_scene(document).empty());
}

// The statistics count what the scene is and what it compiles to, and name
// a limit while there is still room, never only once it is hit.
void statistics_count_the_scene_and_name_limits_in_reach() {
    Document document = new_scene();
    const Uid cube = add_object(document);
    object(document, cube).position = {0.0F, 1.0F, 0.0F};
    const Uid dust = add_particle_system(document);
    static_cast<void>(add_group(document));
    static_cast<void>(add_force(document, ForceKind::Blast));
    static_cast<void>(add_relationship(document, RelationKind::TrailFrom, cube, dust));
    const SceneStats plain = scene_stats(*document.scene, nullptr, nullptr);
    CHECK(plain.objects == 2U && plain.systems == 1U && plain.forces == 1U && plain.groups == 1U &&
          plain.relationships == 1U && plain.particle_capacity == 0U && plain.near_limits.empty());
    const std::vector<std::string> bare = stat_lines(plain);
    CHECK(bare.size() == 1U && bare.front().find("2 objects, 1 system, 1 force, 1 relationship") != std::string::npos);

    const Compiled compiled = ready(document);
    std::map<Uid, std::uint64_t> peaks{{dust, 1234U}};
    const SceneStats full = scene_stats(*document.scene, &compiled, &peaks);
    CHECK(full.particle_capacity == compiled.total_capacity() && full.particle_capacity > 0U);
    CHECK(full.traced && full.peak_particles == 1234U);
    CHECK(full.props == 1U && full.body_capacity >= 1U && full.blasts == 1U && full.state_bytes > 0U);
    const std::vector<std::string> lines = stat_lines(full);
    CHECK(lines.size() == 4U);
    CHECK(lines[0].find("up to 1,234 alive") != std::string::npos);
    CHECK(lines[1].find("1 standing") != std::string::npos && lines[1].find("of 256") != std::string::npos);
    CHECK(lines[2].rfind("State: ", 0U) == 0U);

    // Fourteen forces on one system is within a fifth of the limit of sixteen.
    for (int k = 0; k < 14; ++k) {
        CHECK(add_force(document, ForceKind::Wind) != 0U);
    }
    const SceneStats crowded = scene_stats(*document.scene, nullptr, nullptr);
    CHECK(crowded.forces_on_busiest == 14U && crowded.near_limits.size() == 1U &&
          crowded.near_limits.front() == "Forces on one system: 14 of 16");
    CHECK(stat_lines(crowded).back() == "Near a limit. Forces on one system: 14 of 16");
}

// Particles meet the floor and every other Static object, each as its own
// shape, and pass through them when told to.
void particles_meet_static_objects() {
    Document document = new_scene();
    const Uid ground = document.scene->items.front().uid;
    const Uid wall = add_object(document);
    object(document, wall).motion = ObjectMotion::Static;
    object(document, wall).position = {3.0F, 0.5F, 0.0F};
    object(document, wall).collider = ObjectCollider::Sphere;
    const Uid system = add_particle_system(document);
    auto &dust = std::get<ParticleSystem>(document.scene->item(system)->data);
    dust.sources.front().mode = SourceMode::Initial;
    dust.sources.front().count = 200U;
    dust.position = {0.0F, 1.0F, 0.0F};
    dust.direction = {0.0F, -1.0F, 0.0F};
    dust.speed = {2.0F, 2.0F};
    dust.spread = 0.0F;
    dust.gravity = 1.0F;
    dust.lifetime = {5.0F, 5.0F};
    dust.size = {0.02F, 0.02F};

    const Document lowered = lower_scene(document);
    const Block *plane = lowered.block(ground + 3U);
    const Block *ball = lowered.block(wall + 3U);
    CHECK(plane && plane->kind == "ground" && near(get(*plane, "height").x, 0.0F, 0.001F));
    CHECK(ball && ball->kind == "sphere" && get(*ball, "radius").x == 0.5F && get(*ball, "center").x == 3.0F);

    const Compiled compiled = ready(document);
    auto solver = ajfx::rigid::make_jolt_solver();
    auto run = simulation(compiled, solver.get());
    run.advance_to(180U);
    std::size_t dust_emitter = 0U;
    for (std::size_t e = 0U; e < compiled.effect->emitters.size(); ++e) {
        if (!ajfx::rigid_physics(compiled.effect->emitters[e])) { dust_emitter = e; }
    }
    const auto &state = run.emitter(dust_emitter);
    CHECK(state.alive() == 200U);
    float lowest = 1.0F;
    for (std::size_t i = 0U; i < state.alive(); ++i) {
        lowest = std::min(lowest, state.position[i].y);
    }
    CHECK(lowest > -0.05F); // on the floor, not through it

    dust.collide = false;
    const Compiled through = ready(document);
    auto fall = simulation(through, solver.get());
    fall.advance_to(180U);
    float deepest = 1.0F;
    const auto &fallen = fall.emitter(dust_emitter);
    for (std::size_t i = 0U; i < fallen.alive(); ++i) {
        deepest = std::min(deepest, fallen.position[i].y);
    }
    CHECK(deepest < -1.0F);
    CHECK(lower_scene(document).block(ground + 3U) == nullptr);

    // A rule on the floor can never fire; one on a Dynamic object can.
    const Uid rule = add_relationship(document, RelationKind::EmitOnHit, ground, system);
    CHECK(rule != 0U && !relationship_active(*document.scene, document.scene->relationships.back()));
    const Uid any = add_relationship(document, RelationKind::EmitOnHit, 0U, system);
    CHECK(any != 0U && !relationship_active(*document.scene, document.scene->relationships.back()));
    object(document, wall).motion = ObjectMotion::Dynamic;
    CHECK(relationship_active(*document.scene, document.scene->relationships.back()));
}

// A system made from an object lives only on its rule: no source of its
// own, so nothing plays until the object hits or moves.
void a_bound_system_is_born_only_of_its_object() {
    Document document = new_scene();
    const Uid crate = add_object(document);
    document.scene->item(crate)->name = "Crate";
    object(document, crate).position = {0.0F, 1.5F, 0.0F};
    const Uid dust = add_bound_system(document, RelationKind::EmitOnHit, crate);
    CHECK(dust != 0U && document.scene->item(dust)->name == "Crate hits");
    const auto &system = std::get<ParticleSystem>(document.scene->item(dust)->data);
    CHECK(system.sources.empty());
    CHECK(document.scene->relationships.size() == 1U && document.scene->relationships.front().system == dust &&
          document.scene->relationships.front().object == crate);
    CHECK(validate_scene(document).empty());
    CHECK(add_bound_system(document, RelationKind::TrailFrom, dust) == 0U); // a system is not an object

    // Nothing is born before the crate lands, and dust after.
    const Compiled compiled = ready(document);
    auto solver = ajfx::rigid::make_jolt_solver();
    auto run = simulation(compiled, solver.get());
    std::size_t emitter = 0U;
    for (std::size_t e = 0U; e < compiled.effect->emitters.size(); ++e) {
        if (!ajfx::rigid_physics(compiled.effect->emitters[e])) { emitter = e; }
    }
    run.advance_to(12U);
    CHECK(run.emitter(emitter).spawned == 0U);
    run.advance_to(120U);
    CHECK(run.emitter(emitter).spawned > 0U);

    // Removing the rule leaves a system with no source, refused for playback and kept for storage.
    CHECK(remove_relationship(document, document.scene->relationships.front().uid));
    CHECK(!validate_scene(document).empty());
    CHECK(validate_scene(document, false).empty());
}

// A smoke grid is a scene item: a box of air that particle systems feed and
// ride through relationships. It reaches the effect as a grid volume, its
// bindings as the runtime's own modules, scaled as every metre is.
void smoke_grids_are_fed_and_ridden_by_systems() {
    Document document = empty_scene();
    document.scale = 2.0F;
    const Uid embers = add_particle_system(document);
    const Uid ash = add_particle_system(document);
    const Uid grid = add_smoke_grid(document);
    CHECK(grid != 0U && document.scene->item(grid)->name == "Smoke grid");
    auto &smoke = std::get<SmokeGrid>(document.scene->item(grid)->data);
    smoke.position = {1.0F, 2.0F, -0.5F};
    smoke.size = {1.0F, 1.5F, 2.0F};
    smoke.cell = 0.25F;
    smoke.rises = 1.5F;
    smoke.sinks = 0.1F;
    smoke.swirl = 0.3F;
    smoke.fades = 0.4F;
    smoke.calms = 0.05F;
    smoke.solve = 12U;
    smoke.thickness = 2.5F;
    smoke.color = {0.2F, 0.4F, 0.6F, 1.0F};
    smoke.brightness = 0.7F;
    CHECK((grid_resolution(smoke) == std::array<std::uint32_t, 3>{4U, 6U, 8U}) && grid_cells(smoke) == 192U);
    // Born inside the box, so the grid is fed.
    std::get<ParticleSystem>(document.scene->item(embers)->data).position = smoke.position;
    std::get<ParticleSystem>(document.scene->item(ash)->data).position = smoke.position;

    // One system feeds and rides; another only rides. Once each, and only a
    // grid can be fed or ridden.
    const Uid feed = add_relationship(document, RelationKind::FeedSmoke, grid, embers);
    const Uid ride = add_relationship(document, RelationKind::RideSmoke, grid, embers);
    const Uid ride_ash = add_relationship(document, RelationKind::RideSmoke, grid, ash);
    CHECK(feed != 0U && ride != 0U && ride_ash != 0U);
    CHECK(add_relationship(document, RelationKind::FeedSmoke, grid, embers) == 0U);
    CHECK(add_relationship(document, RelationKind::FeedSmoke, embers, ash) == 0U);
    CHECK(add_relationship(document, RelationKind::EmitOnHit, grid, ash) == 0U);
    CHECK(add_relationship(document, RelationKind::TrailFromSystem, grid, ash) == 0U);
    const auto rule = [&document](Uid uid) -> Relationship & {
        for (Relationship &r : document.scene->relationships) {
            if (r.uid == uid) {
                return r;
            }
        }
        throw std::runtime_error{"no rule"};
    };
    rule(feed).density = 3.0F;
    rule(feed).heat = 0.75F;
    rule(feed).push = 0.5F;
    rule(ride).strength = 4.0F;
    CHECK(validate_scene(document).empty());
    CHECK(relationship_active(*document.scene, rule(feed)));
    // Riding smoke gives a system no particles: it still needs a source.
    auto &ash_system = std::get<ParticleSystem>(document.scene->item(ash)->data);
    const std::vector<ParticleSource> ash_sources = ash_system.sources;
    ash_system.sources.clear();
    CHECK(!validate_scene(document).empty());
    ash_system.sources = ash_sources;
    CHECK(validate_scene(document).empty());

    // Lowering: the grid is a layer of one block (so it has handles), and
    // the bindings are the runtime's modules naming its volume.
    Document lowered = lower_scene(document);
    CHECK(lowered.layers.size() == 3U);
    const Block *box = lowered.block(grid + 1U);
    CHECK(box && box->kind == "smoke_grid" && get(*box, "size") == vec3(1.0F, 1.5F, 2.0F) &&
          get(*box, "center").x == 1.0F && get(*box, "solve").x == 12.0F && get(*box, "cell").x == 0.25F);
    CHECK(handle_block(document, grid) == grid + 1U);
    const Block *feeding = lowered.block(feed);
    CHECK(feeding && feeding->raw);
    const auto *source = feeding ? std::get_if<ajfx::VolumeSource>(&std::get<ajfx::UpdateModule>(*feeding->raw)) : nullptr;
    CHECK(source && source->volume == smoke_grid_volume(grid) && source->density.value == 3.0F &&
          source->temperature.value == 0.75F && source->velocity.value == 0.5F);
    const Block *riding = lowered.block(ride_ash);
    const auto *advect = riding && riding->raw ? std::get_if<ajfx::VolumeAdvect>(&std::get<ajfx::UpdateModule>(*riding->raw)) : nullptr;
    CHECK(advect && advect->volume == smoke_grid_volume(grid) && advect->strength.value == 10.0F);
    // A handle drag on the box writes back to the item.
    lowered.block(grid + 1U)->set("center", vec3(3.0F, 2.0F, -0.5F));
    lowered.block(grid + 1U)->set("size", vec3(1.0F, 1.5F, 4.0F));
    CHECK(adopt_handle_edit(document, lowered, grid));
    CHECK(smoke.position.x == 3.0F && smoke.size.z == 4.0F);
    smoke.position.x = 1.0F;
    smoke.size.z = 2.0F;

    // Compiled: one volume, scaled, with the authored values; the grid
    // builds no emitter of its own.
    const Compiled compiled = ready(document);
    CHECK(compiled.effect->volumes.size() == 1U && compiled.effect->emitters.size() == 2U);
    const ajfx::Volume &v = compiled.effect->volumes.front();
    CHECK(v.name == smoke_grid_volume(grid) && v.nx == 4U && v.ny == 6U && v.nz == 8U && near(v.cell_size, 0.5F));
    CHECK(near(v.origin.x, 1.0F) && near(v.origin.y, 2.5F) && near(v.origin.z, -3.0F));
    CHECK(v.buoyancy == 1.5F && v.weight == 0.1F && v.vorticity == 0.3F && v.pressure_iterations == 12U);
    CHECK(v.density_dissipation == 0.4F && v.temperature_dissipation == 0.4F && v.velocity_dissipation == 0.05F);
    CHECK(v.extinction == 2.5F && v.albedo == 0.7F && v.color.x == 0.2F && v.color.y == 0.4F && v.color.z == 0.6F);
    CHECK(ajfx::validate(*compiled.effect).ok());
    const LayerReport *grid_report = compiled.layer(grid);
    CHECK(grid_report && grid_report->included && grid_report->emitter.empty() && !grid_report->baked);
    const ajfx::Emitter *fed = nullptr;
    const ajfx::Emitter *carried = nullptr;
    for (const ajfx::Emitter &emitter : compiled.effect->emitters) {
        if (emitter.name == compiled.layer(embers)->emitter) { fed = &emitter; }
        if (emitter.name == compiled.layer(ash)->emitter) { carried = &emitter; }
    }
    CHECK(fed && carried);
    const auto *fed_source = find_module<ajfx::VolumeSource>(*fed);
    const auto *fed_advect = find_module<ajfx::VolumeAdvect>(*fed);
    CHECK(fed_source && fed_source->density.value == 3.0F && fed_source->temperature.value == 0.75F &&
          fed_source->velocity.value == 0.5F);
    CHECK(fed_advect && fed_advect->strength.value == 4.0F);
    CHECK(find_module<ajfx::VolumeSource>(*carried) == nullptr && find_module<ajfx::VolumeAdvect>(*carried) != nullptr);
    // Exported, the package carries the volume.
    const auto bytes = ajfx::write_package(*compiled.effect, asset_entries(compiled));
    CHECK(bytes);
    const auto package = ajfx::read_package(bytes.value());
    CHECK(package && package.value().volumes == compiled.effect->volumes);

    // Played, the grid takes smoke from the particles born in it.
    auto run = simulation(compiled);
    run.advance_to(60U);
    float density = 0.0F;
    for (const float d : run.volume(0).density) {
        density += d;
    }
    CHECK(density > 0.0F);

    // Baking: the system feeding smoke stays live, said so, and the bake
    // still takes the rest.
    const std::vector<ajfx::CacheMode> modes = bake_modes(document, compiled);
    CHECK(modes.size() == 2U);
    for (std::size_t e = 0U; e < compiled.effect->emitters.size(); ++e) {
        CHECK(modes[e] == (&compiled.effect->emitters[e] == fed ? ajfx::CacheMode::Live : ajfx::CacheMode::Particles));
    }
    const BakeStatus status = bake_status(document, compiled, nullptr);
    CHECK(status.any_baked && !status.current);
    CHECK(status.layer(embers) && status.layer(embers)->state == BakeState::Live &&
          status.layer(embers)->why.find("feeds smoke") != std::string::npos);
    CHECK(status.layer(ash) && status.layer(ash)->state == BakeState::Unbaked);
    CHECK(status.layer(grid) && status.layer(grid)->state == BakeState::Live);
    const ajfx::Result<BakeRecord> baked = bake_page(document, compiled, 71U, nullptr);
    CHECK(baked);
    CHECK(bake_status(document, compiled, &baked.value()).current);

    // Statistics count the grid and its cells.
    const SceneStats stats = scene_stats(*document.scene, &compiled, nullptr);
    CHECK(stats.grids == 1U && stats.grid_cells == 192U && stats.largest_grid_cells == 192U && stats.near_limits.empty());
    const std::vector<std::string> lines = stat_lines(stats);
    CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string &line) { return line == "Smoke: 1 grid, 192 cells, 1 MiB on the GPU"; }));
    CHECK(lines.back().find("1 smoke grid") != std::string::npos);

    // Saved and read back exactly; unknown fields refused; a rule saved
    // before the smoke amounts existed keeps their defaults.
    const std::string saved = save_document(document);
    const Loaded loaded = load_document(saved);
    if (!loaded.document) {
        throw std::runtime_error{loaded.error};
    }
    CHECK(*loaded.document == document);
    CHECK(save_document(*loaded.document) == saved);
    std::string bad = saved;
    bad.insert(bad.find("\"thickness\""), "\"invented\": 1, ");
    CHECK(!load_document(bad).document);
    bad = saved;
    bad.replace(bad.find("\"kind\": 3"), 9U, "\"kind\": 7");
    CHECK(!load_document(bad).document);
    std::string older = saved;
    const std::size_t at = older.find("\"density\": 3");
    CHECK(at != std::string::npos);
    older.erase(at, older.find("\"heat\"", at) - at);
    const Loaded old_rules = load_document(older);
    CHECK(old_rules.document && *old_rules.document != document && old_rules.document->scene->relationships.size() == 3U);

    // A grid that is switched off is not on the effect, and nothing feeds
    // or rides it.
    document.scene->item(grid)->simulate = false;
    CHECK(!relationship_active(*document.scene, rule(feed)));
    const Compiled off = compile(document, Purpose::Export);
    CHECK(off.effect && off.effect->volumes.empty());
    for (const ajfx::Emitter &emitter : off.effect->emitters) {
        CHECK(find_module<ajfx::VolumeSource>(emitter) == nullptr && find_module<ajfx::VolumeAdvect>(emitter) == nullptr);
    }
    document.scene->item(grid)->simulate = true;

    // The runtime's budgets, named with the item and the limit before the
    // compiler is reached; storage keeps the unfinished grid.
    smoke.cell = 0.01F;
    smoke.size.x = 10.0F;
    std::string error = validate_scene(document);
    CHECK(error.find("Smoke grid") == 0U && error.find("width is 1000 cells") != std::string::npos &&
          error.find(std::to_string(ajfx::max_volume_size)) != std::string::npos);
    CHECK(validate_scene(document, false).empty());
    CHECK(!compile(document, Purpose::Export).effect);
    smoke.size = {2.5F, 2.5F, 2.5F};
    error = validate_scene(document);
    CHECK(error.find("15,625,000") == std::string::npos && error.find("15625000 cells") != std::string::npos &&
          error.find(std::to_string(ajfx::max_volume_cells)) != std::string::npos);
    const SceneStats crowded = scene_stats(*document.scene, nullptr, nullptr);
    CHECK(crowded.near_limits.size() == 1U && crowded.near_limits.front().rfind("Cells in one smoke grid: ", 0U) == 0U);
    smoke.cell = 0.25F;
    smoke.size = {1.0F, 1.5F, 2.0F};
    CHECK(validate_scene(document).empty());

    // Removing the grid removes the rules on it; a rule left pointing at
    // nothing is diagnosed.
    CHECK(remove_item(document, grid));
    CHECK(document.scene->relationships.empty());
    CHECK(validate_scene(document).empty());
    document.scene->relationships.push_back(Relationship{.uid = feed, .kind = RelationKind::FeedSmoke, .object = grid, .system = embers});
    CHECK(validate_scene(document).find("its smoke grid is missing") != std::string::npos);
}

// The live values a preview hands its placement, by the package's parameter
// order: what the renderer does with a placement's parameters.
class LiveValues final : public ajfx::ParameterProvider {
public:
    LiveValues(const ajfx::Effect &effect, const std::vector<LiveParameter> &live) {
        for (const LiveParameter &value : live) {
            for (std::size_t i = 0U; i < effect.parameters.size(); ++i) {
                if (effect.parameters[i].name == value.name) {
                    values_.emplace_back(i, value.value);
                }
            }
        }
    }
    void values_at(std::uint64_t, std::span<ajfx::ParameterValue> values) const override {
        for (const auto &[index, value] : values_) {
            values[index] = value;
        }
    }
    [[nodiscard]] std::size_t size() const { return values_.size(); }

private:
    std::vector<std::pair<std::size_t, ajfx::ParameterValue>> values_;
};

std::size_t emitter_named(const ajfx::Effect &effect, Uid item) {
    const std::string name = "item_" + std::to_string(item);
    for (std::size_t e = 0U; e < effect.emitters.size(); ++e) {
        if (effect.emitters[e].name == name) {
            return e;
        }
    }
    throw std::runtime_error{"no emitter " + name};
}

bool same_particles(const ajfx::EmitterState &a, const ajfx::EmitterState &b) {
    return a.alive() == b.alive() && a.position == b.position && a.velocity == b.velocity;
}

// A preview built with a system live reads its Motion values from the
// placement: given the values as an edit leaves them, it plays exactly as a
// preview built after the edit, so dragging one needs no new package. Any
// other change does.
void live_values_play_as_a_rebuilt_preview() {
    Document document = new_scene();
    const Uid sparks = add_particle_system(document);
    {
        auto &system = std::get<ParticleSystem>(document.scene->item(sparks)->data);
        system.sources.clear();
        system.lifetime = {2.0F, 2.0F};
        system.speed = {1.0F, 3.0F};
        system.spread = 20.0F;
        system.radius = {0.0F, 0.2F};
        system.collide = true;
    }
    CHECK(add_source(document, sparks, SourceMode::Burst) != 0U);
    std::get<ParticleSystem>(document.scene->item(sparks)->data).sources.back().count = 200U;
    CHECK(add_behavior(document, sparks, BehaviorKind::Drag) != 0U);
    CHECK(add_behavior(document, sparks, BehaviorKind::Vortex) != 0U);
    CHECK(validate_scene(document).empty());

    const Compiled live = compile(document, Purpose::Preview, sparks);
    CHECK(live.effect && live.error.empty());
    const std::vector<LiveParameter> before = live_parameters(document, sparks);
    // speed, spread, radius, gravity and two behaviors in two halves, and a direction.
    CHECK(before.size() == 13U);
    for (const LiveParameter &value : before) {
        CHECK(std::any_of(live.effect->parameters.begin(), live.effect->parameters.end(),
                          [&](const ajfx::ParameterDesc &p) { return p.name == value.name; }));
    }

    Document edited = document;
    {
        auto &system = std::get<ParticleSystem>(edited.scene->item(sparks)->data);
        system.speed = {0.5F, 4.0F};
        system.spread = 35.0F;
        system.direction = {1.0F, 2.0F, 0.0F};
        system.radius = {0.1F, 0.3F};
        system.gravity = 0.4F;
        system.motion[0].strength = {0.5F, 0.8F};
        system.motion[1].strength = {2.0F, 3.0F};
    }
    CHECK(same_but_live(document, edited, sparks));
    Document longer = edited;
    std::get<ParticleSystem>(longer.scene->item(sparks)->data).lifetime = {2.5F, 2.5F};
    CHECK(!same_but_live(document, longer, sparks));
    CHECK(!same_but_live(document, edited, document.scene->items.front().uid));

    const Compiled rebuilt = compile(edited, Purpose::Preview);
    CHECK(rebuilt.effect && rebuilt.error.empty());
    const LiveValues values{*live.effect, live_parameters(edited, sparks)};
    CHECK(values.size() == before.size());
    auto solver = ajfx::rigid::make_jolt_solver();
    ajfx::Simulation played = simulation(live, solver.get());
    ajfx::Simulation reference = simulation(rebuilt, solver.get());
    played.reset(&values);
    played.advance_to(90U, &values);
    reference.advance_to(90U);
    const ajfx::EmitterState &moved = played.emitter(emitter_named(*live.effect, sparks));
    CHECK(moved.alive() == 200U);
    CHECK(same_particles(moved, reference.emitter(emitter_named(*rebuilt.effect, sparks))));
    // And the values the package was built with are its defaults: without
    // any, it plays as the scene it was built of.
    ajfx::Simulation unedited = simulation(live, solver.get());
    ajfx::Simulation original = simulation(compile(document, Purpose::Preview), solver.get());
    unedited.advance_to(90U);
    original.advance_to(90U);
    CHECK(same_particles(unedited.emitter(emitter_named(*live.effect, sparks)),
                         original.emitter(emitter_named(*live.effect, sparks))));
    CHECK(!same_particles(unedited.emitter(emitter_named(*live.effect, sparks)), moved));
}

// The selected system's paths are traced first from focus_scene: the scene
// without the systems it is not born of. Its particles there do exactly what
// they do in the whole scene.
void a_focus_scene_moves_its_system_as_the_whole_scene_does() {
    Document document = new_scene();
    const Uid sparks = add_particle_system(document);
    {
        auto &system = std::get<ParticleSystem>(document.scene->item(sparks)->data);
        system.sources.clear();
        system.lifetime = {3.0F, 3.0F};
        system.speed = {2.0F, 4.0F};
        system.spread = 25.0F;
        system.collide = true;
    }
    CHECK(add_source(document, sparks, SourceMode::Burst) != 0U);
    const Uid smoke = add_particle_system(document);
    {
        auto &system = std::get<ParticleSystem>(document.scene->item(smoke)->data);
        system.sources.clear();
        system.lifetime = {1.0F, 1.5F};
        system.speed = {0.0F, 0.3F};
    }
    CHECK(add_relationship(document, RelationKind::TrailFromSystem, sparks, smoke) != 0U);
    const Uid dust = add_particle_system(document);
    CHECK(validate_scene(document).empty());

    const Document smoke_alone = focus_scene(document, smoke);
    CHECK(smoke_alone.scene->item(smoke)->simulate && smoke_alone.scene->item(sparks)->simulate);
    CHECK(!smoke_alone.scene->item(dust)->simulate);
    CHECK(smoke_alone.scene->items.front().simulate); // the ground stays
    const Document sparks_alone = focus_scene(document, sparks);
    CHECK(!sparks_alone.scene->item(smoke)->simulate && !sparks_alone.scene->item(dust)->simulate);

    auto solver = ajfx::rigid::make_jolt_solver();
    const Compiled whole = compile(document, Purpose::Preview);
    CHECK(whole.effect && whole.error.empty());
    ajfx::Simulation everything = simulation(whole, solver.get());
    everything.advance_to(150U);
    for (const auto &[focus, alone] : {std::pair{smoke, smoke_alone}, std::pair{sparks, sparks_alone}}) {
        const Compiled part = compile(alone, Purpose::Preview);
        CHECK(part.effect && part.error.empty());
        CHECK(part.effect->emitters.size() < whole.effect->emitters.size());
        ajfx::Simulation run = simulation(part, solver.get());
        run.advance_to(150U);
        const ajfx::EmitterState &mine = run.emitter(emitter_named(*part.effect, focus));
        CHECK(mine.alive() > 0U);
        CHECK(same_particles(mine, everything.emitter(emitter_named(*whole.effect, focus))));
    }
}

void add_scene_tests(std::vector<studio_test::Case> &cases) {
    cases.push_back({"live_values_play_as_a_rebuilt_preview", live_values_play_as_a_rebuilt_preview});
    cases.push_back({"a_focus_scene_moves_its_system_as_the_whole_scene_does",
                     a_focus_scene_moves_its_system_as_the_whole_scene_does});
    cases.push_back({"smoke_grids_are_fed_and_ridden_by_systems", smoke_grids_are_fed_and_ridden_by_systems});
    cases.push_back({"a_bound_system_is_born_only_of_its_object", a_bound_system_is_born_only_of_its_object});
    cases.push_back({"particles_meet_static_objects", particles_meet_static_objects});
    cases.push_back({"statistics_count_the_scene_and_name_limits_in_reach", statistics_count_the_scene_and_name_limits_in_reach});
    cases.push_back({"timeline_tracks_are_the_scene_moments", timeline_tracks_are_the_scene_moments});
    cases.push_back({"relationships_bind_an_object_to_a_system", relationships_bind_an_object_to_a_system});
    cases.push_back({"a_system_trails_another_systems_particles", a_system_trails_another_systems_particles});
    cases.push_back({"forces_are_scene_items_that_act_on_their_targets", forces_are_scene_items_that_act_on_their_targets});
    cases.push_back({"motion_stack_and_age_drivers_reach_the_runtime", motion_stack_and_age_drivers_reach_the_runtime});
    cases.push_back({"new_scene_ground_catches_objects_and_can_be_removed", new_scene_ground_catches_objects_and_can_be_removed});
    cases.push_back({"a_dynamic_object_starts_where_it_is_placed", a_dynamic_object_starts_where_it_is_placed});
    cases.push_back({"typed_scene_roundtrip_and_strict_fields", typed_scene_roundtrip_and_strict_fields});
    cases.push_back({"independent_surfaces_share_one_colliding_world", independent_surfaces_share_one_colliding_world});
    cases.push_back(
        {"organization_and_rename_do_not_change_simulation", organization_and_rename_do_not_change_simulation});
    cases.push_back({"initial_population_is_bounded_and_persistent", initial_population_is_bounded_and_persistent});
    cases.push_back(
        {"ten_distinct_models_fracture_without_losing_owners", ten_distinct_models_fracture_without_losing_owners});
}
