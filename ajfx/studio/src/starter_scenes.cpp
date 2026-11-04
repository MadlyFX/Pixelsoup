#include "starter_scenes.hpp"

#include "kinds.hpp"
#include "performance.hpp"

#include <array>

namespace ajfx::studio {
namespace {

constexpr std::array<StarterScene, 10> scenes{{
    {"empty", "a floor and nothing on it"},
    {"logs", "a log that is only ever painted in, trailing smoke as it moves"},
    {"wall", "a wall that breaks when hit hard, and a heavy ball to throw at it"},
    {"sandbox", "one of every starter kind standing on the stage"},
    {"campfire", "flames, smoke and embers over a log, a breeze on the smoke"},
    {"explosion", "a crate blown apart: dust where pieces land, sparks behind them, a smoke burst"},
    {"smoke_room", "a fire feeding a smoke grid, ash riding it, round a box and a ball"},
    {"rain", "rain on a raised floor with a mist behind each drop, wind on a cardboard box"},
    {"magic", "sparkles swirled, drawn in and pulled by a driven stone"},
    {"rock_pile", "a heap of rocks beside a stack of crates, dust on every hit, wind on them all"},
}};

ParticleSystem& system_of(Document& document, Uid uid) {
    return std::get<ParticleSystem>(document.scene->item(uid)->data);
}

Object& object_of(Document& document, Uid uid) {
    return std::get<Object>(document.scene->item(uid)->data);
}

Force& force_of(Document& document, Uid uid) {
    return std::get<Force>(document.scene->item(uid)->data);
}

Relationship& rule_of(Document& document, Uid uid) {
    for (Relationship& rule : document.scene->relationships) {
        if (rule.uid == uid) {
            return rule;
        }
    }
    return document.scene->relationships.front();
}

ParticleSource& first_source(ParticleSystem& system) {
    return system.sources.front();
}

// A particle system named `name`, with one source of `mode`.
Uid system_named(Document& document, const char* name, SourceMode mode) {
    const Uid uid = add_particle_system(document);
    document.scene->item(uid)->name = name;
    first_source(system_of(document, uid)).mode = mode;
    return uid;
}

Document campfire() {
    Document document = new_scene();
    const Uid log = add_starter_kind(document, StarterKind::SmoulderingLog, Vec3{0.0F, 0.15F, 0.0F});
    static_cast<void>(add_drop(document.scene->performance, 0U, log, Vec3{0.0F, 0.15F, 0.0F}, Vec3{0.0F, 30.0F, 0.0F}, {}));

    const Uid flames = system_named(document, "Flames", SourceMode::Continuous);
    ParticleSystem& fire = system_of(document, flames);
    first_source(fire).rate = 60.0F;
    fire.position = {0.0F, 0.3F, 0.0F};
    fire.radius = {0.0F, 0.25F};
    fire.lifetime = {0.6F, 1.2F};
    fire.speed = {0.5F, 1.2F};
    fire.spread = 15.0F;
    fire.look = ParticleLook::Glow;
    fire.tint = {1.0F, 0.55F, 0.2F, 1.0F};
    fire.brightness = {2.0F, 3.0F};
    fire.size = {0.12F, 0.2F};
    fire.casts_light = true;
    fire.light_count = 4U;
    fire.size_driver = Driver::Age;
    fire.size_over_age = {{0.0F, 0.6F}, {0.3F, 1.0F}, {1.0F, 0.1F}};
    fire.tint_driver = Driver::Age;
    fire.tint_over_age = {{0.0F, {1.0F, 0.9F, 0.6F, 1.0F}}, {0.5F, {1.0F, 0.4F, 0.1F, 0.8F}},
                          {1.0F, {0.3F, 0.05F, 0.02F, 0.0F}}};
    static_cast<void>(add_behavior(document, flames, BehaviorKind::Turbulence));
    const Uid pull = add_behavior(document, flames, BehaviorKind::DrawIn);
    for (MotionBehavior& b : system_of(document, flames).motion) {
        if (b.uid == pull) {
            b.center = {0.0F, 0.3F, 0.0F};
            b.reach = 1.2F;
        }
    }

    const Uid smoke = system_named(document, "Smoke", SourceMode::Continuous);
    ParticleSystem& rising = system_of(document, smoke);
    first_source(rising).rate = 12.0F;
    rising.position = {0.0F, 1.0F, 0.0F};
    rising.lifetime = {3.0F, 5.0F};
    rising.speed = {0.4F, 0.8F};
    rising.look = ParticleLook::Smoke;
    rising.lighting = ParticleLighting::Lit;
    rising.tint = {0.35F, 0.35F, 0.38F, 0.8F};
    rising.size = {0.3F, 0.6F};
    rising.spin = {10.0F, 40.0F};
    rising.collide = false;
    static_cast<void>(add_behavior(document, smoke, BehaviorKind::Turbulence));
    static_cast<void>(add_behavior(document, smoke, BehaviorKind::Drag));

    const Uid embers = system_named(document, "Embers", SourceMode::Burst);
    ParticleSystem& sparks = system_of(document, embers);
    first_source(sparks).count = 40U;
    first_source(sparks).time = 0.4F;
    sparks.position = {0.0F, 0.3F, 0.0F};
    sparks.speed = {1.0F, 3.0F};
    sparks.spread = 40.0F;
    sparks.gravity = 0.3F;
    sparks.lifetime = {1.0F, 2.0F};
    sparks.size = {0.02F, 0.04F};
    sparks.brightness = {3.0F, 4.0F};
    static_cast<void>(add_source(document, embers, SourceMode::Continuous));
    static_cast<void>(add_behavior(document, embers, BehaviorKind::Drag));

    // A breeze on the smoke alone.
    const Uid breeze = add_force(document, ForceKind::Wind);
    Force& wind = force_of(document, breeze);
    wind.heading = 30.0F;
    wind.strength = {0.4F, 0.8F};
    wind.all_systems = false;
    wind.targets = {smoke};
    return document;
}

Document explosion() {
    Document document = new_scene();
    const Uid crate = add_object(document);
    document.scene->item(crate)->name = "Crate";
    Object& box = object_of(document, crate);
    box.position = {0.0F, 0.5F, 0.0F};
    box.size = 1.0F;
    box.density = 600.0F;
    box.fracture = true;
    box.fragments = 12U;
    box.fracture_speed = 3.0F;
    box.tint = {0.75F, 0.55F, 0.35F, 1.0F};

    const Uid bang = add_force(document, ForceKind::Blast);
    Force& blast = force_of(document, bang);
    blast.position = {0.2F, 0.2F, 0.3F};
    blast.power = 14.0F;
    blast.radius = 4.0F;
    blast.time = 0.5F;

    static_cast<void>(attach_effect(document, crate, EffectTemplate::DustOnHit));
    const Uid sparks = attach_effect(document, crate, EffectTemplate::SparksOnHit);
    static_cast<void>(sparks);
    // Sparks shed behind the pieces as they fly.
    const Uid trail = add_bound_system(document, RelationKind::TrailFrom, crate);
    for (Relationship& rule : document.scene->relationships) {
        if (rule.system == trail) {
            rule.pieces = true;
            rule.per_speed = 6.0F;
        }
    }
    ParticleSystem& streaks = system_of(document, trail);
    streaks.look = ParticleLook::Glow;
    streaks.tint = {1.0F, 0.7F, 0.3F, 1.0F};
    streaks.brightness = {2.0F, 4.0F};

    const Uid puff = system_named(document, "Smoke burst", SourceMode::Burst);
    ParticleSystem& cloud = system_of(document, puff);
    first_source(cloud).count = 80U;
    first_source(cloud).time = 0.5F;
    cloud.position = {0.0F, 0.5F, 0.0F};
    cloud.radius = {0.1F, 0.5F};
    cloud.speed = {1.0F, 4.0F};
    cloud.spread = 180.0F;
    cloud.lifetime = {2.0F, 4.0F};
    cloud.look = ParticleLook::Smoke;
    cloud.size = {0.4F, 0.9F};
    static_cast<void>(add_behavior(document, puff, BehaviorKind::Drag));
    return document;
}

Document smoke_room() {
    Document document = new_scene();
    const Uid grid = add_smoke_grid(document);
    SmokeGrid& air = std::get<SmokeGrid>(document.scene->item(grid)->data);
    air.size = {2.0F, 2.5F, 2.0F};
    air.position = {0.0F, 1.25F, 0.0F};
    air.cell = 0.125F;
    air.swirl = 1.5F;

    const Uid cube = add_object(document);
    document.scene->item(cube)->name = "Box";
    Object& still = object_of(document, cube);
    still.motion = ObjectMotion::Static;
    still.collider = ObjectCollider::Box;
    still.position = {0.6F, 0.4F, 0.0F};
    still.size = 0.8F;
    still.rotation = {0.0F, 25.0F, 0.0F};
    const Uid globe = add_object(document);
    document.scene->item(globe)->name = "Ball";
    Object& round = object_of(document, globe);
    round.motion = ObjectMotion::Static;
    round.collider = ObjectCollider::Sphere;
    round.position = {-0.6F, 0.5F, 0.3F};
    round.size = 0.6F;

    const Uid fire = system_named(document, "Fire", SourceMode::Continuous);
    ParticleSystem& flames = system_of(document, fire);
    flames.position = {0.0F, 0.2F, 0.0F};
    flames.lifetime = {0.8F, 1.4F};
    flames.brightness = {2.0F, 2.0F};
    static_cast<void>(add_relationship(document, RelationKind::FeedSmoke, grid, fire));
    const Uid ash = system_named(document, "Ash", SourceMode::Initial);
    ParticleSystem& flakes = system_of(document, ash);
    first_source(flakes).count = 200U;
    flakes.persistent = true;
    flakes.radius = {0.2F, 0.9F};
    flakes.position = {0.0F, 1.2F, 0.0F};
    flakes.speed = {0.0F, 0.1F};
    flakes.look = ParticleLook::Solid;
    flakes.size = {0.01F, 0.02F};
    static_cast<void>(add_relationship(document, RelationKind::RideSmoke, grid, ash));
    return document;
}

Document rain() {
    Document document = new_scene();
    // The floor stands a little above the ground, so its plane is drawn.
    for (SceneItem& item : document.scene->items) {
        if (auto* floor = std::get_if<Object>(&item.data); floor && floor->mesh == "floor") {
            floor->position.y = 0.1F;
            floor->restitution = 0.2F;
        }
    }
    const Uid drops = system_named(document, "Rain", SourceMode::Continuous);
    ParticleSystem& rain = system_of(document, drops);
    first_source(rain).rate = 150.0F;
    rain.position = {0.0F, 4.0F, 0.0F};
    rain.radius = {0.0F, 3.0F};
    rain.direction = {0.0F, -1.0F, 0.0F};
    rain.speed = {6.0F, 8.0F};
    rain.spread = 3.0F;
    rain.gravity = 1.0F;
    rain.lifetime = {0.8F, 1.0F};
    rain.size = {0.01F, 0.015F};
    rain.look = ParticleLook::Solid;
    const Uid mist = add_bound_system(document, RelationKind::TrailFromSystem, drops);
    ParticleSystem& wake = system_of(document, mist);
    wake.look = ParticleLook::Smoke;
    wake.tint = {0.8F, 0.85F, 0.9F, 0.3F};

    const Uid box = add_starter_kind(document, StarterKind::CardboardBox, Vec3{1.0F, 0.4F, 0.0F});
    static_cast<void>(add_drop(document.scene->performance, 0U, box, Vec3{1.0F, 0.4F, 0.0F}, Vec3{}, {}));
    const Uid gust = add_force(document, ForceKind::Wind);
    Force& wind = force_of(document, gust);
    wind.heading = 200.0F;
    wind.strength = {1.0F, 2.0F};
    wind.bodies = true;
    wind.air_speed = 9.0F;
    wind.gust = 0.4F;
    return document;
}

Document magic() {
    Document document = new_scene();
    const Uid sparkles = system_named(document, "Sparkles", SourceMode::Initial);
    ParticleSystem& glints = system_of(document, sparkles);
    first_source(glints).count = 300U;
    glints.persistent = true;
    glints.position = {0.0F, 1.0F, 0.0F};
    glints.radius = {0.3F, 1.2F};
    glints.speed = {0.0F, 0.3F};
    glints.look = ParticleLook::Solid;
    glints.lighting = ParticleLighting::Lit;
    glints.tint = {0.5F, 0.6F, 1.0F, 1.0F};
    glints.spin = {90.0F, 360.0F};
    glints.size = {0.02F, 0.05F};
    const Uid turn = add_behavior(document, sparkles, BehaviorKind::Vortex);
    const Uid away = add_behavior(document, sparkles, BehaviorKind::Attract);
    for (MotionBehavior& b : system_of(document, sparkles).motion) {
        if (b.uid == turn) {
            b.center = {0.0F, 1.0F, 0.0F};
            b.strength = {2.0F, 4.0F};
            b.seed = 7U;
        } else if (b.uid == away) {
            b.center = {0.0F, 2.0F, 0.0F};
            b.strength = {-1.0F, -0.5F};
            b.reach = 1.5F;
        }
    }
    const Uid funnel = add_force(document, ForceKind::DrawIn);
    force_of(document, funnel).height = 2.0F;
    const Uid lure = add_force(document, ForceKind::Attract);
    Force& pull = force_of(document, lure);
    pull.position = {1.0F, 1.5F, 0.0F};
    pull.reach = 2.0F;
    const Uid spiral = add_force(document, ForceKind::Vortex);
    force_of(document, spiral).axis = VortexAxis::Horizontal;

    // A stone driven through it.
    const Uid stone = add_object(document, "rock");
    document.scene->item(stone)->name = "Stone";
    Object& moving = object_of(document, stone);
    moving.motion = ObjectMotion::Animated;
    moving.collider = ObjectCollider::Sphere;
    moving.position = {-2.0F, 1.0F, 0.0F};
    moving.move_to = {2.0F, 1.0F, 0.0F};
    moving.turn_to = {0.0F, 180.0F, 0.0F};
    moving.move_at = 0.5F;
    moving.move_over = 2.0F;
    moving.size = 0.5F;
    return document;
}

Document rock_pile() {
    Document document = new_scene();
    const Uid rocks = add_object(document, "rock");
    document.scene->item(rocks)->name = "Rocks";
    Object& heap = object_of(document, rocks);
    heap.distribution = Distribution::Pile;
    heap.count = 20U;
    heap.size = 0.3F;
    heap.position = {-1.0F, 0.0F, 0.0F};
    heap.variation = 0.4F;
    heap.collider = ObjectCollider::Hull;
    const Uid crates = add_object(document);
    document.scene->item(crates)->name = "Crates";
    Object& stack = object_of(document, crates);
    stack.distribution = Distribution::Grid;
    stack.count = 8U;
    stack.size = 0.4F;
    stack.width = 0.8F;
    stack.depth = 0.8F;
    stack.position = {1.0F, 0.2F, 0.0F};
    stack.collider = ObjectCollider::Box;
    stack.release = 0.5F;
    const Uid ball = add_object(document);
    document.scene->item(ball)->name = "Ball";
    Object& thrown = object_of(document, ball);
    thrown.collider = ObjectCollider::Sphere;
    thrown.position = {3.0F, 1.0F, 0.0F};
    thrown.velocity = {-6.0F, 1.0F, 0.0F};
    thrown.density = 4000.0F;
    thrown.size = 0.35F;

    // Dust wherever anything lands.
    const Uid dust = add_particle_system(document);
    document.scene->item(dust)->name = "Dust";
    system_of(document, dust).sources.clear();
    const Uid hit = add_relationship(document, RelationKind::EmitOnHit, 0U, dust);
    style_effect(system_of(document, dust), rule_of(document, hit), EffectTemplate::DustOnHit);

    const Uid gust = add_force(document, ForceKind::Wind);
    Force& wind = force_of(document, gust);
    wind.bodies = true;
    wind.air_speed = 4.0F;
    wind.all_systems = false;
    wind.targets = {dust};
    return document;
}

// A log that is only ever painted in, which smoulders: a trail of smoke
// behind it while it moves.
Document logs() {
    Document document = new_scene();
    const Uid log = add_object(document, "slab");
    SceneItem* item = document.scene->item(log);
    item->name = "Log";
    auto& object = std::get<Object>(item->data);
    object.distribution = Distribution::Placed;
    object.size = 0.6F;
    object.position = {0.0F, 0.3F, 0.0F};
    const Uid smoke = add_bound_system(document, RelationKind::TrailFrom, log);
    if (smoke != 0U) {
        auto& particles = std::get<ParticleSystem>(document.scene->item(smoke)->data);
        particles.look = ParticleLook::Smoke;
        particles.tint = {0.9F, 0.45F, 0.15F, 1.0F};
    }
    return document;
}

// A wall that breaks when hit hard, and a ball (a heavy cube) to throw at
// it: pick the ball up, swing it, let go.
Document wall() {
    Document document = new_scene();
    const Uid wall = add_object(document);
    SceneItem* w = document.scene->item(wall);
    w->name = "Wall";
    auto& block = std::get<Object>(w->data);
    block.size = 1.6F;
    block.position = {0.0F, 0.8F, -2.0F};
    block.fracture = true;
    block.fragments = 8U;
    block.fracture_speed = 4.0F;
    const Uid ball = add_object(document);
    SceneItem* b = document.scene->item(ball);
    b->name = "Ball";
    auto& thrown = std::get<Object>(b->data);
    thrown.size = 0.4F;
    thrown.position = {0.0F, 0.2F, 2.0F};
    thrown.density = 3000.0F;
    return document;
}

// One of every starter kind standing on the stage, a step apart along x,
// each a Placed kind put down at step 0.
std::optional<Document> sandbox() {
    Document document = new_scene();
    float x = -4.0F;
    for (const StarterKindInfo& info : starter_kinds()) {
        const Uid made = add_starter_kind(document, info.kind, Vec3{x, 0.5F, -6.0F});
        if (made == 0U) {
            return std::nullopt;
        }
        const Object& o = std::get<Object>(document.scene->item(made)->data);
        static_cast<void>(
            add_drop(document.scene->performance, 0U, made, Vec3{x, o.size * 0.5F + 0.02F, 0.0F}, o.rotation, {}));
        x += 2.0F;
    }
    return document;
}

} // namespace

std::span<const StarterScene> starter_scenes() {
    return scenes;
}

std::optional<Document> make_starter_scene(std::string_view id) {
    if (id == "empty") {
        return new_scene();
    }
    if (id == "logs") {
        return logs();
    }
    if (id == "wall") {
        return wall();
    }
    if (id == "sandbox") {
        return sandbox();
    }
    if (id == "campfire") {
        return campfire();
    }
    if (id == "explosion") {
        return explosion();
    }
    if (id == "smoke_room") {
        return smoke_room();
    }
    if (id == "rain") {
        return rain();
    }
    if (id == "magic") {
        return magic();
    }
    if (id == "rock_pile") {
        return rock_pile();
    }
    return std::nullopt;
}

} // namespace ajfx::studio
