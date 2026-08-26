#include "project_assets.hpp"

#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/assets/content_identity.hpp>
#include <mediaserver/assets/content_resolver.hpp>
#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace assets = mediaserver::assets;
namespace app = mediaserver::app;
namespace doc = mediaserver::doc;
namespace output = mediaserver::output;
namespace scene = mediaserver::scene;

#define PROJECT_ASSET_CHECK(condition)                                      \
    do {                                                                    \
        if (!(condition)) {                                                 \
            throw std::runtime_error{"check failed: " #condition};        \
        }                                                                   \
    } while (false)

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const std::u8string encoded = path.u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const char8_t byte : encoded) {
        result.push_back(static_cast<char>(byte));
    }
    return result;
}

[[nodiscard]] doc::Node asset_node(
    mediaserver::core::u64 id, std::string type, std::string source,
    const std::optional<assets::ContentIdentity>& content = std::nullopt,
    std::string_view policy = assets::schema::policy_external) {
    doc::Node node{doc::NodeId{id}, std::move(type), {}};
    node.properties.emplace(std::string{assets::schema::display_name},
                            doc::PropertyValue{std::string{"Asset"}});
    node.properties.emplace(std::string{assets::schema::source},
                            doc::PropertyValue{std::move(source)});
    node.properties.emplace(std::string{assets::schema::distribution_policy},
                            doc::PropertyValue{std::string{policy}});
    if (content.has_value()) {
        node.properties.emplace(
            std::string{assets::schema::content_sha256},
            doc::PropertyValue{assets::format_sha256(content->sha256)});
        node.properties.emplace(
            std::string{assets::schema::content_size_bytes},
            doc::PropertyValue{static_cast<std::int64_t>(content->byte_size)});
    }
    return node;
}

[[nodiscard]] doc::Node placement(mediaserver::core::u64 id,
                                  std::string_view type,
                                  std::string source,
                                  mediaserver::core::u64 asset = 0U) {
    doc::Node node{doc::NodeId{id}, std::string{type}, {}};
    node.properties.emplace(std::string{scene::schema::source},
                            doc::PropertyValue{std::move(source)});
    if (asset != 0U) {
        node.properties.emplace(
            std::string{scene::schema::asset_id},
            doc::PropertyValue{static_cast<std::int64_t>(asset)});
    }
    return node;
}

void manifest_contains_only_referenced_assets_and_flags_gaps() {
    doc::Root root;
    root = root.set(asset_node(10U, std::string{assets::schema::media_type},
                               "clip.mp4"));
    root = root.set(asset_node(11U, std::string{assets::schema::model_type},
                               "unused.glb"));
    root = root.set(asset_node(
        12U, std::string{assets::schema::projection_mesh_type}, "wall.obj"));
    root = root.set(placement(20U, scene::schema::media_type, "clip.mp4", 10U));
    root = root.set(placement(21U, scene::schema::model_type, "missing.glb"));
    doc::Node surface{doc::NodeId{30U},
                      std::string{output::schema::surface_type}, {}};
    surface.properties.emplace(
        std::string{output::schema::surface_shape},
        doc::PropertyValue{std::string{output::schema::shape_imported_mesh}});
    surface.properties.emplace(std::string{output::schema::mesh_source},
                               doc::PropertyValue{std::string{"legacy.obj"}});
    surface.properties.emplace(
        std::string{output::schema::mesh_asset_id},
        doc::PropertyValue{std::int64_t{12}});
    root = root.set(std::move(surface));

    assets::AssetModel model;
    model.reset(root);
    const auto manifest =
        app::collect_project_asset_manifest(root, model.evaluate());
    PROJECT_ASSET_CHECK(manifest.assets.size() == 2U);
    PROJECT_ASSET_CHECK(manifest.assets[0U].asset == doc::NodeId{10U});
    PROJECT_ASSET_CHECK(manifest.assets[1U].asset == doc::NodeId{12U});
    PROJECT_ASSET_CHECK(manifest.unregistered_references == 1U);
    PROJECT_ASSET_CHECK(manifest.unverified_assets == 2U);
    PROJECT_ASSET_CHECK(manifest.non_shared_assets == 2U);
    PROJECT_ASSET_CHECK(!manifest.shared_store_ready());
}

void legacy_source_references_choose_the_matching_asset_kind() {
    constexpr std::string_view source = "shared-wall.obj";
    doc::Root root;
    root = root.set(asset_node(10U, std::string{assets::schema::model_type},
                               std::string{source}));
    root = root.set(asset_node(
        11U, std::string{assets::schema::projection_mesh_type},
        std::string{source}));
    // Placements without asset_id are valid legacy data and must resolve by
    // both source and role, rather than whichever kind has the lower NodeId.
    root = root.set(
        placement(20U, scene::schema::model_type, std::string{source}));
    doc::Node surface{doc::NodeId{30U},
                      std::string{output::schema::surface_type}, {}};
    surface.properties.emplace(
        std::string{output::schema::surface_shape},
        doc::PropertyValue{std::string{output::schema::shape_imported_mesh}});
    surface.properties.emplace(
        std::string{output::schema::mesh_source},
        doc::PropertyValue{std::string{source}});
    root = root.set(std::move(surface));

    assets::AssetModel model;
    model.reset(root);
    const assets::AssetRegistry registry = model.evaluate();
    PROJECT_ASSET_CHECK(registry.assets.size() == 2U);
    const auto manifest = app::collect_project_asset_manifest(root, registry);
    PROJECT_ASSET_CHECK(manifest.assets.size() == 2U);
    PROJECT_ASSET_CHECK(manifest.assets[0U].asset == doc::NodeId{10U});
    PROJECT_ASSET_CHECK(manifest.assets[0U].kind == assets::AssetKind::Model);
    PROJECT_ASSET_CHECK(manifest.assets[1U].asset == doc::NodeId{11U});
    PROJECT_ASSET_CHECK(manifest.assets[1U].kind ==
                        assets::AssetKind::ProjectionMesh);
    PROJECT_ASSET_CHECK(manifest.unregistered_references == 0U);
    PROJECT_ASSET_CHECK(manifest.kind_mismatches == 0U);
}

void resolution_digest_ignores_unrelated_document_edits() {
    doc::Root root;
    root = root.set(asset_node(10U, std::string{assets::schema::media_type},
                               "clip.mp4"));
    root = root.set(
        placement(20U, scene::schema::media_type, "clip.mp4", 10U));
    assets::AssetModel model;
    model.reset(root);
    const auto original = app::collect_project_asset_manifest(
        root, model.evaluate());

    doc::Node unrelated{doc::NodeId{30U},
                        std::string{scene::schema::object_type}, {}};
    unrelated.properties.emplace(
        std::string{scene::schema::position_x}, doc::PropertyValue{1.0});
    const doc::Root edited = root.set(std::move(unrelated));
    PROJECT_ASSET_CHECK(edited.version() != root.version());
    model.reset(edited);
    const auto after_unrelated_edit =
        app::collect_project_asset_manifest(edited, model.evaluate());
    PROJECT_ASSET_CHECK(after_unrelated_edit.digest == original.digest);

    doc::Node changed_reference = *edited.get(doc::NodeId{20U});
    changed_reference.properties.insert_or_assign(
        std::string{scene::schema::source},
        doc::PropertyValue{std::string{"renamed.mp4"}});
    const doc::Root reference_edited =
        edited.set(std::move(changed_reference));
    model.reset(reference_edited);
    const auto after_reference_edit = app::collect_project_asset_manifest(
        reference_edited, model.evaluate());
    PROJECT_ASSET_CHECK(after_reference_edit.digest != original.digest);
}

void runtime_resolution_preserves_role_and_placement_identity() {
    const auto temporary = std::filesystem::temp_directory_path() /
        "mediaserver-project-assets-role-test";
    const auto store_root = temporary / "store";
    std::error_code ignored;
    std::filesystem::remove_all(temporary, ignored);
    std::filesystem::create_directories(temporary);

    const auto model_bytes = temporary / "model-bytes.glb";
    const auto projection_bytes = temporary / "projection-bytes.glb";
    {
        std::ofstream stream{model_bytes, std::ios::binary | std::ios::trunc};
        stream << "scene model identity";
    }
    {
        std::ofstream stream{projection_bytes,
                             std::ios::binary | std::ios::trunc};
        stream << "projection mesh identity";
    }
    const auto model_identity = assets::hash_file(model_bytes);
    const auto projection_identity = assets::hash_file(projection_bytes);
    PROJECT_ASSET_CHECK(model_identity);
    PROJECT_ASSET_CHECK(projection_identity);
    PROJECT_ASSET_CHECK(model_identity.value() != projection_identity.value());
    const auto model_stored = store_root /
        assets::shared_store_blob_relative_path(model_identity.value());
    const auto projection_stored = store_root /
        assets::shared_store_blob_relative_path(projection_identity.value());
    std::filesystem::create_directories(model_stored.parent_path());
    std::filesystem::create_directories(projection_stored.parent_path());
    std::filesystem::copy_file(model_bytes, model_stored);
    std::filesystem::copy_file(projection_bytes, projection_stored);

    constexpr std::string_view logical_source = "shared-wall.glb";
    doc::Root root;
    root = root.set(asset_node(
        10U, std::string{assets::schema::model_type},
        std::string{logical_source}, model_identity.value(),
        assets::schema::policy_shared_store));
    root = root.set(asset_node(
        11U, std::string{assets::schema::projection_mesh_type},
        std::string{logical_source}, projection_identity.value(),
        assets::schema::policy_shared_store));
    root = root.set(placement(20U, scene::schema::model_type,
                              std::string{logical_source}, 10U));
    doc::Node surface{doc::NodeId{30U},
                      std::string{output::schema::surface_type}, {}};
    surface.properties.emplace(
        std::string{output::schema::surface_shape},
        doc::PropertyValue{std::string{output::schema::shape_imported_mesh}});
    surface.properties.emplace(
        std::string{output::schema::mesh_source},
        doc::PropertyValue{std::string{logical_source}});
    surface.properties.emplace(
        std::string{output::schema::mesh_asset_id},
        doc::PropertyValue{std::int64_t{11}});
    root = root.set(std::move(surface));

    assets::AssetModel model;
    model.reset(root);
    const auto resolved = app::resolve_project_assets(
        root, model.evaluate(),
        assets::ContentResolutionRoots{{},
                                       std::filesystem::absolute(store_root)});
    PROJECT_ASSET_CHECK(resolved.observations.size() == 2U);
    const std::string& model_runtime =
        resolved.placements.at(doc::NodeId{20U});
    const std::string& projection_runtime =
        resolved.placements.at(doc::NodeId{30U});
    PROJECT_ASSET_CHECK(model_runtime != projection_runtime);
    const auto verified_model = assets::verify_file(
        std::filesystem::path{model_runtime}, model_identity.value());
    const auto verified_projection = assets::verify_file(
        std::filesystem::path{projection_runtime},
        projection_identity.value());
    PROJECT_ASSET_CHECK(verified_model);
    PROJECT_ASSET_CHECK(verified_projection);
    PROJECT_ASSET_CHECK(verified_model.value() ==
                        assets::ContentVerification::Verified);
    PROJECT_ASSET_CHECK(verified_projection.value() ==
                        assets::ContentVerification::Verified);
    PROJECT_ASSET_CHECK(
        resolved.sources.at(app::ResolvedProjectAssets::SourceKey{
            assets::AssetKind::Model, std::string{logical_source}}) ==
        model_runtime);
    PROJECT_ASSET_CHECK(
        resolved.sources.at(app::ResolvedProjectAssets::SourceKey{
            assets::AssetKind::ProjectionMesh,
            std::string{logical_source}}) == projection_runtime);

    scene::Scene evaluated{};
    scene::ModelInstance placed_model{};
    placed_model.source = logical_source;
    placed_model.source_node = doc::NodeId{20U};
    evaluated.models.push_back(std::move(placed_model));
    resolved.apply(evaluated);
    PROJECT_ASSET_CHECK(evaluated.models[0U].render_source() ==
                        model_runtime);

    output::FeedMap feed{};
    output::ProjectionSurface physical{};
    physical.node = doc::NodeId{30U};
    physical.shape = output::ProjectionSurfaceShape::ImportedMesh;
    physical.mesh_source = logical_source;
    physical.mesh_asset = doc::NodeId{11U};
    feed.surfaces.push_back(std::move(physical));
    output::OutputMap map{};
    map.resolved_surface = output::ResolvedProjectionSurface{};
    map.resolved_surface->mesh_source = logical_source;
    map.resolved_surface->mesh_asset = doc::NodeId{11U};
    feed.maps.push_back(std::move(map));
    resolved.apply(feed);
    PROJECT_ASSET_CHECK(feed.surfaces[0U].render_mesh_source() ==
                        projection_runtime);
    PROJECT_ASSET_CHECK(feed.maps[0U].resolved_surface->render_mesh_source() ==
                        projection_runtime);
    std::filesystem::remove_all(temporary, ignored);
}

void a_verified_shared_store_path_is_runtime_only() {
    const auto temporary = std::filesystem::temp_directory_path() /
        "mediaserver-project-assets-test";
    std::error_code ignored;
    std::filesystem::remove_all(temporary, ignored);
    const auto source_root =
        temporary / std::filesystem::path{L"\u5a92\u9ad4-source"};
    const auto store_root =
        temporary / std::filesystem::path{L"\u8cc7\u7522-store"};
    std::filesystem::create_directories(source_root);
    const auto original = source_root / "clip.mp4";
    const std::string authored_source = path_utf8(original);
    {
        std::ofstream stream{original, std::ios::binary | std::ios::trunc};
        stream << "portable-media-bytes";
    }
    auto hashed = assets::hash_file(original);
    PROJECT_ASSET_CHECK(hashed);
    const assets::ContentIdentity identity = hashed.value();
    const auto relative = assets::shared_store_relative_path(
        identity, authored_source);
    PROJECT_ASSET_CHECK(relative.has_value());
    const auto stored = store_root / *relative;
    std::filesystem::create_directories(stored.parent_path());
    std::filesystem::copy_file(original, stored);

    doc::Root root;
    root = root.set(asset_node(
        10U, std::string{assets::schema::media_type}, authored_source,
        identity, assets::schema::policy_shared_store));
    root = root.set(
        placement(20U, scene::schema::media_type, authored_source, 10U));
    assets::AssetModel model;
    model.reset(root);
    const auto registry = model.evaluate();
    const auto manifest = app::collect_project_asset_manifest(root, registry);
    PROJECT_ASSET_CHECK(manifest.shared_store_ready());

    const auto resolved = app::resolve_project_assets(
        root, registry,
        assets::ContentResolutionRoots{{},
                                       std::filesystem::absolute(store_root)});
    PROJECT_ASSET_CHECK(resolved.observations.size() == 1U);
    PROJECT_ASSET_CHECK(resolved.observations[0U].state ==
                        app::LocalAssetState::Verified);

    scene::Scene evaluated{};
    scene::MediaInstance media{};
    media.source = authored_source;
    media.source_node = doc::NodeId{20U};
    evaluated.media.push_back(std::move(media));
    const auto before = scene::digest(evaluated);
    resolved.apply(evaluated);
    PROJECT_ASSET_CHECK(evaluated.media[0U].render_source() ==
                        path_utf8(stored));
    PROJECT_ASSET_CHECK(scene::digest(evaluated) == before);

    std::filesystem::remove_all(temporary, ignored);
}

void unresolved_retry_observes_same_manifest_store_arrival() {
    const auto temporary = std::filesystem::temp_directory_path() /
        "mediaserver-project-assets-retry-test";
    std::error_code ignored;
    std::filesystem::remove_all(temporary, ignored);
    const auto source_root = temporary / "source";
    const auto store_root = std::filesystem::absolute(temporary / "store");
    std::filesystem::create_directories(source_root);
    const auto present_source = source_root / "present.mp4";
    const auto arriving_source = source_root / "arriving.mp4";
    {
        std::ofstream stream{present_source,
                             std::ios::binary | std::ios::trunc};
        stream << "already-present-content";
    }
    {
        std::ofstream stream{arriving_source,
                             std::ios::binary | std::ios::trunc};
        stream << "content-that-arrives-after-start";
    }
    const auto present_identity = assets::hash_file(present_source);
    const auto arriving_identity = assets::hash_file(arriving_source);
    PROJECT_ASSET_CHECK(present_identity);
    PROJECT_ASSET_CHECK(arriving_identity);
    const auto present_relative = assets::shared_store_relative_path(
        present_identity.value(), path_utf8(present_source));
    const auto arriving_relative = assets::shared_store_relative_path(
        arriving_identity.value(), path_utf8(arriving_source));
    PROJECT_ASSET_CHECK(present_relative.has_value());
    PROJECT_ASSET_CHECK(arriving_relative.has_value());
    const auto present_alias = store_root / *present_relative;
    const auto arriving_alias = store_root / *arriving_relative;
    std::filesystem::create_directories(present_alias.parent_path());
    std::filesystem::copy_file(present_source, present_alias);

    doc::Root root;
    root = root.set(asset_node(
        10U, std::string{assets::schema::media_type},
        path_utf8(present_source), present_identity.value(),
        assets::schema::policy_shared_store));
    root = root.set(asset_node(
        11U, std::string{assets::schema::media_type},
        path_utf8(arriving_source), arriving_identity.value(),
        assets::schema::policy_shared_store));
    root = root.set(placement(
        20U, scene::schema::media_type, path_utf8(present_source), 10U));
    root = root.set(placement(
        21U, scene::schema::media_type, path_utf8(arriving_source), 11U));
    assets::AssetModel model;
    model.reset(root);
    const assets::AssetRegistry registry = model.evaluate();
    const auto manifest =
        app::collect_project_asset_manifest(root, registry);
    auto resolved = app::resolve_project_assets(
        root, registry, assets::ContentResolutionRoots{{}, store_root});
    PROJECT_ASSET_CHECK(resolved.observations.size() == 2U);
    PROJECT_ASSET_CHECK(resolved.observations[0U].state ==
                        app::LocalAssetState::Verified);
    PROJECT_ASSET_CHECK(resolved.observations[1U].state ==
                        app::LocalAssetState::Missing);
    PROJECT_ASSET_CHECK(resolved.has_retryable_observations());
    const std::string cached_present_path =
        resolved.assets.at(doc::NodeId{10U});

    // If retry accidentally performed another full-manifest resolution, this
    // removal would downgrade asset 10. It must remain cached while only the
    // previously missing asset is checked after its atomic store arrival.
    PROJECT_ASSET_CHECK(std::filesystem::remove(present_alias));
    PROJECT_ASSET_CHECK(std::filesystem::remove(
        store_root /
        assets::shared_store_blob_relative_path(present_identity.value())));
    std::filesystem::copy_file(arriving_source, arriving_alias);
    const auto unchanged_manifest =
        app::collect_project_asset_manifest(root, registry);
    PROJECT_ASSET_CHECK(unchanged_manifest.digest == manifest.digest);

    app::retry_unresolved_project_assets(
        resolved, root, registry,
        assets::ContentResolutionRoots{{}, store_root});
    PROJECT_ASSET_CHECK(resolved.observations.size() == 2U);
    PROJECT_ASSET_CHECK(resolved.observations[0U].state ==
                        app::LocalAssetState::Verified);
    PROJECT_ASSET_CHECK(resolved.observations[1U].state ==
                        app::LocalAssetState::Verified);
    PROJECT_ASSET_CHECK(!resolved.has_retryable_observations());
    PROJECT_ASSET_CHECK(resolved.assets.at(doc::NodeId{10U}) ==
                        cached_present_path);
    PROJECT_ASSET_CHECK(!resolved.assets.at(doc::NodeId{11U}).empty());

    scene::Scene evaluated{};
    scene::MediaInstance arriving{};
    arriving.source = path_utf8(arriving_source);
    arriving.source_node = doc::NodeId{21U};
    evaluated.media.push_back(std::move(arriving));
    resolved.apply(evaluated);
    PROJECT_ASSET_CHECK(evaluated.media[0U].render_source() ==
                        path_utf8(arriving_alias));
    std::filesystem::remove_all(temporary, ignored);
}

void corrupt_managed_content_suppresses_the_authored_fallback() {
    const auto temporary = std::filesystem::temp_directory_path() /
        "mediaserver-project-assets-corrupt-test";
    std::error_code ignored;
    std::filesystem::remove_all(temporary, ignored);
    std::filesystem::create_directories(temporary / "source");
    const auto original = temporary / "source" / "wall.obj";
    {
        std::ofstream stream{original, std::ios::binary | std::ios::trunc};
        stream << "v 0 0 0\nvt 0 0\n";
    }
    auto hashed = assets::hash_file(original);
    PROJECT_ASSET_CHECK(hashed);
    const auto relative = assets::shared_store_relative_path(
        hashed.value(), original.string());
    PROJECT_ASSET_CHECK(relative.has_value());
    const auto stored = temporary / "store" / *relative;
    std::filesystem::create_directories(stored.parent_path());
    {
        std::ofstream stream{stored, std::ios::binary | std::ios::trunc};
        stream << "wrong bytes of same-ish content";
    }

    doc::Root root;
    root = root.set(asset_node(
        10U, std::string{assets::schema::projection_mesh_type},
        original.string(), hashed.value(),
        assets::schema::policy_shared_store));
    doc::Node surface{doc::NodeId{30U},
                      std::string{output::schema::surface_type}, {}};
    surface.properties.emplace(
        std::string{output::schema::surface_shape},
        doc::PropertyValue{std::string{output::schema::shape_imported_mesh}});
    surface.properties.emplace(
        std::string{output::schema::mesh_source},
        doc::PropertyValue{original.string()});
    surface.properties.emplace(
        std::string{output::schema::mesh_asset_id},
        doc::PropertyValue{std::int64_t{10}});
    root = root.set(std::move(surface));
    assets::AssetModel model;
    model.reset(root);
    const auto resolved = app::resolve_project_assets(
        root, model.evaluate(),
        assets::ContentResolutionRoots{{},
                                       std::filesystem::absolute(
                                           temporary / "store")});
    PROJECT_ASSET_CHECK(resolved.observations.size() == 1U);
    PROJECT_ASSET_CHECK(resolved.observations[0U].state ==
                        app::LocalAssetState::Corrupt);

    output::FeedMap feed{};
    output::ProjectionSurface physical{};
    physical.node = doc::NodeId{30U};
    physical.shape = output::ProjectionSurfaceShape::ImportedMesh;
    physical.mesh_source = original.string();
    physical.mesh_asset = doc::NodeId{10U};
    feed.surfaces.push_back(std::move(physical));
    output::OutputMap map{};
    map.resolved_surface = output::ResolvedProjectionSurface{};
    map.resolved_surface->mesh_source = original.string();
    map.resolved_surface->mesh_asset = doc::NodeId{10U};
    feed.maps.push_back(std::move(map));
    const auto before = output::digest(feed);
    resolved.apply(feed);
    PROJECT_ASSET_CHECK(feed.maps[0U].resolved_surface->render_mesh_source()
                            .empty());
    PROJECT_ASSET_CHECK(feed.surfaces[0U].render_mesh_source().empty());
    PROJECT_ASSET_CHECK(output::digest(feed) == before);
    std::filesystem::remove_all(temporary, ignored);
}

void prepared_plan_is_fail_closed_before_background_verification() {
    assets::ContentIdentity content{};
    content.sha256.fill(0x5aU);
    content.byte_size = 3U;
    doc::Root root;
    root = root.set(asset_node(
        10U, std::string{assets::schema::media_type}, "managed.mp4",
        content, assets::schema::policy_shared_store));
    root = root.set(placement(
        20U, scene::schema::media_type, "managed.mp4", 10U));
    assets::AssetModel model;
    model.reset(root);
    const auto plan = app::prepare_project_asset_resolution(
        root, model.evaluate());
    PROJECT_ASSET_CHECK(plan.manifest.assets.size() == 1U);
    PROJECT_ASSET_CHECK(plan.fail_closed.observations.size() == 1U);
    PROJECT_ASSET_CHECK(plan.fail_closed.observations[0U].state ==
                        app::LocalAssetState::Checking);
    PROJECT_ASSET_CHECK(
        !plan.fail_closed.all_required_locally_usable(plan.manifest));
    PROJECT_ASSET_CHECK(
        !plan.fail_closed.all_required_verified(plan.manifest));

    scene::Scene evaluated{};
    scene::MediaInstance media{};
    media.source = "managed.mp4";
    media.source_node = doc::NodeId{20U};
    evaluated.media.push_back(std::move(media));
    plan.fail_closed.apply(evaluated);
    PROJECT_ASSET_CHECK(evaluated.media[0U].render_source().empty());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr Case cases[]{
    {"manifest contains only referenced assets and flags gaps",
     &manifest_contains_only_referenced_assets_and_flags_gaps},
    {"legacy source references choose the matching asset kind",
     &legacy_source_references_choose_the_matching_asset_kind},
    {"resolution digest ignores unrelated document edits",
     &resolution_digest_ignores_unrelated_document_edits},
    {"runtime resolution preserves role and placement identity",
     &runtime_resolution_preserves_role_and_placement_identity},
    {"a verified shared-store path is runtime-only",
     &a_verified_shared_store_path_is_runtime_only},
    {"unresolved retry observes same-manifest store arrival",
     &unresolved_retry_observes_same_manifest_store_arrival},
    {"corrupt managed content suppresses the authored fallback",
     &corrupt_managed_content_suppresses_the_authored_fallback},
    {"prepared plan is fail closed before background verification",
     &prepared_plan_is_fail_closed_before_background_verification},
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
