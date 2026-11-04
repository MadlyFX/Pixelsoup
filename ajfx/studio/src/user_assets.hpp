#pragma once

#include <ajfx/field.hpp>
#include <ajfx/math.hpp>
#include <ajfx/mesh.hpp>
#include <ajfx/texture.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The artist's own files: pictures for sprites, models for debris, and
// models to collide with. Reading a file is the builder's business (it runs
// on the worker, with the media server's image and model decoders); what is
// here turns what was read into what a package carries, deterministically,
// so the same file always exports the same bytes:
// - a picture becomes a sprite texture with its mip chain;
// - a model becomes a debris mesh, centred and fitted to a 1 m box so the
//   Size block sets how big each piece is;
// - a model to collide with becomes a distance field. The model itself never
//   goes into the package, only the field the particles bounce off.
namespace ajfx::studio {

// A picture as read: straight-alpha RGBA8, rows top to bottom.
struct ImportedImage final {
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::vector<std::uint8_t> rgba;
};

// A model's triangles, every node's transform applied. `normals` and `uvs`
// (two floats a vertex) are empty when the file has none.
struct ImportedMesh final {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<float> uvs;
    std::vector<std::uint32_t> indices;
};

// The package entry an artist's file is carried as: "user.<16 hex>", from
// what it is, its path and how it was prepared, so two blocks using the same
// file the same way share one entry.
[[nodiscard]] std::string user_asset_name(std::string_view kind, std::string_view path, std::string_view options = {});

// Largest edge a sprite picture keeps; larger ones are halved until they fit.
inline constexpr std::uint32_t max_sprite_size = 1024U;

[[nodiscard]] std::optional<TextureData> sprite_texture(const ImportedImage& image, std::string& error);

// Centred on its bounding box and scaled so its longest side is 1 m; flat
// normals where the file has none. Refuses a model with more corners than
// a mesh may have.
[[nodiscard]] std::optional<MeshData> debris_mesh(const ImportedMesh& mesh, std::string& error);

enum class ColliderSolid : std::uint8_t {
    Auto,    // solid when the model is closed, else a thin surface
    Solid,   // its inside is inside
    Surface, // a shell: particles stay off both sides
};

struct ColliderBake final {
    // In the model's space times `scale`; SdfCollision places it.
    DistanceField field;
    // Treated as solid (else a thin surface).
    bool solid{true};
};

// Every edge of the model is shared by exactly two triangles (positions that
// coincide count as one corner).
[[nodiscard]] bool closed_mesh(const ImportedMesh& mesh);

// The model as an object stands it: with `fit`, centred on its bounding box
// with its longest side 1 (as debris_mesh draws it); then turned by `turn`
// (degrees, prop_orientation). Corners, normals and triangles otherwise as
// they were.
[[nodiscard]] ImportedMesh placed_mesh(const ImportedMesh& mesh, bool fit, Vec3 turn);

// Bakes the model, times `scale`, into a signed distance field about `cells`
// samples along its longest side, with a margin round it so particles meet
// the surface inside the grid. Exact distances near the surface, carried
// outwards by sweeping; inside is decided by rays along all three axes, two
// of three agreeing.
[[nodiscard]] std::optional<ColliderBake> bake_collider(const ImportedMesh& mesh, float scale, std::uint32_t cells,
                                                         ColliderSolid solid, std::string& error);

// The model's outline for the guides, as pairs of points in its own space:
// its open edges and creases first, at most `max_segments` of them.
[[nodiscard]] std::vector<Vec3> mesh_outline(const ImportedMesh& mesh, std::size_t max_segments);

} // namespace ajfx::studio
