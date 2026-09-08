#pragma once

#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/scene.hpp>

#include <array>
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
// and emissive slots decode sRGB in the shader, while metallic-roughness,
// tangent normal, and occlusion data are sampled linearly.
struct ModelTextureMip final {
    core::u32 width{};
    core::u32 height{};
    std::vector<core::u8> rgba;
    bool operator==(const ModelTextureMip&) const = default;
};

enum class ModelTextureEncoding : core::u8 { Rgba8, Bc7, Bc5 };

struct ModelTexture final {
    core::u32 width{};
    core::u32 height{};
    std::vector<core::u8> rgba;
    // Prepared once by the model worker. Level zero stays in rgba for ingest
    // consumers; lower_mips[0] is level one, ending at 1x1.
    std::vector<ModelTextureMip> lower_mips;
    // rgba contains block bytes for compressed encodings (including each mip).
    ModelTextureEncoding encoding{ModelTextureEncoding::Rgba8};
    // Derived on the worker from any view with minification mipmapping disabled.
    bool requires_base_level{};

    [[nodiscard]] bool valid() const noexcept {
        return width != 0U && height != 0U &&
               rgba.size() == (encoding == ModelTextureEncoding::Rgba8
                   ? static_cast<std::size_t>(width) * height * 4U
                   : static_cast<std::size_t>((width + 3U) / 4U) * ((height + 3U) / 4U) * 16U);
    }

    bool operator==(const ModelTexture&) const = default;
};

// One contiguous material run in `ModelAsset::mesh.indices`. A material is
// considered imported only when the source primitive/face actually names one.
// This distinction lets the renderer use an authored ModelInstance material as
// the deterministic fallback without replacing material factors that did come
// from the asset.
// Per texture view, not per image: two views can share bytes but wrap differently.
enum class ModelAddressMode : core::u8 { Clamp, Repeat, Mirror };
enum class ModelFilter : core::u8 { Nearest, Linear };
enum class ModelMipFilter : core::u8 { None, Nearest, Linear };
struct ModelSampler final {
    ModelAddressMode u{ModelAddressMode::Repeat};
    ModelAddressMode v{ModelAddressMode::Repeat};
    ModelFilter min{ModelFilter::Linear};
    ModelFilter mag{ModelFilter::Linear};
    ModelMipFilter mip{ModelMipFilter::Linear};
    bool operator==(const ModelSampler&) const = default;
};
inline constexpr std::size_t model_material_texture_count = 14U;

struct ModelPart final {
    core::u32 first_index{};
    core::u32 index_count{};
    scene::Material material{};
    bool has_material{};
    // Indices into ModelAsset::textures. Missing, undecodable, or currently
    // unsupported texture views remain null and preserve the material factor.
    std::optional<core::u32> base_color_texture;
    std::optional<core::u32> metallic_roughness_texture;
    std::optional<core::u32> normal_texture;
    std::optional<core::u32> occlusion_texture;
    std::optional<core::u32> emissive_texture;
    std::optional<core::u32> clearcoat_texture;
    std::optional<core::u32> clearcoat_roughness_texture;
    std::optional<core::u32> clearcoat_normal_texture;
    std::optional<core::u32> sheen_color_texture;
    std::optional<core::u32> sheen_roughness_texture;
    std::optional<core::u32> transmission_texture;
    std::optional<core::u32> specular_texture;
    std::optional<core::u32> specular_color_texture;
    std::optional<core::u32> anisotropy_texture;
    // Base, metal/roughness, normal, occlusion, emissive, then the nine lobes.
    std::array<ModelSampler, model_material_texture_count> samplers{};


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
    // Tangents remain a parallel stream so projection-mesh consumers keep the
    // compact MeshVertex layout. The renderer uploads this stream directly.
    // A normal-mapped glTF primitive without TANGENT is filled by the format's
    // default MikkTSpace algorithm during this explicit asset load.
    bool has_tangent{};
    scene::Vec4 tangent{};

    bool operator==(const ModelVertexMetadata&) const = default;
};

// Geometry is combined for one arena upload while parts preserve the source's
// material boundaries. glTF node transforms are baked into both the vertices
// and these bounds.
inline constexpr core::u32 max_model_lods = 8U;

// Level zero is the original mesh. Each later level replaces it at or below
// max_pixels, with strictly descending thresholds. Parts and geometry for all
// levels share one immutable asset and one transactional upload.
struct ModelLod final {
    core::u32 first_part{};
    core::u32 part_count{};
    float max_pixels{};
    bool operator==(const ModelLod&) const = default;
};

struct ModelAsset final {
    scene::MeshData mesh;
    // Always parallel to mesh.vertices after a successful load.
    std::vector<ModelVertexMetadata> vertex_metadata;
    std::vector<ModelPart> parts;
    std::vector<ModelTexture> textures;
    ModelBounds bounds{};
    std::vector<ModelLod> lods;
    // Worker-produced failure reason. Retained when geometry is refused.
    std::string diagnostic;
};

// Worker-only filtering. Shared images used in different material roles are
// split before filtering: colour in linear light, data linearly, normals on
// the unit sphere. No codec/filter work is permitted in render observation.
[[nodiscard]] bool prepare_model_mips(ModelAsset& asset);
// Explicit ingest worker action, after role-correct mip generation.
[[nodiscard]] bool compress_model_textures(ModelAsset& asset);

// An optional source + ".lod" companion contains MSLOD 1 followed by descending
// pixel thresholds and quoted relative model paths, one per line. All levels
// decode on the cache worker; malformed chains refuse the complete generation.
// Fills `into` (cleared first) from glTF/GLB or OBJ. Returns false on any
// failure (unreadable file, unknown extension, malformed or empty geometry),
// which the caller draws as nothing rather than failing the whole frame.
[[nodiscard]] bool load_model_base(const std::string& source, ModelAsset& into);

[[nodiscard]] bool load_model(const std::string& source, ModelAsset& into);

} // namespace mediaserver::render
