#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/render/temporal_history.hpp>
#include <mediaserver/rhi/device.hpp>

#include <vector>

// The temporal antialiasing resolve, and the GPU history it accumulates into.
//
// The CPU half of the temporal contract already exists in `TemporalHistory`:
// it decides Initialize / Advance / Hold / Reset from replicated state alone,
// so every node in a cohort reaches the same decision without talking. This is
// the other half - the colour history those decisions apply to, and the pass
// that folds one frame into it.
//
// The disposition is a table here rather than a set of branches in a shader.
// That mapping is the entire reason the temporal contract was built, and a
// shader that re-derived it would be a second place for it to be wrong.
namespace mediaserver::render {

struct TemporalResolveLimits final {
    // How many surfaces may hold a colour history at once.
    //
    // Bounded separately from `max_history_slots`, which is deliberately large
    // and cheap: a CPU history slot is a few dozen bytes, while a GPU one is
    // three full-extent targets. A show drives Program, Preview, Stage and a
    // handful of mapped views, so a small pool covers real use and the counter
    // below makes it obvious when it does not.
    core::u32 max_targets{4U};

    bool operator==(const TemporalResolveLimits&) const = default;
};

struct TemporalResolveStats final {
    core::u64 resolves{};
    core::u64 initializes{};
    core::u64 holds{};
    // Advances that had to start over because the surface's history had been
    // evicted. Counted rather than hidden: a node that restarts while its
    // neighbour accumulates shows a one-frame difference at their shared edge,
    // and a pool at least as large as the surfaces a node renders is the fix.
    core::u64 restarts{};
    core::u64 evictions{};
    core::u64 targets_allocated{};
    core::u64 allocation_failures{};

    bool operator==(const TemporalResolveStats&) const = default;
};

class TemporalResolve final {
public:
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<TemporalResolve, RenderError> create(
        rhi::Device& device, core::u32 width, core::u32 height,
        TemporalResolveLimits limits,
        rhi::TextureFormat format = rhi::TextureFormat::Rgba16Float) noexcept;

    TemporalResolve(const TemporalResolve&) = delete;
    TemporalResolve& operator=(const TemporalResolve&) = delete;
    TemporalResolve(TemporalResolve&&) = default;
    TemporalResolve& operator=(TemporalResolve&&) = default;
    ~TemporalResolve() = default;

    [[nodiscard]] core::u32 width() const noexcept { return output_.width(); }
    [[nodiscard]] core::u32 height() const noexcept { return output_.height(); }

    // Reallocates when the padded extent changes. Every history is dropped,
    // because a resize is a discontinuity: `TemporalHistory` already reports
    // Reset for one, so nothing is being thrown away that would have been used.
    [[nodiscard]] bool resize(rhi::Device& device, core::u32 width,
                              core::u32 height) noexcept;

    // The sources a resolve reads. All three come from the same recording that
    // produced the colour, so a frame is never mixed with another frame's
    // motion.
    struct Sources final {
        const rhi::Texture* color{};
        // RGBA32F: xy normalized UV motion, z current reversed device depth,
        // w this fragment's expected previous depth (previous camera AND model).
        const rhi::Texture* velocity{};
        const rhi::Texture* object_id{};
        // The authored canvas for ordinary slices. An explicit projection uses
        // its own padded raster: its velocity is normalized to that viewpoint.
        core::u32 canvas_width{};
        core::u32 canvas_height{};
    };

    // Applies the published disposition and returns the colour to publish.
    //
    // Returns the source colour untouched when there is nothing to do, so a
    // caller that always routes through here is correct whether or not
    // temporal antialiasing is authored.
    [[nodiscard]] core::Result<const rhi::Texture*, RenderError> apply(
        rhi::Device& device, rhi::CommandList& commands, core::u32 slot,
        HistoryDisposition disposition, const Sources& sources) noexcept;

    // Drops a surface's history without touching the rest of the pool.
    void forget(core::u32 slot) noexcept;

    [[nodiscard]] const TemporalResolveStats& stats() const noexcept {
        return stats_;
    }
    void reset_stats() noexcept { stats_ = {}; }

private:
    // One surface's accumulated frame. Object ID travels with the colour
    // because rejecting on it needs the *previous* frame's ids, and keeping
    // them in step with the colour they describe is what makes the rejection
    // meaningful.
    struct Entry final {
        rhi::Texture color;
        rhi::Texture object_id;
        rhi::Texture motion_depth;
        rhi::CommandRecordingToken recording;
        core::u64 last_used{};
        core::u32 slot{};
        bool occupied{};
        // False until a frame has been written, so a freshly claimed entry
        // initializes rather than blending against undefined contents.
        bool primed{};
    };

    TemporalResolve(rhi::Texture output, rhi::GraphicsPipeline pipeline,
                    rhi::Sampler sampler, std::vector<Entry> entries) noexcept;

    [[nodiscard]] Entry* claim(rhi::Device& device, core::u32 slot) noexcept;
    [[nodiscard]] bool store(rhi::CommandList& commands, Entry& entry,
                             const rhi::Texture& color,
                             const rhi::Texture& object_id,
                             const rhi::Texture& motion_depth) noexcept;

    rhi::Texture output_;
    rhi::GraphicsPipeline pipeline_;
    rhi::Sampler sampler_;
    std::vector<Entry> entries_;
    core::u64 use_stamp_{};
    TemporalResolveStats stats_{};
};

// How much of the reprojected history survives a blend.
//
// Conservative on purpose. The usual game-engine value keeps far more, and on
// a wall a camera is filming that shows up as smear that is baked into the
// plate and cannot be graded out. A DP will reject visible smear faster than
// visible aliasing, so this errs toward aliasing.
inline constexpr float temporal_history_weight = 0.8F;

} // namespace mediaserver::render
