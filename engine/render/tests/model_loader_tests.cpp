#include <mediaserver/render/model_loader.hpp>

#include <mediaserver/scene/geometry.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

// The model loader, checked with no GPU. It produces the same MeshData the
// renderer uploads, so this proves an imported file becomes geometry - vertices,
// indices, and normals (generated flat when the file has none) - without needing
// a device. glTF is exercised live; OBJ is simple enough to write and assert
// here.
namespace {

namespace render = mediaserver::render;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define MODEL_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] std::string write_temp(const std::string& name, const std::string& body) {
    const auto path = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream out{path, std::ios::trunc};
    out << body;
    out.close();
    check(static_cast<bool>(out), "wrote a temp model", __LINE__);
    return path;
}

void an_obj_quad_loads_to_geometry_with_normals() {
    // A unit quad on the XY plane, two triangles, no normals in the file.
    const auto path = write_temp(
        "mediaserver_model.obj",
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nf 1 3 4\n");

    scene::MeshData mesh{};
    MODEL_CHECK(render::load_model(path, mesh));
    // Each face index expands to its own vertex: two triangles, six of each.
    MODEL_CHECK(mesh.vertices.size() == 6U);
    MODEL_CHECK(mesh.indices.size() == 6U);
    // Normals were absent, so they are the flat face normal: +Z for this
    // counter-clockwise winding.
    for (const auto& vertex : mesh.vertices) {
        MODEL_CHECK(std::abs(vertex.normal.z - 1.0F) <= 1.0e-5F);
        MODEL_CHECK(std::abs(vertex.normal.x) <= 1.0e-5F);
        MODEL_CHECK(std::abs(vertex.normal.y) <= 1.0e-5F);
    }
    // Loading clears the target first, so a reload does not accumulate.
    MODEL_CHECK(render::load_model(path, mesh));
    MODEL_CHECK(mesh.vertices.size() == 6U);
}

void an_unloadable_source_is_refused() {
    scene::MeshData mesh{};
    // Unknown extension.
    MODEL_CHECK(!render::load_model("model.fbx", mesh));
    // A missing file of a known extension.
    MODEL_CHECK(!render::load_model("no_such_model_12345.obj", mesh));
    // Empty source.
    MODEL_CHECK(!render::load_model("", mesh));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 2U> cases{{
    {"an obj quad loads to geometry with normals",
     &an_obj_quad_loads_to_geometry_with_normals},
    {"an unloadable source is refused", &an_unloadable_source_is_refused},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
