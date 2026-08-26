#include "project_asset_import.hpp"
#include "project_asset_import_worker.hpp"

#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/assets/content_identity.hpp>
#include <mediaserver/assets/content_resolver.hpp>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <stop_token>
#include <thread>

namespace {

namespace app = mediaserver::app;
namespace assets = mediaserver::assets;
namespace doc = mediaserver::doc;
namespace fs = std::filesystem;

#define IMPORT_CHECK(condition)                                             \
    do {                                                                    \
        if (!(condition)) {                                                 \
            throw std::runtime_error{"check failed: " #condition};         \
        }                                                                   \
    } while (false)

[[nodiscard]] std::string path_utf8(const fs::path& path) {
    const std::u8string encoded = path.u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const char8_t byte : encoded) {
        result.push_back(static_cast<char>(byte));
    }
    return result;
}

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

[[nodiscard]] assets::MediaAsset media(const fs::path& source) {
    assets::MediaAsset asset{};
    asset.display_name = source.filename().string();
    asset.source = path_utf8(source);
    asset.dimensions = assets::PixelDimensions{1'920U, 1'080U};
    asset.frame_count = 240U;
    asset.frame_rate = assets::MediaFrameRate{24U, 1U};
    return asset;
}

void a_self_contained_import_installs_blob_alias_and_atomic_manifest() {
    TemporaryDirectory temporary;
    const fs::path source_root =
        temporary.path() / fs::path{L"\u5a92\u9ad4-source"};
    fs::create_directory(source_root);
    const fs::path source = source_root / "clip.mp4";
    const fs::path store = temporary.path() / "store";
    write(source, "verified portable video bytes");

    auto imported = app::prepare_project_asset_import(
        assets::Asset{media(source)}, store);
    IMPORT_CHECK(imported);
    const assets::AssetContent& content =
        assets::asset_content(imported.value());
    IMPORT_CHECK(content.policy ==
                 assets::AssetDistributionPolicy::SharedStore);
    IMPORT_CHECK(content.identity.has_value());

    const fs::path blob = store /
        assets::shared_store_blob_relative_path(*content.identity);
    const auto alias_relative = assets::shared_store_relative_path(
        *content.identity, path_utf8(source));
    IMPORT_CHECK(alias_relative.has_value());
    const fs::path alias = store / *alias_relative;
    IMPORT_CHECK(fs::is_regular_file(blob));
    IMPORT_CHECK(fs::is_regular_file(alias));
    const auto blob_verified = assets::verify_file(blob, *content.identity);
    const auto alias_verified = assets::verify_file(alias, *content.identity);
    IMPORT_CHECK(blob_verified);
    IMPORT_CHECK(alias_verified);
    IMPORT_CHECK(blob_verified.value() ==
                 assets::ContentVerification::Verified);
    IMPORT_CHECK(alias_verified.value() ==
                 assets::ContentVerification::Verified);

    const doc::Node node = app::make_project_asset_node(
        doc::NodeId{42U}, imported.value());
    IMPORT_CHECK(node.properties.contains(assets::schema::content_sha256));
    IMPORT_CHECK(node.properties.contains(
        assets::schema::content_size_bytes));
    IMPORT_CHECK(node.properties.contains(
        assets::schema::distribution_policy));
    doc::Root root;
    root = root.set(node);
    assets::AssetModel model;
    model.reset(root);
    const assets::AssetRegistry registry = model.evaluate();
    const assets::Asset* const evaluated =
        registry.asset(doc::NodeId{42U});
    IMPORT_CHECK(evaluated != nullptr);
    IMPORT_CHECK(assets::asset_content(*evaluated) == content);
}

void a_dependency_capable_model_is_identified_but_not_mislabelled_portable() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "compound.gltf";
    write(source, "{\"asset\":{\"version\":\"2.0\"}}");
    assets::ModelAsset model{};
    model.display_name = "compound";
    model.source = source.string();

    auto imported = app::prepare_project_asset_import(
        assets::Asset{std::move(model)}, {});
    IMPORT_CHECK(imported);
    IMPORT_CHECK(assets::asset_content(imported.value()).identity.has_value());
    IMPORT_CHECK(assets::asset_content(imported.value()).policy ==
                 assets::AssetDistributionPolicy::External);
    IMPORT_CHECK(assets::asset_portability(imported.value()) ==
                 assets::AssetPortability::DependencyManifestRequired);
}

void a_pre_cancelled_import_never_hashes_or_creates_the_store() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "cancelled.mp4";
    const fs::path store = temporary.path() / "store";
    write(source, "bytes that must not be imported");
    std::stop_source cancelled;
    IMPORT_CHECK(cancelled.request_stop());
    const auto imported = app::prepare_project_asset_import(
        assets::Asset{media(source)}, store, cancelled.get_token());
    IMPORT_CHECK(!imported);
    IMPORT_CHECK(imported.error().code ==
                 app::ProjectAssetImportErrorCode::Cancelled);
    IMPORT_CHECK(!fs::exists(store));
}

void reimport_replaces_metadata_on_the_stable_source_identity() {
    assets::MediaAsset old_asset{};
    old_asset.display_name = "Old";
    old_asset.source = "same.mp4";
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
    mesh.source = "wall.obj";
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
    scene_model.source = "shared-wall.obj";
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
    IMPORT_CHECK(registry.asset_for_source(
                     "shared-wall.obj", assets::AssetKind::Model) ==
                 registry.asset(doc::NodeId{7U}));
    IMPORT_CHECK(registry.asset_for_source(
                     "shared-wall.obj", assets::AssetKind::ProjectionMesh) ==
                 registry.asset(doc::NodeId{19U}));

    projection.display_name = "Updated LED wall UV";
    const auto reimported = app::make_project_asset_upsert(
        root, assets::Asset{projection}, doc::NodeId{99U});
    IMPORT_CHECK(reimported.after.id == doc::NodeId{19U});
    IMPORT_CHECK(reimported.before.has_value());
    IMPORT_CHECK(reimported.before.value() == added.after);
    IMPORT_CHECK(root.get(doc::NodeId{7U}) != nullptr);
}

void background_imports_are_bounded_and_return_fenced_metadata() {
    TemporaryDirectory temporary;
    const fs::path first_source = temporary.path() / "first.mp4";
    const fs::path second_source = temporary.path() / "second.mp4";
    write(first_source, "first background payload");
    write(second_source, "second background payload");

    app::ProjectAssetImportWorker worker{temporary.path() / "store", 1U};
    IMPORT_CHECK(worker.submit(app::ProjectAssetImportJob{
                     71U, 9U, 101U,
                     assets::Asset{media(first_source)}}) ==
                 app::ProjectAssetImportSubmitResult::Accepted);
    // Capacity counts completed work until playback consumes it. This remains
    // deterministic even if the tiny first file finishes before this call.
    IMPORT_CHECK(worker.submit(app::ProjectAssetImportJob{
                     72U, 9U, 102U,
                     assets::Asset{media(second_source)}}) ==
                 app::ProjectAssetImportSubmitResult::QueueFull);

    std::optional<app::ProjectAssetImportCompletion> completed;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds{5};
    while (!completed.has_value() &&
           std::chrono::steady_clock::now() < deadline) {
        completed = worker.take_completed();
        if (!completed.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    IMPORT_CHECK(completed.has_value());
    IMPORT_CHECK(completed->token == 71U);
    IMPORT_CHECK(completed->project_generation == 9U);
    IMPORT_CHECK(completed->gesture == 101U);
    IMPORT_CHECK(completed->result);

    IMPORT_CHECK(worker.submit(app::ProjectAssetImportJob{
                     72U, 10U, 102U,
                     assets::Asset{media(second_source)}}) ==
                 app::ProjectAssetImportSubmitResult::Accepted);
}

void project_replacement_discards_waiting_import_work() {
    TemporaryDirectory temporary;
    const fs::path first_source = temporary.path() / "first.mp4";
    const fs::path second_source = temporary.path() / "second.mp4";
    const fs::path replacement_source = temporary.path() / "replacement.mp4";
    write(first_source, "first stale payload");
    write(second_source, "second stale payload");
    write(replacement_source, "new project payload");

    std::atomic_bool active_entered{};
    std::atomic_bool active_cancelled{};
    std::atomic_bool allow_cancel_return{};
    std::atomic_uint32_t calls{};
    app::ProjectAssetImportWorker worker{
        temporary.path() / "store", 2U,
        [&](assets::Asset asset, const fs::path&, std::stop_token stop) {
            const auto call =
                calls.fetch_add(1U, std::memory_order_relaxed);
            if (call == 0U) {
                active_entered.store(true, std::memory_order_release);
                while (!stop.stop_requested()) {
                    std::this_thread::yield();
                }
                active_cancelled.store(true, std::memory_order_release);
                while (!allow_cancel_return.load(
                    std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                return app::ProjectAssetImportResult::failure(
                    app::ProjectAssetImportError{
                        app::ProjectAssetImportErrorCode::Cancelled, {}});
            }
            return app::ProjectAssetImportResult::success(std::move(asset));
        }};
    struct ReleaseOnFailure final {
        std::atomic_bool& release;
        ~ReleaseOnFailure() {
            release.store(true, std::memory_order_release);
        }
    };
    [[maybe_unused]] ReleaseOnFailure release_on_failure{
        allow_cancel_return};
    IMPORT_CHECK(worker.submit(app::ProjectAssetImportJob{
                     1U, 3U, 11U,
                     assets::Asset{media(first_source)}}) ==
                 app::ProjectAssetImportSubmitResult::Accepted);
    IMPORT_CHECK(worker.submit(app::ProjectAssetImportJob{
                     2U, 3U, 12U,
                     assets::Asset{media(second_source)}}) ==
                 app::ProjectAssetImportSubmitResult::Accepted);

    const auto active_deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds{2};
    while (!active_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < active_deadline) {
        std::this_thread::yield();
    }
    IMPORT_CHECK(active_entered.load(std::memory_order_acquire));

    worker.discard_waiting();
    worker.discard_waiting();
    // At most the single operation already executing can retain a slot. Queued
    // and completed work from the old generation cannot block the replacement
    // project's first import.
    const auto replacement_submitted =
        worker.submit(app::ProjectAssetImportJob{
            3U, 4U, 13U,
            assets::Asset{media(replacement_source)}});
    // The cancelled active import still owns exactly one slot until it
    // unwinds; repeated discard must neither release it early nor underflow.
    const auto over_capacity = worker.submit(app::ProjectAssetImportJob{
        4U, 4U, 14U,
        assets::Asset{media(replacement_source)}});
    allow_cancel_return.store(true, std::memory_order_release);
    IMPORT_CHECK(replacement_submitted ==
                 app::ProjectAssetImportSubmitResult::Accepted);
    IMPORT_CHECK(over_capacity ==
                 app::ProjectAssetImportSubmitResult::QueueFull);

    std::optional<app::ProjectAssetImportCompletion> completed;
    const auto completion_deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds{2};
    while (!completed.has_value() &&
           std::chrono::steady_clock::now() < completion_deadline) {
        completed = worker.take_completed();
        if (!completed.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    IMPORT_CHECK(active_cancelled.load(std::memory_order_acquire));
    IMPORT_CHECK(completed.has_value());
    IMPORT_CHECK(completed->token == 3U);
    IMPORT_CHECK(completed->project_generation == 4U);
    IMPORT_CHECK(completed->result);
    IMPORT_CHECK(!worker.take_completed().has_value());
    IMPORT_CHECK(calls.load(std::memory_order_relaxed) == 2U);
}

void worker_destruction_cancels_and_joins_the_active_import() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "blocked.mp4";
    write(source, "blocked payload");
    std::atomic_bool entered{};
    std::atomic_bool cancelled{};
    const auto started = std::chrono::steady_clock::now();
    {
        app::ProjectAssetImportWorker worker{
            temporary.path() / "store", 1U,
            [&](assets::Asset asset, const fs::path&,
                std::stop_token stop) {
                entered.store(true, std::memory_order_release);
                while (!stop.stop_requested()) {
                    std::this_thread::yield();
                }
                cancelled.store(true, std::memory_order_release);
                return app::ProjectAssetImportResult::success(
                    std::move(asset));
            }};
        IMPORT_CHECK(worker.submit(app::ProjectAssetImportJob{
                         1U, 1U, 1U,
                         assets::Asset{media(source)}}) ==
                     app::ProjectAssetImportSubmitResult::Accepted);
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds{2};
        while (!entered.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        IMPORT_CHECK(entered.load(std::memory_order_acquire));
    }
    IMPORT_CHECK(cancelled.load(std::memory_order_acquire));
    IMPORT_CHECK(std::chrono::steady_clock::now() - started <
                 std::chrono::seconds{2});
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr Case cases[]{
    {"a self-contained import installs blob, alias, and atomic manifest",
     &a_self_contained_import_installs_blob_alias_and_atomic_manifest},
    {"a dependency-capable model is identified but not mislabelled portable",
     &a_dependency_capable_model_is_identified_but_not_mislabelled_portable},
    {"a pre-cancelled import never hashes or creates the store",
     &a_pre_cancelled_import_never_hashes_or_creates_the_store},
    {"reimport replaces metadata on the stable source identity",
     &reimport_replaces_metadata_on_the_stable_source_identity},
    {"projection meshes are first-class asset records",
     &projection_meshes_are_first_class_asset_records},
    {"one OBJ can be a scene model and a projection mesh",
     &one_obj_can_be_a_scene_model_and_a_projection_mesh},
    {"background imports are bounded and return fenced metadata",
     &background_imports_are_bounded_and_return_fenced_metadata},
    {"project replacement discards waiting import work",
     &project_replacement_discards_waiting_import_work},
    {"worker destruction cancels and joins the active import",
     &worker_destruction_cancels_and_joins_the_active_import},
};

} // namespace

int main() {
    std::size_t failures{};
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
    std::cout << (std::size(cases) - failures) << '/' << std::size(cases)
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
