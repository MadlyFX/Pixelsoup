#include "../output_deployment.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace app = mediaserver::app;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace net = mediaserver::net;
namespace topology = mediaserver::topology;

#define DEPLOY_CHECK(condition)                                              \
    do {                                                                     \
        if (!(condition)) {                                                  \
            throw std::runtime_error{                                        \
                std::string{"line "} + std::to_string(__LINE__) +           \
                ": " #condition};                                           \
        }                                                                    \
    } while (false)

[[nodiscard]] topology::HardwareDevice server(core::u64 id,
                                               core::u32 node_index) {
    topology::HardwareDevice value{};
    value.node = doc::NodeId{id};
    value.kind = topology::HardwareDeviceKind::RenderServer;
    value.name = "Render";
    value.node_index = node_index;
    return value;
}

[[nodiscard]] topology::HardwareOutputBinding binding(
    core::u64 id, core::u8 port, core::u32 feed,
    topology::HardwareDisplayTargetKey target) {
    topology::HardwareOutputBinding value{};
    value.node = doc::NodeId{id};
    value.render_server = doc::NodeId{1U};
    value.logical_port = port;
    value.feed_index = feed;
    value.target = target;
    value.width = 3840U;
    value.height = 2160U;
    value.refresh_numerator = 60'000U;
    value.refresh_denominator = 1'001U;
    value.window_mode = topology::HardwareWindowMode::Fullscreen;
    return value;
}

[[nodiscard]] net::ClusterDisplayTargetInfo target(
    topology::HardwareDisplayTargetKey key, core::i32 x) {
    net::ClusterDisplayTargetInfo value{};
    value.adapter_id = key.adapter_id;
    value.target_id = key.target_id;
    value.connector_instance = key.connector_instance;
    value.active = true;
    value.available = true;
    value.width = 3840U;
    value.height = 2160U;
    value.refresh_numerator = 60'000U;
    value.refresh_denominator = 1'001U;
    value.desktop_x = x;
    value.desktop_y = 0;
    value.name = "LED " + std::to_string(key.target_id);
    return value;
}

[[nodiscard]] app::PlaybackOptions options() {
    app::PlaybackOptions value{};
    value.output = true;
    value.windowed = true;
    value.node_index = 3U;
    value.surface_width = 1280U;
    value.surface_height = 720U;
    return value;
}

void two_bindings_create_two_exact_outputs() {
    constexpr topology::HardwareDisplayTargetKey left{
        0x1122334455667788ULL, 3U, 1U};
    constexpr topology::HardwareDisplayTargetKey right{
        0x1122334455667788ULL, 4U, 2U};
    topology::HardwareMap map{};
    map.devices.push_back(server(1U, 3U));
    map.output_bindings.push_back(binding(10U, 1U, 7U, left));
    map.output_bindings.push_back(binding(11U, 2U, 8U, right));
    net::ClusterMachineInfo inventory{};
    inventory.availability = net::ClusterMachineInfoAvailability::Available;
    inventory.display_state =
        net::ClusterDisplayEnumerationState::Available;
    inventory.display_targets.push_back(target(left, -3840));
    inventory.display_targets.push_back(target(right, 0));

    const auto deployed =
        app::resolve_output_deployment(options(), map, &inventory);
    DEPLOY_CHECK(deployed.ready());
    DEPLOY_CHECK(!deployed.legacy_single_output);
    DEPLOY_CHECK(deployed.render_server == doc::NodeId{1U});
    DEPLOY_CHECK(deployed.outputs.size() == 2U);
    DEPLOY_CHECK(deployed.outputs[0U].feed_index == 7U);
    DEPLOY_CHECK(deployed.outputs[0U].window.position_x == -3840);
    DEPLOY_CHECK(deployed.outputs[0U].window.fullscreen);
    DEPLOY_CHECK(deployed.outputs[0U].window.require_exact_display_mode);
    DEPLOY_CHECK(deployed.outputs[0U].window.refresh_numerator == 60'000U);
    DEPLOY_CHECK(deployed.outputs[1U].feed_index == 8U);
}

void missing_and_duplicate_targets_fail_deterministically() {
    constexpr topology::HardwareDisplayTargetKey key{
        0x2233445566778899ULL, 9U, 4U};
    topology::HardwareMap map{};
    map.devices.push_back(server(1U, 3U));
    map.output_bindings.push_back(binding(10U, 1U, 7U, key));
    net::ClusterMachineInfo inventory{};
    inventory.availability = net::ClusterMachineInfoAvailability::Available;
    inventory.display_state =
        net::ClusterDisplayEnumerationState::Available;

    auto deployed = app::resolve_output_deployment(options(), map, &inventory);
    DEPLOY_CHECK(!deployed.ready());
    DEPLOY_CHECK(deployed.outputs.empty());
    DEPLOY_CHECK(deployed.issues.size() == 1U);
    DEPLOY_CHECK(deployed.issues[0U].code ==
                 app::OutputDeploymentIssueCode::MissingDisplayTarget);
    DEPLOY_CHECK(deployed.issues[0U].binding == doc::NodeId{10U});

    inventory.display_targets.push_back(target(key, 0));
    inventory.display_targets.push_back(target(key, 0));
    deployed = app::resolve_output_deployment(options(), map, &inventory);
    DEPLOY_CHECK(!deployed.ready());
    DEPLOY_CHECK(deployed.issues[0U].code ==
                 app::OutputDeploymentIssueCode::DuplicateDisplayTarget);
}

void legacy_projects_keep_the_single_output_contract() {
    topology::HardwareMap map{};
    auto configured = server(1U, 3U);
    configured.window_mode = topology::HardwareWindowMode::Borderless;
    configured.desktop_x = 1920;
    configured.desktop_y = 0;
    configured.width = 1920U;
    configured.height = 1080U;
    map.devices.push_back(configured);

    const auto deployed = app::resolve_output_deployment(options(), map, nullptr);
    DEPLOY_CHECK(deployed.ready());
    DEPLOY_CHECK(deployed.legacy_single_output);
    DEPLOY_CHECK(deployed.outputs.size() == 1U);
    DEPLOY_CHECK(deployed.outputs[0U].feed_index == 3U);
    DEPLOY_CHECK(deployed.outputs[0U].window.position_x == 1920);
    DEPLOY_CHECK(deployed.outputs[0U].window.borderless);
    DEPLOY_CHECK(!deployed.outputs[0U].window.require_exact_display_mode);
}

struct Case final {
    const char* name;
    void (*run)();
};

constexpr std::array<Case, 3U> cases{{
    {"two bindings create two exact outputs",
     &two_bindings_create_two_exact_outputs},
    {"missing and duplicate targets fail deterministically",
     &missing_and_duplicate_targets_fail_deterministically},
    {"legacy projects keep one output",
     &legacy_projects_keep_the_single_output_contract},
}};

} // namespace

int main() {
    std::size_t failures{};
    for (const auto& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << cases.size() - failures << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
