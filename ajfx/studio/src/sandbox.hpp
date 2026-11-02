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

// Playing with the preview: picking the solid things in it, and the events a
// click in it writes into the page.
//
// An effect has to play the same everywhere, so nothing a click does happens
// to the simulation directly. Each tool writes a block into the page at the
// playhead's time, which the compiler turns into the same modules as any
// block: a dropped crate is an Object that appears then, an explosion is an
// Explosion block that goes off then, a push is a Push block that pushes
// then. Each is one edit, so one undo, and each is a block like any other
// afterwards: opened, dragged by its handle, changed or deleted.
namespace ajfx::studio {

// One solid thing where it stands at one step, as a ball to pick it by.
struct StageBody final {
    Vec3 center{};
    float radius{0.0F};
    // The solid layer it is in, and the block that made it: the Object (or
    // the Stream or Burst) it is, or 0 for a piece of something broken.
    Uid layer{};
    Uid block{};
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
};

// The height of the floor a click lands on, in the effect's space: the
// page's ground, or 0 with none.
[[nodiscard]] float stage_floor(const Document& document);

// The thing or the floor along the ray from `origin` along `direction`
// (unit length). Nothing when it meets neither.
[[nodiscard]] std::optional<StageHit> pick_stage(std::span<const StageBody> bodies, Vec3 origin, Vec3 direction,
                                                 float floor);

// What a tool wrote: the block, and the layer it went into.
struct SandboxEdit final {
    Uid layer{};
    Uid block{};
};

// The material a dropped thing is made of: the selected Object's, else the
// last Object's in `layer`, else wood (a crate).
[[nodiscard]] std::string sandbox_material(const Document& document, Uid selected, Uid layer);

// Drop a thing: an Object (a crate of `material`) that appears at `seconds`
// a metre above `floor_point` (effect space) and falls onto it. It goes into
// `hint` where that is a solid layer, else the page's first solid layer,
// else a new one.
std::optional<SandboxEdit> drop_thing(Document& document, Uid hint, Vec3 floor_point, float seconds,
                                      std::string_view material = {});

// Explode here: an Explosion at `point` (effect space) that goes off at
// `seconds`, in `hint` where that is a layer, else the page's first.
// Nothing on a page with no layer.
std::optional<SandboxEdit> explode_here(Document& document, Uid hint, Vec3 point, float seconds);

// Push: a Push at `point` (effect space) toward `away`, at `seconds`, in
// `hint` where that is a layer, else the page's first.
std::optional<SandboxEdit> push_here(Document& document, Uid hint, Vec3 point, Vec3 away, float seconds);

// Deals new random values: every layer's seed where `layer` is 0, else that
// layer's. A solid layer's seed is its world's, and the page's solid layers
// are one world, so re-rolling one re-rolls them all. Each new seed comes
// from the old one and the layer's uid, so the same press on the same page
// always gives the same seeds. False when there is nothing to re-roll.
bool reroll_seed(Document& document, Uid layer);

// Something on the page that happens at a moment: an Explosion or a Push
// going off, an Object appearing. What the transport marks.
struct PageEvent final {
    Uid layer{};
    Uid block{};
    float seconds{0.0F};
    // "Explosion at 1.25 s".
    std::string label;
};

// The page's events that play, by time, then uid.
[[nodiscard]] std::vector<PageEvent> page_events(const Document& document);

} // namespace ajfx::studio
