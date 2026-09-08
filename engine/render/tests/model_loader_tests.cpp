#include <mediaserver/render/model_loader.hpp>
#include <mediaserver/render/model_package.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// CPU-only importer coverage. These fixtures pin down the seam the renderer
// consumes: combined geometry, source material runs, transformed bounds, and
// normals that remain correct when one glTF primitive supplies them and its
// neighbour does not.
namespace {

namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define MODEL_CHECK(...)                                                       \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] std::filesystem::path temp_path(const std::string& name) {
    const auto directory = std::filesystem::temp_directory_path() /
                           "mediaserver_model_loader_tests";
    std::filesystem::create_directories(directory);
    return directory / name;
}

[[nodiscard]] std::string write_temp(const std::string& name,
                                     const std::string& body) {
    const auto path = temp_path(name);
    std::ofstream out{path, std::ios::trunc};
    out << body;
    out.close();
    check(static_cast<bool>(out), "wrote a temp model", __LINE__);
    return path.string();
}

void write_temp_binary(const std::string& name,
                       const std::vector<core::u8>& bytes) {
    std::ofstream out{temp_path(name), std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    check(static_cast<bool>(out), "wrote a temp model buffer", __LINE__);
}

void write_temp_tga(const std::string& name,
                    const std::array<core::u8, 4U>& rgba) {
    // One uncompressed top-left-origin BGRA texel. TGA keeps these fixtures
    // tiny and exercises the same stb_image decode path as PNG/JPEG assets.
    std::vector<core::u8> bytes(18U, 0U);
    bytes[2U] = 2U;
    bytes[12U] = 1U;
    bytes[14U] = 1U;
    bytes[16U] = 32U;
    bytes[17U] = 0x28U;
    bytes.push_back(rgba[2U]);
    bytes.push_back(rgba[1U]);
    bytes.push_back(rgba[0U]);
    bytes.push_back(rgba[3U]);
    write_temp_binary(name, bytes);
}

void append_u16(std::vector<core::u8>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<core::u8>(value & 0xffU));
    bytes.push_back(static_cast<core::u8>((value >> 8U) & 0xffU));
}

void append_f32(std::vector<core::u8>& bytes, float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    bytes.push_back(static_cast<core::u8>(bits & 0xffU));
    bytes.push_back(static_cast<core::u8>((bits >> 8U) & 0xffU));
    bytes.push_back(static_cast<core::u8>((bits >> 16U) & 0xffU));
    bytes.push_back(static_cast<core::u8>((bits >> 24U) & 0xffU));
}

[[nodiscard]] bool near(float lhs, float rhs, float tolerance = 1.0e-5F) {
    return std::abs(lhs - rhs) <= tolerance;
}

void an_obj_quad_loads_to_geometry_with_normals_and_bounds() {
    // A unit quad on the XY plane, two triangles, no normals or material.
    const auto path =
        write_temp("unassigned_quad.obj", "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                                          "f 1 2 3\nf 1 3 4\n");

    render::ModelAsset asset{};
    MODEL_CHECK(render::load_model(path, asset));
    // OBJ face indices expand to unique vertices: two triangles, six of each.
    MODEL_CHECK(asset.mesh.vertices.size() == 6U);
    MODEL_CHECK(asset.mesh.indices.size() == 6U);
    MODEL_CHECK(asset.vertex_metadata.size() == asset.mesh.vertices.size());
    for (const auto& metadata : asset.vertex_metadata) {
        MODEL_CHECK(!metadata.has_uv0);
    }
    MODEL_CHECK(asset.parts.size() == 1U);
    MODEL_CHECK(asset.parts[0U].first_index == 0U);
    MODEL_CHECK(asset.parts[0U].index_count == 6U);
    MODEL_CHECK(!asset.parts[0U].has_material);
    for (const auto& vertex : asset.mesh.vertices) {
        MODEL_CHECK(near(vertex.normal.z, 1.0F));
        MODEL_CHECK(near(vertex.normal.x, 0.0F));
        MODEL_CHECK(near(vertex.normal.y, 0.0F));
    }
    MODEL_CHECK(asset.bounds.minimum == scene::Vec3{0.0F, 0.0F, 0.0F});
    MODEL_CHECK(asset.bounds.maximum == scene::Vec3{1.0F, 1.0F, 0.0F});

    // Loading clears the target first, so a reload does not accumulate.
    MODEL_CHECK(render::load_model(path, asset));
    MODEL_CHECK(asset.mesh.vertices.size() == 6U);
}

void obj_material_runs_preserve_pbr_factors() {
    const auto material_path =
        write_temp("material_runs.mtl",
                   "newmtl Copper\nKd 0.80 0.20 0.10\nPr 0.35\nPm 0.70\n"
                   "Ke 0.01 0.02 0.03\n"
                   "newmtl Cyan\nKd 0.05 0.75 0.90\nPr 0.80\nPm 0.10\n"
                   "Ke 0.10 0.20 0.30\n");
    MODEL_CHECK(!material_path.empty());
    const auto path =
        write_temp("material_runs.obj", "mtllib material_runs.mtl\n"
                                        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                                        "usemtl Copper\nf 1 2 3\n"
                                        "usemtl Cyan\nf 1 3 4\n");

    render::ModelAsset asset{};
    MODEL_CHECK(render::load_model(path, asset));
    MODEL_CHECK(asset.parts.size() == 2U);
    MODEL_CHECK(asset.parts[0U].first_index == 0U);
    MODEL_CHECK(asset.parts[0U].index_count == 3U);
    MODEL_CHECK(asset.parts[1U].first_index == 3U);
    MODEL_CHECK(asset.parts[1U].index_count == 3U);
    MODEL_CHECK(asset.parts[0U].has_material);
    MODEL_CHECK(asset.parts[1U].has_material);

    const auto& copper = asset.parts[0U].material;
    MODEL_CHECK(near(copper.base_color.x, 0.80F));
    MODEL_CHECK(near(copper.base_color.y, 0.20F));
    MODEL_CHECK(near(copper.base_color.z, 0.10F));
    MODEL_CHECK(near(copper.roughness, 0.35F));
    MODEL_CHECK(near(copper.metallic, 0.70F));
    MODEL_CHECK(near(copper.emissive.z, 0.03F));

    const auto& cyan = asset.parts[1U].material;
    MODEL_CHECK(near(cyan.base_color.x, 0.05F));
    MODEL_CHECK(near(cyan.base_color.y, 0.75F));
    MODEL_CHECK(near(cyan.base_color.z, 0.90F));
    MODEL_CHECK(near(cyan.roughness, 0.80F));
    MODEL_CHECK(near(cyan.metallic, 0.10F));
    MODEL_CHECK(near(cyan.emissive.y, 0.20F));
}

void gltf_parts_keep_materials_bounds_and_correct_normals() {
    std::vector<core::u8> buffer;
    const std::array<float, 18U> attributes{
        // Three positions.
        0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
        // One diagonal normal repeated for the first primitive.
        0.70710677F, 0.70710677F, 0.0F, 0.70710677F, 0.70710677F, 0.0F,
        0.70710677F, 0.70710677F, 0.0F};
    for (float value : attributes) {
        append_f32(buffer, value);
    }
    for (std::size_t primitive = 0U; primitive < 2U; ++primitive) {
        append_u16(buffer, 0U);
        append_u16(buffer, 1U);
        append_u16(buffer, 2U);
    }
    MODEL_CHECK(buffer.size() == 84U);
    write_temp_binary("material_parts.bin", buffer);

    const auto path = write_temp("material_parts.gltf",
                                 R"({
  "asset": {"version": "2.0"},
  "buffers": [{"uri": "material_parts.bin", "byteLength": 84}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 36},
    {"buffer": 0, "byteOffset": 72, "byteLength": 6},
    {"buffer": 0, "byteOffset": 78, "byteLength": 6}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"},
    {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "materials": [
    {"pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.1, 0.2, 1.0],
      "metallicFactor": 0.7, "roughnessFactor": 0.25},
      "emissiveFactor": [0.03, 0.04, 0.05]},
    {"pbrMetallicRoughness": {"baseColorFactor": [0.1, 0.2, 0.9, 1.0],
      "metallicFactor": 0.2, "roughnessFactor": 0.8}}
  ],
  "meshes": [{"primitives": [
    {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0},
    {"attributes": {"POSITION": 0}, "indices": 3, "material": 1}
  ]}],
  "nodes": [{"mesh": 0, "translation": [3, -1, 2], "scale": [2, 1, 1]}],
  "scenes": [{"nodes": [0]}],
  "scene": 0
})");

    render::ModelAsset asset{};
    MODEL_CHECK(render::load_model(path, asset));
    MODEL_CHECK(asset.mesh.vertices.size() == 6U);
    MODEL_CHECK(asset.mesh.indices.size() == 6U);
    MODEL_CHECK(asset.parts.size() == 2U);
    MODEL_CHECK(asset.parts[0U].first_index == 0U);
    MODEL_CHECK(asset.parts[0U].index_count == 3U);
    MODEL_CHECK(asset.parts[1U].first_index == 3U);
    MODEL_CHECK(asset.parts[1U].index_count == 3U);

    MODEL_CHECK(asset.bounds.minimum == scene::Vec3{3.0F, -1.0F, 2.0F});
    MODEL_CHECK(asset.bounds.maximum == scene::Vec3{5.0F, 0.0F, 2.0F});

    // Inverse-transpose(diag(2, 1, 1)) turns (1, 1, 0) into
    // normalize(0.5, 1, 0), not the normalize(2, 1, 0) a plain basis gives.
    MODEL_CHECK(near(asset.mesh.vertices[0U].normal.x, 0.4472136F));
    MODEL_CHECK(near(asset.mesh.vertices[0U].normal.y, 0.8944272F));
    MODEL_CHECK(near(asset.mesh.vertices[0U].normal.z, 0.0F));
    // The neighbouring primitive had no normals; only it was expanded and its
    // transformed face normal generated. The supplied normals above survive.
    for (std::size_t vertex = 3U; vertex < 6U; ++vertex) {
        MODEL_CHECK(near(asset.mesh.vertices[vertex].normal.x, 0.0F));
        MODEL_CHECK(near(asset.mesh.vertices[vertex].normal.y, 0.0F));
        MODEL_CHECK(near(asset.mesh.vertices[vertex].normal.z, 1.0F));
    }

    const auto& first = asset.parts[0U].material;
    MODEL_CHECK(asset.parts[0U].has_material);
    MODEL_CHECK(near(first.base_color.x, 0.8F));
    MODEL_CHECK(near(first.roughness, 0.25F));
    MODEL_CHECK(near(first.metallic, 0.7F));
    MODEL_CHECK(near(first.emissive.y, 0.04F));
    const auto& second = asset.parts[1U].material;
    MODEL_CHECK(asset.parts[1U].has_material);
    MODEL_CHECK(near(second.base_color.z, 0.9F));
    MODEL_CHECK(near(second.roughness, 0.8F));
    MODEL_CHECK(near(second.metallic, 0.2F));
}

void gltf_texture_slots_decode_and_fallback_per_primitive() {
    std::vector<core::u8> buffer;
    const std::array<float, 18U> position_and_normal{
        -1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F, 0.0F, 1.0F, 0.0F,
        0.0F,  0.0F,  1.0F, 0.0F, 0.0F,  1.0F, 0.0F, 0.0F, 1.0F};
    for (float value : position_and_normal) {
        append_f32(buffer, value);
    }
    for (float value :
         std::array<float, 6U>{0.0F, 1.0F, 1.0F, 1.0F, 0.5F, 0.0F}) {
        append_f32(buffer, value);
    }
    for (std::size_t vertex = 0U; vertex < 3U; ++vertex) {
        for (float value : std::array<float, 4U>{1.0F, 0.0F, 0.0F, 1.0F}) {
            append_f32(buffer, value);
        }
    }
    for (std::size_t primitive = 0U; primitive < 2U; ++primitive) {
        append_u16(buffer, 0U);
        append_u16(buffer, 1U);
        append_u16(buffer, 2U);
    }
    MODEL_CHECK(buffer.size() == 156U);
    write_temp_binary("texture_slots.bin", buffer);
    write_temp_tga("base_color.tga", {128U, 64U, 32U, 255U});
    write_temp_tga("metal_rough.tga", {7U, 153U, 204U, 255U});
    write_temp_tga("emissive.tga", {15U, 31U, 63U, 255U});
    write_temp_tga("normal.tga", {128U, 128U, 255U, 255U});
    write_temp_tga("occlusion.tga", {191U, 0U, 0U, 255U});

    const auto path = write_temp("texture_slots.gltf",
                                 R"({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["KHR_materials_emissive_strength"],
  "buffers": [{"uri": "texture_slots.bin", "byteLength": 156}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 36},
    {"buffer": 0, "byteOffset": 72, "byteLength": 24},
    {"buffer": 0, "byteOffset": 96, "byteLength": 48},
    {"buffer": 0, "byteOffset": 144, "byteLength": 6},
    {"buffer": 0, "byteOffset": 150, "byteLength": 6}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
    {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC4"},
    {"bufferView": 4, "componentType": 5123, "count": 3, "type": "SCALAR"},
    {"bufferView": 5, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "images": [
    {"uri": "base_color.tga"},
    {"uri": "metal_rough.tga"},
    {"uri": "emissive.tga"},
    {"uri": "normal.tga"},
    {"uri": "occlusion.tga"},
    {"uri": "missing_texture.tga"}
  ],
  "textures": [
    {"source": 0}, {"source": 1}, {"source": 2}, {"source": 3},
    {"source": 4}, {"source": 5}
  ],
  "materials": [
    {"pbrMetallicRoughness": {
       "baseColorFactor": [0.5, 0.6, 0.7, 1.0],
       "baseColorTexture": {"index": 0},
       "metallicFactor": 0.8,
       "roughnessFactor": 0.4,
       "metallicRoughnessTexture": {"index": 1}},
     "emissiveFactor": [0.2, 0.3, 0.4],
     "emissiveTexture": {"index": 2},
     "normalTexture": {"index": 3, "scale": 1.25},
     "occlusionTexture": {"index": 4, "strength": 0.7},
     "alphaMode": "MASK", "alphaCutoff": 0.3, "doubleSided": true,
     "extensions": {"KHR_materials_emissive_strength": {
       "emissiveStrength": 4.0}}},
    {"pbrMetallicRoughness": {
       "baseColorTexture": {"index": 0, "texCoord": 1},
       "metallicRoughnessTexture": {"index": 5}}}
  ],
  "meshes": [{"primitives": [
    {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
     "indices": 4, "material": 0},
    {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2,
                    "TANGENT": 3},
     "indices": 5, "material": 1}
  ]}],
  "nodes": [{"mesh": 0}],
  "scenes": [{"nodes": [0]}],
  "scene": 0
})");

    render::ModelAsset asset{};
    MODEL_CHECK(!render::load_model(path, asset));
    MODEL_CHECK(asset.diagnostic.find("texture view") != std::string::npos);
    std::ifstream input{path};
    std::string document{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    input.close();
    const std::string unsupported = "\"baseColorTexture\": {\"index\": 0, \"texCoord\": 1},\n       \"metallicRoughnessTexture\": {\"index\": 5}";
    const auto offset = document.find(unsupported); MODEL_CHECK(offset != std::string::npos);
    document.replace(offset, unsupported.size(), "\"baseColorFactor\": [1, 1, 1, 1]");
    { std::ofstream output{path}; output << document; }
    MODEL_CHECK(render::load_model(path, asset));
    MODEL_CHECK(asset.textures.size() == 6U);
    MODEL_CHECK(asset.vertex_metadata.size() == asset.mesh.vertices.size());
    for (std::size_t vertex = 0U;
         vertex < asset.vertex_metadata.size(); ++vertex) {
        const auto& metadata = asset.vertex_metadata[vertex];
        MODEL_CHECK(metadata.has_uv0);
        MODEL_CHECK(metadata.has_tangent);
        MODEL_CHECK(near(metadata.tangent.x, 1.0F));
        // The normal-mapped first primitive omitted TANGENT, so its -V UV
        // orientation is generated by default MikkTSpace. The second
        // primitive retains its explicitly authored +1 sign.
        MODEL_CHECK(near(
            metadata.tangent.w, vertex < 3U ? -1.0F : 1.0F));
    }
    MODEL_CHECK(asset.textures[0U].valid());
    MODEL_CHECK(asset.textures[0U].width == 1U);
    MODEL_CHECK(asset.textures[0U].height == 1U);
    MODEL_CHECK(asset.textures[0U].rgba ==
                std::vector<core::u8>({128U, 64U, 32U, 255U}));
    MODEL_CHECK(asset.textures[1U].rgba ==
                std::vector<core::u8>({7U, 153U, 204U, 255U}));
    MODEL_CHECK(asset.textures[2U].rgba ==
                std::vector<core::u8>({15U, 31U, 63U, 255U}));
    MODEL_CHECK(asset.textures[3U].valid());
    MODEL_CHECK(asset.textures[4U].valid());
    MODEL_CHECK(!asset.textures[5U].valid());

    MODEL_CHECK(asset.parts.size() == 2U);
    MODEL_CHECK(asset.parts[0U].base_color_texture == 0U);
    MODEL_CHECK(asset.parts[0U].metallic_roughness_texture == 1U);
    MODEL_CHECK(asset.parts[0U].emissive_texture == 2U);
    MODEL_CHECK(asset.parts[0U].normal_texture == 3U);
    MODEL_CHECK(asset.parts[0U].occlusion_texture == 4U);
    MODEL_CHECK(near(asset.parts[0U].material.base_color.y, 0.6F));
    MODEL_CHECK(near(asset.parts[0U].material.roughness, 0.4F));
    MODEL_CHECK(near(asset.parts[0U].material.metallic, 0.8F));
    MODEL_CHECK(near(asset.parts[0U].material.emissive.z, 0.4F));
    MODEL_CHECK(near(asset.parts[0U].material.emissive_strength, 4.0F));
    MODEL_CHECK(near(asset.parts[0U].material.normal_scale, 1.25F));
    MODEL_CHECK(near(asset.parts[0U].material.occlusion_strength, 0.7F));
    MODEL_CHECK(asset.parts[0U].material.alpha_mode ==
                scene::MaterialAlphaMode::Mask);
    MODEL_CHECK(near(asset.parts[0U].material.alpha_cutoff, 0.3F));
    MODEL_CHECK(asset.parts[0U].material.double_sided);
    // The corrected second material authors no maps; its slots stay absent.
    MODEL_CHECK(!asset.parts[1U].base_color_texture.has_value());
    MODEL_CHECK(!asset.parts[1U].metallic_roughness_texture.has_value());
    MODEL_CHECK(!asset.parts[1U].normal_texture.has_value());
    MODEL_CHECK(!asset.parts[1U].occlusion_texture.has_value());
    MODEL_CHECK(!asset.parts[1U].emissive_texture.has_value());
}

void an_unloadable_source_is_refused_and_clears_the_asset() {
    render::ModelAsset asset{};
    asset.mesh.vertices.push_back(scene::MeshVertex{});
    asset.parts.push_back(render::ModelPart{});
    // Unknown extension.
    MODEL_CHECK(!render::load_model("model.fbx", asset));
    MODEL_CHECK(asset.mesh.vertices.empty());
    MODEL_CHECK(asset.parts.empty());
    // A missing file of a known extension.
    MODEL_CHECK(!render::load_model("no_such_model_12345.obj", asset));
    MODEL_CHECK(asset.mesh.indices.empty());
    // Empty source.
    MODEL_CHECK(!render::load_model("", asset));
}

void authored_lod_chain_is_atomic_and_uses_shared_ranges() {
    const auto high = write_temp("lod-high.obj", "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nf 1 3 4\n");
    static_cast<void>(write_temp("lod-low.obj", "v 0 0 0\nv 1 0 0\nv 1 1 0\nf 1 2 3\n"));
    static_cast<void>(write_temp("lod-high.obj.lod", "MSLOD 1\n128 \"lod-low.obj\"\n"));
    render::ModelAsset asset;
    MODEL_CHECK(render::load_model(high,asset));
    MODEL_CHECK(asset.lods.size()==2U);
    MODEL_CHECK(asset.lods[0U].part_count==1U);
    MODEL_CHECK(asset.lods[1U].first_part==1U);
    MODEL_CHECK(asset.lods[1U].max_pixels==128.0F);
    MODEL_CHECK(asset.parts[0U].index_count==6U);
    MODEL_CHECK(asset.parts[1U].first_index==6U);
    MODEL_CHECK(asset.parts[1U].index_count==3U);
    MODEL_CHECK(asset.mesh.indices[6U]>=6U);
    MODEL_CHECK(asset.vertex_metadata.size()==asset.mesh.vertices.size());
    render::ModelAsset base;
    MODEL_CHECK(render::load_model_base(high,base));
    MODEL_CHECK(base.mesh.indices.size()==6U);
    for (const auto invalid : {"MSLOD 2\n128 \"lod-low.obj\"\n",
         "MSLOD 1\n64 \"lod-low.obj\"\n128 \"lod-low.obj\"\n",
         "MSLOD 1\n128 \"missing.obj\"\n", "MSLOD 1\n0 \"lod-low.obj\"\n",
         "MSLOD 1\n128 \"lod-low.obj\" trailing\n", "MSLOD 1\n128 lod-low.obj\n", "MSLOD 1\n"}) {
        static_cast<void>(write_temp("lod-high.obj.lod",invalid));
        MODEL_CHECK(!render::load_model(high,asset));
        MODEL_CHECK(asset.mesh.vertices.empty() && asset.parts.empty() && asset.lods.empty());
    }
}


void gltf_extensions_preserve_factors_maps_and_refuse_unknowns() {
    std::vector<core::u8> bytes;
    for (const float v : {-1.0F,-1.0F,0.0F, 1.0F,-1.0F,0.0F, 0.0F,1.0F,0.0F,
                         0.0F,0.0F, 1.0F,0.0F, 0.5F,1.0F}) append_f32(bytes,v);
    write_temp_binary("extensions.bin", bytes);
    write_temp_tga("extensions.tga", {128U,192U,255U,128U});
    const std::string document=R"({"asset":{"version":"2.0"},
"extensionsUsed":["KHR_materials_clearcoat","KHR_materials_sheen","KHR_materials_transmission","KHR_materials_ior","KHR_materials_specular","KHR_materials_anisotropy"],
"buffers":[{"uri":"extensions.bin","byteLength":60}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],
"images":[{"uri":"extensions.tga"}],"textures":[{"source":0}],
"materials":[{"pbrMetallicRoughness":{"baseColorFactor":[0.8,0.7,0.6,0.35]},"extensions":{
"KHR_materials_clearcoat":{"clearcoatFactor":0.8,"clearcoatRoughnessFactor":0.25,"clearcoatTexture":{"index":0},"clearcoatRoughnessTexture":{"index":0},"clearcoatNormalTexture":{"index":0,"scale":0.6}},
"KHR_materials_sheen":{"sheenColorFactor":[0.7,0.2,0.1],"sheenRoughnessFactor":0.4,"sheenColorTexture":{"index":0},"sheenRoughnessTexture":{"index":0}},
"KHR_materials_transmission":{"transmissionFactor":0.65,"transmissionTexture":{"index":0}},
"KHR_materials_ior":{"ior":1.8},
"KHR_materials_specular":{"specularFactor":0.45,"specularColorFactor":[0.2,0.4,0.7],"specularTexture":{"index":0},"specularColorTexture":{"index":0}},
"KHR_materials_anisotropy":{"anisotropyStrength":0.9,"anisotropyRotation":-0.7,"anisotropyTexture":{"index":0}}
}}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"material":0}]}],
"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    const auto path=write_temp("extensions.gltf",document);
    render::ModelAsset asset; MODEL_CHECK(render::load_model(path,asset));
    const auto& m=asset.parts[0].material;
    MODEL_CHECK(near(m.base_alpha,0.35F)); MODEL_CHECK(near(m.clearcoat,0.8F));
    MODEL_CHECK(near(m.clearcoat_roughness,0.25F)); MODEL_CHECK(near(m.clearcoat_normal_scale,0.6F));
    MODEL_CHECK((m.sheen_color==scene::Vec3{0.7F,0.2F,0.1F})); MODEL_CHECK(near(m.sheen_roughness,0.4F));
    MODEL_CHECK(near(m.transmission,0.65F)); MODEL_CHECK(near(m.ior,1.8F)); MODEL_CHECK(near(m.specular,0.45F));
    MODEL_CHECK((m.specular_color==scene::Vec3{0.2F,0.4F,0.7F}));
    MODEL_CHECK(near(m.anisotropy_strength,0.9F)); MODEL_CHECK(near(m.anisotropy_rotation,-0.7F));
    MODEL_CHECK(asset.parts[0].clearcoat_texture==0U);
    MODEL_CHECK(asset.parts[0].clearcoat_roughness_texture==0U);
    MODEL_CHECK(asset.parts[0].clearcoat_normal_texture==0U);
    MODEL_CHECK(asset.parts[0].sheen_color_texture==0U);
    MODEL_CHECK(asset.parts[0].sheen_roughness_texture==0U);
    MODEL_CHECK(asset.parts[0].transmission_texture==0U);
    MODEL_CHECK(asset.parts[0].specular_texture==0U);
    MODEL_CHECK(asset.parts[0].specular_color_texture==0U);
    MODEL_CHECK(asset.parts[0].anisotropy_texture==0U);
    MODEL_CHECK(asset.vertex_metadata[0].has_tangent);
    for (const auto unsupported : {"KHR_materials_volume", "KHR_materials_iridescence", "VENDOR_unknown"}) {
        auto changed=document;
        const auto at=changed.find("KHR_materials_clearcoat");
        changed.replace(at,std::string{"KHR_materials_clearcoat"}.size(),unsupported);
        const auto invalid=write_temp("unsupported.gltf",changed);
        MODEL_CHECK(!render::load_model(invalid,asset));
        MODEL_CHECK(asset.diagnostic.find(unsupported)!=std::string::npos);
        MODEL_CHECK(asset.mesh.vertices.empty());
    }
    // A required extension must be checked even if omitted from extensionsUsed.
    auto required=document;
    required.insert(1U,"\"extensionsRequired\":[\"VENDOR_required\"],");
    MODEL_CHECK(!render::load_model(write_temp("required.gltf",required),asset));
    MODEL_CHECK(asset.diagnostic.find("VENDOR_required")!=std::string::npos);

    // Legacy material with white diffuse and dielectric specular. Gloss is
    // in alpha (linear), while specular RGB and diffuse RGB are sRGB.
    auto legacy = document;
    const auto begin = legacy.find("\"materials\"");
    const auto end = legacy.find("\"meshes\"", begin);
    legacy.replace(begin, end - begin, R"("materials":[{"alphaMode":"BLEND","extensions":{
"KHR_materials_pbrSpecularGlossiness":{"diffuseFactor":[1,1,1,0.5],
"specularFactor":[0.04,0.04,0.04],"glossinessFactor":0.8,
"specularGlossinessTexture":{"index":0}}}}],)");
    legacy.insert(1U, "\"extensionsRequired\":[\"KHR_materials_pbrSpecularGlossiness\"],");
    MODEL_CHECK(render::load_model(write_temp("legacy.gltf", legacy), asset));
    const auto& part = asset.parts[0];
    MODEL_CHECK(part.base_color_texture.has_value());
    MODEL_CHECK(part.metallic_roughness_texture.has_value());
    const auto& base = asset.textures[*part.base_color_texture];
    const auto& mr = asset.textures[*part.metallic_roughness_texture];
    MODEL_CHECK(base.rgba[3] == 128U);
    MODEL_CHECK(mr.rgba[1] == 153U);
    MODEL_CHECK(mr.rgba[2] == 0U);
    MODEL_CHECK(part.material.alpha_mode == scene::MaterialAlphaMode::Blend);
    MODEL_CHECK(render::prepare_model_mips(asset));
    const auto package = render::ingest_model(temp_path("legacy.gltf").string());
    MODEL_CHECK(package.diagnostic.empty());
    MODEL_CHECK(render::load_model_package(package.source, asset));
    MODEL_CHECK(asset.parts[0].base_color_texture.has_value());
    MODEL_CHECK(asset.parts[0].metallic_roughness_texture.has_value());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 7U> cases{{
    {"glTF extensions preserve authored factors and maps", &gltf_extensions_preserve_factors_maps_and_refuse_unknowns},
    {"authored LOD chains are atomic", &authored_lod_chain_is_atomic_and_uses_shared_ranges},
    {"an obj quad loads to geometry with normals and bounds",
     &an_obj_quad_loads_to_geometry_with_normals_and_bounds},
    {"obj material runs preserve pbr factors",
     &obj_material_runs_preserve_pbr_factors},
    {"gltf parts keep materials bounds and correct normals",
     &gltf_parts_keep_materials_bounds_and_correct_normals},
    {"gltf texture slots decode and fallback per primitive",
     &gltf_texture_slots_decode_and_fallback_per_primitive},
    {"an unloadable source is refused and clears the asset",
     &an_unloadable_source_is_refused_and_clears_the_asset},
}};

} // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        render::ModelAsset asset;
        if (!render::load_model(argv[1], asset)) {
            std::cerr << "Import failed: " << asset.diagnostic << '\n';
            return 1;
        }
        std::cout << "Imported " << asset.mesh.vertices.size() << " vertices, "
                  << asset.mesh.indices.size() / 3U << " triangles, "
                  << asset.parts.size() << " parts\n";
        const auto start = std::chrono::steady_clock::now();
        const bool prepared = render::compress_model_textures(asset);
        std::cout << "Texture preparation: " << std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count() << " seconds\n";
        return prepared ? 0 : 1;
    }
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
