#include "test_support.hpp"

#include "budget.hpp"

#include <functional>

// "Maximize budget" (budget.hpp), headless: the measuring is the
// application's, so here a cost model stands in for the GPU -- milliseconds
// as a function of how many particles and solid pieces the compiled page
// holds -- and the search, the scaling and the limits are checked against it.
namespace {

using namespace studio_test;

// What a page costs a frame, by a model: a little per particle and more per
// solid piece, on top of a fixed part, read off what the page compiles to.
struct CostModel final {
    double fixed{0.1};
    double per_particle{0.0};
    double per_piece{0.0};
    std::size_t asked{0U};

    double operator()(const Document& page) {
        ++asked;
        const Compiled compiled = compile(page, Purpose::Preview);
        double pieces = static_cast<double>(compiled.solids.capacity);
        double particles = static_cast<double>(compiled.total_capacity());
        // A solid layer's capacity is the world's; the rest are particles.
        particles = std::max(particles - pieces, 0.0);
        return fixed + per_particle * particles + per_piece * pieces;
    }
};

// Runs a whole "Maximize budget" against the model, as the application
// does against the meter: the limit first, then each factor the run asks for.
BudgetOutcome run(const Document& page, CostModel& model, bool allow_down = false, DensityOptions options = {}) {
    BudgetRun budget{page, options, density_limit(page, options), allow_down};
    std::size_t probes = 0U;
    while (const std::optional<double> factor = budget.next()) {
        CHECK(++probes <= 8U);
        budget.report(model(budget.page_at(*factor)));
    }
    CHECK(budget.done());
    return budget.outcome();
}

Block& add(Document& document, Layer& layer, BlockKind kind,
           std::initializer_list<std::pair<std::string_view, Value>> values = {}) {
    std::optional<Block> block = make_block(document, kind);
    CHECK(block.has_value());
    for (const auto& [control, value] : values) {
        block->set(control, value);
    }
    insert_block(layer, layer.blocks.size(), std::move(*block));
    return layer.blocks.back();
}

// Sparks streaming out and a burst of embers, sized, on a plain page.
Document sparks_page() {
    Document page;
    Layer sparks = make_layer(page, LayerWhat::Sparks, "Sparks");
    sparks.life_min = 1.0F;
    sparks.life_max = 1.5F;
    add(page, sparks, BlockKind::Stream, {{"rate", number(2000.0F)}});
    add(page, sparks, BlockKind::Burst, {{"count", number(1500.0F)}, {"time", number(0.2F)}});
    add(page, sparks, BlockKind::Size, {{"size", range(0.04F, 0.08F)}});
    add(page, sparks, BlockKind::Color, {{"brightness", range(2.0F, 4.0F)}});
    page.layers.push_back(std::move(sparks));
    return page;
}

ajfx::SimulationState play(const Compiled& compiled, std::uint64_t tick) {
    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
    ajfx::Simulation simulation =
        ajfx::Simulation::create(*compiled.effect, 7U,
                                 ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = solver.get()})
            .value();
    simulation.advance_to(tick);
    ajfx::SimulationState state;
    state.tick = simulation.tick();
    for (std::size_t i = 0U; i < compiled.effect->emitters.size(); ++i) {
        ajfx::EmitterState emitter = simulation.emitter(i);
        emitter.solver_state.clear();
        state.emitters.push_back(std::move(emitter));
    }
    return state;
}

void scaling_a_page_scales_its_amounts() {
    const Document page = sparks_page();
    const Document doubled = scale_density(page, 2.0);
    const Layer& before = page.layers[0];
    const Layer& after = doubled.layers[0];
    CHECK(near(number_of(after.blocks[0], "rate"), 4000.0F));
    CHECK(near(number_of(after.blocks[1], "count"), 3000.0F));
    // Smaller as there are more, so twice as many cover what there was.
    const Value size = get(after.blocks[2], "size");
    CHECK(near(size.x, 0.04F / std::sqrt(2.0F)) && near(size.y, 0.08F / std::sqrt(2.0F)));
    // Nothing else moves: when, how bright, the uids and seeds that deal
    // every random value.
    CHECK(near(number_of(after.blocks[1], "time"), 0.2F));
    CHECK(get(after.blocks[3], "brightness") == get(before.blocks[3], "brightness"));
    CHECK(after.uid == before.uid && after.seed == before.seed);
    for (std::size_t b = 0U; b < before.blocks.size(); ++b) {
        CHECK(after.blocks[b].uid == before.blocks[b].uid);
    }
    // Keeping the size is a choice.
    const Document kept = scale_density(page, 2.0, DensityOptions{.keep_look = false});
    CHECK(get(kept.layers[0].blocks[2], "size") == get(before.blocks[2], "size"));
    // Counts are whole: a seventh of 1500 sparks is 214.
    CHECK(near(number_of(scale_density(page, 1.0 / 7.0).layers[0].blocks[1], "count"), 214.0F));
    // The compiled page holds about twice the particles.
    const double ratio = static_cast<double>(compile(doubled, Purpose::Preview).total_capacity()) /
                         static_cast<double>(compile(page, Purpose::Preview).total_capacity());
    CHECK(ratio > 1.9 && ratio < 2.1);
    CHECK(density_knobs(page) == 2U);
    CHECK(density_knobs(Document{}) == 0U);
}

// The search alone, against costs of several shapes: it lands in the band
// (90 to 95% of the budget) in a few probes, from under it and from over it.
void the_search_lands_in_the_band() {
    const std::vector<std::function<double(double)>> shapes{
        [](double f) { return 0.5 * f; },                    // in proportion
        [](double f) { return 0.9 + 0.2 * f; },              // mostly fixed
        [](double f) { return 0.2 * f + 0.05 * f * f; },     // grows faster
        [](double f) { return 0.5 * std::sqrt(f) + 0.1; },   // grows slower
        [](double f) { return 12.0 * f; },                   // far over
    };
    for (std::size_t s = 0U; s < shapes.size(); ++s) {
        BudgetSearch search{BudgetSearch::Settings{.budget_ms = 4.0, .allow_down = true}};
        while (const std::optional<double> factor = search.next()) {
            search.report(shapes[s](*factor));
        }
        CHECK(search.points().size() <= 8U);
        CHECK(search.stop() == BudgetStop::InBand || search.stop() == BudgetStop::Closest);
        CHECK(search.ms() <= 4.0 * 0.95);
        CHECK(search.ms() >= 4.0 * 0.85);
        CHECK(std::fabs(shapes[s](search.factor()) - search.ms()) < 1.0e-12);
    }
    // Already in the band: nothing changes.
    BudgetSearch fits{BudgetSearch::Settings{.budget_ms = 4.0}};
    fits.report(3.7);
    CHECK(fits.stop() == BudgetStop::Fits && fits.factor() == 1.0);
}

// The whole run on a real page, measured by the model: one page comes back,
// just under the budget, with the particles scaled to fill it.
void maximizing_fills_the_budget() {
    Document page = sparks_page();
    page.budget_ms = 4.0F;
    CostModel model{.fixed = 0.1, .per_particle = 2.0e-4};
    const double before = model(page);
    CHECK(before < 2.0);
    const BudgetOutcome outcome = run(page, model);
    CHECK(outcome.stop == BudgetStop::InBand || outcome.stop == BudgetStop::Closest);
    CHECK(outcome.ms <= 4.0 * 0.95 && outcome.ms >= 4.0 * 0.85);
    CHECK(outcome.page.has_value() && outcome.factor > 1.5);
    CHECK(std::fabs(model(*outcome.page) - outcome.ms) < 1.0e-9);
    CHECK(outcome.words.find("of 4 ms") != std::string::npos);
    CHECK(outcome.words.find("x the particles") != std::string::npos);
    // The page the run was given is not touched by its probes.
    CHECK(page == [] {
        Document again = sparks_page();
        again.budget_ms = 4.0F;
        return again;
    }());
}

// A solid world grows until particles could no longer meet it, and the run
// stops there and says so, rather than letting the sparks pass through.
void a_limit_stops_it_and_is_named() {
    Document page;
    Layer rocks = make_layer(page, LayerWhat::Solid, "Rocks");
    rocks.life_min = rocks.life_max = 2.0F;
    add(page, rocks, BlockKind::Stream, {{"rate", number(1000.0F)}});
    page.layers.push_back(std::move(rocks));
    Layer dust = make_layer(page, LayerWhat::Dust, "Dust");
    add(page, dust, BlockKind::Stream, {{"rate", number(50.0F)}});
    page.layers.push_back(std::move(dust));
    const Compiled base = compile(page, Purpose::Preview);
    CHECK(base.solids.meetable() && base.solids.capacity < ajfx::max_body_colliders / 4U);

    const DensityLimit limit = density_limit(page, {});
    CHECK(limit.reason == "the 16,384-piece limit");
    CHECK(limit.factor > 1.0 && limit.factor < 16.0);
    // Cheap pieces: the budget alone would scale far past it.
    CostModel model{.fixed = 0.1, .per_particle = 1.0e-5, .per_piece = 1.0e-5};
    const BudgetOutcome outcome = run(page, model);
    CHECK(outcome.stop == BudgetStop::Limit);
    CHECK(outcome.limit == "the 16,384-piece limit");
    CHECK(outcome.words.find("Held by the 16,384-piece limit") != std::string::npos);
    CHECK(outcome.ms < 4.0 * 0.9);
    CHECK(outcome.page.has_value());
    const Compiled grown = compile(*outcome.page, Purpose::Preview);
    CHECK(grown.solids.meetable());
    CHECK(grown.solids.capacity > ajfx::max_body_colliders * 9U / 10U);

    // Enough independent cuts to reach the world's fixed kind budget before
    // any individual cut reaches its own 64-fragment control limit.
    Document crate;
    Layer layer = make_layer(crate, LayerKind::Rigid, "Crate");
    for (std::size_t i = 0U; i < ajfx::max_rigid_body_kinds / 64U; ++i) {
        add(crate, layer, BlockKind::Object, {{"motion", choice("loose")}});
        add(crate, layer, BlockKind::Break, {{"into", choice("cut")}, {"cells", number(32.0F)}});
    }
    crate.layers.push_back(std::move(layer));
    const DensityLimit cells = density_limit(crate, {});
    CHECK(cells.reason.find("kinds of body") != std::string::npos);
    CHECK(cells.factor > 1.9 && cells.factor < 2.0);
    const Compiled cut = compile(scale_density(crate, cells.factor), Purpose::Export);
    CHECK(cut.error.empty() && cut.layers[0].error.empty());

    // A control's own range binds too: a Burst is a million at most.
    Document burst;
    Layer flash = make_layer(burst, LayerWhat::Sparks, "Flash");
    add(burst, flash, BlockKind::Burst, {{"count", number(400000.0F)}});
    burst.layers.push_back(std::move(flash));
    const DensityLimit count = density_limit(burst, {});
    CHECK(count.reason == "Burst's limit of 1,000,000");
    CHECK(near(static_cast<float>(count.factor), 2.5F));
}

// Over the budget, the run says so and changes nothing unless asked to
// scale down; asked, it comes down to fit.
void over_budget_is_said_and_scaled_down_only_when_asked() {
    Document page = sparks_page();
    page.budget_ms = 1.0F;
    CostModel model{.fixed = 0.05, .per_particle = 4.5e-4};
    CHECK(model(page) > 1.0);
    const BudgetOutcome said = run(page, model);
    CHECK(said.stop == BudgetStop::Over);
    CHECK(!said.page.has_value());
    CHECK(said.words.rfind("Over: uses ", 0U) == 0U);
    const BudgetOutcome down = run(page, model, true);
    CHECK(down.stop == BudgetStop::InBand || down.stop == BudgetStop::Closest);
    CHECK(down.page.has_value() && down.factor < 1.0);
    CHECK(down.ms <= 0.95 && down.ms >= 0.8);
}

// The outcome goes in as one edit -- one step for the history to undo --
// and the scaled page plays to the same bits every time it is run.
void it_is_one_edit_and_reruns_identically() {
    Document page = sparks_page();
    CostModel model{.fixed = 0.1, .per_particle = 2.0e-4};
    const BudgetOutcome outcome = run(page, model);
    CHECK(outcome.page.has_value());
    // As the application keeps history: a copy of the page per step.
    std::vector<Document> history{page};
    page = *outcome.page;
    history.push_back(page);
    CHECK(history.size() == 2U);
    CHECK(history.front() == sparks_page());
    // Only amounts and sizes changed: every uid and seed is the page's own.
    const Layer& was = history.front().layers[0];
    const Layer& now = page.layers[0];
    CHECK(was.uid == now.uid && was.seed == now.seed && was.blocks.size() == now.blocks.size());
    for (std::size_t b = 0U; b < was.blocks.size(); ++b) {
        CHECK(was.blocks[b].uid == now.blocks[b].uid && was.blocks[b].kind == now.blocks[b].kind);
    }
    // Bit-identical reruns, and the same compile twice over.
    const Compiled first = compile(page, Purpose::Preview);
    const Compiled second = compile(page, Purpose::Preview);
    CHECK(first.effect.has_value() && *first.effect == *second.effect);
    const ajfx::SimulationState a = play(first, 90U);
    const ajfx::SimulationState b = play(second, 90U);
    CHECK(a == b);
    CHECK(!a.emitters.empty() && a.emitters[0].alive() > 0U);
    // Undone, the page is the original to the bit.
    page = history.front();
    CHECK(page == sparks_page());
}

void a_page_with_nothing_to_scale_says_so() {
    Document page;
    Layer smoke = make_layer(page, LayerWhat::Smoke, "Smoke");
    add(page, smoke, BlockKind::Size);
    page.layers.push_back(std::move(smoke));
    CostModel model{};
    const BudgetOutcome outcome = run(page, model);
    CHECK(outcome.stop == BudgetStop::Nothing && !outcome.page.has_value());
    CHECK(model.asked == 0U);
}

void the_budget_is_saved_with_the_page() {
    Document page = sparks_page();
    page.budget_ms = 2.5F;
    const Loaded loaded = load_document(save_document(page));
    CHECK(loaded.document.has_value() && near(loaded.document->budget_ms, 2.5F));
    CHECK(near(Document{}.budget_ms, default_budget_ms));
}

} // namespace

void add_budget_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"scaling_a_page_scales_its_amounts", scaling_a_page_scales_its_amounts});
    cases.push_back({"the_search_lands_in_the_band", the_search_lands_in_the_band});
    cases.push_back({"maximizing_fills_the_budget", maximizing_fills_the_budget});
    cases.push_back({"a_limit_stops_it_and_is_named", a_limit_stops_it_and_is_named});
    cases.push_back({"over_budget_is_said_and_scaled_down_only_when_asked",
                     over_budget_is_said_and_scaled_down_only_when_asked});
    cases.push_back({"it_is_one_edit_and_reruns_identically", it_is_one_edit_and_reruns_identically});
    cases.push_back({"a_page_with_nothing_to_scale_says_so", a_page_with_nothing_to_scale_says_so});
    cases.push_back({"the_budget_is_saved_with_the_page", the_budget_is_saved_with_the_page});
}
