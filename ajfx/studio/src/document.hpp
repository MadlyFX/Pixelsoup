#pragma once

#include "controls.hpp"
#include "scene.hpp"

#include <cstdint>
#include <optional>
#include <string>

// An ajfx Studio document: a scene (scene.hpp), what it is called and at
// what scale it plays, and how the preview stands it -- which never reaches
// an export. The compiler (compile.hpp) turns the scene into an ajfx::Effect.
namespace ajfx::studio {

// Stable identity of a scene item, or of one of its components, within its
// document. Module ids and random seeds are derived from it, so adding,
// moving or deleting an item leaves every other item's particles where they
// were.
using Uid = std::uint32_t;

// What the preview stands the effect on. Never exported.
enum class Backdrop : std::uint8_t {
    Studio, // a lit grey floor and a 1.8 m figure for scale
    Night,  // the same floor under a dim moonlit rig, for glowing effects
    Void,   // nothing: black, no floor
};

// The frame-time budget (Document::budget_ms): a share of a 60 Hz frame left
// for the effect once the show around it has drawn.
inline constexpr float default_budget_ms = 4.0F;
inline constexpr float min_budget_ms = 0.1F;
inline constexpr float max_budget_ms = 100.0F;

struct Document final {
    std::optional<Scene> scene;
    std::string name{"Untitled effect"};
    // Multiplies every distance, size, speed and acceleration in the scene,
    // so the whole effect grows or shrinks and keeps its look.
    float scale{1.0F};
    // Seconds the preview plays before it loops: the scene's range.
    float loop_seconds{6.0F};
    Backdrop backdrop{Backdrop::Night};
    // Bloom strength in the preview (scene::RendererSettings::bloom_strength).
    float glow{0.6F};
    // Milliseconds of GPU a frame the effect may take, simulation and
    // drawing together, as the preview measures it. Never exported: it is
    // the artist's target, not part of the effect.
    float budget_ms{default_budget_ms};
    // The next identity to reserve (scene.cpp reserves 32 a item).
    Uid next_uid{1U};

    friend bool operator==(const Document&, const Document&) = default;
};

} // namespace ajfx::studio
