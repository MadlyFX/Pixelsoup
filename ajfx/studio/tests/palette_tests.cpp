#include "hands.hpp"
#include "issues.hpp"
#include "kinds.hpp"
#include "performance.hpp"
#include "test_support.hpp"

// The reactive palette: starter kinds that answer when something happens to
// them, effects attached from templates, wind that blows on light things,
// a firm hand, and the issues list. Each is played headless and checked by
// what can be seen of it: things that break leave pieces, effects bear
// particles when their rule fires, the wind moves the box and not the brick,
// a firm hand puts the ball exactly where it was carried, and the issues
// name the acts that were refused, counted by hand.
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

Document reopened(const Document& document) {
    Loaded loaded = load_document(save_document(document));
    if (!loaded.document) {
        throw std::runtime_error{loaded.error};
    }
    return std::move(*loaded.document);
}

// Particles born so far by the emitter a system lowered to.
std::uint64_t born(const Played& played, const Prepared& ready, Uid system) {
    const ajfx::Effect& effect = *ready.compiled->effect;
    for (std::size_t e = 0U; e < effect.emitters.size(); ++e) {
        if (emitter_item(effect.emitters[e].name) == system) {
            return played.simulation->emitter(e).spawned;
        }
    }
    throw std::runtime_error{"no emitter for system " + std::to_string(system)};
}

// ------------------------------------------------------------ starter kinds

void every_starter_kind_answers_as_it_says() {
    auto solver = ajfx::rigid::make_jolt_solver();
    for (const StarterKindInfo& info : starter_kinds()) {
        Document document = new_scene();
        const Uid kind = add_starter_kind(document, info.kind);
        CHECK(kind != 0U);
        const Object& o = object(document, kind);
        CHECK(o.distribution == Distribution::Placed && o.motion == ObjectMotion::Dynamic);
        CHECK(document.scene->item(kind)->name == info.name);
        // Its effects are attached, one rule and one system each.
        const std::vector<EffectTemplate> effects = starter_effects(info.kind);
        const std::vector<AttachedEffect> attached = attached_effects(*document.scene, kind);
        CHECK(attached.size() == effects.size());
        for (std::size_t k = 0U; k < effects.size(); ++k) {
            CHECK(attached[k].kind == effect_template_info(effects[k]).rule);
        }
        CHECK(validate_scene(document).empty());

        // Dropped from 4 m (8.9 m/s at the floor), harder than any of them
        // breaks at, it breaks if it is breakable and its hit effects fire;
        // a smouldering log smokes and sheds embers whatever it does.
        static_cast<void>(add_drop(document.scene->performance, 0U, kind, {0.0F, 4.0F, 0.0F}, {}, {}));
        const Prepared ready = prepared(document);
        const Played played = play(document, ready, *solver, 150U);
        std::cout << "  " << info.name << ": " << pieces(played, ready) << " pieces";
        CHECK((pieces(played, ready) > 0U) == o.fracture);
        if (o.fracture) {
            CHECK(pieces(played, ready) == o.fragments);
        }
        for (const AttachedEffect& effect : attached) {
            const std::uint64_t count = born(played, ready, effect.system);
            std::cout << ", " << document.scene->item(effect.system)->name << " " << count;
            CHECK(count > 0U);
        }
        std::cout << '\n';
        // Saved and opened again, the kind and its effects are as they were.
        CHECK(*reopened(document).scene == *document.scene);
    }
    CHECK(starter_kind("glass") == StarterKind::GlassPane && !starter_kind("anvil"));
}

void effects_are_ordinary_items_a_template_restyles() {
    Document document = new_scene();
    const Uid cube = add_object(document);
    document.scene->item(cube)->name = "Cube";
    const Uid dust = attach_effect(document, cube, EffectTemplate::DustOnHit);
    CHECK(dust != 0U && document.scene->item(dust)->name == "Cube dust");
    auto& system = std::get<ParticleSystem>(document.scene->item(dust)->data);
    CHECK(system.sources.empty() && system.look == ParticleLook::Smoke);
    const ajfx::Color made = system.tint;
    // The artist edits it as any system, and the template puts it back.
    system.tint = ajfx::Color{0.0F, 1.0F, 0.0F, 1.0F};
    system.lifetime = {9.0F, 9.0F};
    CHECK(apply_effect_template(document, dust, EffectTemplate::DustOnHit));
    const auto& again = std::get<ParticleSystem>(document.scene->item(dust)->data);
    CHECK(again.tint == made && again.lifetime == (Range{0.8F, 1.8F}));
    // Or as another hit effect, with a colour of the artist's; never as a
    // trail, which its rule is not.
    CHECK(apply_effect_template(document, dust, EffectTemplate::SparksOnHit, ajfx::Color{0.2F, 0.4F, 1.0F, 1.0F}));
    const auto& sparks = std::get<ParticleSystem>(document.scene->item(dust)->data);
    CHECK(sparks.look == ParticleLook::Glow && sparks.tint == (ajfx::Color{0.2F, 0.4F, 1.0F, 1.0F}));
    CHECK(document.scene->relationships.front().hardness == 3.0F);
    CHECK(!apply_effect_template(document, dust, EffectTemplate::SmokeTrail));
    // Only objects take effects.
    CHECK(attach_effect(document, dust, EffectTemplate::SmokeTrail) == 0U);
    CHECK(attach_effect(document, 9999U, EffectTemplate::SmokeTrail) == 0U);
    CHECK(validate_scene(document).empty());
}

void a_hit_rule_gives_dust_unless_told_otherwise() {
    Document document = new_scene();
    const Uid crate = add_object(document);
    const Uid hits = add_bound_system(document, RelationKind::EmitOnHit, crate);
    const auto& system = std::get<ParticleSystem>(document.scene->item(hits)->data);
    CHECK(system.look == ParticleLook::Smoke && system.lighting == ParticleLighting::Lit);
    CHECK(document.scene->relationships.front().per_hit == 6.0F);
    CHECK(validate_scene(document).empty());
}

// ------------------------------------------------------------ wind on things

struct Breezy final {
    Document document = new_scene();
    Uid box{};
    Uid brick{};
    Uid wind{};
};

// A cardboard box and a brick on the stage, and a wind along +X.
Breezy breezy(bool on_things) {
    Breezy scene;
    scene.box = add_starter_kind(scene.document, StarterKind::CardboardBox, {0.0F, 0.5F, -5.0F});
    scene.brick = add_starter_kind(scene.document, StarterKind::Brick, {2.0F, 0.5F, -5.0F});
    Performance& performance = scene.document.scene->performance;
    static_cast<void>(add_drop(performance, 0U, scene.box, {0.0F, 0.2F, 0.0F}, {}, {}));
    static_cast<void>(add_drop(performance, 0U, scene.brick, {0.0F, 0.125F, 1.5F}, {}, {}));
    scene.wind = add_force(scene.document, ForceKind::Wind);
    auto& wind = std::get<Force>(scene.document.scene->item(scene.wind)->data);
    wind.heading = 0.0F;
    wind.air_speed = 12.0F;
    wind.bodies = on_things;
    return scene;
}

void a_wind_on_things_carries_the_box_off_and_leaves_the_brick() {
    auto solver = ajfx::rigid::make_jolt_solver();
    Breezy scene = breezy(true);
    const Prepared ready = prepared(scene.document);
    // The world blows with it: along +X at 12 m/s.
    const std::optional<std::size_t> world = solid_world_index(*ready.compiled);
    CHECK(world.has_value());
    std::size_t winds = 0U;
    for (const ajfx::UpdateModule& module : ready.compiled->effect->emitters[*world].update) {
        if (const auto* wind = std::get_if<ajfx::BodyWind>(&module)) {
            ++winds;
            CHECK(length(wind->velocity - ajfx::Vec3{12.0F, 0.0F, 0.0F}) < 1.0e-4F);
        }
    }
    CHECK(winds == 1U);
    const Played played = play(scene.document, ready, *solver, 180U);
    const auto box = find_body(played, ready, BodyRef{.drop = 1U});
    const auto brick = find_body(played, ready, BodyRef{.drop = 2U});
    CHECK(box && brick);
    std::cout << "  after 3 s the box is " << box->position.x << " m downwind, the brick " << brick->position.x
              << " m\n";
    CHECK(box->position.x > 1.0F);
    CHECK(std::fabs(brick->position.x) < 0.02F);
    // Saved, reopened and replayed, the same to the bit.
    const Document again = reopened(scene.document);
    CHECK(state_digest(*play(again, prepared(again), *solver, 180U).simulation) ==
          state_digest(*played.simulation));

    // The same wind on particles alone leaves both where they stand.
    Breezy calm = breezy(false);
    const Prepared calm_ready = prepared(calm.document);
    const Played still = play(calm.document, calm_ready, *solver, 180U);
    CHECK(std::fabs(find_body(still, calm_ready, BodyRef{.drop = 1U})->position.x) < 0.02F);
}

// ------------------------------------------------------------ a firm hand

struct Wall final {
    Document document = new_scene();
    Uid wall{};
    Uid ball{};
};

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

// The ball carried from where it rests, low through the wall to the far
// side, steps 30 to 60: 6 m in half a second.
ajfx::Vec3 carried_to(std::uint64_t tick) {
    const float t = static_cast<float>(tick - 30U) / 30.0F;
    return ajfx::Vec3{0.0F, 0.6F, 2.0F - 6.0F * t};
}

void carry(Document& document, Uid ball, bool firm) {
    Performance& performance = document.scene->performance;
    const std::uint32_t grab = add_grab(performance, 30U, BodyRef{.object = ball, .count = 0U}, carried_to(30U), {},
                                        grab_gain, grab_max_speed, firm);
    for (std::uint64_t tick = 31U; tick <= 60U; ++tick) {
        CHECK(extend_grab(performance, grab, tick, carried_to(tick)));
    }
}

void a_firm_hand_goes_through_what_a_soft_one_meets() {
    auto solver = ajfx::rigid::make_jolt_solver();
    Wall firm = wall_and_ball();
    carry(firm.document, firm.ball, true);
    CHECK(firm.document.scene->performance.acts.back().firm);
    Wall soft = wall_and_ball();
    carry(soft.document, soft.ball, false);
    const BodyRef ball{.object = firm.ball, .count = 0U};
    const Prepared firm_ready = prepared(firm.document);
    const Prepared soft_ready = prepared(soft.document);
    const Played held = play(firm.document, firm_ready, *solver, 61U);
    const Played steered = play(soft.document, soft_ready, *solver, 61U);
    const float firm_off = length(find_body(held, firm_ready, ball)->position - carried_to(60U));
    const float soft_off = length(find_body(steered, soft_ready, ball)->position - carried_to(60U));
    std::cout << "  at the last sample: firm " << firm_off << " m off, steered " << soft_off << " m off; wall pieces "
              << pieces(held, firm_ready) << " and " << pieces(steered, soft_ready) << '\n';
    // Firm: exactly where it was carried, through the wall, which it broke.
    CHECK(firm_off < 1.0e-3F);
    CHECK(pieces(held, firm_ready) == 8U);
    // Steered: the wall stood in its way.
    CHECK(soft_off > 0.2F);
    // Let go, a firm hand throws at the speed it moved: 12 m/s along -Z.
    CHECK(std::fabs(find_body(held, firm_ready, ball)->velocity.z + 12.0F) < 0.05F);
    // Saved strictly and replayed exactly.
    const Document again = reopened(firm.document);
    CHECK(again.scene->performance == firm.document.scene->performance);
    CHECK(state_digest(*play(again, prepared(again), *solver, 120U).simulation) ==
          state_digest(*play(firm.document, firm_ready, *solver, 120U).simulation));
}

// ------------------------------------------------------------ issues

void issues_name_each_refused_drop_and_why() {
    auto solver = ajfx::rigid::make_jolt_solver();
    Document document = new_scene();
    document.scene->physics.max_dropped = 40U;
    const Uid box = add_starter_kind(document, StarterKind::CardboardBox, {0.0F, 0.5F, -5.0F});
    Performance& performance = document.scene->performance;
    // 20 at once at step 10: one step makes at most 16, so 4 are refused.
    std::vector<std::uint32_t> first;
    for (int i = 0; i < 20; ++i) {
        first.push_back(add_drop(performance, 10U, box,
                                 {static_cast<float>(i % 5) * 0.5F, 1.0F + 0.5F * static_cast<float>(i / 5), 0.0F}, {},
                                 {}));
    }
    // Then one a step from 20 to 49: 16 + 24 fill the 40, and 6 are refused.
    std::vector<std::uint32_t> later;
    for (int i = 0; i < 30; ++i) {
        later.push_back(add_drop(performance, 20U + static_cast<std::uint64_t>(i), box,
                                 {static_cast<float>(i % 6) - 3.0F, 2.0F, static_cast<float>(i / 6) - 6.0F}, {}, {}));
    }
    // And a deletion of a body nothing makes.
    const std::uint32_t lost = add_delete(performance, 70U, BodyRef{.object = box + 7U, .count = 3U});
    const Prepared ready = prepared(document);

    const std::vector<Issue> before = scene_issues(document, ready);
    CHECK(std::count_if(before.begin(), before.end(), [&](const Issue& i) {
              return i.kind == IssueKind::Unapplied && i.act == lost && i.tick == 70U;
          }) == 1);
    const std::vector<Issue> issues = play_issues(document, ready, solver.get(), 0U, 120U);
    std::vector<std::uint32_t> per_step;
    std::vector<std::uint32_t> budget;
    for (const Issue& issue : issues) {
        CHECK(issue.kind == IssueKind::RefusedDrop && issue.level == IssueLevel::Warning && issue.item == box);
        (issue.message.find("one step") != std::string::npos ? per_step : budget).push_back(issue.act);
    }
    if (!issues.empty()) {
        std::cout << "  " << issues.front().message << '\n';
    }
    // The last four asked for at step 10, and the last six of the rest.
    CHECK(per_step == std::vector<std::uint32_t>(first.end() - 4, first.end()));
    CHECK(budget == std::vector<std::uint32_t>(later.end() - 6, later.end()));
}

void issues_warn_of_the_kind_budget() {
    // Glass breaks into twelve, each a kind: 15 panes are 15 x 13 kinds.
    Document document = new_scene();
    for (int i = 0; i < 15; ++i) {
        CHECK(add_starter_kind(document, StarterKind::GlassPane, {static_cast<float>(i), 0.5F, -5.0F}) != 0U);
    }
    const Prepared ready = prepared(document);
    const SceneStats stats = scene_stats(*document.scene, &*ready.compiled, nullptr);
    std::cout << "  " << stats.body_kinds << " body kinds\n";
    CHECK(stats.body_kinds >= 15U * 13U);
    const std::vector<Issue> issues = scene_issues(document, ready);
    CHECK(std::count_if(issues.begin(), issues.end(), [](const Issue& i) {
              return i.kind == IssueKind::KindBudget && i.level == IssueLevel::Warning;
          }) == 1);
    // One pane is far from it.
    Document one = new_scene();
    CHECK(add_starter_kind(one, StarterKind::GlassPane) != 0U);
    CHECK(scene_issues(one, prepared(one)).empty());
}

void issues_say_when_the_cache_is_stale() {
    auto solver = ajfx::rigid::make_jolt_solver();
    Breezy scene = breezy(true);
    Prepared ready = prepared(scene.document);
    const ajfx::Result<BakeRecord> baked = bake_page(scene.document, *ready.compiled, 3U, solver.get());
    CHECK(baked.ok());
    CHECK(!cache_issue(scene.document, ready, baked.value().bytes, 3U));
    // Studio works the status out from the scene compiled for export, as it
    // is before its cuts are made.
    const BakeStatus current = bake_status(scene.document, compile(scene.document, Purpose::Export), &baked.value());
    CHECK(current.current && bake_issues(scene.document, current).empty());
    // Another seed, another performance, another wind: each is a stale cache.
    CHECK(cache_issue(scene.document, ready, baked.value().bytes, 4U).has_value());
    Document acted = scene.document;
    static_cast<void>(add_push(acted.scene->performance, 30U, BodyRef{.drop = 1U}, {0.0F, 3.0F, 0.0F}, {}));
    const std::optional<Issue> stale = cache_issue(acted, prepared(acted), baked.value().bytes, 3U);
    CHECK(stale && stale->kind == IssueKind::CacheStale && stale->level == IssueLevel::Warning);
    const std::vector<Issue> from_status =
        bake_issues(acted, bake_status(acted, compile(acted, Purpose::Export), &baked.value()));
    CHECK(!from_status.empty() && from_status.front().kind == IssueKind::CacheStale);
    Document gustier = scene.document;
    std::get<Force>(gustier.scene->item(scene.wind)->data).gust = 0.5F;
    CHECK(cache_issue(gustier, prepared(gustier), baked.value().bytes, 3U).has_value());
}

void a_wind_with_nothing_to_blow_on_is_an_issue() {
    Document document = new_scene();
    const Uid wind = add_force(document, ForceKind::Wind);
    std::get<Force>(document.scene->item(wind)->data).bodies = true;
    const std::vector<Issue> issues = scene_issues(document, prepared(document));
    CHECK(std::count_if(issues.begin(), issues.end(), [&](const Issue& i) {
              return i.kind == IssueKind::WindIdle && i.item == wind;
          }) == 1);
    // Something loose to blow on, and it is not an issue.
    CHECK(add_starter_kind(document, StarterKind::CardboardBox) != 0U);
    CHECK(scene_issues(document, prepared(document)).empty());
    CHECK(emitter_item("item_42") == 42U && emitter_item("item_") == 0U && emitter_item("dust") == 0U);
}

} // namespace

void add_palette_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"every_starter_kind_answers_as_it_says", every_starter_kind_answers_as_it_says});
    cases.push_back({"effects_are_ordinary_items_a_template_restyles", effects_are_ordinary_items_a_template_restyles});
    cases.push_back({"a_hit_rule_gives_dust_unless_told_otherwise", a_hit_rule_gives_dust_unless_told_otherwise});
    cases.push_back({"a_wind_on_things_carries_the_box_off_and_leaves_the_brick",
                     a_wind_on_things_carries_the_box_off_and_leaves_the_brick});
    cases.push_back({"a_firm_hand_goes_through_what_a_soft_one_meets", a_firm_hand_goes_through_what_a_soft_one_meets});
    cases.push_back({"issues_name_each_refused_drop_and_why", issues_name_each_refused_drop_and_why});
    cases.push_back({"issues_warn_of_the_kind_budget", issues_warn_of_the_kind_budget});
    cases.push_back({"issues_say_when_the_cache_is_stale", issues_say_when_the_cache_is_stale});
    cases.push_back({"a_wind_with_nothing_to_blow_on_is_an_issue", a_wind_with_nothing_to_blow_on_is_an_issue});
}
