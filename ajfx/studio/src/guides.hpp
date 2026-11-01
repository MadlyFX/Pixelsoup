#pragma once

#include "document.hpp"

#include <ajfx/math.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// What the preview draws over an effect to show where its blocks act, and
// the handles that move them.
//
// Guides are lines in the effect's own space (metres, page scale applied):
// the spawn shape at both ends of its radius, the launch cone, an arrow for
// each push, the turbulence field's flow, an attractor's reach, each
// collider. The block an artist is working on is drawn strong with all of
// that and its handles; of the rest only the spawn shapes and colliders are
// drawn, faintly, so the stage still reads.
//
// A handle is a point the artist drags. It names the control it sets and
// how a drag moves it (along the floor, up and down, out from a centre, round
// a heading), and handle_value() turns where it was dragged to into the
// control's new value, so the page and the preview agree on every number.
namespace ajfx::studio {

enum class GuideKind : std::uint8_t {
    Shape,    // where particles are born, and how they leave
    Force,    // what pushes them
    Field,    // the turbulence field's flow
    Collider, // what they hit
};

struct GuideStroke final {
    std::vector<Vec3> points;
    bool closed{false};
    // A secondary line: the inner end of a range, a drop line to the floor.
    bool faint{false};
    // Ends in an arrowhead, pointing from the second-last point to the last.
    bool arrow{false};
};

struct Guide final {
    Uid layer{};
    Uid block{};
    GuideKind kind{GuideKind::Shape};
    // The block the artist is working on.
    bool focused{false};
    std::vector<GuideStroke> strokes;
};

enum class HandleMotion : std::uint8_t {
    Floor,   // a point moved across the floor, or with Shift up and down (a position)
    Height,  // a level moved up and down (a height above `origin`)
    Radius,  // a distance out from `origin` (a radius, a reach)
    Heading, // round `origin` on the floor (a heading in degrees)
    Corner,  // a box's corner: twice its offset from `origin` on each axis (a size)
};

struct Handle final {
    Uid block{};
    std::string control;
    HandleMotion motion{HandleMotion::Floor};
    // For a range, the end it moves: 0 the low end, 1 the high.
    int end{1};
    // Where it is drawn, and what it measures from.
    Vec3 at{};
    Vec3 origin{};
    // Effect-space metres for one unit of the control: the page scale.
    float scale{1.0F};
    // A word or two for its tooltip: "Radius", "Offset".
    std::string label;
};

struct GuideSet final {
    std::vector<Guide> guides;
    // Only the focused block's.
    std::vector<Handle> handles;
};

struct GuideOptions final {
    // The block the artist last worked on, 0 for none.
    Uid focus{};
    // The layer the artist is working on: only its guides are drawn, so the
    // stage shows what is being worked on and nothing else. 0 takes the
    // focused block's layer; with no focus either, nothing is drawn.
    Uid layer{};
    // Seconds into the preview, where the turbulence field has drifted to.
    float seconds{0.0F};
    // Where the effect's particles go (from the CPU reference), so the field
    // is drawn where it acts; without it, around each layer's spawn shape.
    std::optional<std::pair<Vec3, Vec3>> bounds;
    // The outlines of the models mesh colliders use, by path, as pairs of
    // points in each model's own space (user_assets.hpp mesh_outline). A
    // model not read yet draws as a marker at its position.
    const std::map<std::string, std::vector<Vec3>, std::less<>>* outlines{nullptr};
};

// The layer `options` names as worked on: its own, or the focused block's.
[[nodiscard]] Uid working_layer(const Document& document, const GuideOptions& options);

// Guides for the working layer's blocks, when it plays in the preview
// (enabled, and not muted by another layer's solo).
[[nodiscard]] GuideSet build_guides(const Document& document, const GuideOptions& options);

// The value `handle`'s control takes when it is dragged to `world`, or
// nothing when its block or control is gone. The result is sanitized.
[[nodiscard]] std::optional<Value> handle_value(const Document& document, const Handle& handle, Vec3 world);

} // namespace ajfx::studio
