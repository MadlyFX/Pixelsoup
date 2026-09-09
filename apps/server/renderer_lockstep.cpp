#include "renderer_lockstep.hpp"

namespace mediaserver::server {
namespace {

[[nodiscard]] net::RuntimeRendererBackend runtime_backend(
    scene::RendererBackend backend) noexcept {
    return backend == scene::RendererBackend::Advanced
        ? net::RuntimeRendererBackend::Advanced
        : net::RuntimeRendererBackend::Basic;
}

void fail(RendererLockstep& state,
          RendererLockstepCode code,
          core::u32 node_index,
          bool local) noexcept {
    state.code = code;
    state.failing_node = node_index;
    state.local_failure = local;
}

} // namespace

std::string_view renderer_lockstep_headline(
    RendererLockstepCode code) noexcept {
    switch (code) {
    case RendererLockstepCode::Inactive:
        return "RENDERER INACTIVE";
    case RendererLockstepCode::Ready:
        return "RENDERER LOCKSTEP";
    case RendererLockstepCode::ObservationMissing:
        return "RENDERER REPORT PENDING";
    case RendererLockstepCode::ProfileMismatch:
        return "RENDERER PROFILE STALE";
    case RendererLockstepCode::RendererUnavailable:
        return "RENDERER UNAVAILABLE";
    case RendererLockstepCode::TemporalHistoryMismatch:
        return "TEMPORAL HISTORY MISMATCH";
    case RendererLockstepCode::ConformancePending: return "RENDER CHECK PENDING";
    case RendererLockstepCode::ConformanceFailed: return "RENDER CHECK FAILED";
    case RendererLockstepCode::ConformanceMismatch: return "RENDER CONFORMANCE MISMATCH";
    case RendererLockstepCode::FrameBudgetExceeded: return "FRAME BUDGET EXCEEDED";
    case RendererLockstepCode::FrameTimingPending: return "FRAME TIMING PENDING";
    case RendererLockstepCode::SelectionMismatch:
        return "RENDERER BACKEND MISMATCH";
    }
    return "RENDERER UNAVAILABLE";
}

RendererLockstep begin_renderer_lockstep(
    const scene::RendererSettings& source,
    bool active) noexcept {
    if (!active) {
        return {};
    }
    const scene::RendererSettings settings = scene::clamped(source);
    RendererLockstep result{};
    result.code = RendererLockstepCode::Ready;
    result.profile_digest = scene::renderer_settings_digest(settings);
    result.requested = runtime_backend(settings.backend);
    result.require_requested_selection = settings.required;
    result.frame_budget_us = settings.frame_budget_us;
    result.require_temporal_agreement = scene::antialiasing_for(settings)
        == scene::AntialiasingMode::Temporal;
    return result;
}

void observe_renderer_lockstep(
    RendererLockstep& state,
    const net::RuntimeRendererReadiness* observation,
    core::u32 node_index,
    bool local) noexcept {
    // Keep folding after a timing warning: a later unavailable renderer must
    // still close Program even under AdvanceIndependently.
    const bool previous_budget = state.code == RendererLockstepCode::FrameBudgetExceeded
        || state.code == RendererLockstepCode::FrameTimingPending;
    auto budget_code = previous_budget ? state.code : RendererLockstepCode::Ready;
    auto budget_node = previous_budget ? state.failing_node : node_index;
    auto budget_local = previous_budget ? state.local_failure : local;
    if (previous_budget) state.code = RendererLockstepCode::Ready;
    struct RestoreBudget { RendererLockstep& state; RendererLockstepCode& code; core::u32& node; bool& local;
        ~RestoreBudget() {
            if (state.code == RendererLockstepCode::Ready && code != RendererLockstepCode::Ready)
                fail(state, code, node, local);
        }
    } restore{state, budget_code, budget_node, budget_local};
    if (state.code == RendererLockstepCode::Inactive ||
        state.code != RendererLockstepCode::Ready) {
        return;
    }
    if (observation == nullptr ||
        observation->state ==
            net::RuntimeRendererReadinessState::Unknown) {
        fail(state, RendererLockstepCode::ObservationMissing,
             node_index, local);
        return;
    }
    if (observation->profile_digest != state.profile_digest ||
        observation->requested != state.requested) {
        fail(state, RendererLockstepCode::ProfileMismatch,
             node_index, local);
        return;
    }
    if (!observation->satisfied ||
        observation->selected == net::RuntimeRendererBackend::Unknown ||
        (state.require_requested_selection &&
         observation->selected != state.requested)) {
        fail(state, RendererLockstepCode::RendererUnavailable,
             node_index, local);
        return;
    }
    if (state.selected != net::RuntimeRendererBackend::Unknown && state.selected != observation->selected) {
        fail(state, RendererLockstepCode::SelectionMismatch, node_index, local);
        return;
    }
    if (observation->selected == net::RuntimeRendererBackend::Advanced) {
        const auto& proof = observation->conformance;
        if (proof.state == core::ConformanceState::Failed) {
            state.conformance_difference = {core::ConformanceMismatch::Failed, proof.failed_pass};
            fail(state, RendererLockstepCode::ConformanceFailed, node_index, local);
            return;
        }
        if (proof.version != core::render_conformance_version && proof.state == core::ConformanceState::Passed) {
            state.conformance_difference = {core::ConformanceMismatch::Version};
            fail(state, RendererLockstepCode::ConformanceMismatch, node_index, local);
            return;
        }
        if (!proof.complete() || proof.request != state.conformance_request) {
            fail(state, RendererLockstepCode::ConformancePending, node_index, local);
            return;
        }
        if (state.selected == net::RuntimeRendererBackend::Unknown) state.conformance = proof;
        else {
            state.conformance_difference = core::compare_render_conformance(state.conformance, proof);
            if (state.conformance_difference.reason != core::ConformanceMismatch::None) {
                fail(state, RendererLockstepCode::ConformanceMismatch, node_index, local);
                return;
            }
        }
    }
    if (state.frame_budget_us != 0U) {
        if (observation->timing.sequence == 0U
            || observation->timing.profile_digest != state.profile_digest
            || (observation->selected == net::RuntimeRendererBackend::Advanced && !observation->timing.gpu_valid)) {
            if (budget_code == RendererLockstepCode::Ready) budget_code = RendererLockstepCode::FrameTimingPending;
        }
        if (observation->timing.over_budget(state.frame_budget_us)) {
            if (budget_code == RendererLockstepCode::Ready) budget_code = RendererLockstepCode::FrameBudgetExceeded;
        }
    }
    if (state.selected == net::RuntimeRendererBackend::Unknown) {
        state.selected = observation->selected;
        state.temporal_restarts = observation->temporal_restarts;
        state.temporal_restart_frame = observation->temporal_restart_frame;
        return;
    }
    if (state.require_temporal_agreement
        && (state.temporal_restarts != observation->temporal_restarts
            || state.temporal_restart_frame != observation->temporal_restart_frame)) {
        fail(state, RendererLockstepCode::TemporalHistoryMismatch, node_index, local);
        return;
    }
    if (state.selected != observation->selected) {
        fail(state, RendererLockstepCode::SelectionMismatch,
             node_index, local);
    }
}

} // namespace mediaserver::server
