#include "model_loader_detail.hpp"

#include <mediaserver/scene/math.hpp>

// ufbx is compiled in its own C library target; stb_image's implementation
// lives in media_source.cpp. Both headers are included with warnings silenced
// because they are not clean under this project's warnings-as-errors.
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
#include <stb_image.h>
#include <ufbx.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mediaserver::render::detail {
namespace {

namespace scene = mediaserver::scene;

// Same bounds the editor applies to an explicitly assigned material image.
constexpr int max_texture_side = 16'384;
constexpr core::u64 max_texture_pixels = 64U * 1024U * 1024U;
constexpr std::uintmax_t max_texture_file_bytes = 512U * 1024U * 1024U;

[[nodiscard]] bool indexable(std::size_t count) noexcept {
    return count <= std::numeric_limits<core::u32>::max();
}

[[nodiscard]] std::string text(const ufbx_string& value) {
    return std::string{value.data, value.length};
}

[[nodiscard]] std::filesystem::path utf8_path(const ufbx_string& value) {
    std::u8string bytes(value.length, u8'\0');
    std::transform(value.data, value.data + value.length, bytes.begin(),
                   [](char c) { return static_cast<char8_t>(c); });
    return std::filesystem::path{bytes};
}

[[nodiscard]] float unit(ufbx_real value) noexcept {
    const auto held = static_cast<float>(value);
    return std::isfinite(held) ? std::clamp(held, 0.0F, 1.0F) : 0.0F;
}

[[nodiscard]] scene::Vec3 unit3(const ufbx_vec3& value) noexcept {
    return {unit(value.x), unit(value.y), unit(value.z)};
}

[[nodiscard]] bool finite(scene::Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

[[nodiscard]] scene::Vec3 as_vec3(const ufbx_vec3& value) noexcept {
    return {static_cast<float>(value.x), static_cast<float>(value.y),
            static_cast<float>(value.z)};
}

[[nodiscard]] bool read_file(const std::filesystem::path& path,
                             std::vector<core::u8>& into) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return false;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0U) {
        return false;
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return false;
    }
    into.resize(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(into.data()),
               static_cast<std::streamsize>(into.size()));
    return static_cast<bool>(input);
}

[[nodiscard]] ModelTexture decode_texture(const core::u8* bytes,
                                          std::size_t size) {
    ModelTexture result{};
    if (bytes == nullptr || size == 0U ||
        size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return result;
    }
    const auto length = static_cast<int>(size);
    int width = 0;
    int height = 0;
    int channels = 0;
    if (stbi_info_from_memory(bytes, length, &width, &height, &channels) == 0 ||
        width <= 0 || height <= 0 || width > max_texture_side ||
        height > max_texture_side ||
        static_cast<core::u64>(width) * static_cast<core::u64>(height) >
            max_texture_pixels) {
        return result;
    }
    stbi_uc* const pixels =
        stbi_load_from_memory(bytes, length, &width, &height, &channels, 4);
    if (pixels == nullptr) {
        return result;
    }
    result.width = static_cast<core::u32>(width);
    result.height = static_cast<core::u32>(height);
    result.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) *
                                            static_cast<std::size_t>(height) *
                                            4U);
    stbi_image_free(pixels);
    return result;
}

struct VertexKey final {
    core::u32 position{};
    core::u32 normal{};
    core::u32 uv{};
    bool operator==(const VertexKey&) const = default;
};

struct VertexKeyHash final {
    [[nodiscard]] std::size_t operator()(const VertexKey& key) const noexcept {
        std::size_t hash = key.position;
        hash = hash * 0x9E3779B97F4A7C15ULL + key.normal;
        hash = hash * 0x9E3779B97F4A7C15ULL + key.uv;
        return hash;
    }
};

class FbxImport final {
public:
    FbxImport(const ufbx_scene& source, std::filesystem::path model_path,
              ModelAsset& into)
        : source_{source}, model_path_{std::move(model_path)}, into_{into} {}

    [[nodiscard]] bool run() {
        for (std::size_t i = 0U; i < source_.nodes.count; ++i) {
            const ufbx_node& node = *source_.nodes.data[i];
            if (node.mesh == nullptr || !node.visible ||
                !node.mesh->vertex_position.exists) {
                continue;
            }
            if (!append_instance(node, *node.mesh)) {
                if (into_.diagnostic.empty()) {
                    into_.diagnostic = "Invalid FBX mesh geometry";
                }
                return false;
            }
        }
        if (into_.mesh.vertices.empty() || into_.mesh.indices.empty() ||
            into_.parts.empty()) {
            into_.diagnostic = "The FBX file contains no visible polygon meshes";
            return false;
        }
        into_.bounds = ModelBounds{minimum_, maximum_};
        return true;
    }

private:
    // One image, possibly embedded, as an index into ModelAsset::textures.
    // Procedural and shader textures with no file layer keep the factor.
    [[nodiscard]] bool resolve_texture(const ufbx_texture* texture,
                                       std::optional<core::u32>& index,
                                       ModelSampler& sampler) {
        index.reset();
        sampler = {};
        if (texture == nullptr) {
            return true;
        }
        const ufbx_texture* file = texture;
        if (texture->type != UFBX_TEXTURE_FILE) {
            if (texture->file_textures.count == 0U) {
                return true;
            }
            file = texture->file_textures.data[0];
        }
        if (texture->has_uv_transform || file->has_uv_transform) {
            into_.diagnostic = "FBX texture \"" + text(file->name) +
                               "\" uses a UV transform, which is not supported";
            return false;
        }
        sampler.u = file->wrap_u == UFBX_WRAP_CLAMP ? ModelAddressMode::Clamp
                                                    : ModelAddressMode::Repeat;
        sampler.v = file->wrap_v == UFBX_WRAP_CLAMP ? ModelAddressMode::Clamp
                                                    : ModelAddressMode::Repeat;
        if (const auto cached = textures_.find(file->element_id);
            cached != textures_.end()) {
            index = cached->second;
            return true;
        }

        ModelTexture decoded{};
        if (file->content.size != 0U) {
            decoded = decode_texture(
                static_cast<const core::u8*>(file->content.data),
                file->content.size);
        } else {
            // Exported paths usually point at the artist's machine. Try the
            // path relative to the model, then as written, then the bare file
            // name beside the model, which covers a copied asset folder.
            // Network locations are never fetched.
            const auto directory = model_path_.parent_path();
            std::vector<std::filesystem::path> candidates;
            for (const ufbx_string* name :
                 {&file->relative_filename, &file->absolute_filename,
                  &file->filename}) {
                if (name->length == 0U ||
                    std::string_view{name->data, name->length}.find("://") !=
                        std::string_view::npos) {
                    continue;
                }
                const auto path = utf8_path(*name);
                candidates.push_back(path.is_absolute() ? path
                                                        : directory / path);
                if (path.has_filename()) {
                    candidates.push_back(directory / path.filename());
                }
            }
            std::vector<core::u8> bytes;
            for (const auto& candidate : candidates) {
                std::error_code error;
                if (std::filesystem::file_size(candidate, error) >
                        max_texture_file_bytes ||
                    error || !read_file(candidate, bytes)) {
                    continue;
                }
                decoded = decode_texture(bytes.data(), bytes.size());
                if (decoded.valid()) {
                    break;
                }
            }
        }
        if (!decoded.valid()) {
            const auto name = file->relative_filename.length != 0U
                                  ? file->relative_filename
                                  : file->absolute_filename.length != 0U
                                  ? file->absolute_filename
                                  : file->name;
            into_.diagnostic =
                "Cannot read FBX texture \"" + text(name) +
                "\": place it beside the model as PNG, JPEG, TGA or BMP "
                "within 16384 pixels";
            return false;
        }
        if (!indexable(into_.textures.size() + 1U)) {
            return false;
        }
        index = static_cast<core::u32>(into_.textures.size());
        into_.textures.push_back(std::move(decoded));
        textures_.emplace(file->element_id, *index);
        return true;
    }

    [[nodiscard]] static bool mapped(const ufbx_material_map& map) noexcept {
        return map.texture != nullptr && map.texture_enabled;
    }

    // The renderer samples roughness from green and metalness from blue of
    // one texture. Separate FBX maps are combined once per material here,
    // before mip generation, with their factors folded in.
    [[nodiscard]] bool bake_metallic_roughness(const ufbx_material& material,
                                               ModelPart& part) {
        const auto& pbr = material.pbr;
        const ufbx_texture* roughness_source =
            mapped(pbr.roughness) ? pbr.roughness.texture : nullptr;
        const bool glossy = roughness_source == nullptr && mapped(pbr.glossiness);
        if (glossy) {
            roughness_source = pbr.glossiness.texture;
        }
        const ufbx_texture* metal_source =
            mapped(pbr.metalness) ? pbr.metalness.texture : nullptr;
        if (roughness_source == nullptr && metal_source == nullptr) {
            return true;
        }
        std::optional<core::u32> roughness_index;
        std::optional<core::u32> metal_index;
        ModelSampler roughness_sampler{};
        ModelSampler metal_sampler{};
        if (!resolve_texture(roughness_source, roughness_index,
                             roughness_sampler) ||
            !resolve_texture(metal_source, metal_index, metal_sampler)) {
            return false;
        }
        if (!roughness_index && !metal_index) {
            return true;
        }
        const ModelTexture* roughness =
            roughness_index ? &into_.textures[*roughness_index] : nullptr;
        const ModelTexture* metal =
            metal_index ? &into_.textures[*metal_index] : nullptr;
        const core::u32 width = std::max(roughness ? roughness->width : 1U,
                                         metal ? metal->width : 1U);
        const core::u32 height = std::max(roughness ? roughness->height : 1U,
                                          metal ? metal->height : 1U);
        ModelTexture combined{
            width, height,
            std::vector<core::u8>(static_cast<std::size_t>(width) * height * 4U)};
        const float roughness_factor = part.material.roughness;
        const float metal_factor = part.material.metallic;
        const auto byte = [](float value) {
            return static_cast<core::u8>(std::clamp(value, 0.0F, 1.0F) * 255.0F +
                                         0.5F);
        };
        for (core::u32 y = 0U; y < height; ++y) {
            for (core::u32 x = 0U; x < width; ++x) {
                const auto sample = [&](const ModelTexture* texture) {
                    const auto sx = static_cast<core::u32>(
                        static_cast<core::u64>(x) * texture->width / width);
                    const auto sy = static_cast<core::u32>(
                        static_cast<core::u64>(y) * texture->height / height);
                    return static_cast<float>(
                               texture->rgba[(static_cast<std::size_t>(sy) *
                                                  texture->width +
                                              sx) *
                                             4U]) /
                           255.0F;
                };
                float rough = roughness_factor;
                if (roughness != nullptr) {
                    rough = glossy ? 1.0F - sample(roughness)
                                   : roughness_factor * sample(roughness);
                }
                const float metallic =
                    metal != nullptr ? metal_factor * sample(metal) : metal_factor;
                const auto offset =
                    (static_cast<std::size_t>(y) * width + x) * 4U;
                combined.rgba[offset] = 255U;
                combined.rgba[offset + 1U] = byte(rough);
                combined.rgba[offset + 2U] = byte(metallic);
                combined.rgba[offset + 3U] = 255U;
            }
        }
        part.material.roughness = 1.0F;
        part.material.metallic = 1.0F;
        part.samplers[1] = roughness_index ? roughness_sampler : metal_sampler;
        part.metallic_roughness_texture =
            static_cast<core::u32>(into_.textures.size());
        into_.textures.push_back(std::move(combined));
        return true;
    }

    // Material factors and texture slots, shared by every run that uses the
    // material. ufbx presents legacy Lambert/Phong and modern PBR shaders
    // (Stingray, Arnold, 3ds Max Physical, ...) through one set of PBR maps.
    [[nodiscard]] bool convert_material(const ufbx_material& material,
                                        ModelPart& part) {
        const auto& pbr = material.pbr;
        scene::Material& result = part.material;
        const float base_factor = pbr.base_factor.has_value
                                      ? unit(pbr.base_factor.value_real)
                                      : 1.0F;
        if (pbr.base_color.has_value) {
            result.base_color = unit3(pbr.base_color.value_vec3) * base_factor;
        }
        if (pbr.roughness.has_value) {
            result.roughness = unit(pbr.roughness.value_real);
        }
        if (pbr.metalness.has_value) {
            result.metallic = unit(pbr.metalness.value_real);
        }
        if (pbr.emission_color.has_value) {
            result.emissive = unit3(pbr.emission_color.value_vec3);
            if (pbr.emission_factor.has_value) {
                const auto factor = static_cast<float>(pbr.emission_factor.value_real);
                result.emissive_strength =
                    std::isfinite(factor) ? std::max(factor, 0.0F) : 0.0F;
            }
        }
        if (pbr.opacity.has_value && !mapped(pbr.opacity)) {
            result.base_alpha = unit(pbr.opacity.value_real);
            if (result.base_alpha < 1.0F) {
                result.alpha_mode = scene::MaterialAlphaMode::Blend;
            }
        }
        result.double_sided = material.features.double_sided.enabled;

        // A connected map replaces the swatch colour in the authoring tools,
        // so the colour no longer tints it; the scalar factor still applies.
        if (mapped(pbr.base_color)) {
            if (!resolve_texture(pbr.base_color.texture, part.base_color_texture,
                                 part.samplers[0])) {
                return false;
            }
            if (part.base_color_texture) {
                result.base_color = {base_factor, base_factor, base_factor};
            }
        }
        if (mapped(pbr.normal_map) &&
            !resolve_texture(pbr.normal_map.texture, part.normal_texture,
                             part.samplers[2])) {
            return false;
        }
        if (mapped(pbr.ambient_occlusion) &&
            !resolve_texture(pbr.ambient_occlusion.texture,
                             part.occlusion_texture, part.samplers[3])) {
            return false;
        }
        if (mapped(pbr.emission_color)) {
            if (!resolve_texture(pbr.emission_color.texture,
                                 part.emissive_texture, part.samplers[4])) {
                return false;
            }
            if (part.emissive_texture) {
                result.emissive = {1.0F, 1.0F, 1.0F};
            }
        }
        // Legacy shaders route a shininess exponent through the roughness map;
        // its texture is not a roughness image and keeps the derived factor.
        return !material.features.pbr.enabled ||
               bake_metallic_roughness(material, part);
    }

    [[nodiscard]] bool material_part(const ufbx_material* material,
                                     ModelPart& part) {
        part = ModelPart{};
        if (material == nullptr) {
            return true;
        }
        if (const auto cached = materials_.find(material->element_id);
            cached != materials_.end()) {
            part = cached->second;
            return true;
        }
        part.has_material = true;
        if (!convert_material(*material, part)) {
            return false;
        }
        materials_.emplace(material->element_id, part);
        return true;
    }

    void include(scene::Vec3 point) noexcept {
        minimum_ = {std::min(minimum_.x, point.x), std::min(minimum_.y, point.y),
                    std::min(minimum_.z, point.z)};
        maximum_ = {std::max(maximum_.x, point.x), std::max(maximum_.y, point.y),
                    std::max(maximum_.z, point.z)};
    }

    [[nodiscard]] bool emit(const scene::MeshVertex& vertex, bool has_uv0) {
        if (!finite(vertex.position) || !finite(vertex.normal) ||
            !indexable(into_.mesh.vertices.size() + 1U)) {
            return false;
        }
        include(vertex.position);
        into_.mesh.indices.push_back(
            static_cast<core::u32>(into_.mesh.vertices.size()));
        into_.mesh.vertices.push_back(vertex);
        into_.vertex_metadata.push_back(ModelVertexMetadata{has_uv0});
        return true;
    }

    [[nodiscard]] bool append_instance(const ufbx_node& node,
                                       const ufbx_mesh& mesh) {
        const ufbx_matrix& world = node.geometry_to_world;
        const ufbx_matrix normal_matrix = ufbx_matrix_for_normals(&world);
        // A mirroring transform turns counter-clockwise faces clockwise.
        const bool mirrored = ufbx_matrix_determinant(&world) < 0.0;
        const bool has_normals = mesh.vertex_normal.exists;
        const bool has_uv0 = mesh.vertex_uv.exists;
        const ufbx_material_list& materials =
            node.materials.count != 0U ? node.materials : mesh.materials;
        std::vector<core::u32> triangles(std::max<std::size_t>(
            mesh.max_face_triangles * 3U, 3U));

        const auto vertex_at = [&](core::u32 corner) {
            scene::MeshVertex vertex{};
            vertex.position = as_vec3(ufbx_transform_position(
                &world, ufbx_get_vertex_vec3(&mesh.vertex_position, corner)));
            if (has_normals) {
                vertex.normal = scene::normalize(
                    as_vec3(ufbx_transform_direction(
                        &normal_matrix,
                        ufbx_get_vertex_vec3(&mesh.vertex_normal, corner))),
                    scene::Vec3{0.0F, 1.0F, 0.0F});
            }
            if (has_uv0) {
                const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh.vertex_uv, corner);
                vertex.u = static_cast<float>(uv.x);
                // FBX texture space is bottom-up, as in OBJ.
                vertex.v = 1.0F - static_cast<float>(uv.y);
            }
            return vertex;
        };

        for (std::size_t part_index = 0U;
             part_index < mesh.material_parts.count; ++part_index) {
            const ufbx_mesh_part& source = mesh.material_parts.data[part_index];
            if (source.num_triangles == 0U) {
                continue;
            }
            ModelPart part{};
            if (!material_part(part_index < materials.count
                                   ? materials.data[part_index]
                                   : nullptr,
                               part)) {
                return false;
            }
            const bool textured =
                part.base_color_texture || part.metallic_roughness_texture ||
                part.normal_texture || part.occlusion_texture ||
                part.emissive_texture;
            if (textured && !has_uv0) {
                into_.diagnostic = "FBX mesh \"" + text(node.name) +
                                   "\" has a textured material but no UV map";
                return false;
            }
            if (!indexable(into_.mesh.indices.size() + source.num_triangles * 3U)) {
                return false;
            }
            part.first_index = static_cast<core::u32>(into_.mesh.indices.size());

            // Corners that share position, normal and UV share one vertex, so
            // an indexed export stays indexed; projection meshes depend on it.
            std::unordered_map<VertexKey, core::u32, VertexKeyHash> shared;
            for (std::size_t f = 0U; f < source.face_indices.count; ++f) {
                const ufbx_face face = mesh.faces.data[source.face_indices.data[f]];
                if (face.num_indices < 3U) {
                    continue;
                }
                const core::u32 count = ufbx_triangulate_face(
                    triangles.data(), triangles.size(), &mesh, face);
                for (core::u32 t = 0U; t < count; ++t) {
                    std::array<core::u32, 3U> corners{
                        triangles[t * 3U], triangles[t * 3U + 1U],
                        triangles[t * 3U + 2U]};
                    if (mirrored) {
                        std::swap(corners[1U], corners[2U]);
                    }
                    if (!has_normals) {
                        std::array<scene::MeshVertex, 3U> flat{
                            vertex_at(corners[0U]), vertex_at(corners[1U]),
                            vertex_at(corners[2U])};
                        const auto normal = scene::normalize(
                            scene::cross(flat[1U].position - flat[0U].position,
                                         flat[2U].position - flat[0U].position),
                            scene::Vec3{0.0F, 1.0F, 0.0F});
                        for (auto& vertex : flat) {
                            vertex.normal = normal;
                            if (!emit(vertex, has_uv0)) {
                                return false;
                            }
                        }
                        continue;
                    }
                    for (const core::u32 corner : corners) {
                        const VertexKey key{
                            mesh.vertex_position.indices.data[corner],
                            mesh.vertex_normal.indices.data[corner],
                            has_uv0 ? mesh.vertex_uv.indices.data[corner] : 0U};
                        if (const auto found = shared.find(key);
                            found != shared.end()) {
                            into_.mesh.indices.push_back(found->second);
                            continue;
                        }
                        const auto index =
                            static_cast<core::u32>(into_.mesh.vertices.size());
                        if (!emit(vertex_at(corner), has_uv0)) {
                            return false;
                        }
                        shared.emplace(key, index);
                    }
                }
            }
            part.index_count =
                static_cast<core::u32>(into_.mesh.indices.size()) - part.first_index;
            if (part.index_count == 0U) {
                continue;
            }
            if (part.normal_texture &&
                !generate_model_tangents(into_, part.first_index,
                                         part.index_count)) {
                into_.diagnostic = "Cannot generate material tangent frame";
                return false;
            }
            into_.parts.push_back(std::move(part));
        }
        return true;
    }

    const ufbx_scene& source_;
    std::filesystem::path model_path_;
    ModelAsset& into_;
    std::map<core::u32, core::u32> textures_;
    std::map<core::u32, ModelPart> materials_;
    scene::Vec3 minimum_{std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max()};
    scene::Vec3 maximum_{std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::lowest()};
};

} // namespace

bool load_fbx(const std::string& path, ModelAsset& into) {
    const std::filesystem::path model_path{path};
    std::vector<core::u8> bytes;
    if (!read_file(model_path, bytes)) {
        return false;
    }

    ufbx_load_opts options{};
    options.file_format = UFBX_FILE_FORMAT_FBX;
    options.no_format_from_content = true;
    options.no_format_from_extension = true;
    // Match the glTF convention the rest of the model pipeline uses. Baking
    // the conversion into geometry keeps every node transform in that space.
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0;
    options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    // Geometric (pivot) offsets are folded into node.geometry_to_world.
    options.geometry_transform_handling =
        UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    // Animation is not imported; nodes keep their authored rest transforms.
    options.ignore_animation = true;
    options.filename = ufbx_string{path.c_str(), path.size()};

    ufbx_error error{};
    ufbx_scene* const loaded =
        ufbx_load_memory(bytes.data(), bytes.size(), &options, &error);
    if (loaded == nullptr) {
        into.diagnostic = "Cannot read FBX file: " + text(error.description);
        return false;
    }
    struct Guard final {
        ufbx_scene* scene;
        ~Guard() { ufbx_free_scene(scene); }
    } guard{loaded};
    bytes = {};

    return FbxImport{*loaded, model_path, into}.run();
}

} // namespace mediaserver::render::detail
