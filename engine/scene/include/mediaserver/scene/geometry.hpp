#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/math.hpp>

#include <string_view>
#include <vector>

// Geometry, generated rather than loaded.
//
// A media server cannot have a bake step. The show is edited while it is
// running, on a stage, with a director watching - so there is no moment at
// which content can be compiled, and no operator who would tolerate one. Every
// mesh here is therefore a pure function of a small key, evaluated on the node
// that needs it, at the moment the document says it exists.
//
// That has a second consequence which matters more than the first: geometry
// never has to be distributed. A box is four bytes of document state on the
// wire, not a vertex buffer, and every node in the cohort generates the same
// vertices from it because the generator uses only the deterministic
// arithmetic in `math.hpp`. Content sync for primitives is free.
//
// The primitive set is deliberately small. It is not the product's geometry
// story - USD ingest is - but it is enough to build a scene that proves
// replication and slicing, and it is the right shape for what replaces it: a
// USD-backed mesh is still a key resolved to vertices behind this same call,
// with a content-addressed cache instead of a generator.
//
// Generation allocates, and is meant to. It runs when the document changes,
// never on the frame path; the renderer caches the result by key.
namespace mediaserver::scene {

enum class Primitive : core::u8 {
    // A unit square in the XZ plane, facing +Y, centred on the origin.
    Plane,
    // A unit cube centred on the origin, so extents run from -0.5 to +0.5.
    Box,
    // A sphere of diameter one, centred on the origin.
    Sphere,
    // A cylinder of diameter one and height one, axis along Y, centred on the
    // origin, with both caps closed.
    Cylinder,
    // A unit square in the XY plane, facing +Z, centred on the origin. Unlike
    // Plane (a floor, facing +Y) this stands up facing the camera, which is what
    // a video screen wants; its UVs run left-to-right and top-to-bottom so an
    // image lands the right way up.
    Quad,
};

// Every primitive is built at unit size on purpose. Size belongs to the
// instance transform, which means two objects of different sizes share one
// mesh and one cache entry.
inline constexpr core::u32 min_mesh_segments = 1U;
// A sphere at this tessellation is around 130,000 triangles. Past it the
// generator is being asked for detail that a displacement or a real asset
// should be supplying instead.
inline constexpr core::u32 max_mesh_segments = 256U;

// What a mesh is, entirely. Two keys that compare equal produce byte-identical
// vertices and indices on any machine, which is what lets the cache be keyed
// on this and nothing else.
struct MeshKey final {
    Primitive primitive{Primitive::Box};
    // Tessellation. Part of the key because it changes topology rather than
    // just vertex values, so two segment counts are two different meshes.
    core::u32 segments{16U};

    bool operator==(const MeshKey&) const = default;
    [[nodiscard]] auto operator<=>(const MeshKey&) const = default;
};

// Position, normal, and one texture coordinate pair: 32 bytes, which is both a
// convenient stride and the smallest record that supports lit, textured
// geometry. The vertex stage pulls these out of a storage buffer by index, so
// this layout is shared with the shader and asserted against it there.
struct MeshVertex final {
    Vec3 position{};
    Vec3 normal{};
    float u{};
    float v{};

    bool operator==(const MeshVertex&) const = default;
};

static_assert(sizeof(MeshVertex) == 32U);
static_assert(alignof(MeshVertex) == 4U);

struct MeshData final {
    std::vector<MeshVertex> vertices;
    std::vector<core::u32> indices;
};

// Clamps the segment count into range. Applied before a key reaches the cache
// so that two documents asking for tessellation 0 and tessellation 1 hit the
// same entry rather than generating the same mesh twice.
[[nodiscard]] MeshKey normalized(MeshKey key) noexcept;

// Replaces the contents of `into`. False only when the key is unrecognised,
// which a normalised key never is - the return exists so that adding a
// primitive cannot silently produce an empty mesh.
[[nodiscard]] bool build_mesh(MeshKey key, MeshData& into);

// The names the document uses. Kept next to the enum so a new primitive cannot
// be added without deciding what an operator types for it.
[[nodiscard]] std::string_view primitive_name(Primitive primitive) noexcept;
[[nodiscard]] bool parse_primitive(
    std::string_view name,
    Primitive& into) noexcept;

} // namespace mediaserver::scene
