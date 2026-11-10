// Times the document work the Director and every node repeat: what is rebuilt
// when the document changes, and what is derived again on every loop wake.
//
//   mediaserver_director_tick_benchmark <project.mstage>... [--scale=N]
//                                       [--check]
//
// --check edits every node of each project in turn - moved, erased, put back
// - and compares the models a frame loop keeps up to date with models rebuilt
// from the same document. They must be identical; the process exits 1 if any
// are not. It is the same comparison the unit tests make, on a real show.
//
// Each project is measured as saved and, with --scale, again with its drawable
// nodes repeated N times, which is how a small saved show stands in for a
// large one. Times are the median of repeated runs, in microseconds. Run the
// Release build: Debug iterators dominate everything here.

#include "director_project_load.hpp"
#include "project_assets.hpp"

#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/compositor/compositor_model.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/output/feed_map_model.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/sync/sync_policy_model.hpp>
#include <mediaserver/topology/hardware_map_model.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace assets = mediaserver::assets;
namespace compositor = mediaserver::compositor;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace output = mediaserver::output;
namespace persist = mediaserver::persist;
namespace scene = mediaserver::scene;
namespace sync = mediaserver::sync;
namespace topology = mediaserver::topology;

// Keeps a result alive past the optimizer without printing it.
volatile std::uint64_t sink = 0U;

template <class Work> [[nodiscard]] double median_microseconds(Work &&work) {
    constexpr std::size_t runs = 21U;
    std::array<double, runs> samples{};
    for (double &sample : samples) {
        const auto start = std::chrono::steady_clock::now();
        work();
        sample = std::chrono::duration<double, std::micro>(
                     std::chrono::steady_clock::now() - start)
                     .count();
    }
    std::nth_element(samples.begin(), samples.begin() + runs / 2U,
                     samples.end());
    return samples[runs / 2U];
}

void row(std::string_view what, double microseconds) {
    std::printf("  %-46.*s %10.1f us\n", static_cast<int>(what.size()),
                what.data(), microseconds);
}

// The project with every drawable node of its own repeated `copies` more
// times under fresh ids. Copies keep their scene, so they are drawn like the
// originals; what Blender linked in is left as it is.
[[nodiscard]] doc::Root scaled(const doc::Root &project, std::uint64_t copies) {
    doc::Root result = project;
    std::uint64_t next = 0U;
    const doc::Delta contents = doc::diff(doc::Root{}, project);
    for (const doc::NodeChange &change : contents.changes) {
        next = std::max(next, change.id.value() + 1U);
    }
    for (std::uint64_t copy = 0U; copy < copies; ++copy) {
        for (const doc::NodeChange &change : contents.changes) {
            const std::string_view type{change.after->type};
            // A Blender link record is one of a kind: a second occurrence
            // of the same object is a document the link parser refuses.
            if ((type != scene::schema::object_type &&
                 type != scene::schema::model_type &&
                 type != scene::schema::media_type) ||
                scene::is_link_record(*change.after)) {
                continue;
            }
            doc::Node repeated = *change.after;
            repeated.id = doc::NodeId{next++};
            result = result.set(std::move(repeated));
        }
    }
    return result;
}

void measure(std::string_view label, const doc::Root &root) {
    const doc::Delta contents = doc::diff(doc::Root{}, root);
    std::printf("\n%.*s: %zu nodes\n", static_cast<int>(label.size()),
                label.data(), contents.changes.size());

    scene::SceneModel scene_model;
    compositor::CompositorModel compositor_model;
    assets::AssetModel asset_model;
    output::FeedMapModel feed_model;
    topology::HardwareMapModel hardware_model;
    sync::SyncPolicyModel sync_model;

    std::puts(" On every document change (each machine):");
    const double enumerate = median_microseconds(
        [&] { sink = sink + doc::diff(doc::Root{}, root).changes.size(); });
    row("walk the whole document once", enumerate);
    const double scene_reset =
        median_microseconds([&] { scene_model.reset(root); });
    row("rebuild scene model", scene_reset);
    // Two parts of that rebuild, to show where it goes.
    row("  of which: copy the document node by node",
        median_microseconds([&] {
            doc::Root copy;
            for (const doc::NodeChange &change : contents.changes) {
                copy = copy.set(*change.after);
            }
            sink = sink + copy.size();
        }));
    row("  of which: parse Blender link records",
        median_microseconds([&] {
            sink = sink + (scene::read_linked_scene(root) ? 1U : 0U);
        }));
    const double compositor_reset =
        median_microseconds([&] { compositor_model.reset(root); });
    row("rebuild compositor model", compositor_reset);
    assets::AssetRegistry registry;
    const double asset_reset = median_microseconds([&] {
        asset_model.reset(root);
        registry = asset_model.evaluate();
    });
    row("rebuild asset model and registry", asset_reset);
    const double feed_reset =
        median_microseconds([&] { feed_model.reset(root); });
    row("rebuild feed map model", feed_reset);
    const double hardware_reset =
        median_microseconds([&] { hardware_model.reset(root); });
    row("rebuild hardware map model", hardware_reset);
    const double sync_reset =
        median_microseconds([&] { sync_model.reset(root); });
    row("rebuild sync policy model", sync_reset);
    row("all six rebuilt", scene_reset + compositor_reset + asset_reset +
                               feed_reset + hardware_reset + sync_reset);

    // One node moved, as a drag does on every frame, applied as a delta.
    const doc::Node *moved = nullptr;
    for (const doc::NodeChange &change : contents.changes) {
        if (std::string_view{change.after->type} ==
                scene::schema::object_type ||
            std::string_view{change.after->type} ==
                scene::schema::media_type ||
            std::string_view{change.after->type} ==
                scene::schema::model_type) {
            moved = change.after.get();
        }
    }
    if (moved != nullptr) {
        doc::Node edited_node = *moved;
        edited_node.properties.insert_or_assign(
            std::string{scene::schema::position_x}, doc::PropertyValue{3.25});
        const doc::Root edited = root.set(std::move(edited_node));
        bool forward = true;
        // What the frame loops do: the scene and compositor models are
        // brought up to date, the four small ones rebuilt.
        scene_model.update(root);
        compositor_model.update(root, scene_model);
        const double updated = median_microseconds([&] {
            const doc::Root &next = forward ? edited : root;
            forward = !forward;
            scene_model.update(next);
            compositor_model.update(next, scene_model);
            asset_model.reset(next);
            registry = asset_model.evaluate();
            feed_model.reset(next);
            hardware_model.reset(next);
            sync_model.reset(next);
        });
        row("as the frame loops do it (one node moved)", updated);
        const double changed = median_microseconds([&] {
            sink = sink + (app::project_content_changed(root, edited) ? 1U : 0U);
        });
        row("unsaved-changes comparison (editor refresh)", changed);
        // Leave every model on `root` for the per-wake rows.
        scene_model.reset(root);
        compositor_model.reset(root);
        asset_model.reset(root);
        feed_model.reset(root);
        hardware_model.reset(root);
        sync_model.reset(root);
    }

    std::puts(" On every Director loop wake:");
    const auto scenes = scene_model.scenes();
    if (scenes.empty()) {
        std::puts("  (no scene)");
        return;
    }
    const scene::SceneId stage = scenes.front().id;
    const scene::SceneTime time{48U, 24U, 1U};
    scene::Scene evaluated;
    const double evaluate = median_microseconds(
        [&] { evaluated = scene_model.evaluate(stage, time); });
    row("evaluate one scene", evaluate);
    const double digest =
        median_microseconds([&] { sink = sink + scene::digest(evaluated); });
    row("digest one evaluated scene", digest);
    const double graph = median_microseconds([&] {
        sink = sink + compositor_model.evaluate(doc::NodeId{stage.value()})
                          .nodes.size();
    });
    row("evaluate one compositor graph", graph);
    const double feed = median_microseconds(
        [&] { sink = sink + feed_model.evaluate(time).maps.size(); });
    row("evaluate feed map", feed);
    const double hardware = median_microseconds(
        [&] { sink = sink + hardware_model.evaluate().devices.size(); });
    row("evaluate hardware map", hardware);
    const double manifest = median_microseconds([&] {
        sink = sink +
               app::collect_project_asset_manifest(root, registry).digest;
    });
    // Program and Stage each evaluate a scene and a graph; the rest once.
    row("one editing wake (2 scenes, 2 graphs, rest once)",
        2.0 * evaluate + digest + 2.0 * graph + feed + hardware);
    // The Director's editor collects this when the document changes; it was
    // once part of every wake.
    row("collect asset manifest (per document change)", manifest);
    std::printf("  evaluated: %zu media, %zu models, %zu instances\n",
                evaluated.media.size(), evaluated.models.size(),
                evaluated.instances.size());
}

// Every node moved, erased and restored in turn; after each, the updated
// models against rebuilt ones. Returns how many comparisons disagreed.
[[nodiscard]] std::size_t check(std::string_view label, const doc::Root &root) {
    scene::SceneModel scene_updated;
    compositor::CompositorModel compositor_updated;
    // The compositor model alone, keeping a scene model of its own.
    compositor::CompositorModel compositor_alone;
    std::size_t steps = 0U;
    std::size_t mismatches = 0U;
    const auto step = [&](const doc::Root &document) {
        scene_updated.update(document);
        compositor_updated.update(document, scene_updated);
        compositor_alone.update(document);
        scene::SceneModel scene_rebuilt;
        scene_rebuilt.reset(document);
        compositor::CompositorModel compositor_rebuilt;
        compositor_rebuilt.reset(document);
        bool same = scene_updated.size() == scene_rebuilt.size() &&
                    scene_updated.scenes() == scene_rebuilt.scenes() &&
                    scene_updated.linked_error().has_value() ==
                        scene_rebuilt.linked_error().has_value() &&
                    compositor_updated.evaluate() ==
                        compositor_rebuilt.evaluate() &&
                    compositor_alone.evaluate() ==
                        compositor_rebuilt.evaluate();
        for (const scene::SceneInfo &info : scene_rebuilt.scenes()) {
            const doc::NodeId scoped{info.id.value()};
            same = same && compositor_updated.evaluate(scoped) ==
                               compositor_rebuilt.evaluate(scoped);
        }
        for (const scene::SceneInfo &info : scene_rebuilt.scenes()) {
            for (const std::uint64_t frame : {0U, 48U, 1'000U}) {
                const scene::SceneTime time{frame, 24U, 1U};
                same = same &&
                       scene::digest(scene_updated.evaluate(info.id, time)) ==
                           scene::digest(scene_rebuilt.evaluate(info.id, time));
            }
        }
        ++steps;
        if (!same) {
            ++mismatches;
        }
    };
    step(root);
    const doc::Delta contents = doc::diff(doc::Root{}, root);
    for (const doc::NodeChange &change : contents.changes) {
        doc::Node moved = *change.after;
        moved.properties.insert_or_assign(
            std::string{scene::schema::position_x}, doc::PropertyValue{3.25});
        step(root.set(std::move(moved)));
        if (const auto erased = root.erase(change.id)) {
            step(*erased);
        }
        step(root);
    }
    std::printf("\n%.*s: %zu edits checked, %zu differ from a rebuild\n",
                static_cast<int>(label.size()), label.data(), steps,
                mismatches);
    return mismatches;
}

} // namespace

int main(int argc, char **argv) {
    std::uint64_t scale = 0U;
    bool checking = false;
    std::size_t mismatches = 0U;
    std::vector<std::string> projects;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        constexpr std::string_view scale_prefix = "--scale=";
        if (argument.starts_with(scale_prefix)) {
            const std::string_view digits =
                argument.substr(scale_prefix.size());
            if (std::from_chars(digits.data(), digits.data() + digits.size(),
                                scale)
                    .ec != std::errc{}) {
                std::cerr << "--scale needs a whole number\n";
                return 2;
            }
        } else if (argument == "--check") {
            checking = true;
        } else {
            projects.emplace_back(argument);
        }
    }
    if (projects.empty()) {
        std::cerr << "usage: mediaserver_director_tick_benchmark "
                     "<project.mstage>... [--scale=N] [--check]\n";
        return 2;
    }
    for (const std::string &project : projects) {
        const auto loaded =
            persist::load_project_file(app::native_path_from_utf8(project));
        if (!loaded) {
            std::cerr << "could not load " << project << " (code "
                      << static_cast<unsigned>(loaded.error().code) << ")\n";
            return 1;
        }
        const std::string name =
            std::filesystem::path{project}.filename().string();
        if (checking) {
            mismatches += check(name, loaded.value());
            continue;
        }
        measure(name, loaded.value());
        if (scale != 0U) {
            measure(name + " x" + std::to_string(scale + 1U),
                    scaled(loaded.value(), scale));
        }
    }
    return mismatches == 0U ? 0 : 1;
}
