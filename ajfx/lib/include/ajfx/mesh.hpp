#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/math.hpp>
#include <ajfx/package.hpp>
#include <ajfx/result.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

// Meshes for mesh particles: each particle draws the emitter's mesh, scaled
// by its size and turned by its rotation or velocity. A package carries each
// mesh as an entry "meshes/<name>", a triangle list built when the package is
// authored. Entry layout, all little-endian:
//
//     "AJMS"                 4 bytes
//     mesh version           u32
//     vertex count           u32 (3 to max_mesh_vertices)
//     index count            u32 (a multiple of 3, to max_mesh_indices)
//     vertices               f32 x 8 each: position xyz, normal xyz, uv
//     indices                u32 each, below the vertex count
//
// Triangles wind counter-clockwise seen from outside.
namespace ajfx {

inline constexpr std::uint32_t mesh_format_version = 1U;
inline constexpr std::uint32_t max_mesh_vertices = 65536U;
inline constexpr std::uint32_t max_mesh_indices = 3U * 65536U;

struct MeshVertex final {
    Vec3 position{};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    float u{};
    float v{};

    friend bool operator==(const MeshVertex&, const MeshVertex&) = default;
};

struct MeshData final {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;

    friend bool operator==(const MeshData&, const MeshData&) = default;
};

// Checks counts and indices; fails with InvalidEffect naming the problem.
[[nodiscard]] Result<MeshData> make_mesh(std::vector<MeshVertex> vertices, std::vector<std::uint32_t> indices);
// A unit cube centred on the origin (half extent 0.5), one normal a face.
[[nodiscard]] MeshData cube_mesh();

[[nodiscard]] std::vector<std::uint8_t> write_mesh(const MeshData& mesh);
// Fails with MalformedPackage for a bad header, count, index or number, and
// UnsupportedVersion for a newer layout.
[[nodiscard]] Result<MeshData> read_mesh(std::span<const std::uint8_t> bytes);

// Meshes by the name emitters use (`SpriteRenderer::mesh`).
using MeshSet = std::map<std::string, MeshData, std::less<>>;

// "meshes/<name>" entries for write_package's `extra`.
[[nodiscard]] std::vector<PackageEntry> mesh_package_entries(const MeshSet& meshes);

// Checks that `meshes` holds every mesh the effect's emitters draw. Fails
// with MissingMesh, naming the emitter.
[[nodiscard]] Result<bool> check_meshes(const Effect& effect, const MeshSet& meshes);

// Every mesh the effect draws, read from its package and checked as above.
[[nodiscard]] Result<MeshSet> read_package_meshes(std::span<const std::uint8_t> package, const Effect& effect);

} // namespace ajfx
