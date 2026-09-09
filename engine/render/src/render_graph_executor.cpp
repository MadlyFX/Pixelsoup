#include <mediaserver/render/render_graph_executor.hpp>

#include <variant>

namespace mediaserver::render {

rhi::ResourceState resource_state_for(GraphState state) noexcept {
    switch (state) {
    case GraphState::ColorTarget:
        return rhi::ResourceState::ColorTarget;
    case GraphState::DepthTarget:
        return rhi::ResourceState::DepthTarget;
    case GraphState::ShaderRead:
        return rhi::ResourceState::ShaderRead;
    case GraphState::StorageRead:
        return rhi::ResourceState::StorageRead;
    case GraphState::StorageWrite:
        return rhi::ResourceState::StorageWrite;
    case GraphState::CopySource:
        return rhi::ResourceState::CopySource;
    case GraphState::CopyDestination:
        return rhi::ResourceState::CopyDestination;
    case GraphState::Present:
        return rhi::ResourceState::Present;
    case GraphState::AccelerationStructureRead:
    case GraphState::AccelerationStructureWrite:
    case GraphState::Undefined:
        break;
    }
    // Acceleration structures have no RHI resource yet, and Undefined is the
    // absence of a state rather than one. Neither can be transitioned to, and
    // execute() refuses a plan that asks - this returns something valid only so
    // the switch has no path off the end.
    return rhi::ResourceState::ShaderRead;
}

namespace {

[[nodiscard]] bool transitionable(GraphState state) noexcept {
    return state != GraphState::Undefined
        && state != GraphState::AccelerationStructureRead
        && state != GraphState::AccelerationStructureWrite;
}

} // namespace

bool RenderGraphExecutor::bind(
    GraphResource resource, rhi::Texture& texture) noexcept {
    if (!resource || resource.value > bindings_.size()) {
        return false;
    }
    bindings_[resource.value - 1U] = &texture;
    return true;
}

void RenderGraphExecutor::reset() noexcept {
    bindings_ = {};
    recorded_transitions_ = 0U;
    recorded_passes_ = 0U;
}

rhi::Texture* RenderGraphExecutor::bound(
    GraphResource resource) const noexcept {
    if (!resource || resource.value > bindings_.size()) {
        return nullptr;
    }
    return bindings_[resource.value - 1U];
}

core::Result<std::monostate, RenderGraphExecutionError>
RenderGraphExecutor::execute(
    rhi::Device& device,
    rhi::CommandList& commands,
    const RenderGraphPlan& plan,
    std::span<const GraphPassBody> bodies, std::size_t first_pass) noexcept {
    using Result = core::Result<std::monostate, RenderGraphExecutionError>;
    recorded_transitions_ = 0U;
    recorded_passes_ = 0U;

    const std::span<const CompiledPass> passes = plan.passes();
    if (first_pass > passes.size() || bodies.size() != passes.size() - first_pass) {
        return Result::failure(RenderGraphExecutionError::BodyCountMismatch);
    }

    // Every refusal is decided before a single barrier is recorded. A plan that
    // is going to be rejected must not leave half its transitions on the list:
    // the caller would then be holding a recording whose resource states came
    // from a frame that never happened.
    const std::span<const GraphTransition> transitions = plan.transitions();
    for (const CompiledPass& pass : passes) {
        // Async compute is not callable yet. The graph models the compute
        // queue, and running its passes on graphics instead would be invisible
        // until someone read a profile - so a plan that names it is refused
        // rather than quietly run somewhere else.
        if (pass.queue != GraphQueue::Graphics) {
            return Result::failure(RenderGraphExecutionError::QueueUnsupported);
        }
    }
    for (const GraphTransition& transition : transitions) {
        if (transition.queue_sync) {
            return Result::failure(RenderGraphExecutionError::QueueUnsupported);
        }
        // Only the destination. `before` is Undefined on a resource's first
        // touch, which is the normal case and not a state anything transitions
        // *to* - checking it would refuse every plan.
        if (!transitionable(transition.after)) {
            return Result::failure(
                RenderGraphExecutionError::StateUnsupported);
        }
        if (bound(transition.resource) == nullptr) {
            return Result::failure(RenderGraphExecutionError::UnboundResource);
        }
    }
    if (!transitions.empty() && !device.capabilities().explicit_barriers) {
        return Result::failure(
            RenderGraphExecutionError::BarriersUnsupported);
    }

    for (const auto& body : bodies) {
        if (!body.record) return Result::failure(RenderGraphExecutionError::BodyCountMismatch);
    }
    for (std::size_t index = first_pass; index < passes.size(); ++index) {
        const CompiledPass& pass = passes[index];
        const auto& body = bodies[index - first_pass];
        const auto timer = commands.begin_gpu_pass(body.category);
        for (core::u16 offset = 0U; offset < pass.transition_count; ++offset) {
            const GraphTransition& transition =
                transitions[static_cast<std::size_t>(pass.first_transition)
                            + offset];
            rhi::Texture* texture = bound(transition.resource);
            if (!commands.transition(
                    *texture, resource_state_for(transition.after))) {
                commands.end_gpu_pass(timer);
                return Result::failure(
                    RenderGraphExecutionError::TransitionRefused);
            }
            ++recorded_transitions_;
        }
        const bool recorded = body.record(commands, body.context);
        commands.end_gpu_pass(timer);
        if (!recorded) {
            return Result::failure(RenderGraphExecutionError::PassFailed);
        }
        ++recorded_passes_;
    }
    return Result::success(std::monostate{});
}

} // namespace mediaserver::render
