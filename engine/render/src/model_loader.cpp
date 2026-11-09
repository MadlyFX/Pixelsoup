#include <mediaserver/render/model_loader.hpp>
#include <mediaserver/render/model_package.hpp>

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
#include <mikktspace.h>
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
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "model_animation_import.hpp"
#include "model_loader_detail.hpp"

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
                   const scene::MeshVertex& vertex, bool has_uv0,
                   std::optional<scene::Vec4> tangent = std::nullopt) {
    bounds.include(vertex.position);
    asset.mesh.vertices.push_back(vertex);
    asset.vertex_metadata.push_back(ModelVertexMetadata{
        has_uv0, tangent.has_value(), tangent.value_or(scene::Vec4{})});
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
        result.base_alpha = pbr.base_color_factor[3];
    }
    result.emissive_strength =
        source.has_emissive_strength != 0
            ? source.emissive_strength.emissive_strength
            : 1.0F;
    result.emissive =
        scene::Vec3{source.emissive_factor[0], source.emissive_factor[1],
                    source.emissive_factor[2]};
    result.normal_scale = source.normal_texture.scale;
    result.occlusion_strength = source.occlusion_texture.scale;
    result.alpha_mode = source.alpha_mode == cgltf_alpha_mode_mask
        ? scene::MaterialAlphaMode::Mask
        : source.alpha_mode == cgltf_alpha_mode_blend
        ? scene::MaterialAlphaMode::Blend
        : scene::MaterialAlphaMode::Opaque;
    result.alpha_cutoff = source.alpha_cutoff;
    result.double_sided = source.double_sided != 0;
    if (source.has_clearcoat) {
        result.clearcoat = source.clearcoat.clearcoat_factor;
        result.clearcoat_roughness = source.clearcoat.clearcoat_roughness_factor;
        result.clearcoat_normal_scale = source.clearcoat.clearcoat_normal_texture.scale;
    }
    if (source.has_sheen) {
        result.sheen_color = {source.sheen.sheen_color_factor[0], source.sheen.sheen_color_factor[1], source.sheen.sheen_color_factor[2]};
        result.sheen_roughness = source.sheen.sheen_roughness_factor;
    }
    if (source.has_transmission) result.transmission = source.transmission.transmission_factor;
    if (source.has_ior) result.ior = source.ior.ior;
    if (source.has_specular) {
        result.specular = source.specular.specular_factor;
        result.specular_color = {source.specular.specular_color_factor[0], source.specular.specular_color_factor[1], source.specular.specular_color_factor[2]};
    }
    if (source.has_anisotropy) {
        result.anisotropy_strength = source.anisotropy.anisotropy_strength;
        result.anisotropy_rotation = source.anisotropy.anisotropy_rotation;
    }
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

[[nodiscard]] bool read_vec4(const cgltf_accessor& accessor, cgltf_size index,
                             scene::Vec4& into) noexcept {
    cgltf_float value[4]{};
    if (cgltf_accessor_read_float(&accessor, index, value, 4U) == 0) {
        return false;
    }
    into = scene::Vec4{value[0], value[1], value[2], value[3]};
    return true;
}

[[nodiscard]] scene::Vec4 transform_tangent(
    const scene::Mat4& world, scene::Vec3 world_normal,
    scene::Vec4 tangent) noexcept {
    const scene::Vec4 transformed = scene::transform(
        world, scene::Vec4{tangent.x, tangent.y, tangent.z, 0.0F});
    const scene::Vec3 raw{transformed.x, transformed.y, transformed.z};
    const scene::Vec3 axis = std::abs(world_normal.y) < 0.999F
        ? scene::Vec3{0.0F, 1.0F, 0.0F}
        : scene::Vec3{1.0F, 0.0F, 0.0F};
    const scene::Vec3 fallback = scene::normalize(
        scene::cross(axis, world_normal), scene::Vec3{1.0F, 0.0F, 0.0F});
    const scene::Vec3 orthogonal = scene::normalize(
        raw - world_normal * scene::dot(world_normal, raw), fallback);
    const scene::Vec3 column0{world.m[0U], world.m[1U], world.m[2U]};
    const scene::Vec3 column1{world.m[4U], world.m[5U], world.m[6U]};
    const scene::Vec3 column2{world.m[8U], world.m[9U], world.m[10U]};
    const float handedness = scene::dot(
        column0, scene::cross(column1, column2)) < 0.0F
        ? -tangent.w
        : tangent.w;
    return scene::Vec4{orthogonal.x, orthogonal.y, orthogonal.z, handedness};
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

struct MikkContext final {
    ModelAsset* asset{};
    std::size_t first_index{};
    std::size_t face_count{};
};

[[nodiscard]] MikkContext& mikk_context(
    const SMikkTSpaceContext* context) noexcept {
    return *static_cast<MikkContext*>(context->m_pUserData);
}

[[nodiscard]] std::size_t mikk_vertex_index(
    const MikkContext& context, int face, int vertex) noexcept {
    const auto corner = context.first_index
        + static_cast<std::size_t>(face) * 3U
        + static_cast<std::size_t>(vertex);
    return context.asset->mesh.indices[corner];
}

[[nodiscard]] int mikk_face_count(
    const SMikkTSpaceContext* context) noexcept {
    return static_cast<int>(mikk_context(context).face_count);
}

[[nodiscard]] int mikk_vertices_per_face(
    const SMikkTSpaceContext*, int) noexcept {
    return 3;
}

void mikk_position(const SMikkTSpaceContext* context, float output[3],
                   int face, int vertex) noexcept {
    const auto& held = mikk_context(context);
    const auto& value =
        held.asset->mesh.vertices[mikk_vertex_index(held, face, vertex)]
            .position;
    output[0U] = value.x;
    output[1U] = value.y;
    output[2U] = value.z;
}

void mikk_normal(const SMikkTSpaceContext* context, float output[3],
                 int face, int vertex) noexcept {
    const auto& held = mikk_context(context);
    const auto& value =
        held.asset->mesh.vertices[mikk_vertex_index(held, face, vertex)]
            .normal;
    output[0U] = value.x;
    output[1U] = value.y;
    output[2U] = value.z;
}

void mikk_uv(const SMikkTSpaceContext* context, float output[2], int face,
             int vertex) noexcept {
    const auto& held = mikk_context(context);
    const auto& value =
        held.asset->mesh.vertices[mikk_vertex_index(held, face, vertex)];
    output[0U] = value.u;
    output[1U] = value.v;
}

void mikk_tangent(const SMikkTSpaceContext* context, const float tangent[3],
                  float sign, int face, int vertex) noexcept {
    auto& held = mikk_context(context);
    auto& output = held.asset->vertex_metadata[
        mikk_vertex_index(held, face, vertex)];
    const auto normal = held.asset->mesh.vertices[
        mikk_vertex_index(held, face, vertex)].normal;
    const scene::Vec3 raw{tangent[0U], tangent[1U], tangent[2U]};
    const scene::Vec3 axis = std::abs(normal.y) < 0.999F
        ? scene::Vec3{0.0F, 1.0F, 0.0F}
        : scene::Vec3{1.0F, 0.0F, 0.0F};
    const scene::Vec3 fallback = scene::normalize(
        scene::cross(axis, normal), scene::Vec3{1.0F, 0.0F, 0.0F});
    const scene::Vec3 orthogonal = scene::normalize(
        raw - normal * scene::dot(normal, raw), fallback);
    output.has_tangent = true;
    output.tangent = scene::Vec4{
        orthogonal.x, orthogonal.y, orthogonal.z, sign < 0.0F ? -1.0F : 1.0F};
}

[[nodiscard]] bool generate_mikk_tangents(ModelAsset& asset,
                                          core::u32 first_index,
                                          core::u32 index_count) noexcept {
    if (index_count == 0U || index_count % 3U != 0U
        || asset.vertex_metadata.size() != asset.mesh.vertices.size()) {
        return false;
    }
    const auto face_count = static_cast<std::size_t>(index_count / 3U);
    if (face_count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    MikkContext user{&asset, first_index, face_count};
    SMikkTSpaceInterface interface{};
    interface.m_getNumFaces = &mikk_face_count;
    interface.m_getNumVerticesOfFace = &mikk_vertices_per_face;
    interface.m_getPosition = &mikk_position;
    interface.m_getNormal = &mikk_normal;
    interface.m_getTexCoord = &mikk_uv;
    interface.m_setTSpaceBasic = &mikk_tangent;
    SMikkTSpaceContext context{&interface, &user};
    return genTangSpaceDefault(&context) != 0;
}

// glTF defaults to repeat addressing and implementation-selected filtering.
// Choose trilinear consistently when a filter is omitted. Invalid values fail ingest.
[[nodiscard]] bool gltf_sampler(const cgltf_texture_view& view, ModelSampler& out) {
    out = {};
    if (!view.texture || !view.texture->sampler) return true;
    const auto& sampler = *view.texture->sampler;
    const auto address = [](int value, ModelAddressMode& into) {
        switch (value) {
        case 10497: into = ModelAddressMode::Repeat; return true;
        case 33071: into = ModelAddressMode::Clamp; return true;
        case 33648: into = ModelAddressMode::Mirror; return true;
        default: return false;
        }
    };
    if (!address(sampler.wrap_s, out.u) || !address(sampler.wrap_t, out.v)) return false;
    switch (sampler.mag_filter) {
    case 0: case 9729: out.mag = ModelFilter::Linear; break;
    case 9728: out.mag = ModelFilter::Nearest; break;
    default: return false;
    }
    switch (sampler.min_filter) {
    case 0: case 9987: out.min = ModelFilter::Linear; out.mip = ModelMipFilter::Linear; break;
    case 9728: out.min = ModelFilter::Nearest; out.mip = ModelMipFilter::None; break;
    case 9729: out.min = ModelFilter::Linear; out.mip = ModelMipFilter::None; break;
    case 9984: out.min = ModelFilter::Nearest; out.mip = ModelMipFilter::Nearest; break;
    case 9985: out.min = ModelFilter::Linear; out.mip = ModelMipFilter::Nearest; break;
    case 9986: out.min = ModelFilter::Nearest; out.mip = ModelMipFilter::Linear; break;
    default: return false;
    }
    return true;
}

// Bake the legacy diffuse/specular workflow into the renderer's metallic /
// roughness textures once per material, before mip generation and compression.
[[nodiscard]] ModelPart convert_specular_glossiness(
    const cgltf_material& material, const cgltf_data& data, ModelAsset& asset) {
    const auto& sg = material.pbr_specular_glossiness;
    ModelSampler diffuse_sampler{}, specular_sampler{};
    if (!gltf_sampler(sg.diffuse_texture, diffuse_sampler)
        || !gltf_sampler(sg.specular_glossiness_texture, specular_sampler)
        || (sg.diffuse_texture.texture && sg.specular_glossiness_texture.texture
            && diffuse_sampler != specular_sampler)) {
        asset.diagnostic = "Cannot combine specular/glossiness textures with different samplers";
        return {};
    }
    const auto converted_sampler = sg.diffuse_texture.texture ? diffuse_sampler : specular_sampler;
    const auto diffuse_index = gltf_texture_index(sg.diffuse_texture, data, asset);
    const auto specular_index = gltf_texture_index(sg.specular_glossiness_texture, data, asset);
    const ModelTexture* diffuse = diffuse_index ? &asset.textures[*diffuse_index] : nullptr;
    const ModelTexture* specular = specular_index ? &asset.textures[*specular_index] : nullptr;
    const core::u32 width = std::max(diffuse ? diffuse->width : 1U, specular ? specular->width : 1U);
    const core::u32 height = std::max(diffuse ? diffuse->height : 1U, specular ? specular->height : 1U);
    ModelTexture base{width, height, std::vector<core::u8>(static_cast<std::size_t>(width) * height * 4U)};
    ModelTexture mr = base;
    const auto linear = [](float v) { return v <= 0.04045F ? v / 12.92F : std::pow((v + 0.055F) / 1.055F, 2.4F); };
    const auto srgb = [](float v) { return v <= 0.0031308F ? v * 12.92F : 1.055F * std::pow(v, 1.0F / 2.4F) - 0.055F; };
    const auto byte = [](float v) { return static_cast<core::u8>(std::clamp(v, 0.0F, 1.0F) * 255.0F + 0.5F); };
    const auto brightness = [](const std::array<float, 3U>& v) {
        return std::sqrt(0.299F*v[0]*v[0] + 0.587F*v[1]*v[1] + 0.114F*v[2]*v[2]);
    };
    for (core::u32 y = 0; y < height; ++y) {
        for (core::u32 x = 0; x < width; ++x) {
            const auto sample = [&](const ModelTexture* texture, std::size_t channel) {
                if (!texture) return 1.0F;
                const auto sx = static_cast<core::u32>(static_cast<core::u64>(x) * texture->width / width);
                const auto sy = static_cast<core::u32>(static_cast<core::u64>(y) * texture->height / height);
                return texture->rgba[(static_cast<std::size_t>(sy) * texture->width + sx) * 4U + channel] / 255.0F;
            };
            std::array<float, 3U> d{}, s{};
            for (std::size_t c = 0; c < 3U; ++c) {
                d[c] = sg.diffuse_factor[c] * linear(sample(diffuse, c));
                s[c] = sg.specular_factor[c] * linear(sample(specular, c));
            }
            const float one_minus_specular = 1.0F - std::max({s[0], s[1], s[2]});
            const float b = brightness(d) * one_minus_specular / 0.96F + brightness(s) - 0.08F;
            const float c = 0.04F - brightness(s);
            const float metallic = brightness(s) < 0.04F ? 0.0F : std::clamp(
                (-b + std::sqrt(std::max(b*b - 0.16F*c, 0.0F))) / 0.08F, 0.0F, 1.0F);
            const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const float from_diffuse = d[channel] * one_minus_specular / (0.96F * std::max(1.0F - metallic, 1.0e-6F));
                const float from_specular = (s[channel] - 0.04F * (1.0F - metallic)) / std::max(metallic, 1.0e-6F);
                const float color = std::lerp(from_diffuse, from_specular, metallic * metallic);
                base.rgba[offset + channel] = byte(srgb(std::clamp(color, 0.0F, 1.0F)));
            }
            base.rgba[offset + 3U] = byte(sg.diffuse_factor[3] * sample(diffuse, 3U));
            mr.rgba[offset] = 255U;
            mr.rgba[offset + 1U] = byte(1.0F - sg.glossiness_factor * sample(specular, 3U));
            mr.rgba[offset + 2U] = byte(metallic);
            mr.rgba[offset + 3U] = 255U;
        }
    }
    ModelPart result{};
    result.samplers[0] = result.samplers[1] = converted_sampler;
    result.material = gltf_material(material);
    result.material.base_color = {1.0F, 1.0F, 1.0F};
    result.material.base_alpha = 1.0F;
    result.material.metallic = 1.0F;
    result.material.roughness = 1.0F;
    result.base_color_texture = static_cast<core::u32>(asset.textures.size());
    asset.textures.push_back(std::move(base));
    result.metallic_roughness_texture = static_cast<core::u32>(asset.textures.size());
    asset.textures.push_back(std::move(mr));
    return result;
}

[[nodiscard]] bool append_gltf_primitive(const cgltf_primitive& primitive,
                                         const cgltf_data& data,
                                         const std::vector<ModelPart>& converted,
                                         const scene::Mat4& world,
                                         ModelAsset& asset,
                                         BoundsBuilder& bounds) {
    const auto* const positions =
        find_attribute(primitive, cgltf_attribute_type_position);
    const auto* const normals =
        find_attribute(primitive, cgltf_attribute_type_normal);
    const auto* const uvs =
        find_attribute(primitive, cgltf_attribute_type_texcoord);
    const auto* const tangents =
        find_attribute(primitive, cgltf_attribute_type_tangent);
    if (positions == nullptr) {
        return true;
    }
    if ((normals != nullptr && normals->count != positions->count) ||
        (uvs != nullptr && uvs->count != positions->count) ||
        (tangents != nullptr && tangents->count != positions->count)) {
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
            scene::Vec4 tangent{};
            if (!read_vec3(*positions, vertex_index, position) ||
                !read_vec3(*normals, vertex_index, normal) ||
                !read_uv(uvs, vertex_index, vertex) ||
                (tangents != nullptr &&
                 !read_vec4(*tangents, vertex_index, tangent))) {
                return false;
            }
            vertex.position = scene::transform_point(world, position);
            vertex.normal = transform_normal(normal_matrix, normal);
            const std::optional<scene::Vec4> world_tangent =
                tangents == nullptr
                ? std::nullopt
                : std::optional<scene::Vec4>{
                      transform_tangent(world, vertex.normal, tangent)};
            append_vertex(asset, bounds, vertex, uvs != nullptr,
                          world_tangent);
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
        if (material.has_pbr_specular_glossiness) {
            const auto& legacy = converted[cgltf_material_index(&data, &material)];
            part.samplers = legacy.samplers;
            part.material = legacy.material;
            part.base_color_texture = legacy.base_color_texture;
            part.metallic_roughness_texture = legacy.metallic_roughness_texture;
        }
        if (!material.has_pbr_specular_glossiness && material.has_pbr_metallic_roughness != 0 && uvs != nullptr) {
            part.base_color_texture = gltf_texture_index(
                material.pbr_metallic_roughness.base_color_texture, data,
                asset);
            part.metallic_roughness_texture = gltf_texture_index(
                material.pbr_metallic_roughness.metallic_roughness_texture,
                data, asset);
        }
        if (uvs != nullptr) {
            part.emissive_texture =
                gltf_texture_index(material.emissive_texture, data, asset);
            part.normal_texture =
                gltf_texture_index(material.normal_texture, data, asset);
            part.occlusion_texture =
                gltf_texture_index(material.occlusion_texture, data, asset);
        }
        part.clearcoat_texture = gltf_texture_index(material.clearcoat.clearcoat_texture, data, asset);
        part.clearcoat_roughness_texture = gltf_texture_index(material.clearcoat.clearcoat_roughness_texture, data, asset);
        part.clearcoat_normal_texture = gltf_texture_index(material.clearcoat.clearcoat_normal_texture, data, asset);
        part.sheen_color_texture = gltf_texture_index(material.sheen.sheen_color_texture, data, asset);
        part.sheen_roughness_texture = gltf_texture_index(material.sheen.sheen_roughness_texture, data, asset);
        part.transmission_texture = gltf_texture_index(material.transmission.transmission_texture, data, asset);
        part.specular_texture = gltf_texture_index(material.specular.specular_texture, data, asset);
        part.specular_color_texture = gltf_texture_index(material.specular.specular_color_texture, data, asset);
        part.anisotropy_texture = gltf_texture_index(material.anisotropy.anisotropy_texture, data, asset);
        std::size_t sampler_slot = 0U;
        for (const auto* view : {&material.pbr_metallic_roughness.base_color_texture, &material.pbr_metallic_roughness.metallic_roughness_texture, &material.normal_texture, &material.occlusion_texture, &material.emissive_texture, &material.clearcoat.clearcoat_texture, &material.clearcoat.clearcoat_roughness_texture, &material.clearcoat.clearcoat_normal_texture, &material.sheen.sheen_color_texture, &material.sheen.sheen_roughness_texture, &material.transmission.transmission_texture, &material.specular.specular_texture, &material.specular.specular_color_texture, &material.anisotropy.anisotropy_texture}) {
            const auto slot = sampler_slot++;
            if ((!material.has_pbr_specular_glossiness || slot >= 2U)
                && !gltf_sampler(*view, part.samplers[slot])) {
                asset.diagnostic = "Unsupported material sampler"; return false;
            }
            if (view->texture && (uvs == nullptr || !gltf_texture_index(*view, data, asset))) {
                asset.diagnostic = "Missing UVs or unreadable material texture"; return false;
            }
        }
        // glTF explicitly names default MikkTSpace for a normal-mapped
        // primitive that omits tangents. Generate against the already-baked
        // positions and normals, so a mirrored/non-uniform node transform and
        // the tangent sign stay in the same space the renderer uploads.
        // Supplied tangents are ignored when normals were absent, as required
        // by the format; the generated flat-normal basis replaces them.
        if ((part.normal_texture.has_value() || part.clearcoat_normal_texture.has_value() || part.material.anisotropy_strength > 0.0F)
            && (tangents == nullptr || normals == nullptr)
            && !generate_mikk_tangents(
                asset, part.first_index, part.index_count)) {
            // Degenerate geometry/UVs can leave MikkTSpace without a basis.
            // Keep the model and its geometric normal rather than sampling a
            // normal map in an invented coordinate system.
            asset.diagnostic = "Cannot generate material tangent frame";
            return false;
        }
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

    const auto supported = [](std::string_view extension) {
        return extension == "KHR_materials_pbrSpecularGlossiness"
            || extension == "KHR_materials_clearcoat" || extension == "KHR_materials_sheen"
            || extension == "KHR_materials_transmission" || extension == "KHR_materials_ior"
            || extension == "KHR_materials_specular" || extension == "KHR_materials_anisotropy"
            || extension == "KHR_materials_emissive_strength";
    };
    for (cgltf_size i = 0U; i < data->extensions_used_count; ++i) {
        if (!supported(data->extensions_used[i])) {
            into.diagnostic = std::string{"Unsupported glTF extension: "} + data->extensions_used[i];
            return false;
        }
    }
    for (cgltf_size i = 0U; i < data->extensions_required_count; ++i) {
        if (!supported(data->extensions_required[i])) {
            into.diagnostic = std::string{"Unsupported required glTF extension: "} + data->extensions_required[i];
            return false;
        }
    }
    for (cgltf_size i = 0U; i < data->materials_count; ++i) {
        const auto& material = data->materials[i];
        if (material.has_volume
            || material.has_iridescence || material.has_dispersion || material.unlit
            || material.extensions_count != 0U) {
            into.diagnostic = "Unsupported glTF material extension";
            return false;
        }
        const auto factors = gltf_material(material);
        if (!std::isfinite(factors.base_alpha) || factors.base_alpha < 0.0F || factors.base_alpha > 1.0F) {
            into.diagnostic = "Invalid glTF material factor: base_alpha"; return false;
        }
        if (!std::isfinite(factors.clearcoat) || factors.clearcoat < 0.0F || factors.clearcoat > 1.0F) {
            into.diagnostic = "Invalid glTF material factor: clearcoat"; return false;
        }
        if (!std::isfinite(factors.clearcoat_roughness) || factors.clearcoat_roughness < 0.0F || factors.clearcoat_roughness > 1.0F) {
            into.diagnostic = "Invalid glTF material factor: clearcoat_roughness"; return false;
        }
        if (!std::isfinite(factors.clearcoat_normal_scale) || factors.clearcoat_normal_scale < 0.0F || factors.clearcoat_normal_scale > 4.0F) {
            into.diagnostic = "Invalid glTF material factor: clearcoat_normal_scale"; return false;
        }
        if (!std::isfinite(factors.sheen_roughness) || factors.sheen_roughness < 0.0F || factors.sheen_roughness > 1.0F) {
            into.diagnostic = "Invalid glTF material factor: sheen_roughness"; return false;
        }
        if (!std::isfinite(factors.transmission) || factors.transmission < 0.0F || factors.transmission > 1.0F) {
            into.diagnostic = "Invalid glTF material factor: transmission"; return false;
        }
        if (!std::isfinite(factors.ior) || factors.ior < 1.0F || factors.ior > 3.0F) {
            into.diagnostic = "Invalid glTF material factor: ior"; return false;
        }
        if (!std::isfinite(factors.specular) || factors.specular < 0.0F || factors.specular > 1.0F) {
            into.diagnostic = "Invalid glTF material factor: specular"; return false;
        }
        if (!std::isfinite(factors.anisotropy_strength) || factors.anisotropy_strength < 0.0F || factors.anisotropy_strength > 1.0F) {
            into.diagnostic = "Invalid glTF material factor: anisotropy_strength"; return false;
        }
        if (!std::isfinite(factors.sheen_color.x) || factors.sheen_color.x < 0.0F || factors.sheen_color.x > 1.0F) {
            into.diagnostic = "Invalid glTF material factor: sheen_color_r"; return false;
        }
        if (!std::isfinite(factors.sheen_color.y) || factors.sheen_color.y < 0.0F || factors.sheen_color.y > 1.0F) {
            into.diagnostic = "Invalid glTF material factor: sheen_color_g"; return false;
        }
        if (!std::isfinite(factors.sheen_color.z) || factors.sheen_color.z < 0.0F || factors.sheen_color.z > 1.0F) {
            into.diagnostic = "Invalid glTF material factor: sheen_color_b"; return false;
        }
        if (!std::isfinite(factors.specular_color.x) || factors.specular_color.x < 0.0F || factors.specular_color.x > 1.0F) {
            into.diagnostic = "Invalid glTF material factor: specular_color_r"; return false;
        }
        if (!std::isfinite(factors.specular_color.y) || factors.specular_color.y < 0.0F || factors.specular_color.y > 1.0F) {
            into.diagnostic = "Invalid glTF material factor: specular_color_g"; return false;
        }
        if (!std::isfinite(factors.specular_color.z) || factors.specular_color.z < 0.0F || factors.specular_color.z > 1.0F) {
            into.diagnostic = "Invalid glTF material factor: specular_color_b"; return false;
        }
        if (!std::isfinite(factors.anisotropy_rotation)) {
            into.diagnostic = "Invalid glTF anisotropy rotation"; return false;
        }
        for (const auto* view : {&material.pbr_metallic_roughness.base_color_texture,
             &material.pbr_specular_glossiness.diffuse_texture,
             &material.pbr_specular_glossiness.specular_glossiness_texture,
             &material.pbr_metallic_roughness.metallic_roughness_texture,
             &material.normal_texture, &material.occlusion_texture, &material.emissive_texture,
             &material.clearcoat.clearcoat_texture,
             &material.clearcoat.clearcoat_roughness_texture,
             &material.clearcoat.clearcoat_normal_texture,
             &material.sheen.sheen_color_texture,
             &material.sheen.sheen_roughness_texture,
             &material.transmission.transmission_texture,
             &material.specular.specular_texture,
             &material.specular.specular_color_texture,
             &material.anisotropy.anisotropy_texture}) {
            if (view->texture != nullptr && (view->texcoord != 0 || view->has_transform
                || view->texture->image == nullptr)) {
                into.diagnostic = "Unsupported glTF texture view (requires TEXCOORD_0 without transform)";
                return false;
            }
        }
    }
    into.textures.reserve(data->images_count);
    const std::filesystem::path model_path{path};
    for (cgltf_size image_index = 0U; image_index < data->images_count;
         ++image_index) {
        into.textures.push_back(
            decode_gltf_image(model_path, data->images[image_index]));
    }

    std::vector<ModelPart> converted(data->materials_count);
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        if (data->materials[i].has_pbr_specular_glossiness) {
            const auto& sg = data->materials[i].pbr_specular_glossiness;
            if (!std::isfinite(sg.glossiness_factor) || sg.glossiness_factor < 0.0F || sg.glossiness_factor > 1.0F
                || !std::all_of(std::begin(sg.diffuse_factor), std::end(sg.diffuse_factor),
                    [](float v) { return std::isfinite(v) && v >= 0.0F && v <= 1.0F; })
                || !std::all_of(std::begin(sg.specular_factor), std::end(sg.specular_factor),
                    [](float v) { return std::isfinite(v) && v >= 0.0F && v <= 1.0F; })) {
                into.diagnostic = "Invalid glTF specular/glossiness factor";
                return false;
            }
            // Exporters often repeat the same texture/factor pair for several
            // mesh parts. Share the baked maps instead of converting them again.
            cgltf_size previous = 0;
            for (; previous < i; ++previous) {
                const auto& other = data->materials[previous].pbr_specular_glossiness;
                if (data->materials[previous].has_pbr_specular_glossiness
                    && sg.diffuse_texture.texture == other.diffuse_texture.texture
                    && sg.specular_glossiness_texture.texture == other.specular_glossiness_texture.texture
                    && sg.glossiness_factor == other.glossiness_factor
                    && std::equal(std::begin(sg.diffuse_factor), std::end(sg.diffuse_factor), std::begin(other.diffuse_factor))
                    && std::equal(std::begin(sg.specular_factor), std::end(sg.specular_factor), std::begin(other.specular_factor))) break;
            }
            if (previous < i) {
                converted[i] = converted[previous];
                auto factors = gltf_material(data->materials[i]);
                factors.base_color = {1.0F, 1.0F, 1.0F};
                factors.base_alpha = factors.metallic = factors.roughness = 1.0F;
                converted[i].material = factors;
                continue;
            }
            converted[i] = convert_specular_glossiness(data->materials[i], *data, into);
            if (!into.diagnostic.empty()) return false;
        }
    }
    BoundsBuilder bounds{};
    const bool animated = data->animations_count != 0 || data->skins_count != 0;
    std::vector<core::u32> animation_remap;
    if (animated && !import_animation_nodes(*data, into, animation_remap)) {
        if (into.diagnostic.empty()) into.diagnostic = "Invalid or excessive glTF animation hierarchy/channels";
        return false;
    }
    for (cgltf_size node_index = 0U; node_index < data->nodes_count;
         ++node_index) {
        const auto& node = data->nodes[node_index];
        if (node.mesh == nullptr) {
            continue;
        }
        cgltf_float world_values[16]{};
        cgltf_node_transform_world(&node, world_values);
        const auto world = as_matrix(world_values);
        const auto binding_base = static_cast<core::u32>(into.pose_bindings.size());
        if (animated) {
            const auto inverse_world = scene::invert_affine(world);
            // A singular baked transform cannot recover the original vertices.
            const auto determinant = scene::dot(scene::Vec3{world.m[0],world.m[1],world.m[2]},
                scene::cross({world.m[4],world.m[5],world.m[6]}, {world.m[8],world.m[9],world.m[10]}));
            if (std::abs(determinant) < 1e-12F) { into.diagnostic="Singular animated mesh bind transform"; return false; }
            if (node.skin) {
                if (node.skin->joints_count == 0 || binding_base + node.skin->joints_count > max_model_pose_bindings) return false;
                if (node.skin->inverse_bind_matrices && (node.skin->inverse_bind_matrices->type != cgltf_type_mat4
                    || node.skin->inverse_bind_matrices->count != node.skin->joints_count)) return false;
                for (cgltf_size j=0; j<node.skin->joints_count; ++j) {
                    auto inverse_bind=scene::identity();
                    if (node.skin->inverse_bind_matrices && !cgltf_accessor_read_float(node.skin->inverse_bind_matrices,j,inverse_bind.m.data(),16)) return false;
                    into.pose_bindings.push_back({animation_remap[cgltf_node_index(data,node.skin->joints[j])],scene::multiply(inverse_bind,inverse_world)});
                }
            } else {
                if (binding_base == max_model_pose_bindings) return false;
                into.pose_bindings.push_back({animation_remap[node_index],inverse_world});
            }
        }
        for (cgltf_size primitive_index = 0U;
             primitive_index < node.mesh->primitives_count; ++primitive_index) {
            const auto& primitive = node.mesh->primitives[primitive_index];
            if (primitive.targets_count != 0) { into.diagnostic="Morph targets are not supported; export node or skin animation"; return false; }
            if (primitive.type != cgltf_primitive_type_triangles) {
                continue;
            }
            const auto first_vertex=into.mesh.vertices.size();
            if (!append_gltf_primitive(primitive, *data, converted, world, into, bounds)) {
                return false;
            }
            if (!animated) continue;
            const auto* joints=find_attribute(primitive,cgltf_attribute_type_joints);
            const auto* weights=find_attribute(primitive,cgltf_attribute_type_weights);
            const auto* positions=find_attribute(primitive,cgltf_attribute_type_position);
            const bool expanded=find_attribute(primitive,cgltf_attribute_type_normal)==nullptr;
            if (node.skin && (!joints || !weights || !positions || joints->count!=positions->count || weights->count!=positions->count
                || joints->type!=cgltf_type_vec4 || weights->type!=cgltf_type_vec4
                || (joints->component_type!=cgltf_component_type_r_8u && joints->component_type!=cgltf_component_type_r_16u)
                || joints->normalized || find_attribute(primitive,cgltf_attribute_type_joints,1)
                || find_attribute(primitive,cgltf_attribute_type_weights,1))) {
                into.diagnostic="Skins require four JOINTS_0/WEIGHTS_0 influences per vertex"; return false;
            }
            for (std::size_t v=first_vertex; v<into.mesh.vertices.size(); ++v) {
                auto& metadata=into.vertex_metadata[v];
                metadata.joints.fill(binding_base);
                if (!node.skin) continue;
                const auto local=expanded ? source_index(primitive,v-first_vertex) : v-first_vertex;
                cgltf_uint joint[4]{}; float weight[4]{};
                if (!cgltf_accessor_read_uint(joints,local,joint,4) || !cgltf_accessor_read_float(weights,local,weight,4)) return false;
                float sum=0;
                for (unsigned k=0; k<4; ++k) {
                    if (joint[k]>=node.skin->joints_count || !std::isfinite(weight[k]) || weight[k]<0) return false;
                    metadata.joints[k]=binding_base+joint[k]; sum+=weight[k];
                }
                if (!std::isfinite(sum) || sum<=0) return false;
                metadata.weights={weight[0]/sum,weight[1]/sum,weight[2]/sum,weight[3]/sum};
            }
        }
    }

    if (into.mesh.vertices.empty() || into.mesh.indices.empty() ||
        into.parts.empty()) {
        return false;
    }
    into.bounds = bounds.finish();
    if (!validate_model_animation(into)) { into.diagnostic="Invalid glTF skin or animation data"; return false; }
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

bool detail::generate_model_tangents(ModelAsset& asset, core::u32 first_index,
                                     core::u32 index_count) noexcept {
    return generate_mikk_tangents(asset, first_index, index_count);
}

namespace {
bool load_level(const std::string& source, ModelAsset& into) {
    if (ends_with(source, ".msmodel")) return load_model_package(source, into);
    if (ends_with(source, ".gltf") || ends_with(source, ".glb")) return load_gltf(source, into);
    if (ends_with(source, ".obj")) return load_obj(source, into);
    if (ends_with(source, ".fbx")) return detail::load_fbx(source, into);
    return false;
}

bool load_chain(const std::string& source, ModelAsset& into) {
    if (ends_with(source, ".msmodel")) return load_model_package(source, into);
    if (!load_level(source, into)) return false;
    into.lods.push_back({0U, static_cast<core::u32>(into.parts.size()), 0.0F});
    const std::filesystem::path companion{source + ".lod"};
    std::error_code error;
    if (!std::filesystem::exists(companion, error)) return !error;
    if (!into.animation_nodes.empty()) { into.diagnostic="Animated models cannot use an external LOD chain"; return false; }
    const auto bytes = std::filesystem::file_size(companion, error);
    if (error || bytes > 65'536U) return false;
    std::ifstream input{companion};
    std::string header;
    if (!std::getline(input, header)) return false;
    if (!header.empty() && header.back() == '\r') header.pop_back();
    if (header != "MSLOD 1") return false;
    float previous = std::numeric_limits<float>::infinity();
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row{line};
        row >> std::ws;
        if (row.eof()) continue;
        float threshold{};
        std::string relative;
        if (!(row >> threshold)) return false;
        row >> std::ws;
        if (row.peek() != '"') return false;
        if (!(row >> std::quoted(relative)) || relative.empty()
            || !std::isfinite(threshold) || threshold <= 0.0F || threshold >= previous
            || into.lods.size() >= max_model_lods) return false;
        row >> std::ws;
        if (!row.eof()) return false;
        ModelAsset level;
        // Read base geometry only: companions cannot recurse or form cycles.
        if (!load_level((companion.parent_path() / relative).lexically_normal().string(), level)) { into.diagnostic = level.diagnostic; return false; }
        if (!level.animation_nodes.empty()) { into.diagnostic="Animated models cannot use an external LOD chain"; return false; }
        if (!indexable(into.mesh.vertices.size() + level.mesh.vertices.size())
            || !indexable(into.mesh.indices.size() + level.mesh.indices.size())
            || !indexable(into.parts.size() + level.parts.size())
            || !indexable(into.textures.size() + level.textures.size())) return false;
        const auto vertex_base = static_cast<core::u32>(into.mesh.vertices.size());
        const auto index_base = static_cast<core::u32>(into.mesh.indices.size());
        const auto texture_base = static_cast<core::u32>(into.textures.size());
        into.lods.push_back({static_cast<core::u32>(into.parts.size()),
                            static_cast<core::u32>(level.parts.size()), threshold});
        into.mesh.vertices.insert(into.mesh.vertices.end(), level.mesh.vertices.begin(), level.mesh.vertices.end());
        into.vertex_metadata.insert(into.vertex_metadata.end(), level.vertex_metadata.begin(), level.vertex_metadata.end());
        for (auto index : level.mesh.indices) into.mesh.indices.push_back(index + vertex_base);
        for (auto& part : level.parts) {
            part.first_index += index_base;
            for (auto* index : {&part.base_color_texture, &part.metallic_roughness_texture,
                               &part.normal_texture, &part.occlusion_texture, &part.emissive_texture,
                               &part.clearcoat_texture, &part.clearcoat_roughness_texture, &part.clearcoat_normal_texture, &part.sheen_color_texture, &part.sheen_roughness_texture, &part.transmission_texture, &part.specular_texture, &part.specular_color_texture, &part.anisotropy_texture}) {
                if (*index) **index += texture_base;
            }
            into.parts.push_back(std::move(part));
        }
        for (auto& texture : level.textures) into.textures.push_back(std::move(texture));
        into.bounds.minimum = {std::min(into.bounds.minimum.x,level.bounds.minimum.x),
            std::min(into.bounds.minimum.y,level.bounds.minimum.y),std::min(into.bounds.minimum.z,level.bounds.minimum.z)};
        into.bounds.maximum = {std::max(into.bounds.maximum.x,level.bounds.maximum.x),
            std::max(into.bounds.maximum.y,level.bounds.maximum.y),std::max(into.bounds.maximum.z,level.bounds.maximum.z)};
        previous = threshold;
    }
    return input.eof() && into.lods.size() > 1U;
}
} // namespace

bool load_model_base(const std::string& source, ModelAsset& into) {
    into = ModelAsset{};
    try { if (load_level(source, into)) return true; } catch (...) {}
    auto diagnostic = std::move(into.diagnostic);
    into = ModelAsset{};
    into.diagnostic = diagnostic.empty() ? "Invalid or unreadable model" : std::move(diagnostic);
    return false;
}

bool load_model(const std::string& source, ModelAsset& into) {
    into = ModelAsset{};
    try {
        if (load_chain(source, into)) return true;
    } catch (...) {
        // A chain is one cache generation. Never publish a partially decoded
        // level set after malformed input or a failed allocation.
    }
    auto diagnostic = std::move(into.diagnostic);
    into = ModelAsset{};
    into.diagnostic = diagnostic.empty() ? "Invalid or unreadable model" : std::move(diagnostic);
    return false;
}

bool assign_model_texture(ModelAsset& asset, core::u32 part_index,
    core::u32 role, const std::string& image_source) {
    if (part_index >= asset.parts.size() || role >= 5U) {
        asset.diagnostic = "Material part or texture channel no longer exists";
        return false;
    }
    auto& part = asset.parts[part_index];
    const std::array<std::optional<core::u32>*, 5U> slots{
        &part.base_color_texture, &part.metallic_roughness_texture,
        &part.normal_texture, &part.occlusion_texture, &part.emissive_texture};
    if (image_source.empty()) { slots[role]->reset(); return true; }
    for (core::u32 i = 0; i < part.index_count; ++i) {
        const auto index = static_cast<std::size_t>(part.first_index) + i;
        if (index >= asset.mesh.indices.size() || asset.mesh.indices[index] >= asset.vertex_metadata.size()
            || !asset.vertex_metadata[asset.mesh.indices[index]].has_uv0) {
            asset.diagnostic = "This material part has no UV map; add UVs in the source model first";
            return false;
        }
    }
    int width{}, height{}, channels{};
    if (!stbi_info(image_source.c_str(), &width, &height, &channels)
        || width <= 0 || height <= 0 || width > 16384 || height > 16384
        || static_cast<core::u64>(width) * static_cast<core::u64>(height) > 64U * 1024U * 1024U) {
        asset.diagnostic = "Cannot read texture: use PNG, JPEG, TGA or BMP within 16384 pixels / 64 megapixels";
        return false;
    }
    auto* pixels = stbi_load(image_source.c_str(), &width, &height, &channels, 4);
    auto texture = decoded_texture(pixels, width, height);
    if (!texture.valid()) { asset.diagnostic = "Cannot decode texture image"; return false; }
    if (role == 2U && !generate_mikk_tangents(asset, part.first_index, part.index_count)) {
        asset.diagnostic = "Cannot generate tangent frame for this normal map"; return false;
    }
    *slots[role] = static_cast<core::u32>(asset.textures.size());
    asset.textures.push_back(std::move(texture));
    part.has_material = true;
    part.samplers[role] = {};
    // An explicitly assigned image should retain its own colour/value rather
    // than inheriting a dark source factor that previously stood in for it.
    if (role == 0U) part.material.base_color = {1, 1, 1};
    if (role == 1U) { part.material.roughness = 1; part.material.metallic = 1; }
    if (role == 4U) part.material.emissive = {1, 1, 1};
    return true;
}

} // namespace mediaserver::render
