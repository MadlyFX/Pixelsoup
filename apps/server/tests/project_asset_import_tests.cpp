#include "project_asset_import.hpp"

#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/assets/asset_path.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace {

namespace app = mediaserver::app;
namespace assets = mediaserver::assets;
namespace doc = mediaserver::doc;
namespace fs = std::filesystem;

#define IMPORT_CHECK(condition)                                             \
    do {                                                                    \
        if (!(condition)) {                                                 \
            throw std::runtime_error{"check failed: " #condition};          \
        }                                                                   \
    } while (false)

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = fs::temp_directory_path() /
            ("mediaserver_project_import_" + std::to_string(stamp));
        std::error_code error;
        if (!fs::create_directory(path_, error)) {
            throw std::runtime_error{"could not create temporary directory"};
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code ignored;
        static_cast<void>(fs::remove_all(path_, ignored));
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void write(const fs::path& path, std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        throw std::runtime_error{"could not write test asset"};
    }
}

[[nodiscard]] std::string absolute_source(std::string_view name) {
#if defined(_WIN32)
    return std::string{"D:\\show\\"} + std::string{name};
#else
    return std::string{"/show/"} + std::string{name};
#endif
}

[[nodiscard]] assets::MediaAsset media(std::string source) {
    assets::MediaAsset asset{};
    asset.display_name = "Clip";
    asset.source = std::move(source);
    asset.dimensions = assets::PixelDimensions{1'920U, 1'080U};
    asset.frame_count = 240U;
    asset.frame_rate = assets::MediaFrameRate{24U, 1U};
    return asset;
}

void an_import_references_the_file_and_never_copies_it() {
    TemporaryDirectory temporary;
    // A non-ASCII directory proves the UTF-8 document text survives the round
    // trip through a native path on every platform.
    const fs::path source_root =
        temporary.path() / fs::path{L"\u5a92\u9ad4-source"};
    fs::create_directory(source_root);
    const fs::path source = source_root / "clip.mp4";
    write(source, "frames");

    const std::string authored = assets::asset_source_from_path(source);
    auto prepared =
        app::prepare_project_asset_import(assets::Asset{media(authored)});
    IMPORT_CHECK(static_cast<bool>(prepared));
    IMPORT_CHECK(assets::asset_source(prepared.value()) == authored);

    // Nothing beside the original may appear: the whole point is that a 40 GB
    // clip costs one directory entry, not two.
    std::size_t entries = 0U;
    for (const auto& entry : fs::recursive_directory_iterator{
             temporary.path()}) {
        static_cast<void>(entry);
        ++entries;
    }
    IMPORT_CHECK(entries == 2U);

    const doc::Node node = app::make_project_asset_node(
        doc::NodeId{5U}, prepared.value());
    IMPORT_CHECK(std::get<std::string>(node.properties.at(
                     std::string{assets::schema::source})) == authored);
    IMPORT_CHECK(std::get<std::int64_t>(node.properties.at(
                     std::string{assets::schema::media_frame_count})) == 240);
}

void a_relative_source_is_refused_before_it_reaches_the_document() {
    auto prepared =
        app::prepare_project_asset_import(assets::Asset{media("clip.mp4")});
    IMPORT_CHECK(!prepared);
    IMPORT_CHECK(prepared.error().code ==
                 app::ProjectAssetImportErrorCode::InvalidSource);
}

void a_source_this_machine_cannot_open_is_refused() {
    auto prepared = app::prepare_project_asset_import(
        assets::Asset{media(absolute_source("definitely-not-here-4f1c.mp4"))});
    IMPORT_CHECK(!prepared);
    IMPORT_CHECK(prepared.error().code ==
                 app::ProjectAssetImportErrorCode::SourceMissing);
}

void a_directory_is_not_an_importable_asset() {
    TemporaryDirectory temporary;
    auto prepared = app::prepare_project_asset_import(assets::Asset{
        media(assets::asset_source_from_path(temporary.path()))});
    IMPORT_CHECK(!prepared);
    IMPORT_CHECK(prepared.error().code ==
                 app::ProjectAssetImportErrorCode::SourceMissing);
}

void reimport_replaces_metadata_on_the_stable_source_identity() {
    assets::MediaAsset old_asset = media(absolute_source("same.mp4"));
    old_asset.display_name = "Old";
    const doc::Node old_node = app::make_project_asset_node(
        doc::NodeId{7U}, assets::Asset{old_asset});
    doc::Root root;
    root = root.set(old_node);

    assets::MediaAsset replacement = old_asset;
    replacement.display_name = "Updated";
    replacement.frame_count = 999U;
    const auto upsert = app::make_project_asset_upsert(
        root, assets::Asset{replacement}, doc::NodeId{99U});
    IMPORT_CHECK(upsert.after.id == doc::NodeId{7U});
    IMPORT_CHECK(upsert.before.has_value());
    IMPORT_CHECK(upsert.before.value() == old_node);
    IMPORT_CHECK(std::get<std::string>(upsert.after.properties.at(
                     std::string{assets::schema::display_name})) == "Updated");
    IMPORT_CHECK(std::get<std::int64_t>(upsert.after.properties.at(
                     std::string{assets::schema::media_frame_count})) == 999);
}

void projection_meshes_are_first_class_asset_records() {
    assets::ModelAsset mesh{};
    mesh.display_name = "Wall patch";
    mesh.source = absolute_source("wall.obj");
    mesh.triangle_count = 2U;
    mesh.vertex_count = 4U;
    mesh.projection_mesh = true;
    const doc::Node node = app::make_project_asset_node(
        doc::NodeId{12U}, assets::Asset{mesh});
    IMPORT_CHECK(node.type == assets::schema::projection_mesh_type);
    IMPORT_CHECK(std::get<std::int64_t>(node.properties.at(
                     std::string{assets::schema::projection_vertex_count})) ==
                 4);
    IMPORT_CHECK(std::get<std::int64_t>(node.properties.at(
                     std::string{assets::schema::projection_triangle_count})) ==
                 2);
}

void one_obj_can_be_a_scene_model_and_a_projection_mesh() {
    assets::ModelAsset scene_model{};
    scene_model.display_name = "Scenic wall";
    scene_model.source = absolute_source("shared-wall.obj");
    const doc::Node model_node = app::make_project_asset_node(
        doc::NodeId{7U}, assets::Asset{scene_model});
    doc::Root root;
    root = root.set(model_node);

    assets::ModelAsset projection = scene_model;
    projection.display_name = "LED wall UV";
    projection.projection_mesh = true;
    projection.vertex_count = 4U;
    const auto added = app::make_project_asset_upsert(
        root, assets::Asset{projection}, doc::NodeId{19U});
    IMPORT_CHECK(added.after.id == doc::NodeId{19U});
    IMPORT_CHECK(!added.before.has_value());
    root = root.set(added.after);

    assets::AssetModel evaluated_model;
    evaluated_model.reset(root);
    const assets::AssetRegistry registry = evaluated_model.evaluate();
    IMPORT_CHECK(registry.assets.size() == 2U);
    IMPORT_CHECK(registry.asset_for_source(absolute_source("shared-wall.obj"),
                                           assets::AssetKind::Model) ==
                 registry.asset(doc::NodeId{7U}));
    IMPORT_CHECK(registry.asset_for_source(absolute_source("shared-wall.obj"),
                                           assets::AssetKind::ProjectionMesh) ==
                 registry.asset(doc::NodeId{19U}));

    projection.display_name = "Updated LED wall UV";
    const auto reimported = app::make_project_asset_upsert(
        root, assets::Asset{projection}, doc::NodeId{99U});
    IMPORT_CHECK(reimported.after.id == doc::NodeId{19U});
    IMPORT_CHECK(reimported.before.has_value());
    IMPORT_CHECK(reimported.before.value() == added.after);
    IMPORT_CHECK(root.get(doc::NodeId{7U}) != nullptr);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr Case cases[]{
    {"an import references the file and never copies it",
     &an_import_references_the_file_and_never_copies_it},
    {"a relative source is refused before it reaches the document",
     &a_relative_source_is_refused_before_it_reaches_the_document},
    {"a source this machine cannot open is refused",
     &a_source_this_machine_cannot_open_is_refused},
    {"a directory is not an importable asset",
     &a_directory_is_not_an_importable_asset},
    {"reimport replaces metadata on the stable source identity",
     &reimport_replaces_metadata_on_the_stable_source_identity},
    {"projection meshes are first-class asset records",
     &projection_meshes_are_first_class_asset_records},
    {"one OBJ can be a scene model and a projection mesh",
     &one_obj_can_be_a_scene_model_and_a_projection_mesh},
};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    const std::size_t total = std::size(cases);
    std::cout << (total - failures) << '/' << total << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
