#pragma once

#include <mediaserver/render/render_graph.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>

#include <array>
#include <span>
#include <variant>

// Execution for a compiled render graph.
//
// The graph stays a value-level compiler: it decides pass order and the exact
// transitions between them, touching no graphics API. This turns that plan into
// recorded commands. It owns ordering and resource state and nothing else - the
// renderer still owns what a pass draws, because a framework that also owned
// that would have to know about every pass the renderer will ever have.
//
// Why it exists: the Advanced frame is already several passes and track 3 adds
// prefiltering, TAA resolve, AO, bloom and reflections. Somewhere past ten,
// hand-written barriers stop being reviewable, and the failure mode - a missing
// transition that works on one driver and corrupts on another - is the worst
// possible bug for a system whose whole premise is that two machines produce
// identical pixels.
namespace mediaserver::render {

// A texture the plan can name. Bound by the caller before execution; the
// executor never creates one, which keeps ownership where it already is.
struct GraphBinding final {
    GraphResource resource{};
    rhi::Texture* texture{};

    bool operator==(const GraphBinding&) const = default;
};

// One pass body.
//
// A plain function pointer and a context pointer rather than a type-erased
// callable: the frame path allocates nothing, and `std::function` would either
// heap-allocate or need a fixed-size buffer whose size is another number to get
// wrong. The cost is that a pass body is a free function or a lambda with no
// captures, which is also what keeps a pass from quietly capturing frame state
// it should have declared.
//
// Returning false aborts execution at that pass. Transitions already recorded
// stay recorded - the list is still consistent, and a caller that cannot finish
// a frame cancels it.
struct GraphPassBody final {
    bool (*record)(rhi::CommandList& commands, void* context){};
    void* context{};
};

enum class RenderGraphExecutionError : core::u8 {
    // A pass in the plan has no body, or a body was supplied for a pass that
    // does not exist. The two spans are positional and must match the plan.
    BodyCountMismatch,
    // A resource the plan transitions has no bound texture.
    UnboundResource,
    // The plan names a queue this device cannot run. See the note on
    // async compute below.
    QueueUnsupported,
    // The plan transitions a resource into a state this RHI has no equivalent
    // for. Acceleration structures are the only such state today: the graph
    // models them and no RHI resource exists to carry one.
    StateUnsupported,
    // This device exposes no explicit barrier, so a plan's transitions cannot
    // be applied. Distinct from a backend failure: nothing about the plan is
    // wrong.
    BarriersUnsupported,
    // A transition was refused by the backend, which means this recording may
    // not claim that texture.
    TransitionRefused,
    // A pass body returned false.
    PassFailed,
};

inline constexpr std::size_t max_graph_bindings = max_render_graph_resources;

class RenderGraphExecutor final {
public:
    // Binds a texture to a resource for the next execution. Rebinding a
    // resource replaces it; the set is cleared by `reset`.
    [[nodiscard]] bool bind(
        GraphResource resource, rhi::Texture& texture) noexcept;
    void reset() noexcept;

    // Walks `plan` in order, applies each pass's transitions, then records that
    // pass through its body.
    //
    // Pass order is the plan's order, which is declaration order. Nothing here
    // sorts, reorders, or skips: which passes exist is decided by replicated
    // authored state, and a node that dropped a pass because it was behind
    // would be rendering a different image from its neighbours.
    //
    // `bodies` is positional against `plan.passes()`.
    // The device is taken explicitly rather than read back off the command
    // list, because what the executor needs from it is a capability - whether
    // this backend can be asked for a barrier at all - and a command list has
    // no business answering that.
    [[nodiscard]] core::Result<std::monostate, RenderGraphExecutionError>
    execute(rhi::Device& device,
            rhi::CommandList& commands,
            const RenderGraphPlan& plan,
            std::span<const GraphPassBody> bodies) noexcept;

    // Transitions actually recorded by the last execution. A plan's transition
    // list is what the compiler decided; this is what reached the command list,
    // and they differ when a texture was already in the state asked for.
    [[nodiscard]] core::u64 recorded_transitions() const noexcept {
        return recorded_transitions_;
    }
    [[nodiscard]] core::u64 recorded_passes() const noexcept {
        return recorded_passes_;
    }

private:
    [[nodiscard]] rhi::Texture* bound(GraphResource resource) const noexcept;

    std::array<rhi::Texture*, max_graph_bindings> bindings_{};
    core::u64 recorded_transitions_{};
    core::u64 recorded_passes_{};
};

// The resource state a graph state means to the RHI.
//
// Total and explicit rather than a cast: the two enumerations are allowed to
// diverge, and a silent numeric correspondence between them is the kind of
// coupling that survives right up until someone inserts a value.
[[nodiscard]] rhi::ResourceState resource_state_for(GraphState state) noexcept;

} // namespace mediaserver::render
