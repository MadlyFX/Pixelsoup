#include "bake.hpp"
#include "compile.hpp"
#include "document.hpp"
#include "page_revision.hpp"
#include "scene.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// What the application does on its interface thread, timed: the deep scene
// comparisons it makes to learn whether the scene changed, the compile an
// edit triggers for the viewport's handles, and the compile an edit triggers. Not a test; run it to see where
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
    std::size_t items{};
    double equal_us{};
    double compile_us{};
    double bake_us{};
    // The same questions asked through PageRevision and PageMark.
    double tracked_us{};
};

Row time_scene(const std::string& name, const Document& document, double budget) {
    Row row;
    row.name = name;
    row.items = document.scene->items.size();
    // The comparison is made with a copy, the case that costs most: equal
    // scenes are walked to the end.
    const Document copy = document;
    row.equal_us = measure(budget, [&] { sink = sink + (document == copy ? 1U : 0U); });
    const ajfx::studio::Compiled compiled = ajfx::studio::compile(document);
    row.compile_us = measure(budget, [&] {
        sink = sink + ajfx::studio::compile(document).items.size();
    });
    row.bake_us = measure(budget, [&] { sink = sink + ajfx::studio::bake_status(document, compiled, nullptr).any_baked; });
    // A frame with the scene unchanged, asked the way the application asks
    // it: one look at the scene, then six answers from the number it leaves
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

// A scene of `objects` dynamic cubes in a row and `systems` particle systems,
// standing on the default Ground.
Document scene_of(std::size_t objects, std::size_t systems) {
    Document document = ajfx::studio::new_scene();
    for (std::size_t i = 0U; i < objects; ++i) {
        const auto uid = ajfx::studio::add_object(document);
        std::get<ajfx::studio::Object>(document.scene->item(uid)->data).position =
            ajfx::Vec3{static_cast<float>(i % 10U) * 1.2F, 1.0F + static_cast<float>(i / 10U) * 1.2F, 0.0F};
    }
    for (std::size_t i = 0U; i < systems; ++i) {
        static_cast<void>(ajfx::studio::add_particle_system(document));
    }
    return document;
}

} // namespace

int main(int argc, char** argv) {
    const double budget = argc > 1 ? std::atof(argv[1]) : 0.25;
    std::vector<Row> rows;
    rows.push_back(time_scene("one object", scene_of(1U, 0U), budget));
    rows.push_back(time_scene("ten objects, two systems", scene_of(10U, 2U), budget));
    rows.push_back(time_scene("100 objects, ten systems", scene_of(100U, 10U), budget));

    std::printf("%-26s %6s | %9s | %9s %9s | %9s\n", "scene", "items", "== (us)", "compile", "bake", "tracked");
    double worst_tracked = 0.0;
    for (const Row& row : rows) {
        std::printf("%-26s %6zu | %9.2f | %9.1f %9.1f | %9.2f\n", row.name.c_str(), row.items, row.equal_us,
                    row.compile_us, row.bake_us, row.tracked_us);
        worst_tracked = std::max(worst_tracked, row.tracked_us);
    }
    std::printf("\nper frame, scene unchanged: tracked (one look, six cached answers), worst scene %.1f us "
                "(a frame is 16667 us)\n",
                worst_tracked);
    return sink == 0xFFFFFFFFFFFFFFFFULL ? 1 : 0;
}
