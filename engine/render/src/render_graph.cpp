#include <mediaserver/render/render_graph.hpp>

#include <variant>

namespace mediaserver::render {
namespace {

[[nodiscard]] bool reads(GraphAccessMode mode) noexcept {
    return mode == GraphAccessMode::Read || mode == GraphAccessMode::ReadWrite;
}

[[nodiscard]] bool writes(GraphAccessMode mode) noexcept {
    return mode == GraphAccessMode::Write || mode == GraphAccessMode::ReadWrite;
}

[[nodiscard]] bool valid_state(
    GraphResourceKind kind,
    GraphState state) noexcept {
    if (state == GraphState::Undefined) {
        return false;
    }
    switch (kind) {
    case GraphResourceKind::Texture:
        return state != GraphState::AccelerationStructureRead &&
            state != GraphState::AccelerationStructureWrite;
    case GraphResourceKind::Buffer:
        return state != GraphState::ColorTarget &&
            state != GraphState::DepthTarget && state != GraphState::Present &&
            state != GraphState::AccelerationStructureRead &&
            state != GraphState::AccelerationStructureWrite;
    case GraphResourceKind::AccelerationStructure:
        return state == GraphState::AccelerationStructureRead ||
            state == GraphState::AccelerationStructureWrite;
    }
    return false;
}

} // namespace

core::Result<GraphResource, RenderGraphError> RenderGraph::add_resource(
    const GraphResourceDesc& desc) noexcept {
    using Result = core::Result<GraphResource, RenderGraphError>;
    if (desc.name.empty()) {
        return Result::failure(RenderGraphError::InvalidResource);
    }
    if (resource_count_ >= resources_.size()) {
        return Result::failure(RenderGraphError::CapacityExceeded);
    }
    resources_[resource_count_] = desc;
    ++resource_count_;
    return Result::success(
        GraphResource{static_cast<core::u16>(resource_count_)});
}

core::Result<std::monostate, RenderGraphError> RenderGraph::add_pass(
    const GraphPassDesc& desc) noexcept {
    using Result = core::Result<std::monostate, RenderGraphError>;
    if (desc.name.empty() || desc.accesses.empty()) {
        return Result::failure(RenderGraphError::InvalidPass);
    }
    if (pass_count_ >= passes_.size() ||
        desc.accesses.size() > accesses_.size() - access_count_) {
        return Result::failure(RenderGraphError::CapacityExceeded);
    }
    for (std::size_t index = 0U; index < desc.accesses.size(); ++index) {
        const GraphAccess& access = desc.accesses[index];
        if (!access.resource || access.resource.value > resource_count_ ||
            !valid_state(resources_[access.resource.value - 1U].kind,
                         access.state)) {
            return Result::failure(RenderGraphError::InvalidResource);
        }
        for (std::size_t earlier = 0U; earlier < index; ++earlier) {
            if (desc.accesses[earlier].resource == access.resource) {
                return Result::failure(RenderGraphError::DuplicateAccess);
            }
        }
    }

    StoredPass& pass = passes_[pass_count_];
    pass.name = desc.name;
    pass.kind = desc.kind;
    pass.queue = desc.queue;
    pass.first_access = static_cast<core::u16>(access_count_);
    pass.access_count = static_cast<core::u16>(desc.accesses.size());
    for (const GraphAccess& access : desc.accesses) {
        accesses_[access_count_++] = access;
    }
    ++pass_count_;
    return Result::success(std::monostate{});
}

core::Result<RenderGraphPlan, RenderGraphError> RenderGraph::compile()
    const noexcept {
    using Result = core::Result<RenderGraphPlan, RenderGraphError>;
    struct ResourceState final {
        GraphState state{GraphState::Undefined};
        GraphQueue queue{GraphQueue::Graphics};
        bool initialized{};
        bool written{};
    };
    std::array<ResourceState, max_render_graph_resources> state{};
    for (std::size_t index = 0U; index < resource_count_; ++index) {
        state[index].initialized = resources_[index].imported;
    }

    RenderGraphPlan plan{};
    for (std::size_t pass_index = 0U; pass_index < pass_count_; ++pass_index) {
        const StoredPass& source = passes_[pass_index];
        CompiledPass& destination = plan.passes_[plan.pass_count_++];
        destination.name = source.name;
        destination.kind = source.kind;
        destination.queue = source.queue;
        destination.first_transition =
            static_cast<core::u16>(plan.transition_count_);

        for (std::size_t offset = 0U; offset < source.access_count; ++offset) {
            const GraphAccess& access = accesses_[source.first_access + offset];
            ResourceState& previous = state[access.resource.value - 1U];
            if (reads(access.mode) && !previous.initialized) {
                return Result::failure(RenderGraphError::ReadBeforeWrite);
            }
            const bool queue_sync = previous.initialized &&
                previous.queue != source.queue;
            if (!previous.initialized || previous.state != access.state ||
                queue_sync) {
                if (plan.transition_count_ >= plan.transitions_.size()) {
                    return Result::failure(RenderGraphError::CapacityExceeded);
                }
                plan.transitions_[plan.transition_count_++] = GraphTransition{
                    access.resource,
                    static_cast<core::u16>(pass_index),
                    previous.state,
                    access.state,
                    previous.queue,
                    source.queue,
                    queue_sync};
            }
            previous.state = access.state;
            previous.queue = source.queue;
            previous.initialized = true;
            previous.written = previous.written || writes(access.mode);
        }
        destination.transition_count = static_cast<core::u16>(
            plan.transition_count_ - destination.first_transition);
    }

    for (std::size_t index = 0U; index < resource_count_; ++index) {
        if (resources_[index].exported && !state[index].written) {
            return Result::failure(RenderGraphError::ExportNeverWritten);
        }
    }
    return Result::success(plan);
}

void RenderGraph::clear() noexcept {
    resource_count_ = 0U;
    pass_count_ = 0U;
    access_count_ = 0U;
}

} // namespace mediaserver::render
