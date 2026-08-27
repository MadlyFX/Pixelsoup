#include <mediaserver/sync/sync_group.hpp>
#include <mediaserver/sync/sync_readiness.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace sync = mediaserver::sync;
namespace topology = mediaserver::topology;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define SYNC_CHECK(...)                                                        \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] topology::HardwareDevice
server(core::u64 id, core::u32 node_index) {
    topology::HardwareDevice device{};
    device.node = doc::NodeId{id};
    device.kind = topology::HardwareDeviceKind::RenderServer;
    device.name = "server";
    device.node_index = node_index;
    return device;
}

[[nodiscard]] topology::HardwareDevice
downstream(core::u64 id, topology::HardwareDeviceKind kind) {
    topology::HardwareDevice device{};
    device.node = doc::NodeId{id};
    device.kind = kind;
    device.name = "downstream";
    return device;
}

[[nodiscard]] topology::HardwareLink link(core::u64 id, core::u64 from,
                                          core::u8 from_port, core::u64 to,
                                          core::u8 to_port) {
    return topology::HardwareLink{
        doc::NodeId{id},
        topology::HardwareEndpoint{doc::NodeId{from}, from_port},
        topology::HardwareEndpoint{doc::NodeId{to}, to_port}};
}

[[nodiscard]] topology::HardwareOutputBinding
binding(core::u64 id, core::u64 render_server, core::u8 port,
        core::u32 feed_index, core::u64 adapter, core::u32 target,
        core::u32 width = 1920U, core::u32 height = 1080U,
        core::u32 refresh_numerator = 60U,
        core::u32 refresh_denominator = 1U) {
    topology::HardwareOutputBinding value{};
    value.node = doc::NodeId{id};
    value.render_server = doc::NodeId{render_server};
    value.logical_port = port;
    value.feed_index = feed_index;
    value.target = topology::HardwareDisplayTargetKey{adapter, target, 0U};
    value.width = width;
    value.height = height;
    value.refresh_numerator = refresh_numerator;
    value.refresh_denominator = refresh_denominator;
    return value;
}

// Two render servers, each with one cabled output, both feeding one LED
// processor that drives one wall: exactly one synchronization group.
[[nodiscard]] topology::HardwareMap shared_wall_map() {
    topology::HardwareMap map{};
    map.devices.push_back(server(10U, 0U));
    map.devices.push_back(server(11U, 1U));
    map.devices.push_back(
        downstream(20U, topology::HardwareDeviceKind::LedProcessor));
    map.devices.push_back(
        downstream(21U, topology::HardwareDeviceKind::LedWall));
    map.links.push_back(link(30U, 10U, 1U, 20U, 0U));
    map.links.push_back(link(31U, 11U, 1U, 20U, 0U));
    map.links.push_back(link(32U, 20U, 1U, 21U, 0U));
    map.output_bindings.push_back(binding(40U, 10U, 1U, 0U, 0x1111U, 1U));
    map.output_bindings.push_back(binding(41U, 11U, 1U, 1U, 0x2222U, 1U));
    return map;
}

[[nodiscard]] sync::SyncPolicy framelock_policy() {
    sync::SyncPolicy policy{};
    policy.authored = true;
    policy.node = doc::NodeId{1U};
    policy.mode = sync::SyncMode::Framelock;
    policy.group = 1U;
    policy.barrier = 1U;
    return policy;
}

[[nodiscard]] sync::NodeSyncStatus locked_node(core::u32 node_index,
                                               core::u64 adapter,
                                               core::u32 target) {
    sync::NodeSyncStatus status{};
    status.node_index = node_index;
    status.board = sync::SyncDeviceAvailability::Present;
    status.reference = sync::SyncReferenceState::InUse;
    status.barrier = sync::PresentBarrierState::Joined;
    status.barrier_group = 1U;
    status.barrier_id = 1U;
    status.barrier_joined_outputs = 1U;
    status.barrier_expected_outputs = 1U;
    status.machine_reported = true;
    status.runtime_reported = true;
    sync::SyncOutputStatus output{};
    output.target = topology::HardwareDisplayTargetKey{adapter, target, 0U};
    output.lock = sync::SyncLockState::Locked;
    output.timing = sync::SyncTiming{1920U, 1080U, 60U, 1U};
    output.timing_reported = true;
    status.outputs.push_back(output);
    return status;
}

[[nodiscard]] bool has_diagnostic(const sync::SyncTopology& derived,
                                  sync::SyncGroupDiagnosticCode code) {
    return std::any_of(derived.diagnostics.begin(), derived.diagnostics.end(),
                       [code](const sync::SyncGroupDiagnostic& diagnostic) {
                           return diagnostic.code == code;
                       });
}

void outputs_reaching_one_wall_form_one_group() {
    const auto derived =
        sync::derive_sync_topology(shared_wall_map(), framelock_policy());
    SYNC_CHECK(derived.groups.size() == 1U);
    SYNC_CHECK(derived.groups.front().members.size() == 2U);
    SYNC_CHECK(derived.groups.front().index == 0U);
    SYNC_CHECK(derived.groups.front().downstream.size() == 2U);
    SYNC_CHECK(derived.node_indices == std::vector<core::u32>{0U, 1U});
    SYNC_CHECK(derived.valid());
    SYNC_CHECK(derived.group_for(doc::NodeId{11U}, 1U) != nullptr);
    SYNC_CHECK(derived.group_for(doc::NodeId{11U}, 2U) == nullptr);
}

void two_outputs_of_one_machine_on_separate_walls_are_separate_groups() {
    topology::HardwareMap map{};
    map.devices.push_back(server(10U, 0U));
    map.devices.push_back(
        downstream(20U, topology::HardwareDeviceKind::LedWall));
    map.devices.push_back(
        downstream(21U, topology::HardwareDeviceKind::LedWall));
    map.links.push_back(link(30U, 10U, 1U, 20U, 0U));
    map.links.push_back(link(31U, 10U, 2U, 21U, 0U));
    map.output_bindings.push_back(binding(40U, 10U, 1U, 0U, 0x1111U, 1U));
    map.output_bindings.push_back(binding(41U, 10U, 2U, 1U, 0x1111U, 2U));

    const auto derived = sync::derive_sync_topology(map, framelock_policy());
    SYNC_CHECK(derived.groups.size() == 2U);
    SYNC_CHECK(derived.groups[0U].members.size() == 1U);
    SYNC_CHECK(derived.groups[1U].members.size() == 1U);
    SYNC_CHECK(derived.groups[0U].members.front().logical_port == 1U);
    SYNC_CHECK(derived.groups[1U].members.front().logical_port == 2U);
    SYNC_CHECK(derived.valid());
}

void mismatched_timing_inside_a_group_is_reported() {
    topology::HardwareMap map = shared_wall_map();
    map.output_bindings[1U] =
        binding(41U, 11U, 1U, 1U, 0x2222U, 1U, 1920U, 1080U, 60000U, 1001U);

    const auto derived = sync::derive_sync_topology(map, framelock_policy());
    SYNC_CHECK(derived.groups.size() == 1U);
    SYNC_CHECK(!derived.groups.front().timing_consistent);
    SYNC_CHECK(has_diagnostic(derived,
                              sync::SyncGroupDiagnosticCode::RefreshMismatch));
    SYNC_CHECK(!has_diagnostic(derived,
                               sync::SyncGroupDiagnosticCode::RasterMismatch));
    SYNC_CHECK(!derived.valid());
}

void a_policy_timing_that_the_wiring_does_not_match_is_reported() {
    sync::SyncPolicy policy = framelock_policy();
    policy.required_timing = sync::SyncTiming{3840U, 2160U, 60U, 1U};
    const auto derived = sync::derive_sync_topology(shared_wall_map(), policy);
    SYNC_CHECK(derived.groups.size() == 1U);
    // The group is internally consistent; it just is not what the project
    // policy requires.
    SYNC_CHECK(derived.groups.front().timing_consistent);
    SYNC_CHECK(has_diagnostic(
        derived, sync::SyncGroupDiagnosticCode::PolicyTimingMismatch));
}

void unbound_and_uncabled_outputs_are_distinguished() {
    topology::HardwareMap map = shared_wall_map();
    map.output_bindings.erase(map.output_bindings.begin());
    map.output_bindings.push_back(binding(42U, 10U, 4U, 4U, 0x1111U, 4U));

    const auto derived = sync::derive_sync_topology(map, framelock_policy());
    SYNC_CHECK(has_diagnostic(derived,
                              sync::SyncGroupDiagnosticCode::UnboundOutput));
    SYNC_CHECK(has_diagnostic(derived,
                              sync::SyncGroupDiagnosticCode::UncabledBinding));
    const auto uncabled = std::find_if(
        derived.diagnostics.begin(), derived.diagnostics.end(),
        [](const sync::SyncGroupDiagnostic& diagnostic) {
            return diagnostic.code ==
                sync::SyncGroupDiagnosticCode::UncabledBinding;
        });
    SYNC_CHECK(uncabled != derived.diagnostics.end());
    SYNC_CHECK(uncabled->group_index == sync::no_sync_group_index);
}

void network_cables_never_create_a_sync_group() {
    topology::HardwareMap map{};
    map.devices.push_back(server(10U, 0U));
    topology::HardwareDevice operator_machine{};
    operator_machine.node = doc::NodeId{11U};
    operator_machine.kind = topology::HardwareDeviceKind::OperatorMachine;
    map.devices.push_back(operator_machine);
    map.links.push_back(link(30U, 11U, 0U, 10U, 0U));

    const auto derived = sync::derive_sync_topology(map, framelock_policy());
    SYNC_CHECK(derived.groups.empty());
    SYNC_CHECK(derived.node_indices.empty());
}

void an_unsynced_policy_needs_no_physical_evidence() {
    sync::SyncPolicy policy{};
    const auto derived = sync::derive_sync_topology(shared_wall_map(), policy);
    sync::SyncReadinessInput input{};
    input.policy = &policy;
    input.topology = &derived;
    const auto readiness = sync::evaluate_sync_readiness(input);
    SYNC_CHECK(readiness.satisfied);
    SYNC_CHECK(!readiness.verified);
    SYNC_CHECK(readiness.code == sync::SyncReadinessCode::Unsynced);
    SYNC_CHECK(readiness.headline() == "SYNC POLICY UNSYNCED");
}

void a_framelock_policy_with_no_reports_is_unverified() {
    const sync::SyncPolicy policy = framelock_policy();
    const auto derived = sync::derive_sync_topology(shared_wall_map(), policy);
    sync::SyncReadinessInput input{};
    input.policy = &policy;
    input.topology = &derived;
    const auto readiness = sync::evaluate_sync_readiness(input);
    SYNC_CHECK(!readiness.satisfied);
    SYNC_CHECK(!readiness.verified);
    SYNC_CHECK(readiness.code == sync::SyncReadinessCode::Unverified);
    SYNC_CHECK(readiness.expected_outputs == 2U);
}

void a_fully_locked_group_is_verified() {
    const sync::SyncPolicy policy = framelock_policy();
    const auto derived = sync::derive_sync_topology(shared_wall_map(), policy);
    const std::array<sync::NodeSyncStatus, 2U> nodes{
        {locked_node(0U, 0x1111U, 1U), locked_node(1U, 0x2222U, 1U)}};
    sync::SyncReadinessInput input{};
    input.policy = &policy;
    input.topology = &derived;
    input.nodes = nodes;
    const auto readiness = sync::evaluate_sync_readiness(input);
    SYNC_CHECK(readiness.satisfied);
    SYNC_CHECK(readiness.verified);
    SYNC_CHECK(!readiness.degraded);
    SYNC_CHECK(readiness.code == sync::SyncReadinessCode::Ready);
    SYNC_CHECK(readiness.locked_outputs == 2U);
    SYNC_CHECK(readiness.barrier_joined_outputs == 2U);
}

void one_unjoined_barrier_reports_swap_barrier_incomplete() {
    const sync::SyncPolicy policy = framelock_policy();
    const auto derived = sync::derive_sync_topology(shared_wall_map(), policy);
    std::array<sync::NodeSyncStatus, 2U> nodes{
        {locked_node(0U, 0x1111U, 1U), locked_node(1U, 0x2222U, 1U)}};
    nodes[1U].barrier = sync::PresentBarrierState::NotJoined;
    sync::SyncReadinessInput input{};
    input.policy = &policy;
    input.topology = &derived;
    input.nodes = nodes;
    const auto readiness = sync::evaluate_sync_readiness(input);
    SYNC_CHECK(!readiness.satisfied);
    SYNC_CHECK(!readiness.verified);
    SYNC_CHECK(readiness.code ==
               sync::SyncReadinessCode::SwapBarrierIncomplete);
    SYNC_CHECK(readiness.headline() == "SWAP BARRIER INCOMPLETE");
}

void a_lost_reference_is_reported_only_for_external_genlock() {
    sync::SyncPolicy policy = framelock_policy();
    const auto derived = sync::derive_sync_topology(shared_wall_map(), policy);
    std::array<sync::NodeSyncStatus, 2U> nodes{
        {locked_node(0U, 0x1111U, 1U), locked_node(1U, 0x2222U, 1U)}};
    nodes[0U].reference = sync::SyncReferenceState::Absent;

    sync::SyncReadinessInput input{};
    input.policy = &policy;
    input.topology = &derived;
    input.nodes = nodes;
    SYNC_CHECK(sync::evaluate_sync_readiness(input).code ==
               sync::SyncReadinessCode::Ready);

    policy.mode = sync::SyncMode::ExternalGenlock;
    const auto genlock_topology = sync::derive_sync_topology(
        shared_wall_map(), policy);
    input.topology = &genlock_topology;
    const auto readiness = sync::evaluate_sync_readiness(input);
    SYNC_CHECK(readiness.code == sync::SyncReadinessCode::ReferenceLost);
    SYNC_CHECK(!readiness.satisfied);

    nodes[0U].reference = sync::SyncReferenceState::Present;
    const auto idle = sync::evaluate_sync_readiness(input);
    SYNC_CHECK(idle.code == sync::SyncReadinessCode::ReferenceNotInUse);
}

void a_degraded_policy_promotes_but_never_claims_verification() {
    sync::SyncPolicy policy = framelock_policy();
    policy.failure = sync::SyncFailurePolicy::Degraded;
    const auto derived = sync::derive_sync_topology(shared_wall_map(), policy);
    std::array<sync::NodeSyncStatus, 2U> nodes{
        {locked_node(0U, 0x1111U, 1U), locked_node(1U, 0x2222U, 1U)}};
    nodes[1U].outputs.front().lock = sync::SyncLockState::Unlocked;
    sync::SyncReadinessInput input{};
    input.policy = &policy;
    input.topology = &derived;
    input.nodes = nodes;
    const auto readiness = sync::evaluate_sync_readiness(input);
    SYNC_CHECK(readiness.satisfied);
    SYNC_CHECK(readiness.degraded);
    SYNC_CHECK(!readiness.verified);
    SYNC_CHECK(readiness.code == sync::SyncReadinessCode::OutputsUnlocked);
}

void simulated_evidence_never_satisfies_a_strict_policy() {
    const sync::SyncPolicy policy = framelock_policy();
    const auto derived = sync::derive_sync_topology(shared_wall_map(), policy);
    std::array<sync::NodeSyncStatus, 2U> nodes{
        {locked_node(0U, 0x1111U, 1U), locked_node(1U, 0x2222U, 1U)}};
    nodes[0U].simulated = true;
    sync::SyncReadinessInput input{};
    input.policy = &policy;
    input.topology = &derived;
    input.nodes = nodes;
    const auto readiness = sync::evaluate_sync_readiness(input);
    SYNC_CHECK(!readiness.satisfied);
    SYNC_CHECK(!readiness.verified);
    SYNC_CHECK(readiness.code == sync::SyncReadinessCode::Simulated);
}

void upstream_show_gates_are_never_relaxed_by_a_degraded_policy() {
    sync::SyncPolicy policy = framelock_policy();
    policy.failure = sync::SyncFailurePolicy::Degraded;
    const auto derived = sync::derive_sync_topology(shared_wall_map(), policy);
    const std::array<sync::NodeSyncStatus, 2U> nodes{
        {locked_node(0U, 0x1111U, 1U), locked_node(1U, 0x2222U, 1U)}};
    sync::SyncReadinessInput input{};
    input.policy = &policy;
    input.topology = &derived;
    input.nodes = nodes;

    input.assets_adopted = false;
    SYNC_CHECK(!sync::evaluate_sync_readiness(input).satisfied);
    SYNC_CHECK(sync::evaluate_sync_readiness(input).code ==
               sync::SyncReadinessCode::AssetsNotAdopted);

    input.assets_adopted = true;
    input.render_nodes_live = false;
    SYNC_CHECK(sync::evaluate_sync_readiness(input).code ==
               sync::SyncReadinessCode::RenderNodesMissing);

    input.render_nodes_live = true;
    input.exact_show_frame_ready = false;
    SYNC_CHECK(sync::evaluate_sync_readiness(input).code ==
               sync::SyncReadinessCode::ShowFrameNotReady);
}

// The physical verdict must stay timing-specific even when Program is held
// for an unrelated reason. "Assets not adopted" is a true reason Program is
// held; it is not a statement about raster boundaries.
void the_physical_verdict_is_independent_of_the_upstream_gates() {
    const sync::SyncPolicy policy = framelock_policy();
    const auto derived = sync::derive_sync_topology(shared_wall_map(), policy);
    const std::array<sync::NodeSyncStatus, 2U> nodes{
        {locked_node(0U, 0x1111U, 1U), locked_node(1U, 0x2222U, 1U)}};
    sync::SyncReadinessInput input{};
    input.policy = &policy;
    input.topology = &derived;
    input.nodes = nodes;
    input.assets_adopted = false;

    const auto readiness = sync::evaluate_sync_readiness(input);
    SYNC_CHECK(!readiness.satisfied);
    SYNC_CHECK(readiness.code == sync::SyncReadinessCode::AssetsNotAdopted);
    SYNC_CHECK(readiness.physical == sync::SyncReadinessCode::Ready);
    SYNC_CHECK(readiness.physical_headline() == "SYNC VERIFIED");
    SYNC_CHECK(readiness.verified);
    SYNC_CHECK(readiness.locked_outputs == 2U);

    // And the reverse: an unproven barrier is reported as a timing fact even
    // though every upstream gate is satisfied.
    std::array<sync::NodeSyncStatus, 2U> incomplete{
        {locked_node(0U, 0x1111U, 1U), locked_node(1U, 0x2222U, 1U)}};
    incomplete[1U].barrier = sync::PresentBarrierState::Unsupported;
    input.assets_adopted = true;
    input.nodes = incomplete;
    const auto blocked = sync::evaluate_sync_readiness(input);
    SYNC_CHECK(blocked.code == blocked.physical);
    SYNC_CHECK(blocked.physical ==
               sync::SyncReadinessCode::SwapBarrierIncomplete);
    SYNC_CHECK(!blocked.verified);
}

void a_timing_mismatch_blocks_before_any_status_is_consulted() {
    const sync::SyncPolicy policy = framelock_policy();
    topology::HardwareMap map = shared_wall_map();
    map.output_bindings[1U] =
        binding(41U, 11U, 1U, 1U, 0x2222U, 1U, 3840U, 2160U, 60U, 1U);
    const auto derived = sync::derive_sync_topology(map, policy);
    const std::array<sync::NodeSyncStatus, 2U> nodes{
        {locked_node(0U, 0x1111U, 1U), locked_node(1U, 0x2222U, 1U)}};
    sync::SyncReadinessInput input{};
    input.policy = &policy;
    input.topology = &derived;
    input.nodes = nodes;
    const auto readiness = sync::evaluate_sync_readiness(input);
    SYNC_CHECK(!readiness.satisfied);
    SYNC_CHECK(readiness.code == sync::SyncReadinessCode::TimingMismatch);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 16U> cases{{
    {"outputs reaching one wall form one group",
     &outputs_reaching_one_wall_form_one_group},
    {"two outputs of one machine on separate walls are separate groups",
     &two_outputs_of_one_machine_on_separate_walls_are_separate_groups},
    {"mismatched timing inside a group is reported",
     &mismatched_timing_inside_a_group_is_reported},
    {"a policy timing that the wiring does not match is reported",
     &a_policy_timing_that_the_wiring_does_not_match_is_reported},
    {"unbound and uncabled outputs are distinguished",
     &unbound_and_uncabled_outputs_are_distinguished},
    {"network cables never create a sync group",
     &network_cables_never_create_a_sync_group},
    {"an unsynced policy needs no physical evidence",
     &an_unsynced_policy_needs_no_physical_evidence},
    {"a framelock policy with no reports is unverified",
     &a_framelock_policy_with_no_reports_is_unverified},
    {"a fully locked group is verified", &a_fully_locked_group_is_verified},
    {"one unjoined barrier reports swap barrier incomplete",
     &one_unjoined_barrier_reports_swap_barrier_incomplete},
    {"a lost reference is reported only for external genlock",
     &a_lost_reference_is_reported_only_for_external_genlock},
    {"a degraded policy promotes but never claims verification",
     &a_degraded_policy_promotes_but_never_claims_verification},
    {"simulated evidence never satisfies a strict policy",
     &simulated_evidence_never_satisfies_a_strict_policy},
    {"upstream show gates are never relaxed by a degraded policy",
     &upstream_show_gates_are_never_relaxed_by_a_degraded_policy},
    {"a timing mismatch blocks before any status is consulted",
     &a_timing_mismatch_blocks_before_any_status_is_consulted},
    {"the physical verdict is independent of the upstream gates",
     &the_physical_verdict_is_independent_of_the_upstream_gates},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
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
