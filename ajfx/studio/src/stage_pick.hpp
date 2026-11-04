#pragma once

#include "compile.hpp"
#include "document.hpp"

#include <ajfx/math.hpp>
#include <ajfx/simulation.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Picking the solid things in the preview: where each stands at a step of
// the loop, and which one a ray from the camera meets.
namespace ajfx::studio {

// One solid thing where it stands at one step, as a ball to pick it by.
struct StageBody final {
    Vec3 center{};
    float radius{0.0F};
    // The solid layer it is in, and the block that made it: the Object (or
    // the Stream or Burst) it is, or 0 for a piece of something broken.
    Uid layer{};
    Uid block{};
    // A body's spawn index in the world (what a deletion names); none for
    // a prop, which is part of the scene rather than a body in it.
    std::optional<std::uint32_t> spawn_index;
};

// The page's solid things at one step of the preview loop.
struct StageBodies final {
    std::uint64_t tick{0U};
    std::vector<StageBody> bodies;
};

// The most solid things a step keeps for picking: a world may hold far more
// pieces, and a click needs only the ones near the pointer to be right.
inline constexpr std::size_t max_stage_bodies = 4096U;

// The page's one solid world in the compiled effect (SolidWorld), by its
// place among the emitters; nothing where none plays.
[[nodiscard]] std::optional<std::size_t> solid_world_index(const Compiled& compiled);

// Every body and prop of the solid world in `world` (its state, from a run
// of `compiled`), each with its layer and block, in birth order and then the
// props'. At most max_stage_bodies bodies.
[[nodiscard]] std::vector<StageBody> stage_bodies(const Compiled& compiled, const EmitterState& world);

// Of samples taken through the loop, oldest first, the newest at or before
// `frame`; null where there is none.
[[nodiscard]] const StageBodies* stage_bodies_at(const std::vector<StageBodies>& samples, std::uint64_t frame);

// Where a click in the preview lands, in the effect's space (page scale
// applied): on the nearest solid thing along the ray, else on the floor.
struct StageHit final {
    // Where the ray met the thing, or the floor.
    Vec3 point{};
    // The middle of the thing met; the point itself on the floor.
    Vec3 center{};
    Uid layer{};
    Uid block{};
    bool on_body{false};
    // The body met, when it is one (StageBody::spawn_index).
    std::optional<std::uint32_t> spawn_index;
};

// The height of the floor a click lands on, in the effect's space: the
// page's ground, or 0 with none.
[[nodiscard]] float stage_floor(const Document& document);

// The thing or the floor along the ray from `origin` along `direction`
// (unit length). Nothing when it meets neither.
[[nodiscard]] std::optional<StageHit> pick_stage(std::span<const StageBody> bodies, Vec3 origin, Vec3 direction,
                                                 float floor);

} // namespace ajfx::studio
