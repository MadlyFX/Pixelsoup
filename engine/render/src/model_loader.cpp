#include <mediaserver/render/model_loader.hpp>

#include <mediaserver/scene/math.hpp>

// cgltf and tiny_obj_loader are single-header libraries; this is the one
// translation unit that compiles their implementations. stb_image's
// implementation remains in media_source.cpp and is only declared here. The
// headers are not clean under this project's warnings-as-errors, so they are
// included with warnings silenced. The code below stays fully checked.
#define CGLTF_IMPLEMENTATION
#define TINYOBJLOADER_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include <cgltf.h>
#include <stb_image.h>
#include <tiny_obj_loader.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mediaserver::render {
namespace {

namespace scene = mediaserver::scene;

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) {
    if (suffix.size() > text.size()) {
        return false;
    }
    auto lower = [](char c) {
        return static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    };
    for (std::size_t i = 0U; i < suffix.size(); ++i) {
        if (lower(text[text.size() - suffix.size() + i]) != lower(suffix[i])) {
            return false;
        }
    }
    return true;
}

struct BoundsBuilder final {
    scene::Vec3 minimum{std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max()};
    scene::Vec3 maximum{std::numeric_limits<float>::lowest(),
                        std::numeric_limits<float>::lowest(),
                        std::numeric_limits<float>::lowest()};

    void include(scene::Vec3 point) noexcept {
        minimum.x = std::min(minimum.x, point.x);
        minimum.y = std::min(minimum.y, point.y);
        minimum.z = std::min(minimum.z, point.z);
        maximum.x = std::max(maximum.x, point.x);
        maximum.y = std::max(maximum.y, point.y);
        maximum.z = std::max(maximum.z, point.z);
    }

    [[nodiscard]] ModelBounds finish() const noexcept {
        return ModelBounds{minimum, maximum};
    }
};

[[nodiscard]] bool indexable(std::size_t count) noexcept {
    return count <= std::numeric_limits<core::u32>::max();
}

[[nodiscard]] int base64_value(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '+') {
        return 62;
    }
    return value == '/' ? 63 : -1;
}

[[nodiscard]] bool decode_base64(std::string_view encoded,
                                 std::vector<core::u8>& into) {
    into.clear();
    into.reserve((encoded.size() / 4U) * 3U);
    core::u32 held = 0U;
    unsigned held_bits = 0U;
    bool padded = false;
    for (const char character : encoded) {
        if (character == '=') {
            padded = true;
            continue;
        }
        if (character == ' ' || character == '\t' || character == '\r' ||
            character == '\n') {
            continue;
        }
        if (padded) {
            return false;
        }
        const int decoded = base64_value(character);
        if (decoded < 0) {
            return false;
        }
        held = (held << 6U) | static_cast<core::u32>(decoded);
        held_bits += 6U;
        if (held_bits >= 8U) {
            held_bits -= 8U;
            into.push_back(static_cast<core::u8>(held >> held_bits));
            held &= held_bits == 0U ? 0U : (1U << held_bits) - 1U;
        }
    }
    // Base64 may leave two or four padding bits, but never a complete orphaned
    // sextet. Rejecting that malformed tail keeps bad data deterministic.
    return held_bits != 6U;
}

[[nodiscard]] ModelTexture decoded_texture(stbi_uc* pixels, int width,
                                           int height) {
    ModelTexture result{};
    if (pixels == nullptr || width <= 0 || height <= 0) {
        if (pixels != nullptr) {
            stbi_image_free(pixels);
        }
        return result;
    }
    const auto held_width = static_cast<std::size_t>(width);
    const auto held_height = static_cast<std::size_t>(height);
    if (held_width >
        std::numeric_limits<std::size_t>::max() / held_height / 4U) {
        stbi_image_free(pixels);
        return result;
    }
    const auto byte_count = held_width * held_height * 4U;
    result.width = static_cast<core::u32>(width);
    result.height = static_cast<core::u32>(height);
    result.rgba.assign(pixels, pixels + byte_count);
    stbi_image_free(pixels);
    return result;
}

[[nodiscard]] ModelTexture decode_image_memory(const core::u8* bytes,
                                               std::size_t size) {
    if (bytes == nullptr || size == 0U ||
        size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return ModelTexture{};
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* const pixels = stbi_load_from_memory(
        bytes, static_cast<int>(size), &width, &height, &channels, 4);
    return decoded_texture(pixels, width, height);
}

[[nodiscard]] ModelTexture
decode_gltf_image(const std::filesystem::path& model_path,
                  const cgltf_image& image) {
    if (image.buffer_view != nullptr) {
        const auto* const bytes = cgltf_buffer_view_data(image.buffer_view);
        return decode_image_memory(bytes, image.buffer_view->size);
    }
    if (image.uri == nullptr || *image.uri == '\0') {
        return ModelTexture{};
    }

    const std::string_view uri{image.uri};
    if (uri.starts_with("data:")) {
        const auto comma = uri.find(',');
        if (comma == std::string_view::npos ||
            uri.substr(0U, comma).find(";base64") == std::string_view::npos) {
            return ModelTexture{};
        }
        std::vector<core::u8> decoded;
        if (!decode_base64(uri.substr(comma + 1U), decoded)) {
            return ModelTexture{};
        }
        return decode_image_memory(decoded.data(), decoded.size());
    }
    // Network/file schemes are deliberately not fetched by a render node.
    // Relative and absolute filesystem paths remain deterministic local assets.
    if (uri.find("://") != std::string_view::npos) {
        return ModelTexture{};
    }
    std::string decoded_uri{uri};
    decoded_uri.resize(cgltf_decode_uri(decoded_uri.data()));
    const auto path =
        std::filesystem::path{decoded_uri}.is_absolute()
            ? std::filesystem::path{decoded_uri}
            : model_path.parent_path() / std::filesystem::path{decoded_uri};
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* const pixels =
        stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    return decoded_texture(pixels, width, height);
}

void append_vertex(ModelAsset& asset, BoundsBuilder& bounds,
                   const scene::MeshVertex& vertex, bool has_uv0) {
    bounds.include(vertex.position);
    asset.mesh.vertices.push_back(vertex);
    asset.vertex_metadata.push_back(ModelVertexMetadata{has_uv0});
}

[[nodiscard]] scene::Mat4 as_matrix(const cgltf_float values[16]) noexcept {
    scene::Mat4 result{};
    std::copy_n(values, result.m.size(), result.m.begin());
    return result;
}

[[nodiscard]] scene::Vec3 transform_normal(const scene::Mat4& normal_matrix,
                                           scene::Vec3 normal) noexcept {
    const auto transformed = scene::transform(
        normal_matrix, scene::Vec4{normal.x, normal.y, normal.z, 0.0F});
    return scene::normalize(
        scene::Vec3{transformed.x, transformed.y, transformed.z},
        scene::Vec3{0.0F, 1.0F, 0.0F});
}

[[nodiscard]] scene::Material
gltf_material(const cgltf_material& source) noexcept {
    // glTF's metallic-roughness defaults are white, fully rough, and fully
    // metallic. cgltf only fills its factor block when that workflow is
    // present, so establish the specification defaults first.
    scene::Material result{scene::Vec3{1.0F, 1.0F, 1.0F}, 1.0F, 1.0F,
                           scene::Vec3{0.0F, 0.0F, 0.0F}};
    if (source.has_pbr_metallic_roughness != 0) {
        const auto& pbr = source.pbr_metallic_roughness;
        result.base_color =
            scene::Vec3{pbr.base_color_factor[0], pbr.base_color_factor[1],
                        pbr.base_color_factor[2]};
        result.roughness = pbr.roughness_factor;
        result.metallic = pbr.metallic_factor;
    }
    const float emissive_strength =
        source.has_emissive_strength != 0
            ? source.emissive_strength.emissive_strength
            : 1.0F;
    result.emissive =
        scene::Vec3{source.emissive_factor[0] * emissive_strength,
                    source.emissive_factor[1] * emissive_strength,
                    source.emissive_factor[2] * emissive_strength};
    return result;
}

[[nodiscard]] std::optional<core::u32>
gltf_texture_index(const cgltf_texture_view& view, const cgltf_data& data,
                   const ModelAsset& asset) noexcept {
    // This slice intentionally consumes TEXCOORD_0 directly. A transformed or
    // alternate UV set needs either another vertex stream or per-material UV
    // constants; until then its factor-only fallback is stable and explicit.
    if (view.texture == nullptr || view.texcoord != 0 ||
        view.has_transform != 0 || view.texture->image == nullptr) {
        return std::nullopt;
    }
    const cgltf_size index = cgltf_image_index(&data, view.texture->image);
    if (index >= asset.textures.size() ||
        index > std::numeric_limits<core::u32>::max() ||
        !asset.textures[index].valid()) {
        return std::nullopt;
    }
    return static_cast<core::u32>(index);
}

[[nodiscard]] scene::Material
obj_material(const tinyobj::material_t& source) noexcept {
    return scene::Material{
        scene::Vec3{source.diffuse[0], source.diffuse[1], source.diffuse[2]},
        source.roughness, source.metallic,
        scene::Vec3{source.emission[0], source.emission[1],
                    source.emission[2]}};
}

[[nodiscard]] const cgltf_accessor*
find_attribute(const cgltf_primitive& primitive, cgltf_attribute_type type,
               cgltf_int index = 0) noexcept {
    for (cgltf_size i = 0U; i < primitive.attributes_count; ++i) {
        const auto& attribute = primitive.attributes[i];
        if (attribute.type == type && attribute.index == index) {
            return attribute.data;
        }
    }
    return nullptr;
}

[[nodiscard]] bool read_vec3(const cgltf_accessor& accessor, cgltf_size index,
                             scene::Vec3& into) noexcept {
    cgltf_float value[3]{};
    if (cgltf_accessor_read_float(&accessor, index, value, 3U) == 0) {
        return false;
    }
    into = scene::Vec3{value[0], value[1], value[2]};
    return true;
}

[[nodiscard]] bool read_uv(const cgltf_accessor* accessor, cgltf_size index,
                           scene::MeshVertex& into) noexcept {
    if (accessor == nullptr) {
        return true;
    }
    cgltf_float value[2]{};
    if (cgltf_accessor_read_float(accessor, index, value, 2U) == 0) {
        return false;
    }
    into.u = value[0];
    into.v = value[1];
    return true;
}

[[nodiscard]] cgltf_size source_index(const cgltf_primitive& primitive,
                                      cgltf_size index) noexcept {
    return primitive.indices != nullptr
               ? cgltf_accessor_read_index(primitive.indices, index)
               : index;
}

[[nodiscard]] bool append_gltf_primitive(const cgltf_primitive& primitive,
                                         const cgltf_data& data,
                                         const scene::Mat4& world,
                                         ModelAsset& asset,
                                         BoundsBuilder& bounds) {
    const auto* const positions =
        find_attribute(primitive, cgltf_attribute_type_position);
    const auto* const normals =
        find_attribute(primitive, cgltf_attribute_type_normal);
    const auto* const uvs =
        find_attribute(primitive, cgltf_attribute_type_texcoord);
    if (positions == nullptr) {
        return true;
    }
    if ((normals != nullptr && normals->count != positions->count) ||
        (uvs != nullptr && uvs->count != positions->count)) {
        return false;
    }

    const cgltf_size index_count = primitive.indices != nullptr
                                       ? primitive.indices->count
                                       : positions->count;
    if (index_count == 0U || index_count % 3U != 0U) {
        return false;
    }
    if (!indexable(asset.mesh.indices.size() + index_count)) {
        return false;
    }

    const auto first_index = static_cast<core::u32>(asset.mesh.indices.size());
    const auto normal_matrix = scene::normal_basis(world);

    if (normals != nullptr) {
        if (!indexable(asset.mesh.vertices.size() + positions->count)) {
            return false;
        }
        const auto base_vertex =
            static_cast<core::u32>(asset.mesh.vertices.size());
        for (cgltf_size vertex_index = 0U; vertex_index < positions->count;
             ++vertex_index) {
            scene::MeshVertex vertex{};
            scene::Vec3 position{};
            scene::Vec3 normal{};
            if (!read_vec3(*positions, vertex_index, position) ||
                !read_vec3(*normals, vertex_index, normal) ||
                !read_uv(uvs, vertex_index, vertex)) {
                return false;
            }
            vertex.position = scene::transform_point(world, position);
            vertex.normal = transform_normal(normal_matrix, normal);
            append_vertex(asset, bounds, vertex, uvs != nullptr);
        }
        for (cgltf_size index = 0U; index < index_count; ++index) {
            const auto local = source_index(primitive, index);
            if (local >= positions->count ||
                local > std::numeric_limits<core::u32>::max()) {
                return false;
            }
            asset.mesh.indices.push_back(base_vertex +
                                         static_cast<core::u32>(local));
        }
    } else {
        // Expand only this primitive. Generating normals for the whole asset
        // would overwrite normals supplied by a neighbouring primitive, while
        // retaining shared vertices would make flat normals order-dependent.
        if (!indexable(asset.mesh.vertices.size() + index_count)) {
            return false;
        }
        for (cgltf_size triangle = 0U; triangle < index_count; triangle += 3U) {
            std::array<scene::MeshVertex, 3U> vertices{};
            for (cgltf_size corner = 0U; corner < 3U; ++corner) {
                const auto local = source_index(primitive, triangle + corner);
                if (local >= positions->count) {
                    return false;
                }
                scene::Vec3 position{};
                if (!read_vec3(*positions, local, position) ||
                    !read_uv(uvs, local, vertices[corner])) {
                    return false;
                }
                vertices[corner].position =
                    scene::transform_point(world, position);
            }
            const auto face_normal = scene::normalize(
                scene::cross(vertices[1U].position - vertices[0U].position,
                             vertices[2U].position - vertices[0U].position),
                scene::Vec3{0.0F, 1.0F, 0.0F});
            for (auto& vertex : vertices) {
                vertex.normal = face_normal;
                asset.mesh.indices.push_back(
                    static_cast<core::u32>(asset.mesh.vertices.size()));
                append_vertex(asset, bounds, vertex, uvs != nullptr);
            }
        }
    }

    ModelPart part{};
    part.first_index = first_index;
    part.index_count =
        static_cast<core::u32>(asset.mesh.indices.size()) - first_index;
    part.has_material = primitive.material != nullptr;
    if (primitive.material != nullptr) {
        const cgltf_material& material = *primitive.material;
        part.material = gltf_material(material);
        if (material.has_pbr_metallic_roughness != 0) {
            part.base_color_texture = gltf_texture_index(
                material.pbr_metallic_roughness.base_color_texture, data,
                asset);
            part.metallic_roughness_texture = gltf_texture_index(
                material.pbr_metallic_roughness.metallic_roughness_texture,
                data, asset);
        }
        part.emissive_texture =
            gltf_texture_index(material.emissive_texture, data, asset);
    }
    asset.parts.push_back(part);
    return true;
}

[[nodiscard]] bool load_gltf(const std::string& path, ModelAsset& into) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) !=
        cgltf_result_success) {
        return false;
    }
    struct Guard final {
        cgltf_data* data;
        ~Guard() { cgltf_free(data); }
    } guard{data};
    if (cgltf_load_buffers(&options, data, path.c_str()) !=
            cgltf_result_success ||
        cgltf_validate(data) != cgltf_result_success) {
        return false;
    }

    into.textures.reserve(data->images_count);
    const std::filesystem::path model_path{path};
    for (cgltf_size image_index = 0U; image_index < data->images_count;
         ++image_index) {
        into.textures.push_back(
            decode_gltf_image(model_path, data->images[image_index]));
    }

    BoundsBuilder bounds{};
    for (cgltf_size node_index = 0U; node_index < data->nodes_count;
         ++node_index) {
        const auto& node = data->nodes[node_index];
        if (node.mesh == nullptr) {
            continue;
        }
        cgltf_float world_values[16]{};
        cgltf_node_transform_world(&node, world_values);
        const auto world = as_matrix(world_values);
        for (cgltf_size primitive_index = 0U;
             primitive_index < node.mesh->primitives_count; ++primitive_index) {
            const auto& primitive = node.mesh->primitives[primitive_index];
            if (primitive.type != cgltf_primitive_type_triangles) {
                continue;
            }
            if (!append_gltf_primitive(primitive, *data, world, into, bounds)) {
                return false;
            }
        }
    }

    if (into.mesh.vertices.empty() || into.mesh.indices.empty() ||
        into.parts.empty()) {
        return false;
    }
    into.bounds = bounds.finish();
    return true;
}

[[nodiscard]] bool valid_obj_vertex(const tinyobj::attrib_t& attrib,
                                    const tinyobj::index_t& index) noexcept {
    if (index.vertex_index < 0 || attrib.vertices.size() < 3U) {
        return false;
    }
    const auto vertex = static_cast<std::size_t>(index.vertex_index);
    return vertex <= (attrib.vertices.size() - 3U) / 3U;
}

[[nodiscard]] bool append_obj_vertex(const tinyobj::attrib_t& attrib,
                                     const tinyobj::index_t& index,
                                     scene::MeshVertex& vertex,
                                     bool& has_normal, bool& has_uv0) noexcept {
    if (!valid_obj_vertex(attrib, index)) {
        return false;
    }
    const auto position = static_cast<std::size_t>(index.vertex_index) * 3U;
    vertex.position =
        scene::Vec3{attrib.vertices[position], attrib.vertices[position + 1U],
                    attrib.vertices[position + 2U]};

    has_normal = index.normal_index >= 0;
    if (has_normal) {
        const auto normal = static_cast<std::size_t>(index.normal_index) * 3U;
        if (normal + 2U >= attrib.normals.size()) {
            return false;
        }
        vertex.normal = scene::normalize(
            scene::Vec3{attrib.normals[normal], attrib.normals[normal + 1U],
                        attrib.normals[normal + 2U]},
            scene::Vec3{0.0F, 1.0F, 0.0F});
    }
    has_uv0 = index.texcoord_index >= 0;
    if (has_uv0) {
        const auto texcoord =
            static_cast<std::size_t>(index.texcoord_index) * 2U;
        if (texcoord + 1U >= attrib.texcoords.size()) {
            return false;
        }
        vertex.u = attrib.texcoords[texcoord];
        // OBJ texture space is bottom-up; flip to the top-left origin the rest
        // of the pipeline uses.
        vertex.v = 1.0F - attrib.texcoords[texcoord + 1U];
    }
    return true;
}

[[nodiscard]] bool load_obj(const std::string& path, ModelAsset& into) {
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;
    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, config)) {
        return false;
    }

    const auto& attrib = reader.GetAttrib();
    const auto& materials = reader.GetMaterials();
    BoundsBuilder bounds{};
    bool part_open = false;
    int current_material = -1;
    ModelPart part{};

    const auto close_part = [&]() {
        if (!part_open) {
            return;
        }
        part.index_count =
            static_cast<core::u32>(into.mesh.indices.size()) - part.first_index;
        if (part.index_count != 0U) {
            into.parts.push_back(part);
        }
        part_open = false;
    };

    for (const auto& shape : reader.GetShapes()) {
        std::size_t shape_index = 0U;
        for (std::size_t face = 0U; face < shape.mesh.num_face_vertices.size();
             ++face) {
            const auto face_vertices =
                static_cast<std::size_t>(shape.mesh.num_face_vertices[face]);
            if (face_vertices != 3U ||
                shape_index + face_vertices > shape.mesh.indices.size()) {
                return false;
            }
            const int material_id = face < shape.mesh.material_ids.size()
                                        ? shape.mesh.material_ids[face]
                                        : -1;
            if (!part_open || material_id != current_material) {
                close_part();
                current_material = material_id;
                part = ModelPart{};
                part.first_index =
                    static_cast<core::u32>(into.mesh.indices.size());
                if (material_id >= 0 &&
                    static_cast<std::size_t>(material_id) < materials.size()) {
                    part.has_material = true;
                    part.material = obj_material(
                        materials[static_cast<std::size_t>(material_id)]);
                }
                part_open = true;
            }
            if (!indexable(into.mesh.vertices.size() + 3U) ||
                !indexable(into.mesh.indices.size() + 3U)) {
                return false;
            }

            std::array<scene::MeshVertex, 3U> vertices{};
            std::array<bool, 3U> has_normals{};
            std::array<bool, 3U> has_uv0{};
            for (std::size_t corner = 0U; corner < 3U; ++corner) {
                if (!append_obj_vertex(attrib,
                                       shape.mesh.indices[shape_index + corner],
                                       vertices[corner], has_normals[corner],
                                       has_uv0[corner])) {
                    return false;
                }
            }
            const auto face_normal = scene::normalize(
                scene::cross(vertices[1U].position - vertices[0U].position,
                             vertices[2U].position - vertices[0U].position),
                scene::Vec3{0.0F, 1.0F, 0.0F});
            for (std::size_t corner = 0U; corner < 3U; ++corner) {
                if (!has_normals[corner]) {
                    vertices[corner].normal = face_normal;
                }
                into.mesh.indices.push_back(
                    static_cast<core::u32>(into.mesh.vertices.size()));
                append_vertex(into, bounds, vertices[corner], has_uv0[corner]);
            }
            shape_index += face_vertices;
        }
        if (shape_index != shape.mesh.indices.size()) {
            return false;
        }
    }
    close_part();

    if (into.mesh.vertices.empty() || into.mesh.indices.empty() ||
        into.parts.empty()) {
        return false;
    }
    into.bounds = bounds.finish();
    return true;
}

} // namespace

bool load_model(const std::string& source, ModelAsset& into) {
    into = ModelAsset{};
    bool loaded = false;
    if (ends_with(source, ".gltf") || ends_with(source, ".glb")) {
        loaded = load_gltf(source, into);
    } else if (ends_with(source, ".obj")) {
        loaded = load_obj(source, into);
    }
    if (!loaded) {
        into = ModelAsset{};
    }
    return loaded;
}

} // namespace mediaserver::render
