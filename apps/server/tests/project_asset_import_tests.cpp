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

[[nodiscard]] assets::MediaAsset fingerprinted_media(
    std::string source, mediaserver::core::u64 revision) {
    assets::MediaAsset asset = media(std::move(source));
    assets::ContentFingerprint fingerprint{};
    fingerprint.byte_length = 1'000U + revision;
    fingerprint.sha256.bytes[0U] =
        static_cast<mediaserver::core::u8>(revision & 0xffU);
    asset.fingerprint = fingerprint;
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
    IMPORT_CHECK(assets::asset_fingerprint(prepared.value()).has_value());
    IMPORT_CHECK(
        assets::asset_fingerprint(prepared.value())->byte_length == 6U);

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
    IMPORT_CHECK(std::get<std::int64_t>(node.properties.at(
                     std::string{assets::schema::expected_byte_length})) == 6);
    IMPORT_CHECK(std::get<std::string>(node.properties.at(
                     std::string{assets::schema::expected_sha256})) ==
                 assets::sha256_hex(
                     assets::asset_fingerprint(prepared.value())->sha256));
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

// The Library's columns read what import recorded, and an import of a file
// the Library already holds keeps it in the folder the operator filed it in.
void library_metadata_round_trips_and_reimport_keeps_the_folder() {
    assets::MediaAsset clip = media(absolute_source("interview.mov"));
    clip.codec = "prores";
    clip.start_timecode = "01:00:00:00";
    clip.folder = doc::NodeId{40U};
    assets::ModelAsset car{};
    car.display_name = "Car";
    car.source = absolute_source("car.glb");
    car.vertex_count = 64'000U;
    car.triangle_count = 30'000U;
    car.texture_count = 3U;
    car.largest_texture = assets::PixelDimensions{4'096U, 4'096U};

    doc::Root root;
    root = root.set(app::make_project_asset_node(doc::NodeId{7U},
                                                 assets::Asset{clip}));
    root = root.set(app::make_project_asset_node(doc::NodeId{8U},
                                                 assets::Asset{car}));
    assets::AssetModel model;
    model.reset(root);
    const assets::AssetRegistry registry = model.evaluate();
    const auto* const read_clip =
        std::get_if<assets::MediaAsset>(registry.asset(doc::NodeId{7U}));
    IMPORT_CHECK(read_clip != nullptr);
    IMPORT_CHECK(read_clip->codec == "prores");
    IMPORT_CHECK(read_clip->start_timecode == "01:00:00:00");
    IMPORT_CHECK(read_clip->folder == doc::NodeId{40U});
    const auto* const read_car =
        std::get_if<assets::ModelAsset>(registry.asset(doc::NodeId{8U}));
    IMPORT_CHECK(read_car != nullptr);
    IMPORT_CHECK(read_car->vertex_count == 64'000U);
    IMPORT_CHECK(read_car->texture_count == 3U);
    IMPORT_CHECK((read_car->largest_texture ==
                  assets::PixelDimensions{4'096U, 4'096U}));

    assets::MediaAsset again = clip;
    again.folder = {};
    const auto upsert = app::make_project_asset_upsert(
        root, assets::Asset{again}, doc::NodeId{99U});
    IMPORT_CHECK(upsert.after.id == doc::NodeId{7U});
    IMPORT_CHECK(std::get<std::int64_t>(upsert.after.properties.at(
                     std::string{assets::schema::folder})) == 40);
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

void particle_effect_packages_are_asset_records() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "embers.ajfx";
    write(source, "AJFX package");
    assets::ParticleEffectAsset effect{};
    effect.display_name = "Embers";
    effect.source = assets::asset_source_from_path(source);
    auto prepared = app::prepare_project_asset_import(assets::Asset{effect});
    IMPORT_CHECK(static_cast<bool>(prepared));
    IMPORT_CHECK(assets::asset_fingerprint(prepared.value())->byte_length == 12U);
    const doc::Node node = app::make_project_asset_node(doc::NodeId{14U}, prepared.value());
    IMPORT_CHECK(node.type == assets::schema::particle_effect_type);
    // Read back, it is the same record.
    assets::AssetModel model;
    model.reset(doc::Root{}.set(node));
    const assets::AssetRegistry registry = model.evaluate();
    const auto* const held = registry.asset(doc::NodeId{14U});
    IMPORT_CHECK(held != nullptr && assets::asset_kind(*held) == assets::AssetKind::ParticleEffect);
    IMPORT_CHECK(assets::asset_source(*held) == effect.source);
    IMPORT_CHECK(assets::asset_fingerprint(*held) == assets::asset_fingerprint(prepared.value()));
    // And unplaced, it can be removed.
    IMPORT_CHECK(static_cast<bool>(app::plan_project_asset_delete(doc::Root{}.set(node), doc::NodeId{14U})));
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

void referenced_assets_report_every_use_and_refuse_deletion() {
    const doc::NodeId asset_id{7U};
    doc::Root root;
    root = root.set(app::make_project_asset_node(
        asset_id, assets::Asset{fingerprinted_media(
                      absolute_source("plate.mp4"), 1U)}));
    doc::Node placement{};
    placement.id = doc::NodeId{20U};
    placement.type = "scene.media";
    placement.properties.emplace("asset_id", std::int64_t{7});
    root = root.set(placement);
    doc::Node projection{};
    projection.id = doc::NodeId{30U};
    projection.type = "output.surface";
    projection.properties.emplace("mesh_asset_id", std::int64_t{7});
    root = root.set(projection);

    const auto plan = app::plan_project_asset_delete(root, asset_id);
    IMPORT_CHECK(!plan);
    IMPORT_CHECK(plan.error().code ==
                 app::ProjectAssetAuthoringErrorCode::InUse);
    IMPORT_CHECK(plan.error().references ==
                 (std::vector<app::ProjectAssetReference>{
                     {doc::NodeId{20U}, "asset_id"},
                     {doc::NodeId{30U}, "mesh_asset_id"}}));
    IMPORT_CHECK(root.get(asset_id) != nullptr);
}

void unreferenced_asset_deletion_is_one_exact_replacement_plan() {
    const doc::NodeId asset_id{7U};
    const doc::Node node = app::make_project_asset_node(
        asset_id, assets::Asset{fingerprinted_media(
                      absolute_source("unused.mp4"), 2U)});
    doc::Root root;
    root = root.set(node);
    const auto plan = app::plan_project_asset_delete(root, asset_id);
    IMPORT_CHECK(plan);
    IMPORT_CHECK(plan.value().erased == node);
    IMPORT_CHECK(plan.value().after.get(asset_id) == nullptr);
    IMPORT_CHECK(root.get(asset_id) != nullptr);
}

void accept_revision_preserves_identity_policy_and_unknown_metadata() {
    const doc::NodeId asset_id{7U};
    assets::MediaAsset old_asset = fingerprinted_media(
        absolute_source("plate.mp4"), 1U);
    old_asset.display_name = "Stable plate";
    old_asset.requested_proxy_profile =
        assets::DerivativeProfileId::Proxy720pCfrH264;
    old_asset.proxy_playback_policy =
        assets::ProxyPlaybackPolicy::RequireProxy;
    doc::Node old_node = app::make_project_asset_node(
        asset_id, assets::Asset{old_asset});
    old_node.properties.emplace("operator_note", std::string{"hero"});
    doc::Root root;
    root = root.set(old_node);

    assets::MediaAsset replacement = fingerprinted_media(
        old_asset.source, 9U);
    replacement.display_name = "File chooser name";
    replacement.frame_count = 999U;
    const auto plan = app::plan_project_asset_revision(
        root, asset_id, assets::Asset{replacement},
        app::ProjectAssetRevisionMode::AcceptCurrentSource);
    IMPORT_CHECK(plan);
    IMPORT_CHECK(plan.value().modified);
    IMPORT_CHECK(plan.value().before == old_node);
    IMPORT_CHECK(plan.value().changed.id == asset_id);
    IMPORT_CHECK(std::get<std::string>(plan.value().changed.properties.at(
                     std::string{assets::schema::display_name})) ==
                 "Stable plate");
    IMPORT_CHECK(std::get<std::int64_t>(plan.value().changed.properties.at(
                     std::string{assets::schema::media_frame_count})) == 999);
    IMPORT_CHECK(std::get<std::string>(plan.value().changed.properties.at(
                     "operator_note")) == "hero");
    IMPORT_CHECK(std::get<std::string>(plan.value().changed.properties.at(
                     std::string{assets::schema::proxy_playback_policy})) ==
                 assets::schema::proxy_policy_require);
    IMPORT_CHECK(plan.value().after.get(asset_id) != nullptr);
}

void relink_changes_only_the_content_locator_and_probe_fields() {
    const doc::NodeId asset_id{7U};
    const assets::MediaAsset old_asset = fingerprinted_media(
        absolute_source("old.mp4"), 1U);
    doc::Root root;
    root = root.set(app::make_project_asset_node(
        asset_id, assets::Asset{old_asset}));
    assets::MediaAsset replacement = fingerprinted_media(
        absolute_source("new.mp4"), 2U);
    replacement.frame_rate = assets::MediaFrameRate{30'000U, 1'001U};

    const auto rejected = app::plan_project_asset_revision(
        root, asset_id, assets::Asset{replacement},
        app::ProjectAssetRevisionMode::AcceptCurrentSource);
    IMPORT_CHECK(!rejected);
    IMPORT_CHECK(rejected.error().code ==
                 app::ProjectAssetAuthoringErrorCode::SourceMismatch);

    const auto relinked = app::plan_project_asset_revision(
        root, asset_id, assets::Asset{replacement},
        app::ProjectAssetRevisionMode::Relink);
    IMPORT_CHECK(relinked);
    IMPORT_CHECK(std::get<std::string>(relinked.value().changed.properties.at(
                     std::string{assets::schema::source})) ==
                 replacement.source);
    IMPORT_CHECK(std::get<std::int64_t>(
                     relinked.value().changed.properties.at(
                         std::string{assets::schema::media_rate_numerator})) ==
                 30'000);
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
    {"Library metadata round-trips and reimport keeps the folder",
     &library_metadata_round_trips_and_reimport_keeps_the_folder},
    {"projection meshes are first-class asset records",
     &projection_meshes_are_first_class_asset_records},
    {"particle effect packages are asset records",
     &particle_effect_packages_are_asset_records},
    {"one OBJ can be a scene model and a projection mesh",
     &one_obj_can_be_a_scene_model_and_a_projection_mesh},
    {"referenced assets report every use and refuse deletion",
     &referenced_assets_report_every_use_and_refuse_deletion},
    {"unreferenced asset deletion is one exact replacement plan",
     &unreferenced_asset_deletion_is_one_exact_replacement_plan},
    {"accept revision preserves identity policy and unknown metadata",
     &accept_revision_preserves_identity_policy_and_unknown_metadata},
    {"relink changes only the content locator and probe fields",
     &relink_changes_only_the_content_locator_and_probe_fields},
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
