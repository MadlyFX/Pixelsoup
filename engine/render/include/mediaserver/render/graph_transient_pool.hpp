#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>

#include <array>
#include <cstddef>

// Textures that live only inside one frame.
//
// A render graph pass needs somewhere to put its output that is not a product
// anyone outside the frame holds. Allocating one per frame is not an option -
// the frame path allocates nothing - so the pool is filled at construction and
// hands out whole textures by exact descriptor match.
//
// **Nothing is aliased.** Two transients never share memory, even when their
// lifetimes provably do not overlap. Aliasing means the contents of a region
// are undefined until written, and two nodes reading undefined memory read
// different garbage - a seam that appears only sometimes, which is the worst
// kind to find. Proving every transient is fully written before it is read is
// exactly the sort of argument that is correct right up until someone adds a
// pass. Aliasing can come later, behind a test that fills aliased memory with a
// known pattern and asserts no pass reads it.
namespace mediaserver::render {

struct GraphTransientPoolLimits final {
    // How many distinct transient textures this pool can hold. Small on
    // purpose: a frame that wants more than this has almost certainly stopped
    // reusing them.
    std::size_t max_textures{16U};

    bool operator==(const GraphTransientPoolLimits&) const = default;
};

struct GraphTransientPoolStats final {
    // Textures handed out since the last frame began.
    core::u64 acquired{};
    // Requests refused because every matching texture was already in use this
    // frame, or because the pool has no room for a new descriptor. Counted
    // rather than allocated around: a frame that outgrew its pool is a
    // configuration to fix, and quietly allocating would move the cost onto
    // the frame path where it must never be.
    core::u64 refusals{};
    // Requests served by a texture the pool already had. The rest created one,
    // which only ever happens before the first frame settles.
    core::u64 reuses{};
    std::size_t textures{};

    bool operator==(const GraphTransientPoolStats&) const = default;
};

class GraphTransientPool final {
public:
    explicit GraphTransientPool(
        GraphTransientPoolLimits limits = {}) noexcept
        : limits_(limits) {}

    GraphTransientPool(const GraphTransientPool&) = delete;
    GraphTransientPool& operator=(const GraphTransientPool&) = delete;
    GraphTransientPool(GraphTransientPool&&) = default;
    GraphTransientPool& operator=(GraphTransientPool&&) = default;
    ~GraphTransientPool() = default;

    // Releases every texture back for reuse. Call once per frame, before the
    // graph executes. It does not free anything.
    void begin_frame() noexcept;

    // A texture matching `desc` exactly, held until the next begin_frame.
    //
    // Exact match, not "big enough": handing back a larger texture would make
    // a pass's viewport and its target extent disagree, and every downstream
    // sampling coordinate is derived from the target extent.
    //
    // The first frame creates what it needs; later frames reuse. A creation on
    // the frame path is a one-time cost as a show settles, and it is visible in
    // the stats rather than hidden.
    [[nodiscard]] core::Result<rhi::Texture*, RenderError> acquire(
        rhi::Device& device, const rhi::TextureDesc& desc) noexcept;

    [[nodiscard]] const GraphTransientPoolStats& stats() const noexcept {
        return stats_;
    }
    [[nodiscard]] const GraphTransientPoolLimits& limits() const noexcept {
        return limits_;
    }

private:
    struct Entry final {
        rhi::Texture texture;
        rhi::TextureDesc desc{};
        bool in_use{};
    };

    static constexpr std::size_t capacity = 64U;

    GraphTransientPoolLimits limits_{};
    std::array<Entry, capacity> entries_{};
    std::size_t count_{};
    GraphTransientPoolStats stats_{};
};

} // namespace mediaserver::render
