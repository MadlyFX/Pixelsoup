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
    // Deliberately not the signal size above: the desktop extent is the
    // source mode, and a driver that scales makes the two differ. Distinct
    // values here keep a test from passing because they happened to match.
    value.desktop_width = 2560U;
    value.desktop_height = 1440U;
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
    map.output_bindings[0U].output_bit_depth = 10U;
    map.output_bindings[1U].output_bit_depth = 16U;
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
    DEPLOY_CHECK(deployed.outputs[0U].window.output_bit_depth == 10U);
    DEPLOY_CHECK(deployed.outputs[1U].window.output_bit_depth == 16U);
    DEPLOY_CHECK(deployed.outputs[0U].feed_index == 7U);
    DEPLOY_CHECK(deployed.outputs[0U].window.position_x == -3840);
    DEPLOY_CHECK(deployed.outputs[0U].window.fullscreen);
    DEPLOY_CHECK(deployed.outputs[0U].window.require_exact_display_mode);
    DEPLOY_CHECK(deployed.outputs[0U].window.refresh_numerator == 60'000U);
    DEPLOY_CHECK(deployed.outputs[1U].feed_index == 8U);
    // The display each output drives travels as the identified target's own
    // desktop rectangle, not as the window size: a presenter that matched on
    // the binding's raster would pick the wrong display the moment a driver
    // scales, which is exactly when it matters.
    DEPLOY_CHECK(deployed.outputs[0U].window.display_bounds_x == -3840);
    DEPLOY_CHECK(deployed.outputs[0U].window.display_bounds_y == 0);
    DEPLOY_CHECK(deployed.outputs[0U].window.display_bounds_width == 2560U);
    DEPLOY_CHECK(deployed.outputs[0U].window.display_bounds_height == 1440U);
    DEPLOY_CHECK(deployed.outputs[1U].window.display_bounds_x == 0);
    DEPLOY_CHECK(app::runtime_output_deployment_state(deployed) ==
                 net::RuntimeOutputDeploymentState::Ready);
}

// Two active targets occupying one desktop area are mirrored. The binding is
// refused with a reason of its own rather than opening a window at a shared
// origin, which is how an exclusive mode ends up applied to the other screen.
void a_mirrored_display_is_refused_with_its_own_reason() {
    constexpr topology::HardwareDisplayTargetKey bound{
        0x5566778899aabbccULL, 11U, 0U};
    constexpr topology::HardwareDisplayTargetKey panel{
        0x00ddeeff00112233ULL, 12U, 0U};
    topology::HardwareMap map{};
    map.devices.push_back(server(1U, 3U));
    map.output_bindings.push_back(binding(10U, 1U, 7U, bound));
    net::ClusterMachineInfo inventory{};
    inventory.availability = net::ClusterMachineInfoAvailability::Available;
    inventory.display_state =
        net::ClusterDisplayEnumerationState::Available;
    inventory.display_targets.push_back(target(bound, 0));
    inventory.display_targets.push_back(target(panel, 0));

    const auto deployed =
        app::resolve_output_deployment(options(), map, &inventory);
    DEPLOY_CHECK(!deployed.ready());
    DEPLOY_CHECK(deployed.outputs.empty());
    DEPLOY_CHECK(deployed.issues.size() == 1U);
    DEPLOY_CHECK(deployed.issues[0U].code ==
                 app::OutputDeploymentIssueCode::MirroredDisplayTarget);
    DEPLOY_CHECK(deployed.issues[0U].binding == doc::NodeId{10U});
    // The reason is what the Director is told, so it can say "mirrored"
    // rather than letting the node fall silent and reading that as loading.
    DEPLOY_CHECK(app::runtime_output_deployment_state(deployed) ==
                 net::RuntimeOutputDeploymentState::MirroredDisplayTarget);
}

// The same two displays at different origins are an extended desktop, which
// is the configuration this refusal is asking the operator to move to.
void extended_displays_at_distinct_origins_still_deploy() {
    constexpr topology::HardwareDisplayTargetKey bound{
        0x5566778899aabbccULL, 11U, 0U};
    constexpr topology::HardwareDisplayTargetKey panel{
        0x00ddeeff00112233ULL, 12U, 0U};
    topology::HardwareMap map{};
    map.devices.push_back(server(1U, 3U));
    map.output_bindings.push_back(binding(10U, 1U, 7U, bound));
    net::ClusterMachineInfo inventory{};
    inventory.availability = net::ClusterMachineInfoAvailability::Available;
    inventory.display_state =
        net::ClusterDisplayEnumerationState::Available;
    inventory.display_targets.push_back(target(bound, 2560));
    inventory.display_targets.push_back(target(panel, 0));

    const auto deployed =
        app::resolve_output_deployment(options(), map, &inventory);
    DEPLOY_CHECK(deployed.ready());
    DEPLOY_CHECK(deployed.outputs.size() == 1U);
    DEPLOY_CHECK(deployed.outputs[0U].window.display_bounds_x == 2560);
    DEPLOY_CHECK(deployed.outputs[0U].window.display_bounds_width == 2560U);
}

// An inactive neighbour is not a mirror. Windows reports every connector it
// knows about, most of them switched off and all of them at the origin.
void inactive_targets_at_the_origin_are_not_mirrors() {
    constexpr topology::HardwareDisplayTargetKey bound{
        0x5566778899aabbccULL, 11U, 0U};
    constexpr topology::HardwareDisplayTargetKey spare{
        0x5566778899aabbccULL, 13U, 1U};
    topology::HardwareMap map{};
    map.devices.push_back(server(1U, 3U));
    map.output_bindings.push_back(binding(10U, 1U, 7U, bound));
    net::ClusterMachineInfo inventory{};
    inventory.availability = net::ClusterMachineInfoAvailability::Available;
    inventory.display_state =
        net::ClusterDisplayEnumerationState::Available;
    inventory.display_targets.push_back(target(bound, 0));
    auto unused = target(spare, 0);
    unused.active = false;
    inventory.display_targets.push_back(unused);

    const auto deployed =
        app::resolve_output_deployment(options(), map, &inventory);
    DEPLOY_CHECK(deployed.ready());
    DEPLOY_CHECK(deployed.outputs.size() == 1U);
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
    // Cabled for control only: no bound display identities yet.
    topology::HardwareDevice operator_machine{};
    operator_machine.node = doc::NodeId{2U};
    operator_machine.kind = topology::HardwareDeviceKind::OperatorMachine;
    map.devices.push_back(operator_machine);
    map.links.push_back(topology::HardwareLink{
        doc::NodeId{20U}, topology::HardwareEndpoint{doc::NodeId{2U}, 0U},
        topology::HardwareEndpoint{doc::NodeId{1U}, 0U}});

    const auto deployed = app::resolve_output_deployment(options(), map, nullptr);
    DEPLOY_CHECK(deployed.ready());
    DEPLOY_CHECK(deployed.legacy_single_output);
    DEPLOY_CHECK(deployed.outputs.size() == 1U);
    DEPLOY_CHECK(deployed.outputs[0U].feed_index == 3U);
    DEPLOY_CHECK(deployed.outputs[0U].window.position_x == 1920);
    DEPLOY_CHECK(deployed.outputs[0U].window.borderless);
    DEPLOY_CHECK(!deployed.outputs[0U].window.require_exact_display_mode);
}

// A second card on the same node index with no cable is a parked
// alternative: the bound card still deploys, and the parked card alone falls
// back to the launch output rather than claiming the process.
void a_parked_card_does_not_shadow_the_bound_server() {
    constexpr topology::HardwareDisplayTargetKey key{
        0x3344556677889900ULL, 5U, 1U};
    topology::HardwareMap map{};
    map.devices.push_back(server(1U, 3U));
    map.devices.push_back(server(2U, 3U));
    map.output_bindings.push_back(binding(10U, 1U, 7U, key));
    net::ClusterMachineInfo inventory{};
    inventory.availability = net::ClusterMachineInfoAvailability::Available;
    inventory.display_state =
        net::ClusterDisplayEnumerationState::Available;
    inventory.display_targets.push_back(target(key, 0));

    auto deployed = app::resolve_output_deployment(options(), map, &inventory);
    DEPLOY_CHECK(deployed.ready());
    DEPLOY_CHECK(deployed.render_server == doc::NodeId{1U});
    DEPLOY_CHECK(deployed.outputs.size() == 1U);

    map.output_bindings.clear();
    deployed = app::resolve_output_deployment(options(), map, &inventory);
    DEPLOY_CHECK(deployed.ready());
    DEPLOY_CHECK(deployed.legacy_single_output);
    DEPLOY_CHECK(deployed.render_server == doc::NodeId{});
    DEPLOY_CHECK(deployed.outputs[0U].display_name == "launch output");
}

struct Case final {
    const char* name;
    void (*run)();
};

constexpr std::array<Case, 7U> cases{{
    {"two bindings create two exact outputs",
     &two_bindings_create_two_exact_outputs},
    {"a mirrored display is refused with its own reason",
     &a_mirrored_display_is_refused_with_its_own_reason},
    {"extended displays at distinct origins still deploy",
     &extended_displays_at_distinct_origins_still_deploy},
    {"inactive targets at the origin are not mirrors",
     &inactive_targets_at_the_origin_are_not_mirrors},
    {"missing and duplicate targets fail deterministically",
     &missing_and_duplicate_targets_fail_deterministically},
    {"legacy projects keep one output",
     &legacy_projects_keep_the_single_output_contract},
    {"a parked card does not shadow the bound server",
     &a_parked_card_does_not_shadow_the_bound_server},
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
