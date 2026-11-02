#include "bake.hpp"
#include "coach.hpp"
#include "compile.hpp"
#include "document.hpp"
#include "page_revision.hpp"
#include "recipes.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// What the application does on its interface thread, timed: the deep page
// comparisons it makes to learn whether the page changed, the coach's
// `advise`, and the compile an edit triggers. Not a test; run it to see where
// a frame's time goes and whether a change to that work paid off.
//
//   ajfx_studio_frame_bench [seconds-per-measure]
//
// Each figure is the median of several batches, in microseconds.
namespace {

using ajfx::studio::Document;
using Clock = std::chrono::steady_clock;

volatile std::uint64_t sink = 0U;

// Microseconds one call of `work` takes, from the median of seven batches
// each sized to run for about `budget` seconds.
template <class Work>
double measure(double budget, Work&& work) {
    // Size a batch from a first timing.
    std::size_t batch = 1U;
    for (;;) {
        const auto begin = Clock::now();
        for (std::size_t i = 0U; i < batch; ++i) {
            work();
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        if (seconds * 7.0 >= budget || batch >= (1U << 24U)) {
            break;
        }
        batch = seconds < budget / 700.0 ? batch * 8U : batch * 2U;
    }
    std::vector<double> samples;
    for (int round = 0; round < 7; ++round) {
        const auto begin = Clock::now();
        for (std::size_t i = 0U; i < batch; ++i) {
            work();
        }
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count() /
                          static_cast<double>(batch));
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2U];
}

struct Row final {
    std::string name;
    std::size_t layers{};
    std::size_t blocks{};
    double equal_us{};
    double advise_us{};
    double loop_us{};
    double compile_us{};
    double bake_us{};
    // The same questions asked through PageRevision and PageMark.
    double tracked_us{};
};

Row time_page(const std::string& name, const Document& document, double budget) {
    Row row;
    row.name = name;
    row.layers = document.layers.size();
    for (const auto& layer : document.layers) {
        row.blocks += layer.blocks.size();
    }
    // The comparison is made with a copy, the case that costs most: equal
    // pages are walked to the end.
    const Document copy = document;
    row.equal_us = measure(budget, [&] { sink = sink + (document == copy ? 1U : 0U); });
    const ajfx::studio::Compiled compiled = ajfx::studio::compile(document, ajfx::studio::Purpose::Preview);
    const ajfx::studio::BakeStatus status = ajfx::studio::bake_status(document, compiled, nullptr);
    row.advise_us = measure(budget, [&] { sink = sink + ajfx::studio::advise(document, compiled, &status).size(); });
    row.loop_us = measure(budget, [&] { sink = sink + static_cast<std::uint64_t>(ajfx::studio::natural_loop_seconds(compiled)); });
    row.compile_us = measure(budget, [&] {
        sink = sink + ajfx::studio::compile(document, ajfx::studio::Purpose::Preview).layers.size();
    });
    row.bake_us = measure(budget, [&] { sink = sink + ajfx::studio::bake_status(document, compiled, nullptr).any_baked; });
    // A frame with the page unchanged, asked the way the application asks it
    // now: one look at the page, then six answers from the number it leaves
    // (title, status, history, compiled, bake and submitted).
    ajfx::studio::PageRevision changes;
    ajfx::studio::PageMark marks[6];
    changes.observe(document);
    for (ajfx::studio::PageMark& mark : marks) {
        mark.set(document, changes.value());
    }
    row.tracked_us = measure(budget, [&] {
        const std::uint64_t at = changes.observe(copy);
        std::uint64_t differing = 0U;
        for (ajfx::studio::PageMark& mark : marks) {
            differing += mark.differs(copy, at) ? 1U : 0U;
        }
        sink = sink + differing;
    });
    return row;
}

} // namespace

int main(int argc, char** argv) {
    const double budget = argc > 1 ? std::atof(argv[1]) : 0.25;
    std::vector<Row> rows;
    Document everything;
    everything.name = "Every recipe on one page";
    for (const ajfx::studio::Recipe& recipe : ajfx::studio::recipes()) {
        std::optional<Document> made = ajfx::studio::make_recipe(recipe.id);
        if (!made) {
            continue;
        }
        rows.push_back(time_page(std::string{recipe.id}, *made, budget));
        for (const ajfx::studio::Layer& layer : made->layers) {
            everything.layers.push_back(ajfx::studio::copy_layer(everything, layer));
        }
    }
    rows.push_back(time_page("(every recipe's layers)", everything, budget));

    std::printf("%-26s %6s %6s | %9s %9s %9s | %9s %9s | %9s\n", "page", "layers", "blocks", "== (us)", "advise", "loop",
                "compile", "bake", "tracked");
    double worst_frame = 0.0;
    double worst_tracked = 0.0;
    for (const Row& row : rows) {
        std::printf("%-26s %6zu %6zu | %9.2f %9.2f %9.2f | %9.1f %9.1f | %9.2f\n", row.name.c_str(), row.layers,
                    row.blocks, row.equal_us, row.advise_us, row.loop_us, row.compile_us, row.bake_us, row.tracked_us);
        // A frame as it was: seven comparisons (title and status, history,
        // compiled, bake and submitted), advise and the loop.
        worst_frame = std::max(worst_frame, 7.0 * row.equal_us + row.advise_us + row.loop_us);
        worst_tracked = std::max(worst_tracked, row.tracked_us);
    }
    std::printf("\nper frame before: 7 x (==) + advise + loop, worst page %.1f us\n", worst_frame);
    std::printf("per frame now, page unchanged: tracked (one look, six cached answers), worst page %.1f us "
                "(a frame is 16667 us)\n",
                worst_tracked);
    return sink == 0xFFFFFFFFFFFFFFFFULL ? 1 : 0;
}
