#include "test_support.hpp"

// Baking a scene: what the cache holds of each item, and when it goes stale.
namespace {

using namespace studio_test;

// The scene compiled with its files read and its cuts made, as the builder
// hands it to bake_scene.
Compiled ready_to_bake(const Document& document) {
    Compiled compiled = compile(document);
    std::string issue;
    make_cuts(compiled, issue);
    CHECK(issue.empty() && compiled.effect.has_value());
    return compiled;
}

Object& object_of(Document& document, Uid uid) {
    return std::get<Object>(document.scene->item(uid)->data);
}

void a_bake_says_what_it_holds_and_when_it_goes_stale() {
    Document document = starter("explosion");
    const Uid crate = item_named(document, "Crate");
    const Uid smoke = item_named(document, "Smoke burst");
    // The smoke passes through things, so nothing of the crate's reaches it.
    std::get<ParticleSystem>(document.scene->item(smoke)->data).collide = false;
    // Before any bake: baked and not yet cached, with what it would cost.
    BakeStatus status = bake_status(document, compile(document), nullptr);
    CHECK(status.any_baked && !status.current);
    CHECK(status.item(crate)->state == BakeState::Unbaked && status.item(smoke)->state == BakeState::Unbaked);
    CHECK(status.cost.bytes > 0U && status.cost.bytes_per_second > 0U && status.cost.seconds < 0.0);

    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
    const Compiled ready = ready_to_bake(document);
    double heard = 0.0;
    const ajfx::Result<BakeRecord> baked =
        bake_scene(document, ready, 5U, solver.get(), [&heard](double share) { heard = share; return true; });
    if (!baked) {
        std::cout << "  " << baked.error().message << '\n';
    }
    CHECK(baked.ok());
    const BakeRecord& record = baked.value();
    CHECK(near(static_cast<float>(heard), 1.0F));
    CHECK(record.frames == bake_frames(document, ready));
    CHECK(record.item(crate) && record.item(crate)->mode == ajfx::CacheMode::Rigid);
    CHECK(record.item(smoke) && record.item(smoke)->mode == ajfx::CacheMode::Particles);
    std::cout << "  the explosion bakes to " << bytes_text(record.report.bytes) << " over " << record.frames
              << " steps in " << format_number(static_cast<float>(record.seconds), 2) << " s, estimated "
              << bytes_text(status.cost.bytes) << "\n";
    // It opens against the effect the export writes.
    const ajfx::Result<ajfx::Cache> opened =
        ajfx::Cache::open(record.bytes, *ready.effect, 5U, {}, ready.meshes, ready.fields);
    if (!opened) {
        std::cout << "  " << opened.error().message << '\n';
    }
    CHECK(opened.ok());

    status = bake_status(document, compile(document), &record);
    CHECK(status.current && status.cached_frames == record.frames);
    CHECK(status.item(crate)->state == BakeState::Current);
    CHECK(status.cost.seconds >= 0.0);

    // How the crate looks never reaches the cache.
    Document edited = document;
    object_of(edited, crate).tint = {1.0F, 0.5F, 0.5F, 1.0F};
    status = bake_status(edited, compile(edited), &record);
    if (status.item(crate)->state != BakeState::Current) {
        std::cout << "  unexpectedly stale: " << status.item(crate)->why << '\n';
    }
    CHECK(status.current && status.item(crate)->state == BakeState::Current);

    // When it breaks does, and says what changed; the smoke, which passes
    // through it, is still as it was.
    object_of(edited, crate).fracture_speed = 9.0F;
    status = bake_status(edited, compile(edited), &record);
    const ItemBake* stale = status.item(crate);
    CHECK(stale->state == BakeState::Stale && !status.current);
    std::cout << "  stale because " << stale->why << '\n';
    CHECK(stale->why == "its \"Impact threshold\" changed");
    CHECK(status.item(smoke)->state == BakeState::Current);
    // The dust is born of the crate's hits, so it goes stale with it, and
    // says that something it plays beside changed.
    const ItemBake* dust = status.item(item_named(edited, "Crate dust"));
    CHECK(dust && dust->state == BakeState::Stale && dust->why.find("Crate") != std::string::npos);
    // And a cache handed the edited effect refuses.
    const Compiled edited_ready = ready_to_bake(edited);
    CHECK(ajfx::Cache::open(record.bytes, *edited_ready.effect, 5U, {}, edited_ready.meshes, edited_ready.fields)
              .error()
              .code == ajfx::ErrorCode::StaleCache);

    // The blast moved: the crate's own settings are as they were, so it says so.
    Document moved = document;
    for (SceneItem& item : moved.scene->items) {
        if (auto* force = std::get_if<Force>(&item.data)) {
            force->position.x += 0.5F;
        }
    }
    status = bake_status(moved, compile(moved), &record);
    CHECK(status.item(crate)->state == BakeState::Stale);
    CHECK(status.item(crate)->why.find("Blast changed") != std::string::npos);
}

void a_switched_off_item_and_a_smoke_grid_wait_or_stay_live() {
    Document document = starter("smoke_room");
    const Uid fire = item_named(document, "Fire");
    const Uid ash = item_named(document, "Ash");
    const Uid grid = item_named(document, "Smoke grid");
    BakeStatus status = bake_status(document, compile(document), nullptr);
    CHECK(status.item(grid)->state == BakeState::Live);
    CHECK(status.item(fire)->state == BakeState::Live && status.item(fire)->why.find("feeds smoke") != std::string::npos);
    CHECK(status.item(ash)->state == BakeState::Unbaked);
    document.scene->item(ash)->simulate = false;
    status = bake_status(document, compile(document), nullptr);
    CHECK(status.item(ash)->state == BakeState::Unbaked && status.item(ash)->why == "it is switched off");
}

void nothing_baked_is_refused() {
    // A scene of smoke alone: nothing a cache can keep.
    Document document = empty_scene();
    const Uid grid = add_smoke_grid(document);
    const Uid fire = add_particle_system(document);
    CHECK(add_relationship(document, RelationKind::FeedSmoke, grid, fire) != 0U);
    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
    const ajfx::Result<BakeRecord> baked = bake_scene(document, ready_to_bake(document), 5U, solver.get());
    CHECK(!baked.ok() && baked.error().message.find("Nothing is baked") != std::string::npos);
}

} // namespace

void add_bake_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"a bake says what it holds and when it goes stale", a_bake_says_what_it_holds_and_when_it_goes_stale});
    cases.push_back({"a switched-off item and a smoke grid wait or stay live",
                     a_switched_off_item_and_a_smoke_grid_wait_or_stay_live});
    cases.push_back({"nothing baked is refused", nothing_baked_is_refused});
}
