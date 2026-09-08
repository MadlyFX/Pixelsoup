#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/render/display_transform.hpp>
#include <mediaserver/scene/scene.hpp>

#include <memory>
#include <string>

// Residency for the baked display table.
//
// A display pipeline is a graph of transforms, and neither evaluating it per
// pixel nor baking it on the frame path is acceptable: the bake is a hundred
// thousand transcendentals, and the show look it composes with is a file on a
// disk. So the table is built by a process-shared worker and observed without
// blocking, on `EnvironmentCache`'s contract - Unseen / Pending / Ready /
// Failed / Busy, a generation that moves on refresh, and a result the frame
// path adopts on a frame boundary rather than midway through a recording.
//
// One resident table rather than a budgeted set, for the same reason the
// environment cache holds one map: a show has one look, and a byte budget with
// an eviction policy would be machinery guarding a set of size one.
namespace mediaserver::render {

struct DisplayLutRequest final {
    scene::DisplayTransform transform{
        scene::DisplayTransform::AcesApproximate};
    // Path to the `.cube` look composed after the transform. Empty is no look,
    // which is the ordinary case and bakes without touching a disk.
    std::string show_lut_source;
    core::u32 size{default_display_lut_size};
    core::u64 authored_generation{};

    bool operator==(const DisplayLutRequest&) const = default;
};

enum class DisplayLutState : core::u8 {
    Unseen,
    Pending,
    Ready,
    // The look file could not be read or is not a 3D `.cube`. Sticky until the
    // request changes or `refresh` is called. The table published alongside is
    // the transform *alone*: a wall showing the right transform without the
    // grade is a far more predictable failure than one showing whatever look
    // happened to be resident before, and the operator is told which it is.
    Failed,
    // Another thread holds the state for this observation. The frame path
    // never waits, so this is the honest answer for one frame.
    Busy,
};

struct DisplayLutCacheStats final {
    core::u64 admissions{};
    core::u64 completions{};
    core::u64 failures{};
    core::u64 busy_observations{};
    core::u64 late_results_discarded{};

    bool operator==(const DisplayLutCacheStats&) const = default;
};

struct DisplayLutObservation final {
    // The newest complete bake for the observed request, or null while one is
    // pending. Present on Failed as well - see the state's note.
    std::shared_ptr<const DisplayLut> table;
    DisplayLutState state{DisplayLutState::Unseen};
    core::u64 generation{};

    [[nodiscard]] bool ready() const noexcept {
        return table != nullptr && table->valid();
    }
};

class DisplayLutCache final {
public:
    // Default handles share one process-wide worker and one resident table, so
    // every output on this machine grades from the same bytes.
    DisplayLutCache();

    DisplayLutCache(const DisplayLutCache&) noexcept = default;
    DisplayLutCache& operator=(const DisplayLutCache&) noexcept = default;
    DisplayLutCache(DisplayLutCache&&) noexcept = default;
    DisplayLutCache& operator=(DisplayLutCache&&) noexcept = default;
    ~DisplayLutCache() = default;

    // Render-facing observation and admission. Never waits, reads, bakes or
    // joins. A request different from the resident one is admitted and
    // reported Pending; the previous table is deliberately not offered for it,
    // because it is not this request's table.
    [[nodiscard]] DisplayLutObservation observe(
        const DisplayLutRequest& request) noexcept;

    // Operator or control path. Clears a sticky failure and moves the
    // generation on, so a late bake from before the refresh is discarded
    // rather than adopted.
    [[nodiscard]] bool refresh(const DisplayLutRequest& request) noexcept;

    [[nodiscard]] DisplayLutCacheStats stats() const noexcept;

    // Deterministic test seam: blocks until no work is outstanding. Production
    // code never calls it - the whole point is that the frame path does not
    // wait - and it exists so a test can assert on a completed bake without a
    // sleep.
    [[nodiscard]] bool wait_for_test() noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace mediaserver::render
