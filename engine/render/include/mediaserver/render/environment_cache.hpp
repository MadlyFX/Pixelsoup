#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/image_decode.hpp>

#include <memory>
#include <string>

// Residency for the authored environment map.
//
// The frame path never waits on a disk, so the map is read and decoded by a
// process-shared worker and observed without blocking. The contract is
// `ModelCache`'s - Unseen / Pending / Ready / Failed / Busy, sticky failure,
// generation-fenced refresh, a publication epoch a command list latches once -
// because a renderer already knows how to consume that shape and a second
// residency vocabulary would be one more thing to get subtly wrong.
//
// What it deliberately does **not** have is `ModelCache`'s budgeting. That
// cache holds up to 256 models against a byte cap and has to evict; a scene has
// exactly one environment. A byte budget, an eviction policy and a bounded
// source table would all be machinery guarding a set of size one.
namespace mediaserver::render {

enum class EnvironmentCacheState : core::u8 {
    // Nothing has asked for this source yet.
    Unseen,
    Pending,
    Ready,
    // Read or decode failed. Sticky until `refresh`, because retrying a bad
    // path every frame is how a stage ends up with a disk being hammered by a
    // typo.
    Failed,
    // Another thread holds the state. The frame path never waits, so this is
    // the honest answer for one frame rather than a stall.
    Busy,
};

struct EnvironmentCacheStats final {
    core::u64 admissions{};
    core::u64 completions{};
    core::u64 failures{};
    core::u64 cache_hits{};
    core::u64 busy_observations{};
    // Results that arrived after a refresh moved the generation on. Counted
    // rather than adopted: a late decode of the previous map would replace the
    // one the operator just asked for.
    core::u64 late_results_discarded{};

    bool operator==(const EnvironmentCacheStats&) const = default;
};

struct EnvironmentCacheObservation final {
    // The newest complete decode valid at the caller's recording fence. It may
    // still be present while `state` reports a newer generation Pending, which
    // is what keeps a map on screen across a refresh instead of flashing.
    std::shared_ptr<const DecodedFrame> frame;
    EnvironmentCacheState state{EnvironmentCacheState::Unseen};
    core::u64 generation{};
    core::u64 publication_epoch{};

    [[nodiscard]] bool ready() const noexcept {
        return frame != nullptr && frame->valid();
    }
};

class EnvironmentCache final {
public:
    // Default handles share one process-wide worker and one decoded map, so
    // every renderer on this machine lights from the same bytes.
    EnvironmentCache();

    EnvironmentCache(const EnvironmentCache&) noexcept = default;
    EnvironmentCache& operator=(const EnvironmentCache&) noexcept = default;
    EnvironmentCache(EnvironmentCache&&) noexcept = default;
    EnvironmentCache& operator=(EnvironmentCache&&) noexcept = default;
    ~EnvironmentCache() = default;

    // Render-facing observation and admission. Never waits, reads, decodes or
    // joins. An empty source is Unseen, which is the ordinary case of a scene
    // that authored no environment and must not read as a failure.
    //
    // A decode newer than `publication_fence` is withheld until the next
    // recording rather than becoming visible midway through this one.
    [[nodiscard]] EnvironmentCacheObservation observe(
        const std::string& source, core::u64 publication_fence, core::u64 authored_generation = 0U) noexcept;

    // Operator or control path. Clears a sticky failure and moves the
    // generation on, so a late result from before the refresh is discarded
    // rather than adopted.
    [[nodiscard]] bool refresh(const std::string& source) noexcept;

    // Monotonic and nonzero once anything has been published.
    [[nodiscard]] core::u64 publication_epoch() const noexcept;
    [[nodiscard]] EnvironmentCacheStats stats() const noexcept;

    // Deterministic test seam: blocks until no work is outstanding. Production
    // code never calls it - the whole point is that the frame path does not
    // wait - and it exists so a test can assert on a completed decode without
    // a sleep.
    [[nodiscard]] bool wait_for_test() noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace mediaserver::render
