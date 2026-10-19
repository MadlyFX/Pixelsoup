#include "output_window_config.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace app = mediaserver::app;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace topology = mediaserver::topology;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line) +
            ": check failed: " + expression};
    }
}

#define WINDOW_CHECK(...)                                                     \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] app::PlaybackOptions launch() {
    app::PlaybackOptions options{};
    options.node_index = 7U;
    options.surface_width = 1280U;
    options.surface_height = 720U;
    options.window_x = -320;
    options.window_y = 90;
    options.window_position_explicit = true;
    options.windowed = true;
    options.output = true;
    options.borderless = true;
    return options;
}

[[nodiscard]] topology::HardwareDevice server(
    doc::NodeId id, topology::HardwareWindowMode mode) {
    topology::HardwareDevice device{};
    device.node = id;
    device.kind = topology::HardwareDeviceKind::RenderServer;
    device.node_index = 7U;
    device.width = 3840U;
    device.height = 2160U;
    device.window_mode = mode;
    device.desktop_x = -3840;
    device.desktop_y = -240;
    return device;
}

// Only a cabled Render Server owns its node index. Each server here runs a
// video cable to one LED wall, as a deployed card does.
[[nodiscard]] topology::HardwareMap cabled(topology::HardwareMap map) {
    topology::HardwareDevice wall{};
    wall.node = doc::NodeId{900U};
    wall.kind = topology::HardwareDeviceKind::LedWall;
    core::u64 link = 950U;
    for (const topology::HardwareDevice& device : map.devices) {
        if (device.kind == topology::HardwareDeviceKind::RenderServer) {
            map.links.push_back(topology::HardwareLink{
                doc::NodeId{link++},
                topology::HardwareEndpoint{device.node, 1U},
                topology::HardwareEndpoint{wall.node, 0U}});
        }
    }
    map.devices.push_back(std::move(wall));
    return map;
}

void inherit_preserves_every_launch_field() {
    const app::PlaybackOptions options = launch();
    topology::HardwareMap map{};
    map.devices.push_back(
        server(doc::NodeId{1U}, topology::HardwareWindowMode::Inherit));
    const auto resolved = app::presenter_window_settings(options, cabled(map));
    WINDOW_CHECK(resolved.width == options.surface_width);
    WINDOW_CHECK(resolved.height == options.surface_height);
    WINDOW_CHECK(resolved.position_x == options.window_x);
    WINDOW_CHECK(resolved.position_y == options.window_y);
    WINDOW_CHECK(resolved.position_explicit ==
                 options.window_position_explicit);
    WINDOW_CHECK(resolved.borderless == options.borderless);
}

void authored_modes_apply_signed_desktop_placement() {
    const app::PlaybackOptions options = launch();
    topology::HardwareMap map{};
    map.devices.push_back(
        server(doc::NodeId{1U}, topology::HardwareWindowMode::Fullscreen));
    auto resolved = app::presenter_window_settings(options, cabled(map));
    WINDOW_CHECK(resolved.width == 3840U);
    WINDOW_CHECK(resolved.height == 2160U);
    WINDOW_CHECK(resolved.position_x == -3840);
    WINDOW_CHECK(resolved.position_y == -240);
    WINDOW_CHECK(resolved.position_explicit);
    WINDOW_CHECK(resolved.fullscreen);
    WINDOW_CHECK(!resolved.borderless);

    map.devices.front().window_mode =
        topology::HardwareWindowMode::Borderless;
    resolved = app::presenter_window_settings(options, cabled(map));
    WINDOW_CHECK(!resolved.fullscreen);
    WINDOW_CHECK(resolved.borderless);

    map.devices.front().window_mode = topology::HardwareWindowMode::Windowed;
    resolved = app::presenter_window_settings(options, cabled(map));
    WINDOW_CHECK(!resolved.fullscreen);
    WINDOW_CHECK(!resolved.borderless);
}

void duplicate_or_invalid_authorship_falls_back_to_launch() {
    const app::PlaybackOptions options = launch();
    const auto fallback = app::presenter_window_settings(options, {});

    topology::HardwareMap duplicate{};
    duplicate.devices.push_back(
        server(doc::NodeId{1U}, topology::HardwareWindowMode::Windowed));
    duplicate.devices.push_back(
        server(doc::NodeId{2U}, topology::HardwareWindowMode::Fullscreen));
    WINDOW_CHECK(app::presenter_window_settings(options, cabled(duplicate)) ==
                 fallback);

    topology::HardwareMap invalid{};
    invalid.devices.push_back(
        server(doc::NodeId{3U}, topology::HardwareWindowMode::Windowed));
    invalid.devices.front().width =
        topology::max_hardware_output_width + 1U;
    WINDOW_CHECK(app::presenter_window_settings(options, cabled(invalid)) ==
                 fallback);
    invalid.devices.front().width = 0U;
    WINDOW_CHECK(app::presenter_window_settings(options, cabled(invalid)) ==
                 fallback);
}

void authored_windows_do_not_enable_a_different_playback_mode() {
    topology::HardwareMap map{};
    map.devices.push_back(
        server(doc::NodeId{1U}, topology::HardwareWindowMode::Fullscreen));

    app::PlaybackOptions headless = launch();
    headless.windowed = false;
    headless.output = true;
    const auto headless_fallback =
        app::presenter_window_settings(headless, {});
    WINDOW_CHECK(app::presenter_window_settings(headless, cabled(map)) ==
                 headless_fallback);

    app::PlaybackOptions scene_preview = launch();
    scene_preview.output = false;
    scene_preview.scene = true;
    const auto scene_fallback =
        app::presenter_window_settings(scene_preview, {});
    WINDOW_CHECK(app::presenter_window_settings(scene_preview, cabled(map)) ==
                 scene_fallback);
}

void resolved_settings_rebuild_the_presenter_options_only() {
    const app::PlaybackOptions base = launch();
    topology::HardwareMap map{};
    map.devices.push_back(
        server(doc::NodeId{1U}, topology::HardwareWindowMode::Borderless));
    const auto window = app::presenter_window_settings(base, cabled(map));
    const app::PlaybackOptions rebuilt = app::presenter_options(base, window);
    WINDOW_CHECK(rebuilt.surface_width == 3840U);
    WINDOW_CHECK(rebuilt.surface_height == 2160U);
    WINDOW_CHECK(rebuilt.window_x == -3840);
    WINDOW_CHECK(rebuilt.window_y == -240);
    WINDOW_CHECK(rebuilt.window_position_explicit);
    WINDOW_CHECK(rebuilt.borderless);
    WINDOW_CHECK(rebuilt.node_index == base.node_index);
    WINDOW_CHECK(rebuilt.output == base.output);
    WINDOW_CHECK(rebuilt.lifecycle_secret == base.lifecycle_secret);
}

void rejected_nodes_only_fence_an_ambiguous_target() {
    const app::PlaybackOptions options = launch();
    const auto fallback = app::presenter_window_settings(options, {});

    topology::HardwareMap unique{};
    unique.devices.push_back(
        server(doc::NodeId{1U}, topology::HardwareWindowMode::Borderless));
    const auto unrelated_rejection =
        app::presenter_window_update(options, cabled(unique), 3U);
    WINDOW_CHECK(unrelated_rejection.has_value());
    WINDOW_CHECK(unrelated_rejection->position_x == -3840);
    WINDOW_CHECK(unrelated_rejection->borderless);

    unique.devices.front().window_mode =
        topology::HardwareWindowMode::Inherit;
    const auto inherited_with_rejection =
        app::presenter_window_update(options, cabled(unique), 1U);
    WINDOW_CHECK(inherited_with_rejection.has_value());
    WINDOW_CHECK(*inherited_with_rejection == fallback);

    topology::HardwareMap missing{};
    WINDOW_CHECK(!app::presenter_window_update(options, cabled(missing), 1U));
    const auto clean_removal =
        app::presenter_window_update(options, cabled(missing), 0U);
    WINDOW_CHECK(clean_removal.has_value());
    WINDOW_CHECK(*clean_removal == fallback);

    topology::HardwareMap duplicate{};
    duplicate.devices.push_back(
        server(doc::NodeId{2U}, topology::HardwareWindowMode::Windowed));
    duplicate.devices.push_back(
        server(doc::NodeId{3U}, topology::HardwareWindowMode::Fullscreen));
    WINDOW_CHECK(
        !app::presenter_window_update(options, cabled(duplicate), 0U));

    topology::HardwareMap invalid{};
    invalid.devices.push_back(
        server(doc::NodeId{4U}, topology::HardwareWindowMode::Windowed));
    invalid.devices.front().width = 0U;
    WINDOW_CHECK(!app::presenter_window_update(options, cabled(invalid), 0U));
}

// A parked card - no cable, no binding - is an alternative kept on the map.
// It neither supplies a window nor makes the cabled card's index ambiguous.
void a_parked_card_never_owns_or_shadows_a_node_index() {
    const app::PlaybackOptions options = launch();
    const auto fallback = app::presenter_window_settings(options, {});

    topology::HardwareMap parked_only{};
    parked_only.devices.push_back(
        server(doc::NodeId{1U}, topology::HardwareWindowMode::Fullscreen));
    WINDOW_CHECK(app::presenter_window_settings(options, parked_only) ==
                 fallback);

    topology::HardwareMap alternative = cabled(parked_only);
    alternative.devices.insert(
        alternative.devices.begin() + 1,
        server(doc::NodeId{2U}, topology::HardwareWindowMode::Windowed));
    const auto resolved =
        app::presenter_window_settings(options, alternative);
    WINDOW_CHECK(resolved.fullscreen);
    WINDOW_CHECK(resolved.width == 3840U);
    const auto updated =
        app::presenter_window_update(options, alternative, 0U);
    WINDOW_CHECK(updated.has_value());
    WINDOW_CHECK(updated->fullscreen);
}

void fresh_editors_seed_exactly_one_output_map_batch() {
    app::PlaybackOptions options{};
    WINDOW_CHECK(
        app::initial_feed_map_plan(options, false).kind ==
        app::InitialFeedMapKind::None);

    options.edit = true;
    const auto fresh_editor = app::initial_feed_map_plan(options, false);
    WINDOW_CHECK(fresh_editor.kind == app::InitialFeedMapKind::EditorCanvas);
    WINDOW_CHECK(
        fresh_editor.source_width ==
        mediaserver::show::demo_edit_canvas_width);
    WINDOW_CHECK(
        fresh_editor.source_height ==
        mediaserver::show::demo_edit_canvas_height);
    WINDOW_CHECK(fresh_editor.source_width == 1'920U);
    WINDOW_CHECK(fresh_editor.source_height == 1'080U);
    WINDOW_CHECK(
        app::initial_feed_map_plan(options, true).kind ==
        app::InitialFeedMapKind::None);

    // The combined mode still resolves to one authoring plan. Output
    // demos retain their historical behaviour even when a WAL was recovered.
    options.output = true;
    WINDOW_CHECK(
        app::initial_feed_map_plan(options, false).kind ==
        app::InitialFeedMapKind::OutputDemo);
    WINDOW_CHECK(
        app::initial_feed_map_plan(options, true).kind ==
        app::InitialFeedMapKind::OutputDemo);

    options.edit = false;
    WINDOW_CHECK(
        app::initial_feed_map_plan(options, false).kind ==
        app::InitialFeedMapKind::OutputDemo);
    WINDOW_CHECK(
        app::initial_feed_map_plan(options, true).kind ==
        app::InitialFeedMapKind::OutputDemo);
}

void mosaic_raster_preserves_authored_dimensions() {
    topology::HardwareMap map{};
    auto mosaic = server(doc::NodeId{1U},
                         topology::HardwareWindowMode::Borderless);
    mosaic.width = 15'360U;
    mosaic.height = 1'546U;
    map.devices.push_back(mosaic);
    const auto resolved = app::presenter_window_settings(launch(), cabled(map));
    WINDOW_CHECK(resolved.width == 15'360U);
    WINDOW_CHECK(resolved.height == 1'546U);
    WINDOW_CHECK(resolved.borderless);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 9U> cases{{
    {"Mosaic preserves the authored raster", &mosaic_raster_preserves_authored_dimensions},
    {"inherit preserves every launch field",
     &inherit_preserves_every_launch_field},
    {"authored modes apply signed desktop placement",
     &authored_modes_apply_signed_desktop_placement},
    {"duplicate or invalid authorship falls back to launch",
     &duplicate_or_invalid_authorship_falls_back_to_launch},
    {"authored windows do not enable a different playback mode",
     &authored_windows_do_not_enable_a_different_playback_mode},
    {"resolved settings rebuild presenter options only",
     &resolved_settings_rebuild_the_presenter_options_only},
    {"rejected nodes only fence an ambiguous target",
     &rejected_nodes_only_fence_an_ambiguous_target},
    {"a parked card never owns or shadows a node index",
     &a_parked_card_never_owns_or_shadows_a_node_index},
    {"fresh editors seed one output map batch",
     &fresh_editors_seed_exactly_one_output_map_batch},
}};

} // namespace

int main() {
    std::size_t failures{};
    for (const Case& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
