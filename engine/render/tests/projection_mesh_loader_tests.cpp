#include <mediaserver/render/projection_mesh_loader.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace output = mediaserver::output;
namespace render = mediaserver::render;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define MESH_CHECK(...)                                                        \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class Fixture final {
  public:
    Fixture() {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        directory_ = std::filesystem::temp_directory_path() /
                     (std::string{"mediaserver-projection-mesh-"} +
                      std::to_string(stamp));
        std::filesystem::create_directories(directory_);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    ~Fixture() {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove_all(directory_, ignored));
    }

    [[nodiscard]] std::string write(std::string_view name,
                                    std::string_view contents) const {
        const auto path = directory_ / name;
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream.write(contents.data(),
                     static_cast<std::streamsize>(contents.size()));
        stream.close();
        MESH_CHECK(stream.good());
        return path.string();
    }

  private:
    std::filesystem::path directory_;
};

[[nodiscard]] output::PhysicalSurface calibration() {
    output::PhysicalSurface value{};
    value.center = {10.0F, 2.0F, -5.0F};
    value.right = {1.0F, 0.0F, 0.0F};
    value.up = {0.0F, 1.0F, 0.0F};
    value.width = 8.0F;
    value.height = 4.5F;
    value.raster = {1920U, 1080U};
    return value;
}

void an_external_patch_retains_authored_coordinates_and_uv0() {
    Fixture files;
    const std::string path = files.write("patch.obj", "v -1 1 0\n"
                                                      "v -1 -1 0\n"
                                                      "v 1 1 0\n"
                                                      "v 1 -1 0\n"
                                                      "vt 0 1\n"
                                                      "vt 0 0\n"
                                                      "vt 1 1\n"
                                                      "vt 1 0\n"
                                                      "f 1/1 2/2 3/3\n"
                                                      "f 3/3 2/2 4/4\n");

    const auto loaded = render::load_projection_mesh(path, calibration(), 2.0F);
    MESH_CHECK(loaded.has_value());
    MESH_CHECK(loaded.value().indices.size() == 6U);
    MESH_CHECK(loaded.value().vertices.size() == 6U);
    const auto& first = loaded.value().vertices.front();
    MESH_CHECK(first.position.x == 8.0F);
    MESH_CHECK(first.position.y == 4.0F);
    MESH_CHECK(first.position.z == -5.0F);
    MESH_CHECK(first.destination_u == 0.0F);
    MESH_CHECK(first.destination_v == 0.0F);
}

void split_patches_keep_the_same_external_origin() {
    Fixture files;
    const std::string left =
        files.write("left.obj", "v -2 1 0\nv -2 -1 0\nv 0 1 0\n"
                                "vt 0 1\nvt 0 0\nvt 1 1\nf 1/1 2/2 3/3\n");
    const std::string right =
        files.write("right.obj", "v 0 1 0\nv 0 -1 0\nv 2 1 0\n"
                                 "vt 0 1\nvt 0 0\nvt 1 1\nf 1/1 2/2 3/3\n");

    const auto a = render::load_projection_mesh(left, calibration());
    const auto b = render::load_projection_mesh(right, calibration());
    MESH_CHECK(a.has_value());
    MESH_CHECK(b.has_value());
    MESH_CHECK(a.value().vertices[2U].position ==
               b.value().vertices[0U].position);
}

void a_mesh_without_uv0_is_rejected() {
    Fixture files;
    const std::string path =
        files.write("missing.obj", "v 0 1 0\nv 0 -1 0\nv 1 1 0\nf 1 2 3\n");
    const auto loaded = render::load_projection_mesh(path, calibration());
    MESH_CHECK(!loaded.has_value());
    MESH_CHECK(loaded.error() ==
               render::ProjectionMeshLoadError::MissingOrDegenerateUv);
}

void every_triangle_corner_requires_authored_uv0() {
    Fixture files;
    // Numerically this triangle has non-zero UV area because the missing third
    // corner becomes MeshVertex's ordinary (0,0) default. Presence metadata is
    // what lets projection import reject it instead of mistaking that default
    // for an authored coordinate.
    const std::string partial_obj =
        files.write("partial.obj", "v 0 1 0\nv 0 -1 0\nv 1 1 0\n"
                                   "vt 0.2 0.2\nvt 0.8 0.2\nf 1/1 2/2 3\n");
    const auto obj = render::load_projection_mesh(partial_obj, calibration());
    MESH_CHECK(!obj.has_value());
    MESH_CHECK(obj.error() ==
               render::ProjectionMeshLoadError::MissingOrDegenerateUv);

    const std::string missing_gltf = files.write("missing.gltf",
                                                 R"({
  "asset": {"version": "2.0"},
  "buffers": [{
    "uri": "data:application/octet-stream;base64,AAAAAAAAgD8AAAAAAAAAAAAAgL8AAAAAAACAPwAAgD8AAAAA",
    "byteLength": 36
  }],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "accessors": [{
    "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"
  }],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "nodes": [{"mesh": 0}],
  "scenes": [{"nodes": [0]}],
  "scene": 0
})");
    const auto gltf = render::load_projection_mesh(missing_gltf, calibration());
    MESH_CHECK(!gltf.has_value());
    MESH_CHECK(gltf.error() ==
               render::ProjectionMeshLoadError::MissingOrDegenerateUv);
}

void uv0_outside_the_connector_is_rejected() {
    Fixture files;
    const std::string path =
        files.write("outside.obj", "v 0 1 0\nv 0 -1 0\nv 1 1 0\n"
                                   "vt 0 1\nvt 0 0\nvt 2 1\nf 1/1 2/2 3/3\n");
    const auto loaded = render::load_projection_mesh(path, calibration());
    MESH_CHECK(!loaded.has_value());
    MESH_CHECK(loaded.error() ==
               render::ProjectionMeshLoadError::UvOutsideConnector);
}

void invalid_scale_and_capacity_are_rejected() {
    Fixture files;
    const std::string path =
        files.write("patch.obj", "v 0 1 0\nv 0 -1 0\nv 1 1 0\nv 1 -1 0\n"
                                 "vt 0 1\nvt 0 0\nvt 1 1\nvt 1 0\n"
                                 "f 1/1 2/2 3/3\nf 3/3 2/2 4/4\n");
    const auto scale = render::load_projection_mesh(path, calibration(), 0.0F);
    MESH_CHECK(!scale.has_value());
    MESH_CHECK(scale.error() ==
               render::ProjectionMeshLoadError::InvalidCalibration);
    const auto capacity = render::load_projection_mesh(
        path, calibration(), 1.0F, render::ProjectionMeshLoadLimits{3U, 3U});
    MESH_CHECK(!capacity.has_value());
    MESH_CHECK(capacity.error() ==
               render::ProjectionMeshLoadError::CapacityExceeded);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 6U> cases{{
    {"an external patch retains authored coordinates and UV0",
     &an_external_patch_retains_authored_coordinates_and_uv0},
    {"split patches keep the same external origin",
     &split_patches_keep_the_same_external_origin},
    {"a mesh without UV0 is rejected", &a_mesh_without_uv0_is_rejected},
    {"every triangle corner requires authored UV0",
     &every_triangle_corner_requires_authored_uv0},
    {"UV0 outside the connector is rejected",
     &uv0_outside_the_connector_is_rejected},
    {"invalid scale and capacity are rejected",
     &invalid_scale_and_capacity_are_rejected},
}};

} // namespace

int main() {
    std::size_t failures{};
    for (const Case& test : cases) {
        try {
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << cases.size() - failures << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
