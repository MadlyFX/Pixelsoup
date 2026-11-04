#include "package_builder.hpp"
#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// The preview's builder, with the real kernel compiler and CPU reference.
// A build never waits for the CPU run of the build before it, and that run is
// not thrown away by the edit that follows: while a value is dragged the
// preview keeps pace and the paths keep arriving.
namespace {

using namespace ajfx::studio;
using Clock = std::chrono::steady_clock;

#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            throw std::runtime_error{"line " + std::to_string(__LINE__) + ": check failed: " #__VA_ARGS__};            \
        }                                                                                                              \
    } while (false)

// A burst of sparks that land on the ground and trail smoke as they go: the
// CPU run takes far longer than a build once the kernels are compiled.
Document sparks_over_ground(float fastest) {
    Document document = new_scene();
    const Uid sparks = add_particle_system(document);
    {
        auto& system = std::get<ParticleSystem>(document.scene->item(sparks)->data);
        system.sources.clear();
        system.position = {0.0F, 1.0F, 0.0F};
        system.lifetime = {3.0F, 3.0F};
        system.speed = {1.0F, fastest};
        system.spread = 20.0F;
        system.collide = true;
    }
    CHECK(add_source(document, sparks, SourceMode::Burst) != 0U);
    std::get<ParticleSystem>(document.scene->item(sparks)->data).sources.back().count = 1000U;
    const Uid smoke = add_particle_system(document);
    {
        auto& system = std::get<ParticleSystem>(document.scene->item(smoke)->data);
        system.sources.clear();
        system.lifetime = {1.0F, 2.0F};
        system.speed = {0.0F, 0.2F};
    }
    CHECK(add_relationship(document, RelationKind::TrailFromSystem, sparks, smoke) != 0U);
    CHECK(validate_scene(document).empty());
    return document;
}

template <typename Take>
auto wait_for(Take take, double seconds) {
    const auto until = Clock::now() + std::chrono::duration<double>(seconds);
    while (Clock::now() < until) {
        if (auto taken = take()) {
            return taken;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return decltype(take()){};
}

void a_build_does_not_wait_for_the_cpu_run_before_it() {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / ("ajfx_builder_tests_" + std::to_string(std::time(nullptr)));
    std::filesystem::create_directories(folder);
    {
        PackageBuilder builder{folder / "preview.ajfx", 1U};
        builder.submit(sparks_over_ground(3.0F), 1U);
        // The first build compiles the kernels; the second reuses them.
        const std::optional<BuildResult> first = wait_for([&] { return builder.take_build(); }, 300.0);
        CHECK(first && first->written && first->revision == 1U);

        // The first CPU run is under way. A second edit builds without it.
        builder.submit(sparks_over_ground(1.5F), 2U);
        std::vector<std::uint64_t> runs;
        const std::optional<BuildResult> second = wait_for(
            [&] {
                if (std::optional<Analysis> analysis = builder.take_analysis()) {
                    runs.push_back(analysis->revision);
                }
                return builder.take_build();
            },
            60.0);
        CHECK(second && second->written && second->revision == 2U);
        CHECK(runs.empty());
        CHECK(!builder.busy());

        // The run under way finished, then the newest build was run.
        const auto start = Clock::now();
        while (Clock::now() - start < std::chrono::seconds{300} && (runs.empty() || runs.back() != 2U)) {
            if (std::optional<Analysis> analysis = builder.take_analysis()) {
                CHECK(analysis->traced && !analysis->paths.empty());
                runs.push_back(analysis->revision);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        CHECK(runs == std::vector<std::uint64_t>{1U, 2U});
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// With a focus, the selected system's paths come first, from a run of it
// alone, and then the whole scene's run with everything. Asked for only the
// CPU run, the builder writes no package.
void the_selected_systems_paths_come_first() {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / ("ajfx_builder_focus_" + std::to_string(std::time(nullptr)));
    std::filesystem::create_directories(folder);
    {
        const Document document = sparks_over_ground(3.0F);
        const Uid sparks = document.scene->items[1].uid;
        CHECK(std::holds_alternative<ParticleSystem>(document.scene->items[1].data));
        PackageBuilder builder{folder / "preview.ajfx", 1U};
        builder.submit(document, 1U, {}, sparks, sparks);
        const std::optional<BuildResult> built = wait_for([&] { return builder.take_build(); }, 300.0);
        CHECK(built && built->written && built->live == sparks);
        const auto next = [&] { return wait_for([&] { return builder.take_analysis(); }, 300.0); };
        const std::optional<Analysis> first = next();
        CHECK(first && first->focus == sparks && first->revision == 1U && !first->paths.empty());
        CHECK(std::all_of(first->paths.begin(), first->paths.end(),
                          [&](const ParticlePath& path) { return path.layer == sparks; }));
        const std::optional<Analysis> whole = next();
        CHECK(whole && whole->focus == 0U && whole->revision == 1U);
        CHECK(std::any_of(whole->paths.begin(), whole->paths.end(),
                          [&](const ParticlePath& path) { return path.layer != sparks; }));
        // The focus run's paths are the whole run's paths of that system.
        std::vector<ParticlePath> mine;
        for (const ParticlePath& path : whole->paths) {
            if (path.layer == sparks) {
                mine.push_back(path);
            }
        }
        CHECK(mine.size() == first->paths.size());
        for (std::size_t i = 0U; i < mine.size(); ++i) {
            CHECK(mine[i].birth == first->paths[i].birth && mine[i].points == first->paths[i].points);
        }

        const auto written = std::filesystem::last_write_time(folder / "preview.ajfx");
        builder.submit_analysis(sparks_over_ground(2.0F), 2U, {}, sparks);
        const std::optional<Analysis> again = next();
        CHECK(again && again->revision == 2U && again->focus == sparks);
        CHECK(!builder.take_build());
        CHECK(std::filesystem::last_write_time(folder / "preview.ajfx") == written);
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// The stage settles on the builder's own thread, with the files the scene
// names read as an export does, and what it learned comes back whole: the
// performance as it began and as it ended, for the caller to adopt only if
// nothing else was done meanwhile.
void settling_the_stage_comes_back_from_the_worker() {
    Document document = new_scene();
    const Uid crate = add_object(document);
    std::get<Object>(document.scene->item(crate)->data).distribution = Distribution::Placed;
    std::get<Object>(document.scene->item(crate)->data).size = 1.0F;
    // Two crates a metre and a half up and three up, side by side.
    const std::uint32_t low = add_drop(document.scene->performance, 0U, crate, {-1.0F, 1.5F, 0.0F}, {}, {});
    const std::uint32_t high = add_drop(document.scene->performance, 0U, crate, {1.0F, 3.0F, 0.0F}, {0.0F, 30.0F, 0.0F}, {});
    const Performance began = document.scene->performance;

    PackageBuilder builder{std::filesystem::temp_directory_path() / "ajfx-settle-test.ajfx", 71U, false};
    builder.submit_settle(document);
    const std::optional<SettleOutcome> outcome = wait_for([&] { return builder.take_settle(); }, 60.0);
    CHECK(outcome.has_value());
    CHECK(outcome->ok && outcome->message == "Settled 2 things");
    CHECK(outcome->result.settled == 2U && outcome->result.moving == 0U);
    CHECK(outcome->before == began);
    // The scene itself is untouched; the outcome holds the settled stage.
    CHECK(document.scene->performance == began);
    for (const std::uint32_t serial : {low, high}) {
        const Act* act = find_act(outcome->after, serial);
        CHECK(act != nullptr && act->touched);
        // On the ground: its middle half a metre above the floor, give or take a skin.
        CHECK(std::abs(act->position.y - (stage_floor(document) + 0.5F)) < 0.06F);
    }
    CHECK(!builder.take_settle());

    // A scene with nothing on the stage says so rather than doing nothing.
    Document empty = new_scene();
    builder.submit_settle(empty);
    const std::optional<SettleOutcome> refused = wait_for([&] { return builder.take_settle(); }, 60.0);
    CHECK(refused && !refused->ok && !refused->message.empty());
}

} // namespace

int main() {
    try {
        settling_the_stage_comes_back_from_the_worker();
        std::cout << "[pass] settling the stage comes back from the worker\n";
        a_build_does_not_wait_for_the_cpu_run_before_it();
        std::cout << "[pass] a build does not wait for the CPU run before it, and that run is not thrown away\n";
        the_selected_systems_paths_come_first();
        std::cout << "[pass] the selected system's paths come first\n";
    } catch (const std::exception& failure) {
        std::cerr << "[fail] " << failure.what() << '\n';
        return 1;
    }
    return 0;
}
