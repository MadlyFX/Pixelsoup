#include "test_support.hpp"

#include "scene.hpp"

#include <ajfx/package.hpp>

#include <cstdlib>
#include <random>

// The scene compiler: every starter scene compiles whole, and over many
// generated scenes -- every kind of item, Motion behavior, force, rule and
// Object setting, at random -- the effect is a function of the scene alone.
namespace {

using namespace studio_test;
using namespace ajfx;

Simulation simulation(const Compiled& compiled, const RigidSolver* solver) {
    auto created =
        Simulation::create(*compiled.effect, 71U, SimulationOptions{.meshes = compiled.meshes, .rigid = solver});
    if (!created) {
        throw std::runtime_error{created.error().message};
    }
    return std::move(created).value();
}

class Dice final {
public:
    explicit Dice(std::uint32_t seed) : rng_{seed} {}
    float real(float lo, float hi) { return std::uniform_real_distribution<float>{lo, hi}(rng_); }
    int whole(int lo, int hi) { return std::uniform_int_distribution<int>{lo, hi}(rng_); }
    bool coin(float p = 0.5F) { return real(0.0F, 1.0F) < p; }
    template <class T>
    const T& pick(const std::vector<T>& from) {
        return from[static_cast<std::size_t>(whole(0, static_cast<int>(from.size()) - 1))];
    }
    Vec3 point(float spread) { return Vec3{real(-spread, spread), real(0.0F, spread), real(-spread, spread)}; }
    Range range(float lo, float hi) {
        const float a = real(lo, hi);
        const float b = coin(0.3F) ? a : real(a, hi);
        return Range{a, b};
    }
    std::uint32_t seed() { return coin(0.2F) ? 0U : coin(0.1F) ? 20000000U + static_cast<std::uint32_t>(whole(0, 99)) : static_cast<std::uint32_t>(whole(1, 400)); }

private:
    std::mt19937 rng_;
};

Document random_scene(Dice& dice) {
    Document document = empty_scene();
    Scene& scene = *document.scene;
    document.scale = dice.pick(std::vector<float>{1.0F, 1.0F, 0.5F, 2.25F});
    scene.physics.gravity = dice.pick(std::vector<float>{1.0F, 1.0F, 0.0F, 0.4F});
    scene.physics.angular_damping = dice.real(0.0F, 0.5F);
    scene.physics.allow_sleep = dice.coin(0.8F);
    scene.physics.max_dropped = dice.pick(std::vector<std::uint32_t>{64U, 0U, 12U});

    std::vector<Uid> objects;
    std::vector<Uid> systems;
    std::vector<Uid> grids;
    const int object_count = dice.whole(0, 4);
    for (int i = 0; i < object_count; ++i) {
        const std::string mesh =
            dice.pick(std::vector<std::string>{"cube", "slab", "rock", "shard", "floor", "models/crate.glb", "cube"});
        const Uid uid = mesh == "floor" ? add_floor(document) : add_object(document, mesh);
        SceneItem& item = *scene.item(uid);
        item.seed = dice.seed();
        item.simulate = dice.coin(0.9F);
        auto& o = std::get<Object>(item.data);
        o.position = dice.point(2.0F);
        o.rotation = Vec3{dice.real(-90.0F, 90.0F), dice.real(-180.0F, 180.0F), 0.0F};
        o.size = dice.real(0.2F, 2.0F);
        o.tint = Color{dice.real(0.0F, 1.0F), dice.real(0.0F, 1.0F), dice.real(0.0F, 1.0F), 1.0F};
        if (mesh != "floor") {
            o.motion = dice.pick(std::vector<ObjectMotion>{ObjectMotion::Dynamic, ObjectMotion::Dynamic,
                                                            ObjectMotion::Static, ObjectMotion::Animated});
            o.collider = dice.pick(std::vector<ObjectCollider>{ObjectCollider::Hull, ObjectCollider::Sphere,
                                                                ObjectCollider::Box});
            if (o.motion == ObjectMotion::Animated && o.collider == ObjectCollider::Hull) {
                o.collider = ObjectCollider::Box;
            }
            if (o.motion == ObjectMotion::Dynamic) {
                o.distribution = dice.pick(std::vector<Distribution>{Distribution::Single, Distribution::Grid,
                                                                      Distribution::Pile, Distribution::Placed});
                o.count = static_cast<std::uint32_t>(dice.whole(2, 30));
                o.fracture = dice.coin(0.3F);
                o.fragments = static_cast<std::uint32_t>(dice.whole(2, 16));
            }
        }
        o.density = dice.real(100.0F, 5000.0F);
        o.friction = dice.real(0.0F, 1.0F);
        o.restitution = dice.real(0.0F, 1.0F);
        o.velocity = dice.coin() ? Vec3{} : dice.point(3.0F);
        o.release = dice.coin() ? 0.0F : dice.real(0.0F, 2.0F);
        o.move_to = dice.point(2.0F);
        o.turn_to = Vec3{0.0F, dice.real(0.0F, 90.0F), 0.0F};
        o.move_at = dice.real(0.0F, 2.0F);
        o.move_over = dice.real(0.1F, 2.0F);
        o.width = dice.real(0.5F, 3.0F);
        o.depth = dice.real(0.5F, 3.0F);
        o.spacing = dice.real(0.8F, 1.5F);
        o.variation = dice.real(0.0F, 1.0F);
        o.fracture_speed = dice.real(1.0F, 10.0F);
        objects.push_back(uid);
    }
    const int system_count = dice.whole(0, 3);
    for (int i = 0; i < system_count; ++i) {
        const Uid uid = add_particle_system(document);
        SceneItem& item = *scene.item(uid);
        item.seed = dice.seed();
        item.simulate = dice.coin(0.9F);
        auto& p = std::get<ParticleSystem>(item.data);
        p.position = dice.point(2.0F);
        p.radius = dice.range(0.0F, 1.0F);
        p.sources.clear();
        const int sources = dice.whole(0, 3);
        for (int k = 0; k < sources; ++k) {
            const SourceMode mode =
                dice.pick(std::vector<SourceMode>{SourceMode::Continuous, SourceMode::Burst, SourceMode::Initial});
            const Uid source = add_source(document, uid, mode);
            for (ParticleSource& s : std::get<ParticleSystem>(scene.item(uid)->data).sources) {
                if (s.uid == source) {
                    s.count = static_cast<std::uint32_t>(dice.whole(1, 400));
                    s.rate = dice.real(1.0F, 200.0F);
                    s.time = dice.real(0.0F, 3.0F);
                }
            }
        }
        auto& q = std::get<ParticleSystem>(scene.item(uid)->data);
        q.persistent = dice.coin(0.1F) && std::all_of(q.sources.begin(), q.sources.end(), [](const ParticleSource& s) {
                           return s.mode != SourceMode::Continuous;
                       });
        q.lifetime = dice.range(0.2F, 5.0F);
        q.direction = dice.coin(0.1F) ? Vec3{} : dice.point(1.0F);
        q.speed = dice.range(0.0F, 6.0F);
        q.spread = dice.real(0.0F, 180.0F);
        q.gravity = dice.real(0.0F, 2.0F);
        const int behaviors = dice.whole(0, 4);
        for (int k = 0; k < behaviors; ++k) {
            const BehaviorKind kind =
                dice.pick(std::vector<BehaviorKind>{BehaviorKind::Drag, BehaviorKind::Turbulence, BehaviorKind::Vortex,
                                                    BehaviorKind::Wind, BehaviorKind::Attract, BehaviorKind::DrawIn});
            static_cast<void>(add_behavior(document, uid, kind));
        }
        auto& r = std::get<ParticleSystem>(scene.item(uid)->data);
        for (MotionBehavior& b : r.motion) {
            b.enabled = dice.coin(0.85F);
            b.strength = dice.range(0.0F, 4.0F);
            b.size = dice.real(0.1F, 2.0F);
            b.churn = dice.real(0.0F, 2.0F);
            b.center = dice.point(2.0F);
            b.axis = dice.coin() ? VortexAxis::Vertical : VortexAxis::Horizontal;
            b.heading = dice.real(0.0F, 360.0F);
            b.reach = dice.real(0.0F, 3.0F);
            b.seed = dice.seed();
        }
        r.collide = dice.coin(0.8F);
        r.tint = Color{dice.real(0.0F, 1.0F), dice.real(0.0F, 1.0F), dice.real(0.0F, 1.0F), dice.real(0.5F, 1.0F)};
        r.brightness = dice.range(0.0F, 3.0F);
        r.size = dice.range(0.01F, 0.3F);
        r.spin = dice.coin() ? Range{} : dice.range(0.0F, 200.0F);
        r.look = dice.pick(std::vector<ParticleLook>{ParticleLook::Glow, ParticleLook::Smoke, ParticleLook::Solid});
        r.lighting = dice.coin() ? ParticleLighting::Glows : ParticleLighting::Lit;
        r.curvature = dice.real(0.0F, 1.0F);
        r.wrap = dice.real(0.0F, 1.0F);
        r.ambient = dice.real(0.0F, 2.0F);
        r.shadowed = dice.coin();
        r.casts_light = dice.coin(0.3F);
        r.light_count = static_cast<std::uint32_t>(dice.whole(1, 16));
        r.light_strength = dice.range(0.0F, 4.0F);
        r.light_reach = dice.range(0.1F, 3.0F);
        if (dice.coin(0.3F)) {
            r.size_driver = Driver::Age;
            r.size_over_age = {{0.0F, dice.real(0.0F, 2.0F)}, {1.0F, dice.real(0.0F, 2.0F)}};
        }
        if (dice.coin(0.3F)) {
            r.tint_driver = Driver::Age;
            r.tint_over_age = {{0.0F, {1.0F, 0.5F, 0.2F, 1.0F}}, {1.0F, {0.2F, 0.2F, 0.2F, 0.0F}}};
        }
        systems.push_back(uid);
    }
    const int force_count = dice.whole(0, 3);
    for (int i = 0; i < force_count; ++i) {
        const ForceKind kind = dice.pick(std::vector<ForceKind>{ForceKind::Wind, ForceKind::Vortex, ForceKind::Attract,
                                                                ForceKind::DrawIn, ForceKind::Blast});
        const Uid uid = add_force(document, kind);
        SceneItem& item = *scene.item(uid);
        item.seed = dice.seed();
        item.simulate = dice.coin(0.9F);
        auto& f = std::get<Force>(item.data);
        f.position = dice.point(2.0F);
        f.strength = dice.range(0.0F, 4.0F);
        f.heading = dice.real(0.0F, 360.0F);
        f.axis = dice.coin() ? VortexAxis::Vertical : VortexAxis::Horizontal;
        f.reach = dice.real(0.0F, 3.0F);
        f.height = dice.real(0.0F, 3.0F);
        f.power = dice.real(1.0F, 30.0F);
        f.radius = dice.real(0.5F, 6.0F);
        f.time = dice.real(0.0F, 3.0F);
        f.bodies = kind == ForceKind::Wind && dice.coin(0.5F);
        f.air_speed = dice.real(0.0F, 12.0F);
        f.gust = dice.real(0.0F, 1.0F);
        if (!systems.empty() && dice.coin(0.3F)) {
            f.all_systems = false;
            f.targets = {dice.pick(systems)};
        }
    }
    const int grid_count = dice.whole(0, 1);
    for (int i = 0; i < grid_count; ++i) {
        const Uid uid = add_smoke_grid(document);
        SceneItem& item = *scene.item(uid);
        item.simulate = dice.coin(0.85F);
        auto& g = std::get<SmokeGrid>(item.data);
        g.position = dice.point(1.0F);
        g.size = Vec3{dice.real(0.5F, 2.0F), dice.real(0.5F, 2.0F), dice.real(0.5F, 2.0F)};
        g.cell = dice.real(0.08F, 0.25F);
        g.rises = dice.real(0.0F, 3.0F);
        g.swirl = dice.real(0.0F, 3.0F);
        grids.push_back(uid);
    }
    if (dice.coin(0.3F)) {
        static_cast<void>(add_group(document));
    }
    // Rules.
    const int rule_count = systems.empty() ? 0 : dice.whole(0, 3);
    for (int i = 0; i < rule_count; ++i) {
        const RelationKind kind =
            dice.pick(std::vector<RelationKind>{RelationKind::EmitOnHit, RelationKind::TrailFrom,
                                                RelationKind::TrailFromSystem, RelationKind::FeedSmoke,
                                                RelationKind::RideSmoke});
        Uid source = 0U;
        if (kind == RelationKind::TrailFromSystem) {
            source = dice.pick(systems);
        } else if (kind == RelationKind::FeedSmoke || kind == RelationKind::RideSmoke) {
            if (grids.empty()) {
                continue;
            }
            source = dice.pick(grids);
        } else if (!objects.empty() && dice.coin(0.7F)) {
            source = dice.pick(objects);
        }
        const Uid rule = add_relationship(document, kind, source, dice.pick(systems));
        for (Relationship& r : scene.relationships) {
            if (r.uid == rule) {
                r.hardness = dice.real(0.0F, 3.0F);
                r.per_hit = dice.real(0.0F, 5.0F);
                r.per_speed = dice.real(0.0F, 6.0F);
                r.spread = dice.real(0.0F, 180.0F);
                r.min_speed = dice.real(0.0F, 2.0F);
                r.rate = dice.real(0.0F, 20.0F);
                r.pieces = dice.coin(0.3F);
                r.density = dice.real(0.0F, 2.0F);
                r.heat = dice.real(0.0F, 2.0F);
                r.push = dice.real(0.0F, 2.0F);
                r.strength = dice.real(0.0F, 20.0F);
                r.seed = dice.seed();
            }
        }
    }
    return document;
}

std::string first_difference(const std::string& a, const std::string& b) {
    std::size_t at = 0U;
    while (at < a.size() && at < b.size() && a[at] == b[at]) {
        ++at;
    }
    const std::size_t from = at > 160U ? at - 160U : 0U;
    return "first: ..." + a.substr(from, 320U) + "\nsecond: ..." + b.substr(from, 320U);
}

int setting(const char* name, int fallback) {
#pragma warning(suppress : 4996)
    const char* value = std::getenv(name);
    return value != nullptr ? std::atoi(value) : fallback;
}

// How many generated scenes each property is checked over: AJFX_SCENES sets
// it, and AJFX_SCENE_SEED the dice, to look further than the suite does.
int generated() {
    return setting("AJFX_SCENES", 600);
}

void same_effect(const Compiled& a, const Compiled& b, const std::string& what) {
    if (a.error.empty() != b.error.empty() || a.effect.has_value() != b.effect.has_value()) {
        throw std::runtime_error{what + ": one compiled and the other did not (" + a.error + " / " + b.error + ")"};
    }
    if (a.effect && !(*a.effect == *b.effect)) {
        throw std::runtime_error{what + "\n" + first_difference(effect_to_json(*a.effect), effect_to_json(*b.effect))};
    }
}

// Every starter scene compiles whole: each item it holds reported, every
// emitter checked by the runtime and holding at least what it can bear.
void every_starter_scene_compiles_whole() {
    for (const StarterScene& entry : starter_scenes()) {
        const Document document = starter(entry.id);
        const Compiled compiled = compile(document);
        if (!compiled.error.empty() || !compiled.effect) {
            throw std::runtime_error{std::string{entry.id} + ": " + compiled.error};
        }
        CHECK(validate(*compiled.effect).ok());
        for (const SceneItem& item : document.scene->items) {
            const bool reported = std::holds_alternative<Object>(item.data) ||
                                  std::holds_alternative<ParticleSystem>(item.data) ||
                                  std::holds_alternative<SmokeGrid>(item.data);
            CHECK((compiled.item(item.uid) != nullptr) == reported);
            if (reported && item.simulate) {
                CHECK(compiled.item(item.uid)->included);
            }
        }
        for (const Emitter& emitter : compiled.effect->emitters) {
            CHECK(emitter.capacity >= particle_bound(*compiled.effect, emitter));
        }
    }
}

// The effect is a function of the scene alone: compiled twice it is the
// same, the tree's order and the items' names change nothing, and an edit to
// one particle system leaves every other emitter exactly as it was.
void the_effect_depends_on_the_scene_alone() {
    Dice dice{static_cast<std::uint32_t>(setting("AJFX_SCENE_SEED", 20261105))};
    int edited = 0;
    for (int i = 0; i < generated(); ++i) {
        Document document = random_scene(dice);
        const Compiled first = compile(document);
        same_effect(first, compile(document), "scene " + std::to_string(i) + " compiled twice");
        Document shuffled = document;
        std::reverse(shuffled.scene->items.begin(), shuffled.scene->items.end());
        for (SceneItem& item : shuffled.scene->items) {
            item.name += " moved";
        }
        same_effect(first, compile(shuffled), "scene " + std::to_string(i) + " in another order");
        if (!first.effect) {
            continue;
        }
        // A new seed for one system deals its own particles afresh.
        for (SceneItem& item : document.scene->items) {
            if (!std::holds_alternative<ParticleSystem>(item.data) || !item.simulate) {
                continue;
            }
            item.seed += 1000U;
            const Compiled after = compile(document);
            CHECK(after.effect.has_value() && after.effect->emitters.size() == first.effect->emitters.size());
            const std::string mine = "item_" + std::to_string(item.uid);
            for (std::size_t e = 0U; e < first.effect->emitters.size(); ++e) {
                const Emitter& before = first.effect->emitters[e];
                CHECK((before == after.effect->emitters[e]) == (before.name != mine));
            }
            ++edited;
            break;
        }
    }
    CHECK(edited > generated() / 4);
}

// Scale multiplies distances, sizes, speeds and accelerations, and leaves
// counts and rates alone.
void scale_scales_the_whole_scene() {
    Document document = starter("campfire");
    const Compiled one = compile(document);
    document.scale = 2.0F;
    const Compiled two = compile(document);
    const Uid flames = item_named(document, "Flames");
    const Emitter& small = emitter_of(one, flames);
    const Emitter& large = emitter_of(two, flames);
    CHECK(near(find_module<ShapeLocation>(large)->center.value.y, 2.0F * find_module<ShapeLocation>(small)->center.value.y));
    CHECK(near(find_module<ShapeLocation>(large)->radius_max.value, 2.0F * find_module<ShapeLocation>(small)->radius_max.value));
    CHECK(near(find_module<Velocity>(large)->speed_max.value, 2.0F * find_module<Velocity>(small)->speed_max.value));
    CHECK(near(find_module<InitSize>(large)->max.value, 2.0F * find_module<InitSize>(small)->max.value));
    CHECK(find_module<SpawnRate>(large)->rate.value == find_module<SpawnRate>(small)->rate.value);
    CHECK(find_module<Lifetime>(large)->max.value == find_module<Lifetime>(small)->max.value);
}

// A live system's Motion values are parameters of the effect, at the values
// the scene holds, so the preview changes them without a new package.
void a_live_system_publishes_its_motion() {
    const Document document = starter("campfire");
    const Uid flames = item_named(document, "Flames");
    const Compiled plain = compile(document);
    CHECK(plain.effect->parameters.empty() && plain.item(flames)->baked);
    const Compiled live = compile(document, flames);
    CHECK(live.error.empty() && !live.item(flames)->baked);
    const std::vector<LiveParameter> values = live_parameters(document, flames);
    CHECK(!values.empty() && values.size() == live.effect->parameters.size());
    for (const LiveParameter& value : values) {
        const ParameterDesc* declared = parameter_of(live, value.name);
        CHECK(declared != nullptr);
        for (std::uint32_t c = 0U; c < value.components; ++c) {
            CHECK(declared->default_value[c] == value.value[c]);
        }
    }
    // Played at those values, it plays as the plain effect does.
    auto solver = rigid::make_jolt_solver();
    auto a = simulation(plain, solver.get());
    auto b = simulation(live, solver.get());
    a.advance_to(45U);
    b.advance_to(45U);
    const std::size_t at = static_cast<std::size_t>(
        std::find_if(plain.effect->emitters.begin(), plain.effect->emitters.end(),
                     [&](const Emitter& e) { return e.name == plain.item(flames)->emitter; }) -
        plain.effect->emitters.begin());
    CHECK(a.emitter(at).alive() == b.emitter(at).alive() && a.emitter(at).position == b.emitter(at).position);
}

// What only the old layer-and-block compiler refused: a system with two draw-in forces,
// and a scene whose switched-off system trails a system that comes after it.
void scenes_compile_without_the_old_layer_limits() {
    Document document = starter("magic");
    const Uid second = add_force(document, ForceKind::DrawIn);
    std::get<Force>(document.scene->item(second)->data).position = {1.0F, 0.0F, 0.0F};
    const Compiled drawn = compile(document);
    CHECK(drawn.error.empty());
    CHECK(modules_of<Converge>(emitter_of(drawn, item_named(document, "Sparkles"))).size() == 2U);

    Document trails = starter("campfire");
    const Uid off = add_particle_system(trails);
    const Uid later = add_particle_system(trails);
    CHECK(add_relationship(trails, RelationKind::TrailFromSystem, later, off) != 0U);
    trails.scene->item(off)->simulate = false;
    const Compiled compiled = compile(trails);
    CHECK(compiled.error.empty() && compiled.effect && !compiled.item(off)->included &&
          compiled.item(later)->included);
}

// A scene that cannot play whole does not play, and says which item is at
// fault.
void a_scene_that_cannot_play_names_the_item_at_fault() {
    Document document = starter("wall");
    const Uid wall = item_named(document, "Wall");
    std::get<Object>(document.scene->item(wall)->data).motion = ObjectMotion::Static;
    const Compiled compiled = compile(document);
    CHECK(!compiled.effect && compiled.error.starts_with("Wall"));
}

} // namespace

#if AJFX_STUDIO_TEST_KERNELS
bool kernels_compile_for_every_starter_scene() {
    using namespace studio_test;
    ajfx::Result<std::unique_ptr<ajfx::gpu::ShaderCompiler>> compiler = ajfx::d3d12::create_shader_compiler();
    if (!compiler) {
        std::cout << "[skip] kernels_compile_for_every_starter_scene: " << compiler.error().message << '\n';
        return false;
    }
    for (const StarterScene& entry : starter_scenes()) {
        const Compiled compiled = compile(starter(entry.id));
        const ajfx::Result<ajfx::gpu::KernelSet> kernels =
            ajfx::gpu::compile_kernels(*compiled.effect, *compiler.value());
        if (!kernels) {
            throw std::runtime_error{std::string{entry.id} + ": " + kernels.error().message};
        }
    }
    return true;
}
#endif

void add_compile_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"every starter scene compiles whole", every_starter_scene_compiles_whole});
    cases.push_back({"the effect depends on the scene alone", the_effect_depends_on_the_scene_alone});
    cases.push_back({"scale scales the whole scene", scale_scales_the_whole_scene});
    cases.push_back({"a live system publishes its motion", a_live_system_publishes_its_motion});
    cases.push_back({"scenes compile without the old layer limits", scenes_compile_without_the_old_layer_limits});
    cases.push_back({"a scene that cannot play names the item at fault", a_scene_that_cannot_play_names_the_item_at_fault});
}
