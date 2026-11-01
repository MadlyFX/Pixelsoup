#include <ajfx/mesh.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string_view>

namespace ajfx {
namespace {

constexpr char mesh_magic[4] = {'A', 'J', 'M', 'S'};
constexpr std::size_t header_bytes = 16U;
constexpr std::size_t vertex_bytes = 32U;
constexpr std::string_view mesh_prefix = "meshes/";

std::string quoted(std::string_view text) {
    return "'" + std::string{text} + "'";
}

// Why a mesh is unusable, or nothing.
std::optional<std::string> problem(const std::vector<MeshVertex>& vertices, const std::vector<std::uint32_t>& indices) {
    if (vertices.size() < 3U || vertices.size() > max_mesh_vertices) {
        return "has " + std::to_string(vertices.size()) + " vertices; it needs 3 to " +
               std::to_string(max_mesh_vertices);
    }
    if (indices.empty() || indices.size() % 3U != 0U || indices.size() > max_mesh_indices) {
        return "has " + std::to_string(indices.size()) + " indices; it needs whole triangles, at most " +
               std::to_string(max_mesh_indices);
    }
    for (const std::uint32_t index : indices) {
        if (index >= vertices.size()) {
            return "has an index past its vertices";
        }
    }
    for (const MeshVertex& v : vertices) {
        for (const float f : {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, v.u, v.v}) {
            if (!std::isfinite(f)) {
                return "has a number that is not finite";
            }
        }
    }
    return std::nullopt;
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (std::uint32_t i = 0U; i < 4U; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
    }
}

void put_f32(std::vector<std::uint8_t>& out, float value) {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, 4U);
    put_u32(out, bits);
}

std::uint32_t get_u32(std::span<const std::uint8_t> bytes, std::size_t at) {
    std::uint32_t value = 0U;
    for (std::uint32_t i = 0U; i < 4U; ++i) {
        value |= static_cast<std::uint32_t>(bytes[at + i]) << (8U * i);
    }
    return value;
}

float get_f32(std::span<const std::uint8_t> bytes, std::size_t at) {
    const std::uint32_t bits = get_u32(bytes, at);
    float value = 0.0F;
    std::memcpy(&value, &bits, 4U);
    return value;
}

// A mesh the effect needs, and who needs it, ready for the message when the
// package does not hold it. Two things name a mesh: a renderer that draws
// one, and a prop shaped by one.
struct MeshWant final {
    std::string name;
    std::string wanted_by;
};

std::vector<MeshWant> wanted_meshes(const Effect& effect) {
    std::vector<MeshWant> wants;
    for (const Emitter& emitter : effect.emitters) {
        const std::string who = "emitter " + quoted(emitter.name);
        if (emitter.renderer.geometry == Geometry::Mesh) {
            wants.push_back({emitter.renderer.mesh, who + " draws mesh " + quoted(emitter.renderer.mesh)});
        }
        const RigidPhysics* physics = rigid_physics(emitter);
        if (physics == nullptr) {
            continue;
        }
        for (const RigidBodyKind& body : physics->bodies) {
            // A kind that names a model collides as that model's hull; one
            // that names none wraps the renderer's, which is wanted above.
            if (!body.mesh.empty()) {
                wants.push_back({body.mesh, who + " has a body kind shaped by mesh " + quoted(body.mesh)});
            }
        }
        for (const RigidProp& prop : physics->props) {
            // Whenever a prop names a model it is needed: to collide as, for
            // the mesh and hull shapes, and to draw by in every case.
            if (!prop.mesh.empty()) {
                wants.push_back({prop.mesh, who + " has a prop shaped by mesh " + quoted(prop.mesh)});
            }
        }
    }
    return wants;
}

} // namespace

Result<MeshData> make_mesh(std::vector<MeshVertex> vertices, std::vector<std::uint32_t> indices) {
    if (std::optional<std::string> why = problem(vertices, indices)) {
        return Error{ErrorCode::InvalidEffect, "a mesh " + *why};
    }
    return MeshData{std::move(vertices), std::move(indices)};
}

MeshData cube_mesh() {
    // Each face: its normal n and two axes with u x v = n, so the corners
    // -u-v, u-v, u+v, -u+v wind counter-clockwise seen from outside.
    struct Face final {
        Vec3 n;
        Vec3 u;
        Vec3 v;
    };
    const Face faces[6] = {
        {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}},
        {{-1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.0F}},
        {{0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F}},
        {{0.0F, -1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}},
        {{0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}},
        {{0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}},
    };
    const float corners[4][2] = {{-1.0F, -1.0F}, {1.0F, -1.0F}, {1.0F, 1.0F}, {-1.0F, 1.0F}};
    MeshData mesh;
    for (const Face& face : faces) {
        const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
        for (const auto& c : corners) {
            mesh.vertices.push_back(
                {(face.n + face.u * c[0] + face.v * c[1]) * 0.5F, face.n, (c[0] + 1.0F) * 0.5F, (c[1] + 1.0F) * 0.5F});
        }
        for (const std::uint32_t k : {0U, 1U, 2U, 0U, 2U, 3U}) {
            mesh.indices.push_back(first + k);
        }
    }
    return mesh;
}

std::vector<std::uint8_t> write_mesh(const MeshData& mesh) {
    std::vector<std::uint8_t> out(std::begin(mesh_magic), std::end(mesh_magic));
    put_u32(out, mesh_format_version);
    put_u32(out, static_cast<std::uint32_t>(mesh.vertices.size()));
    put_u32(out, static_cast<std::uint32_t>(mesh.indices.size()));
    for (const MeshVertex& v : mesh.vertices) {
        for (const float f : {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, v.u, v.v}) {
            put_f32(out, f);
        }
    }
    for (const std::uint32_t index : mesh.indices) {
        put_u32(out, index);
    }
    return out;
}

Result<MeshData> read_mesh(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < header_bytes || std::memcmp(bytes.data(), mesh_magic, 4U) != 0) {
        return Error{ErrorCode::MalformedPackage, "mesh has no AJMS header"};
    }
    const std::uint32_t version = get_u32(bytes, 4U);
    if (version > mesh_format_version) {
        return Error{ErrorCode::UnsupportedVersion,
                     "mesh version " + std::to_string(version) + " is newer than this library reads"};
    }
    const std::uint64_t vertex_count = get_u32(bytes, 8U);
    const std::uint64_t index_count = get_u32(bytes, 12U);
    if (vertex_count > max_mesh_vertices || index_count > max_mesh_indices ||
        bytes.size() != header_bytes + vertex_count * vertex_bytes + index_count * 4U) {
        return Error{ErrorCode::MalformedPackage, "mesh is not as long as its counts say"};
    }
    MeshData mesh;
    mesh.vertices.resize(vertex_count);
    std::size_t at = header_bytes;
    for (MeshVertex& v : mesh.vertices) {
        float f[8];
        for (float& value : f) {
            value = get_f32(bytes, at);
            at += 4U;
        }
        v = MeshVertex{{f[0], f[1], f[2]}, {f[3], f[4], f[5]}, f[6], f[7]};
    }
    mesh.indices.resize(index_count);
    for (std::uint32_t& index : mesh.indices) {
        index = get_u32(bytes, at);
        at += 4U;
    }
    if (std::optional<std::string> why = problem(mesh.vertices, mesh.indices)) {
        return Error{ErrorCode::MalformedPackage, "mesh " + *why};
    }
    return mesh;
}

std::vector<PackageEntry> mesh_package_entries(const MeshSet& meshes) {
    std::vector<PackageEntry> entries;
    for (const auto& [name, mesh] : meshes) {
        entries.push_back({std::string{mesh_prefix} + name, write_mesh(mesh)});
    }
    return entries;
}

Result<bool> check_meshes(const Effect& effect, const MeshSet& meshes) {
    for (const MeshWant& want : wanted_meshes(effect)) {
        if (!meshes.contains(want.name)) {
            return Error{ErrorCode::MissingMesh, want.wanted_by + ", which the package does not hold"};
        }
    }
    return true;
}

Result<MeshSet> read_package_meshes(std::span<const std::uint8_t> package, const Effect& effect) {
    Result<std::vector<PackageEntry>> entries = read_container(package);
    if (!entries) {
        return entries.error();
    }
    MeshSet meshes;
    for (const MeshWant& want : wanted_meshes(effect)) {
        const std::string& name = want.name;
        if (meshes.contains(name)) {
            continue;
        }
        const std::string entry_name = std::string{mesh_prefix} + name;
        const auto entry = std::find_if(entries.value().begin(), entries.value().end(),
                                        [&](const PackageEntry& e) { return e.name == entry_name; });
        if (entry == entries.value().end()) {
            continue; // check_meshes names the emitter
        }
        Result<MeshData> mesh = read_mesh(entry->data);
        if (!mesh) {
            return Error{mesh.error().code, quoted(entry_name) + ": " + mesh.error().message};
        }
        meshes.emplace(name, std::move(mesh).value());
    }
    if (Result<bool> checked = check_meshes(effect, meshes); !checked) {
        return checked.error();
    }
    return meshes;
}

} // namespace ajfx
