#include "renderer_lockstep.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mediaserver {
namespace {

#define LOCKSTEP_CHECK(condition)                                             \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error{std::string{"check failed: "} +         \
                                     #condition};                             \
        }                                                                     \
    } while (false)

[[nodiscard]] scene::RendererSettings advanced(bool required = true) {
    scene::RendererSettings settings{};
    settings.backend = scene::RendererBackend::Advanced;
    settings.required = required;
    return settings;
}

[[nodiscard]] net::RuntimeRendererReadiness observation(
    const scene::RendererSettings& settings,
    net::RuntimeRendererBackend selected,
    bool satisfied = true) {
    const auto requested = settings.backend == scene::RendererBackend::Advanced
        ? net::RuntimeRendererBackend::Advanced
        : net::RuntimeRendererBackend::Basic;
    auto result = net::RuntimeRendererReadiness{
        scene::renderer_settings_digest(settings),
        satisfied
            ? (selected == requested
                   ? net::RuntimeRendererReadinessState::Ready
                   : net::RuntimeRendererReadinessState::BasicFallback)
            : net::RuntimeRendererReadinessState::
                  AdvancedImplementationUnavailable,
        requested,
        selected,
        0U,
        satisfied,
        satisfied && selected != requested,
    };
    auto& proof = result.conformance;
    proof.state = core::ConformanceState::Passed; proof.version = core::render_conformance_version;
    proof.adapter.name[0] = 'G'; proof.adapter.driver_version = 17U;
    proof.adapter.vendor = 0x10deU; proof.adapter.device = 0x2204U;
    proof.strict_float = true; proof.shader_bundle.fill(0x3aU);
    for (std::size_t i = 0; i < proof.passes.size(); ++i) proof.passes[i] = 42U + i;
    return result;
}

void exact_advanced_cohort_is_ready() {
    // Equal restart counts can still describe different temporal sequences.
    auto temporal_settings = advanced();
    temporal_settings.taau = true;
    auto a = observation(temporal_settings, net::RuntimeRendererBackend::Advanced);
    auto b = a;
    a.temporal_restarts = b.temporal_restarts = 2U;
    a.temporal_restart_frame = 31U;
    b.temporal_restart_frame = 32U;
    auto temporal_state = server::begin_renderer_lockstep(temporal_settings, true);
    server::observe_renderer_lockstep(temporal_state, &a, 0U, true);
    server::observe_renderer_lockstep(temporal_state, &b, 1U, false);
    LOCKSTEP_CHECK(temporal_state.code == server::RendererLockstepCode::TemporalHistoryMismatch);
    b = a;
    temporal_state = server::begin_renderer_lockstep(temporal_settings, true);
    server::observe_renderer_lockstep(temporal_state, &a, 0U, true);
    server::observe_renderer_lockstep(temporal_state, &b, 1U, false);
    LOCKSTEP_CHECK(temporal_state.satisfied());
    ++b.temporal_restarts;
    server::observe_renderer_lockstep(temporal_state, &b, 2U, false);
    LOCKSTEP_CHECK(!temporal_state.satisfied());
    // A replicated profile change and fresh zero reports recover the cohort.
    temporal_settings.taa_sequence_length = 16U;
    a = observation(temporal_settings, net::RuntimeRendererBackend::Advanced);
    b = a;
    temporal_state = server::begin_renderer_lockstep(temporal_settings, true);
    server::observe_renderer_lockstep(temporal_state, &a, 0U, true);
    server::observe_renderer_lockstep(temporal_state, &b, 1U, false);
    LOCKSTEP_CHECK(temporal_state.satisfied());

    const auto settings = advanced();
    auto state = server::begin_renderer_lockstep(settings, true);
    const auto local = observation(
        settings, net::RuntimeRendererBackend::Advanced);
    const auto node_one = local;
    const auto node_two = local;
    server::observe_renderer_lockstep(state, &local, 0U, true);
    server::observe_renderer_lockstep(state, &node_one, 1U, false);
    server::observe_renderer_lockstep(state, &node_two, 2U, false);
    LOCKSTEP_CHECK(state.satisfied());
    LOCKSTEP_CHECK(state.code == server::RendererLockstepCode::Ready);
    LOCKSTEP_CHECK(state.selected == net::RuntimeRendererBackend::Advanced);
}

void missing_stale_and_unavailable_reports_fail_closed() {
    const auto settings = advanced();

    auto missing = server::begin_renderer_lockstep(settings, true);
    server::observe_renderer_lockstep(missing, nullptr, 4U, false);
    LOCKSTEP_CHECK(!missing.satisfied());
    LOCKSTEP_CHECK(
        missing.code == server::RendererLockstepCode::ObservationMissing);
    LOCKSTEP_CHECK(missing.failing_node == 4U);

    auto stale = server::begin_renderer_lockstep(settings, true);
    auto stale_report = observation(
        settings, net::RuntimeRendererBackend::Advanced);
    ++stale_report.profile_digest;
    server::observe_renderer_lockstep(stale, &stale_report, 5U, false);
    LOCKSTEP_CHECK(!stale.satisfied());
    LOCKSTEP_CHECK(
        stale.code == server::RendererLockstepCode::ProfileMismatch);

    auto unavailable = server::begin_renderer_lockstep(settings, true);
    const auto unavailable_report = observation(
        settings, net::RuntimeRendererBackend::Advanced, false);
    server::observe_renderer_lockstep(
        unavailable, &unavailable_report, 6U, false);
    LOCKSTEP_CHECK(!unavailable.satisfied());
    LOCKSTEP_CHECK(unavailable.code ==
                   server::RendererLockstepCode::RendererUnavailable);
}

void optional_fallback_requires_every_member_to_fallback() {
    const auto settings = advanced(false);
    const auto fallback = observation(
        settings, net::RuntimeRendererBackend::Basic);

    auto common = server::begin_renderer_lockstep(settings, true);
    server::observe_renderer_lockstep(common, &fallback, 0U, true);
    server::observe_renderer_lockstep(common, &fallback, 1U, false);
    LOCKSTEP_CHECK(common.satisfied());
    LOCKSTEP_CHECK(common.selected == net::RuntimeRendererBackend::Basic);

    auto mixed = server::begin_renderer_lockstep(settings, true);
    const auto native = observation(
        settings, net::RuntimeRendererBackend::Advanced);
    server::observe_renderer_lockstep(mixed, &native, 0U, true);
    server::observe_renderer_lockstep(mixed, &fallback, 1U, false);
    LOCKSTEP_CHECK(!mixed.satisfied());
    LOCKSTEP_CHECK(
        mixed.code == server::RendererLockstepCode::SelectionMismatch);
}

void required_policy_rejects_fallback_and_digest_covers_every_edit() {
    const auto required = advanced(true);
    auto state = server::begin_renderer_lockstep(required, true);
    auto forged_fallback = observation(
        required, net::RuntimeRendererBackend::Basic);
    server::observe_renderer_lockstep(state, &forged_fallback, 8U, false);
    LOCKSTEP_CHECK(!state.satisfied());
    LOCKSTEP_CHECK(state.code ==
                   server::RendererLockstepCode::RendererUnavailable);

    auto changed = required;
    changed.required = false;
    LOCKSTEP_CHECK(scene::renderer_settings_digest(changed) !=
                   scene::renderer_settings_digest(required));
    changed = required;
    changed.quality = scene::RendererQuality::Cinematic;
    LOCKSTEP_CHECK(scene::renderer_settings_digest(changed) !=
                   scene::renderer_settings_digest(required));
    changed = required;
    changed.ray_tracing = true;
    LOCKSTEP_CHECK(scene::renderer_settings_digest(changed) !=
                   scene::renderer_settings_digest(required));
    changed = required;
    ++changed.slice_apron_limit;
    LOCKSTEP_CHECK(scene::renderer_settings_digest(changed) !=
                   scene::renderer_settings_digest(required));
    // Shadow cascades and bounce strength decide what a node draws, so an
    // edit to either has to invalidate capability evidence gathered before it.
    changed = required;
    ++changed.shadow_cascades;
    LOCKSTEP_CHECK(scene::renderer_settings_digest(changed) !=
                   scene::renderer_settings_digest(required));
    changed = required;
    changed.gi_strength = required.gi_strength * 0.5F;
    LOCKSTEP_CHECK(scene::renderer_settings_digest(changed) !=
                   scene::renderer_settings_digest(required));
    // Clamping happens once, at the model boundary. Two out-of-contract edits
    // that resolve to the same authored settings must not look like different
    // profiles to a Director comparing digests.
    auto over = required;
    over.shadow_cascades = scene::max_shadow_cascades + 7U;
    over.gi_strength = scene::max_gi_strength * 4.0F;
    auto clamped_edit = required;
    clamped_edit.shadow_cascades = scene::max_shadow_cascades;
    clamped_edit.gi_strength = scene::max_gi_strength;
    LOCKSTEP_CHECK(scene::renderer_settings_digest(over) ==
                   scene::renderer_settings_digest(clamped_edit));
    auto invalid = required;
    invalid.gi_strength = std::numeric_limits<float>::quiet_NaN();
    LOCKSTEP_CHECK(scene::renderer_settings_digest(invalid) ==
                   scene::renderer_settings_digest(required));
}

void conformance_evidence_gates_program_and_names_the_first_pass() {
    const auto settings = advanced();
    auto local = observation(settings, net::RuntimeRendererBackend::Advanced);
    auto remote = local;
    const auto evaluate = [&](const net::RuntimeRendererReadiness& node, core::u64 request = 0U) {
        auto gate = server::begin_renderer_lockstep(settings, true);
        gate.conformance_request = request;
        server::observe_renderer_lockstep(gate, &local, 0U, true);
        server::observe_renderer_lockstep(gate, &node, 7U, false);
        return gate;
    };
    remote.conformance = {};
    LOCKSTEP_CHECK(evaluate(remote).code == server::RendererLockstepCode::ConformancePending);
    // Even when another readiness condition owns the headline, an unsynced
    // show cannot promote a node that has not supplied render evidence.
    LOCKSTEP_CHECK(evaluate(remote).holds_program(false, false));
    LOCKSTEP_CHECK(evaluate(remote).holds_program(false, true));
    remote = local; remote.conformance.state = core::ConformanceState::Failed;
    remote.conformance.failed_pass = core::ConformancePass::Fog;
    LOCKSTEP_CHECK(evaluate(remote).code == server::RendererLockstepCode::ConformanceFailed);
    for (std::size_t i = 0; i < core::conformance_pass_count; ++i) {
        remote = local; remote.conformance.passes[i] ^= 1U;
        // Also differ downstream: the earliest pass must win.
        remote.conformance.passes.back() ^= 2U;
        auto gate = evaluate(remote);
        LOCKSTEP_CHECK(!gate.satisfied());
        LOCKSTEP_CHECK(gate.failing_node == 7U && !gate.local_failure);
        LOCKSTEP_CHECK(static_cast<std::size_t>(gate.conformance_difference.pass) == i);
        LOCKSTEP_CHECK(core::conformance_detail(gate.conformance_difference) == core::conformance_pass_names[i]);
        server::observe_renderer_lockstep(gate, &local, 8U, false);
        LOCKSTEP_CHECK(!gate.satisfied());
    }
    remote = local; remote.conformance.shader_bundle[13] ^= 1U;
    LOCKSTEP_CHECK(evaluate(remote).conformance_difference.reason == core::ConformanceMismatch::Bundle);
    remote = local; ++remote.conformance.adapter.driver_version;
    LOCKSTEP_CHECK(evaluate(remote).conformance_difference.reason == core::ConformanceMismatch::Adapter);
    remote = local; ++remote.conformance.version;
    LOCKSTEP_CHECK(evaluate(remote).conformance_difference.reason == core::ConformanceMismatch::Version);
    remote = local; remote.conformance.strict_float = false;
    LOCKSTEP_CHECK(evaluate(remote).conformance_difference.reason == core::ConformanceMismatch::Bundle);
    LOCKSTEP_CHECK(evaluate(local, 1U).code == server::RendererLockstepCode::ConformancePending);
    local.conformance.request = 1U;
    LOCKSTEP_CHECK(evaluate(remote, 1U).code == server::RendererLockstepCode::ConformancePending);
    remote = local;
    LOCKSTEP_CHECK(evaluate(remote, 1U).satisfied());
    LOCKSTEP_CHECK(!evaluate(remote, 1U).holds_program(false, false));
    LOCKSTEP_CHECK(evaluate(remote, 1U).holds_program(true, false));
}

void inactive_cohort_needs_no_observation() {
    auto state = server::begin_renderer_lockstep(advanced(), false);
    server::observe_renderer_lockstep(state, nullptr, 99U, false);
    LOCKSTEP_CHECK(state.satisfied());
    LOCKSTEP_CHECK(state.code == server::RendererLockstepCode::Inactive);
    LOCKSTEP_CHECK(server::renderer_lockstep_headline(state.code) ==
                   "RENDERER INACTIVE");
}

} // namespace
} // namespace mediaserver

int main() {
    using namespace mediaserver;
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"exact Advanced cohort", &exact_advanced_cohort_is_ready},
        {"missing stale unavailable fail closed",
         &missing_stale_and_unavailable_reports_fail_closed},
        {"optional fallback cohort parity",
         &optional_fallback_requires_every_member_to_fallback},
        {"required fallback and exact digest",
         &required_policy_rejects_fallback_and_digest_covers_every_edit},
        {"conformance evidence", &conformance_evidence_gates_program_and_names_the_first_pass},
        {"inactive cohort", &inactive_cohort_needs_no_observation},
    };
    std::size_t failures{};
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[pass] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0U ? 0 : 1;
}
