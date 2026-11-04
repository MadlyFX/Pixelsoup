#pragma once

#include "document.hpp"

#include <ajfx/math.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// What the preview draws over the scene to show where the selected item acts,
// and the handles that move it.
//
// Guides are lines in the effect's own space (metres, scene scale applied):
// an Object's model outline and the shape it collides as, a particle
// system's birth ball and the Static things its particles meet, a force's
// arrow or axis, a blast's reach, a smoke grid's box. Only the selected
// item's are drawn, so the stage shows what is being worked on.
//
// A handle is a point the artist drags. It names the item and the property
// it sets and how a drag moves it (along the floor, up and down, out from a
// centre, round a heading), and drag_handle() writes where it was dragged to
// into the item, so the inspector and the preview agree on every number.
namespace ajfx::studio {

enum class GuideKind : std::uint8_t {
    Shape,    // where particles are born
    Force,    // what pushes them
    Field,    // a box of air
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
    // The item it shows.
    Uid item{};
    GuideKind kind{GuideKind::Shape};
    // The selected item's own, drawn strong; another's it is shown beside
    // (the system a force acts on, the things a system meets) are not.
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

// The property of an item a handle sets.
enum class HandleTarget : std::uint8_t {
    Position, // where it is
    Radius,   // a system's birth radius (`end` says which), an Object's ball, a blast's reach
    Size,     // a smoke grid's box
    Heading,  // a wind's heading
    Reach,    // an attractor's reach
    Height,   // where a draw-in fades by
};

struct Handle final {
    Uid item{};
    HandleTarget target{HandleTarget::Position};
    HandleMotion motion{HandleMotion::Floor};
    // For a range, the end it moves: 0 the low end, 1 the high.
    int end{1};
    // Where it is drawn, and what it measures from.
    Vec3 at{};
    Vec3 origin{};
    // Effect-space metres for one scene metre: the scene scale.
    float scale{1.0F};
    // A word or two for its tooltip: "Radius", "Position".
    std::string label;

    [[nodiscard]] bool same(const Handle& other) const noexcept {
        return item == other.item && target == other.target && end == other.end;
    }
};

struct GuideSet final {
    std::vector<Guide> guides;
    // Only the selected item's.
    std::vector<Handle> handles;
};

struct GuideOptions final {
    // The selected item; 0 draws nothing.
    Uid item{};
    // The outlines of the artist's models, by path, as pairs of points in
    // each model's own space (user_assets.hpp mesh_outline). A model not read
    // yet draws as a marker at its position.
    const std::map<std::string, std::vector<Vec3>, std::less<>>* outlines{nullptr};
};

// Guides and handles for the selected item, when it simulates.
[[nodiscard]] GuideSet build_guides(const Document& document, const GuideOptions& options);

// Writes what `handle` sets when it is dragged to `world` (effect space) into
// its item. False when nothing changed or the item is gone.
bool drag_handle(Document& document, const Handle& handle, Vec3 world);

} // namespace ajfx::studio
