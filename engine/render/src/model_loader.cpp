#include <mediaserver/render/model_loader.hpp>

#include <mediaserver/scene/math.hpp>

// cgltf and tiny_obj_loader are single-header libraries; this is the one
// translation unit that compiles their implementations. Neither is clean under
// this project's warnings-as-errors, so both are included with warnings
// silenced. The code below stays fully checked.
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
#include <tiny_obj_loader.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>

namespace mediaserver::render {
namespace {

namespace scene = mediaserver::scene;

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) {
    if (suffix.size() > text.size()) {
        return false;
    }
    auto lower = [](char c) {
        return static_cast<char>(
            (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    };
    for (std::size_t i = 0U; i < suffix.size(); ++i) {
        if (lower(text[text.size() - suffix.size() + i]) != lower(suffix[i])) {
            return false;
        }
    }
    return true;
}

// A point through a column-major 4x4 (cgltf's layout), w assumed 1.
[[nodiscard]] scene::Vec3 transform_point(const cgltf_float m[16], scene::Vec3 p) {
    return scene::Vec3{
        m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
        m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
        m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}

// A direction through the upper 3x3 (no translation). Good enough for normals
// under rigid and uniform-scale transforms, which covers most imported models.
[[nodiscard]] scene::Vec3 transform_dir(const cgltf_float m[16], scene::Vec3 d) {
    return scene::Vec3{
        m[0] * d.x + m[4] * d.y + m[8] * d.z,
        m[1] * d.x + m[5] * d.y + m[9] * d.z,
        m[2] * d.x + m[6] * d.y + m[10] * d.z};
}

// Replaces every vertex normal with its triangle's face normal. Used when a
// file carries no normals; vertices are already expanded per index, so flat
// shading is exact rather than an approximation of shared normals.
void flat_normals(scene::MeshData& mesh) {
    for (std::size_t i = 0U; i + 2U < mesh.indices.size(); i += 3U) {
        const auto a = mesh.vertices[mesh.indices[i]].position;
        const auto b = mesh.vertices[mesh.indices[i + 1U]].position;
        const auto c = mesh.vertices[mesh.indices[i + 2U]].position;
        const auto normal =
            scene::normalize(scene::cross(b - a, c - a), scene::Vec3{0.0F, 1.0F, 0.0F});
        mesh.vertices[mesh.indices[i]].normal = normal;
        mesh.vertices[mesh.indices[i + 1U]].normal = normal;
        mesh.vertices[mesh.indices[i + 2U]].normal = normal;
    }
}

[[nodiscard]] bool load_gltf(const std::string& path, scene::MeshData& into) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        return false;
    }
    struct Guard final {
        cgltf_data* data;
        ~Guard() { cgltf_free(data); }
    } guard{data};
    if (cgltf_load_buffers(&options, data, path.c_str())
        != cgltf_result_success) {
        return false;
    }

    bool any_normals = false;
    for (cgltf_size n = 0U; n < data->nodes_count; ++n) {
        const cgltf_node& node = data->nodes[n];
        if (node.mesh == nullptr) {
            continue;
        }
        cgltf_float world[16];
        cgltf_node_transform_world(&node, world);
        for (cgltf_size p = 0U; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node.mesh->primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) {
                continue;
            }
            const cgltf_accessor* positions = nullptr;
            const cgltf_accessor* normals = nullptr;
            const cgltf_accessor* uvs = nullptr;
            for (cgltf_size a = 0U; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                if (attr.type == cgltf_attribute_type_position) {
                    positions = attr.data;
                } else if (attr.type == cgltf_attribute_type_normal) {
                    normals = attr.data;
                } else if (
                    attr.type == cgltf_attribute_type_texcoord
                    && attr.index == 0) {
                    uvs = attr.data;
                }
            }
            if (positions == nullptr) {
                continue;
            }
            const auto base =
                static_cast<core::u32>(into.vertices.size());
            for (cgltf_size v = 0U; v < positions->count; ++v) {
                scene::MeshVertex vertex{};
                float value[3]{};
                cgltf_accessor_read_float(positions, v, value, 3U);
                vertex.position = transform_point(
                    world, scene::Vec3{value[0], value[1], value[2]});
                if (normals != nullptr) {
                    cgltf_accessor_read_float(normals, v, value, 3U);
                    vertex.normal = scene::normalize(
                        transform_dir(
                            world, scene::Vec3{value[0], value[1], value[2]}),
                        scene::Vec3{0.0F, 1.0F, 0.0F});
                    any_normals = true;
                }
                if (uvs != nullptr) {
                    float uv[2]{};
                    cgltf_accessor_read_float(uvs, v, uv, 2U);
                    vertex.u = uv[0];
                    vertex.v = uv[1];
                }
                into.vertices.push_back(vertex);
            }
            if (prim.indices != nullptr) {
                for (cgltf_size i = 0U; i < prim.indices->count; ++i) {
                    into.indices.push_back(
                        base
                        + static_cast<core::u32>(
                            cgltf_accessor_read_index(prim.indices, i)));
                }
            } else {
                for (cgltf_size i = 0U; i < positions->count; ++i) {
                    into.indices.push_back(base + static_cast<core::u32>(i));
                }
            }
        }
    }

    if (into.vertices.empty() || into.indices.empty()) {
        return false;
    }
    if (!any_normals) {
        flat_normals(into);
    }
    return true;
}

[[nodiscard]] bool load_obj(const std::string& path, scene::MeshData& into) {
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;
    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, config)) {
        return false;
    }
    const tinyobj::attrib_t& attrib = reader.GetAttrib();
    bool any_normals = false;
    for (const auto& shape : reader.GetShapes()) {
        for (const auto& index : shape.mesh.indices) {
            scene::MeshVertex vertex{};
            const auto vi = static_cast<std::size_t>(index.vertex_index);
            vertex.position = scene::Vec3{
                attrib.vertices[3U * vi + 0U],
                attrib.vertices[3U * vi + 1U],
                attrib.vertices[3U * vi + 2U]};
            if (index.normal_index >= 0) {
                const auto ni = static_cast<std::size_t>(index.normal_index);
                vertex.normal = scene::Vec3{
                    attrib.normals[3U * ni + 0U],
                    attrib.normals[3U * ni + 1U],
                    attrib.normals[3U * ni + 2U]};
                any_normals = true;
            }
            if (index.texcoord_index >= 0) {
                const auto ti = static_cast<std::size_t>(index.texcoord_index);
                vertex.u = attrib.texcoords[2U * ti + 0U];
                // OBJ texture space is bottom-up; flip to the top-left origin
                // the rest of the pipeline uses.
                vertex.v = 1.0F - attrib.texcoords[2U * ti + 1U];
            }
            into.indices.push_back(static_cast<core::u32>(into.vertices.size()));
            into.vertices.push_back(vertex);
        }
    }

    if (into.vertices.empty() || into.indices.empty()) {
        return false;
    }
    if (!any_normals) {
        flat_normals(into);
    }
    return true;
}

} // namespace

bool load_model(const std::string& source, scene::MeshData& into) {
    into.vertices.clear();
    into.indices.clear();
    if (ends_with(source, ".gltf") || ends_with(source, ".glb")) {
        return load_gltf(source, into);
    }
    if (ends_with(source, ".obj")) {
        return load_obj(source, into);
    }
    return false;
}

} // namespace mediaserver::render
