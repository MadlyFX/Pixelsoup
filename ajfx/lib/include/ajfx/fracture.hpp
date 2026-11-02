#pragma once

#include <ajfx/math.hpp>
#include <ajfx/mesh.hpp>
#include <ajfx/result.hpp>

#include <cstdint>
#include <vector>

// Cutting a model into the pieces it breaks into.
//
// A break authored by hand places shard models beside one another and hopes
// they add up to the thing that broke. Cell fracture earns it instead: it
// drops sites inside the model and keeps, for each one, the volume that is
// nearer it than any other site. The pieces fill the model with no gap and no
// overlap, so the silhouette survives the break and torn faces meet their
// neighbours exactly.
//
// What gets cut is the convex hull of the model's points, not its concave
// geometry. A cell is an intersection of half-spaces - the perpendicular
// bisector between its site and every other site, closed off by the hull's
// own faces - so a concave model fractures as its hull: a cup breaks as the
// solid block around it, and a dent in a wall is gone from the pieces. That
// is already the shape ajfx collides a rigid body as, and it is the right
// shape for crates, stones and concrete. A model whose hollows must survive
// the break needs a mesh boolean, which this is not.
//
// Only the model's vertex positions are read; its triangles are not, since
// what is cut is the hull of those points.
//
// Cutting belongs to baking an effect and never to playing one. Most of the
// cost is the hull, which weighs the model's points against its own faces and
// so grows with the square of the points: a thousand-point model is cut in a
// fifth of a second, eight thousand points take a quarter of a minute, and
// the 65536 a mesh may hold would take the better part of a quarter of an
// hour. The cells themselves are cheap beside that - 256 of them cost a
// second more, not a minute. Nothing here may be called on a frame path.
namespace ajfx {

// Cells per model, fixed up front like every other ajfx budget.
inline constexpr std::uint32_t max_fracture_cells = 1024U;

// Where the cells come from: `count` sites placed inside the model, hashed
// from `seed` so the same seed cuts the same way every time.
struct FractureOptions final {
    std::uint32_t count{8U};
    std::uint32_t seed{0U};
    // Sites drawn towards this point, for a break centred on an impact:
    // 0 scatters them evenly, 1 clusters them tightly at `focus`. A focus
    // outside the model draws them as near to it as the model allows, since a
    // site that left the model would own no volume.
    Vec3 focus{};
    float focus_bias{0.0F};
};

// One piece of a fractured model: its geometry in the model's own frame, and
// where its middle sits, so a caller can place it as the model had it. The
// mesh is closed, convex and wound outwards; its texture coordinates are the
// distance across each face from that face's first corner, in model units,
// for a caller to scale as its material wants.
struct FractureCell final {
    MeshData mesh;
    Vec3 center{};
    float volume{0.0F};
};

// The model cut into cells that fill its hull. Cells come back in site order,
// and the same model and options give byte-identical cells on any machine:
// the sites are `particle_hash` draws, and the cutting is addition,
// multiplication, division and square root, none of which a conforming
// compiler may reorder under ajfx's floating-point settings.
//
// Fails with InvalidEffect when there is nothing to cut (fewer than four
// corners, or every corner in one line or plane), when `count` is zero, when
// `focus` is not a number or `focus_bias` falls outside 0 to 1, and when
// `count` sites will not fit inside the model; with BudgetExceeded above
// `max_fracture_cells`.
[[nodiscard]] Result<std::vector<FractureCell>> fracture(const MeshData& model, const FractureOptions& options);

} // namespace ajfx
