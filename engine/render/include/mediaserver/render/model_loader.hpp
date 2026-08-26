#pragma once

#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/scene.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// Loads an imported 3D model file into the renderer's mesh format.
//
// Behind the same idea as the media decode seam: the scene carries only a path,
// and this turns it into geometry the renderer uploads and caches. glTF and OBJ
// are supported, chosen by extension. Only base geometry is read - positions,
// normals (generated flat per primitive if absent), UVs, material factors,
// material textures, and bounds. Pure with respect to the file, so two nodes
// with the same file build the same asset.
namespace mediaserver::render {

// One decoded glTF image. Pixels remain ordinary RGBA8 bytes here; colour-space
// interpretation belongs to the material slot that samples them. Base-colour
// and emissive slots decode sRGB in the shader, while metallic-roughness reads
// its packed G/B channels linearly.
struct ModelTexture final {
    core::u32 width{};
    core::u32 height{};
    std::vector<core::u8> rgba;

    [[nodiscard]] bool valid() const noexcept {
        return width != 0U && height != 0U &&
               rgba.size() == static_cast<std::size_t>(width) * height * 4U;
    }

    bool operator==(const ModelTexture&) const = default;
};

// One contiguous material run in `ModelAsset::mesh.indices`. A material is
// considered imported only when the source primitive/face actually names one.
// This distinction lets the renderer use an authored ModelInstance material as
// the deterministic fallback without replacing material factors that did come
// from the asset.
struct ModelPart final {
    core::u32 first_index{};
    core::u32 index_count{};
    scene::Material material{};
    bool has_material{};
    // Indices into ModelAsset::textures. Missing, undecodable, or currently
    // unsupported texture views remain null and preserve the material factor.
    std::optional<core::u32> base_color_texture;
    std::optional<core::u32> metallic_roughness_texture;
    std::optional<core::u32> emissive_texture;

    bool operator==(const ModelPart&) const = default;
};

// Axis-aligned bounds in the same model space as the baked mesh. Successful
// loads always contain geometry, so minimum/maximum are valid without a second
// state bit.
struct ModelBounds final {
    scene::Vec3 minimum{};
    scene::Vec3 maximum{};

    bool operator==(const ModelBounds&) const = default;
};

// Source-presence facts that cannot be represented by MeshVertex itself.
// MeshVertex deliberately keeps its fixed GPU layout, where a missing UV and
// an authored (0,0) UV have the same numeric value. Importers that require an
// authored mapping, such as LED projection meshes, use this parallel metadata
// to distinguish those two cases exactly.
struct ModelVertexMetadata final {
    bool has_uv0{};

    bool operator==(const ModelVertexMetadata&) const = default;
};

// Geometry is combined for one arena upload while parts preserve the source's
// material boundaries. glTF node transforms are baked into both the vertices
// and these bounds.
struct ModelAsset final {
    scene::MeshData mesh;
    // Always parallel to mesh.vertices after a successful load.
    std::vector<ModelVertexMetadata> vertex_metadata;
    std::vector<ModelPart> parts;
    std::vector<ModelTexture> textures;
    ModelBounds bounds{};
};

// Fills `into` (cleared first) from glTF/GLB or OBJ. Returns false on any
// failure (unreadable file, unknown extension, malformed or empty geometry),
// which the caller draws as nothing rather than failing the whole frame.
[[nodiscard]] bool load_model(const std::string& source, ModelAsset& into);

} // namespace mediaserver::render
