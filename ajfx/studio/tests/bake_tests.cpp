#include "test_support.hpp"

// Baking a layer: what its simulation reads, and when a cache goes stale.
namespace {

using namespace studio_test;

// The page compiled for export with its files read and its cuts made, as
// the builder hands it to bake_page.
Compiled ready_to_bake(const Document& document) {
    Compiled compiled = compile(document, Purpose::Export);
    std::string issue;
    make_cuts(compiled, issue);
    CHECK(issue.empty() && compiled.effect.has_value());
    return compiled;
}

// Every publishable control says when it takes effect, and only the ones
// that drawing alone reads are playback-time.
void published_controls_say_when_they_take_effect() {
    std::size_t playback = 0U;
    for (const BlockDef& def : block_catalog()) {
        for (const ControlDef& control : def.controls) {
            if (control.timing == ajfx::ParameterTiming::Playback) {
                ++playback;
                CHECK(control.publishable);
                CHECK(def.kind == BlockKind::Appearance);
            }
        }
    }
    CHECK(playback == 4U);
    CHECK(control_timing("appearance", "tint") == ajfx::ParameterTiming::Playback);
    CHECK(control_timing("stream", "rate") == ajfx::ParameterTiming::Bake);
    CHECK(control_timing("nonsense", "rate") == ajfx::ParameterTiming::Bake);

    Document document = recipe("breaking_crates");
    Layer& snow = layer_named(document, "Snow");
    if (std::none_of(snow.blocks.begin(), snow.blocks.end(), [](const Block& b) { return b.kind == BlockKind::Appearance; })) {
        insert_block(snow, snow.blocks.size(), *make_block(document, "appearance"));
    }
    block_of(snow, "appearance").publish(Published{"opacity", "snow_opacity", 0.0F, 1.0F});
    block_of(snow, "appearance").publish(Published{"tint", "snow_tint", 0.0F, 1.0F});
    block_of(snow, "stream").publish(Published{"rate", "snow_rate", 0.0F, 200.0F});
    const Compiled compiled = compile(document, Purpose::Export);
    if (!compiled.error.empty()) {
        std::cout << "  " << compiled.error << '\n';
    }
    CHECK(compiled.error.empty());
    CHECK(parameter_of(compiled, "snow_opacity")->timing == ajfx::ParameterTiming::Playback);
    CHECK(parameter_of(compiled, "snow_tint")->timing == ajfx::ParameterTiming::Playback);
    CHECK(parameter_of(compiled, "snow_rate")->timing == ajfx::ParameterTiming::Bake);
    const ajfx::Emitter& flakes = emitter_of(compiled, snow);
    CHECK(flakes.renderer.opacity.parameter == "snow_opacity");
    CHECK(flakes.renderer.tint.parameter == "snow_tint");
    // What the package carries says so too, for the media server's Inspector.
    const ajfx::Effect back = ajfx::read_package(ajfx::write_package(*compiled.effect).value()).value();
    CHECK(back.parameters == compiled.effect->parameters);

    // One name may not be both.
    block_of(snow, "appearance").publish(Published{"opacity", "snow_rate", 0.0F, 1.0F});
    CHECK(!compile(document, Purpose::Export).error.empty());
}

void a_baked_layer_holds_what_its_simulation_reads() {
    Document document = recipe("breaking_crates");
    Layer& snow = layer_named(document, "Snow");
    if (std::none_of(snow.blocks.begin(), snow.blocks.end(), [](const Block& b) { return b.kind == BlockKind::Appearance; })) {
        insert_block(snow, snow.blocks.size(), *make_block(document, "appearance"));
    }
    block_of(snow, "appearance").publish(Published{"opacity", "snow_opacity", 0.0F, 1.0F});
    block_of(snow, "stream").publish(Published{"rate", "snow_rate", 0.0F, 200.0F});
    snow.baked = true;
    const Compiled compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty());
    // The amount is held where it is set; the opacity is still an operator's.
    CHECK(parameter_of(compiled, "snow_rate") == nullptr);
    CHECK(parameter_of(compiled, "snow_opacity") != nullptr);
    const LayerReport* report = compiled.layer(snow.uid);
    CHECK(report->baked);
    CHECK(report->held.size() == 1U);
    CHECK(report->held[0].name == "snow_rate");
    CHECK(report->held[0].block == block_of(snow, "stream").uid);
}

void a_bake_says_what_it_holds_and_when_it_goes_stale() {
    Document document = recipe("breaking_crates");
    Layer& crates = layer_named(document, "Crates and a dropped ball");
    crates.baked = true;
    // Before any bake: baked and not yet cached, with what it would cost.
    BakeStatus status = bake_status(document, compile(document, Purpose::Export), nullptr);
    CHECK(status.any_baked && !status.current);
    CHECK(status.layer(crates.uid)->state == BakeState::Unbaked);
    CHECK(status.layer(layer_named(document, "Snow").uid)->state == BakeState::Live);
    CHECK(status.cost.bytes > 0U && status.cost.bytes_per_second > 0U && status.cost.seconds < 0.0);

    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
    const Compiled ready = ready_to_bake(document);
    double heard = 0.0;
    const ajfx::Result<BakeRecord> baked =
        bake_page(document, ready, 5U, solver.get(), [&heard](double share) { heard = share; return true; });
    if (!baked) {
        std::cout << "  " << baked.error().message << '\n';
    }
    CHECK(baked.ok());
    const BakeRecord& record = baked.value();
    CHECK(near(static_cast<float>(heard), 1.0F));
    CHECK(record.frames == bake_frames(document, ready));
    CHECK(record.layers.size() == 1U && record.layers[0].mode == ajfx::CacheMode::Rigid);
    std::cout << "  the crates bake to " << bytes_text(record.report.bytes) << " over " << record.frames
              << " steps in " << format_number(static_cast<float>(record.seconds), 2) << " s, estimated "
              << bytes_text(status.cost.bytes) << "\n";
    // It opens against the effect the export writes, and plays the crates.
    const ajfx::Result<ajfx::Cache> opened =
        ajfx::Cache::open(record.bytes, *ready.effect, 5U, {}, ready.meshes, ready.fields);
    if (!opened) {
        std::cout << "  " << opened.error().message << '\n';
    }
    CHECK(opened.ok());

    status = bake_status(document, compile(document, Purpose::Export), &record);
    CHECK(status.current && status.cached_frames == record.frames);
    CHECK(status.layer(crates.uid)->state == BakeState::Current);
    CHECK(status.cost.seconds >= 0.0);

    // Changing the live snow, or how the crates look, leaves the bake right.
    Document edited = document;
    block_of(layer_named(edited, "Snow"), "stream").set("rate", number(5.0F));
    Block& look = block_of(layer_named(edited, "Crates and a dropped ball"), "appearance");
    look.set("tint", color(1.0F, 0.5F, 0.5F));
    look.set("opacity", number(0.5F));
    status = bake_status(edited, compile(edited, Purpose::Export), &record);
    if (status.layer(crates.uid)->state != BakeState::Current) {
        std::cout << "  unexpectedly stale: " << status.layer(crates.uid)->why << '\n';
    }
    CHECK(status.layer(crates.uid)->state == BakeState::Current);

    // Changing when the crates break does not, and says what changed.
    Block& breaking = block_of(layer_named(edited, "Crates and a dropped ball"), "break");
    breaking.set("force", number(9.0F));
    status = bake_status(edited, compile(edited, Purpose::Export), &record);
    const LayerBake* stale = status.layer(crates.uid);
    CHECK(stale->state == BakeState::Stale && !status.current);
    CHECK(stale->block == breaking.uid);
    std::cout << "  stale because " << stale->why << '\n';
    CHECK(stale->why.find("Break") != std::string::npos);
    // And so does a cache handed the edited effect.
    const Compiled edited_ready = ready_to_bake(edited);
    CHECK(ajfx::Cache::open(record.bytes, *edited_ready.effect, 5U, {}, edited_ready.meshes, edited_ready.fields)
              .error()
              .code == ajfx::ErrorCode::StaleCache);
}

void both_kinds_of_layer_bake() {
    Document document = recipe("breaking_crates");
    layer_named(document, "Crates and a dropped ball").baked = true;
    layer_named(document, "Snow").baked = true;
    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
    const Compiled ready = ready_to_bake(document);
    const ajfx::Result<BakeRecord> baked = bake_page(document, ready, 5U, solver.get());
    CHECK(baked.ok());
    CHECK(baked.value().layers.size() == 2U);
    const auto snow = std::find_if(baked.value().report.layers.begin(), baked.value().report.layers.end(),
                                   [](const ajfx::CacheLayer& l) { return l.mode == ajfx::CacheMode::Particles; });
    CHECK(snow != baked.value().report.layers.end() && snow->bytes > 0U);
    // Nothing is baked: refused with a reason. A solid layer is baked from
    // the start, so it is made live first.
    Document live = recipe("breaking_crates");
    CHECK(layer_named(live, "Crates and a dropped ball").baked);
    for (Layer& layer : live.layers) {
        layer.baked = false;
    }
    CHECK(!bake_page(live, ready_to_bake(live), 5U, solver.get()).ok());
}

} // namespace

void add_bake_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"published_controls_say_when_they_take_effect", published_controls_say_when_they_take_effect});
    cases.push_back({"a_baked_layer_holds_what_its_simulation_reads", a_baked_layer_holds_what_its_simulation_reads});
    cases.push_back({"a_bake_says_what_it_holds_and_when_it_goes_stale", a_bake_says_what_it_holds_and_when_it_goes_stale});
    cases.push_back({"both_kinds_of_layer_bake", both_kinds_of_layer_bake});
}
