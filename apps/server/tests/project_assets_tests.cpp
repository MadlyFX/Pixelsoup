#include "project_assets.hpp"

#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/assets/asset_path.hpp>
#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace assets = mediaserver::assets;
namespace app = mediaserver::app;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace output = mediaserver::output;
namespace scene = mediaserver::scene;
namespace fs = std::filesystem;

#define PROJECT_ASSET_CHECK(condition)                                      \
    do {                                                                    \
        if (!(condition)) {                                                 \
            throw std::runtime_error{"check failed: " #condition};          \
        }                                                                   \
    } while (false)

// Sources must be absolute. These tests author them under one fixed root so
// the interesting part of each name stays readable at the call site.
[[nodiscard]] std::string src(std::string_view name) {
#if defined(_WIN32)
    return std::string{"D:\\show\\"} + std::string{name};
#else
    return std::string{"/show/"} + std::string{name};
#endif
}

[[nodiscard]] doc::Node asset_node(core::u64 id, std::string_view type,
                                   std::string source) {
    doc::Node node{doc::NodeId{id}, std::string{type}, {}};
    node.properties.emplace(std::string{assets::schema::display_name},
                            doc::PropertyValue{std::string{"Asset"}});
    node.properties.emplace(std::string{assets::schema::source},
                            doc::PropertyValue{std::move(source)});
    return node;
}

[[nodiscard]] doc::Node fingerprinted_asset_node(
    core::u64 id, std::string_view type, std::string source,
    const assets::ContentFingerprint& fingerprint) {
    doc::Node node = asset_node(id, type, std::move(source));
    node.properties.emplace(
        std::string{assets::schema::expected_byte_length},
        doc::PropertyValue{
            static_cast<std::int64_t>(fingerprint.byte_length)});
    node.properties.emplace(
        std::string{assets::schema::expected_sha256},
        doc::PropertyValue{assets::sha256_hex(fingerprint.sha256)});
    return node;
}

[[nodiscard]] doc::Node placement(core::u64 id, std::string_view type,
                                  std::string source, core::u64 asset = 0U) {
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

[[nodiscard]] doc::Node imported_surface(core::u64 id, std::string source,
                                         core::u64 asset = 0U) {
    doc::Node node{doc::NodeId{id}, std::string{output::schema::surface_type},
                   {}};
    node.properties.emplace(
        std::string{output::schema::surface_shape},
        doc::PropertyValue{std::string{output::schema::shape_imported_mesh}});
    node.properties.emplace(std::string{output::schema::mesh_source},
                            doc::PropertyValue{std::move(source)});
    if (asset != 0U) {
        node.properties.emplace(
            std::string{output::schema::mesh_asset_id},
            doc::PropertyValue{static_cast<std::int64_t>(asset)});
    }
    return node;
}

[[nodiscard]] app::ProjectAssetManifest manifest_of(const doc::Root& root) {
    assets::AssetModel model;
    model.reset(root);
    return app::collect_project_asset_manifest(root, model.evaluate());
}

[[nodiscard]] bool requires_source(const app::ProjectAssetManifest& manifest,
                                   std::string_view source) {
    return std::any_of(manifest.assets.begin(), manifest.assets.end(),
                       [source](const app::RequiredProjectAsset& required) {
                           return required.source == source;
                       });
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = fs::temp_directory_path() /
            ("mediaserver_project_assets_" + std::to_string(stamp));
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

void the_manifest_is_what_the_document_actually_opens() {
    doc::Root root;
    // An imported-but-unplaced Library row costs nothing: an operator can keep
    // a shelf of assets without a node reporting them missing.
    root = root.set(
        asset_node(10U, assets::schema::media_type, src("clip.mp4")));
    root = root.set(
        asset_node(11U, assets::schema::model_type, src("unused.glb")));
    root = root.set(asset_node(12U, assets::schema::projection_mesh_type,
                               src("wall.obj")));
    root = root.set(
        placement(20U, scene::schema::media_type, src("clip.mp4"), 10U));
    root = root.set(imported_surface(30U, src("wall.obj"), 12U));

    const auto manifest = manifest_of(root);
    PROJECT_ASSET_CHECK(manifest.assets.size() == 2U);
    PROJECT_ASSET_CHECK(requires_source(manifest, src("clip.mp4")));
    PROJECT_ASSET_CHECK(requires_source(manifest, src("wall.obj")));
    PROJECT_ASSET_CHECK(!requires_source(manifest, src("unused.glb")));
    PROJECT_ASSET_CHECK(manifest.kind_mismatches == 0U);
}

void a_placement_with_no_library_record_still_has_to_be_opened() {
    // This is the whole point of the path model. Deleting the Library row does
    // not change which file the renderer opens, so it must not change which
    // file a node is asked about either - otherwise the Director and the node
    // disagree about what "ready" means.
    doc::Root root;
    root = root.set(
        asset_node(10U, assets::schema::media_type, src("clip.mp4")));
    root = root.set(
        placement(20U, scene::schema::media_type, src("clip.mp4"), 10U));

    const auto with_record = manifest_of(root);
    PROJECT_ASSET_CHECK(with_record.assets.size() == 1U);
    PROJECT_ASSET_CHECK(with_record.assets[0U].asset == doc::NodeId{10U});

    const auto erased = root.erase(doc::NodeId{10U});
    PROJECT_ASSET_CHECK(erased.has_value());
    const auto without_record = manifest_of(*erased);
    PROJECT_ASSET_CHECK(without_record.assets.size() == 1U);
    PROJECT_ASSET_CHECK(without_record.assets[0U].source == src("clip.mp4"));
    // Only the Library link is gone; the file the show opens is unchanged.
    PROJECT_ASSET_CHECK(without_record.assets[0U].asset == doc::NodeId{});
}

void source_references_choose_the_matching_asset_role() {
    const std::string source = src("shared-wall.obj");
    doc::Root root;
    root = root.set(asset_node(10U, assets::schema::model_type, source));
    root = root.set(
        asset_node(11U, assets::schema::projection_mesh_type, source));
    // Placements without asset_id must resolve by both source and role rather
    // than by whichever kind holds the lower NodeId.
    root = root.set(placement(20U, scene::schema::model_type, source));
    root = root.set(imported_surface(30U, source));

    const auto manifest = manifest_of(root);
    // One file, two manifest roles, but only one path to check.
    PROJECT_ASSET_CHECK(manifest.assets.size() == 2U);
    PROJECT_ASSET_CHECK(manifest.assets[0U].kind == assets::AssetKind::Model);
    PROJECT_ASSET_CHECK(manifest.assets[0U].asset == doc::NodeId{10U});
    PROJECT_ASSET_CHECK(manifest.assets[1U].kind ==
                        assets::AssetKind::ProjectionMesh);
    PROJECT_ASSET_CHECK(manifest.assets[1U].asset == doc::NodeId{11U});
    PROJECT_ASSET_CHECK(manifest.kind_mismatches == 0U);
}

void an_asset_id_naming_the_wrong_role_is_counted_and_repaired() {
    doc::Root root;
    root = root.set(
        asset_node(10U, assets::schema::media_type, src("clip.mp4")));
    root = root.set(
        asset_node(11U, assets::schema::model_type, src("set.glb")));
    // The placement is a model but points at the media record.
    root = root.set(
        placement(20U, scene::schema::model_type, src("set.glb"), 10U));

    const auto manifest = manifest_of(root);
    PROJECT_ASSET_CHECK(manifest.kind_mismatches == 1U);
    // The mismatch is an authoring error, not a reason to stop opening the
    // file: exact-source lookup still finds the right record.
    PROJECT_ASSET_CHECK(manifest.assets.size() == 1U);
    PROJECT_ASSET_CHECK(manifest.assets[0U].source == src("set.glb"));
    PROJECT_ASSET_CHECK(manifest.assets[0U].asset == doc::NodeId{11U});
}

void the_digest_tracks_required_paths_and_nothing_else() {
    doc::Root root;
    root = root.set(
        asset_node(10U, assets::schema::media_type, src("clip.mp4")));
    root = root.set(
        placement(20U, scene::schema::media_type, src("clip.mp4"), 10U));
    const core::u64 reference = manifest_of(root).digest;

    // Moving the clip in the scene changes the document, not the manifest.
    doc::Node moved = *root.get(doc::NodeId{20U});
    moved.properties.insert_or_assign(std::string{scene::schema::position_x},
                                      doc::PropertyValue{4.0});
    PROJECT_ASSET_CHECK(manifest_of(root.set(moved)).digest == reference);

    // Adding an unused Library row does not either.
    PROJECT_ASSET_CHECK(
        manifest_of(root.set(asset_node(11U, assets::schema::model_type,
                                        src("shelf.glb"))))
            .digest == reference);

    // Repointing the placement at another file does.
    doc::Node repointed = *root.get(doc::NodeId{20U});
    repointed.properties.insert_or_assign(
        std::string{scene::schema::source},
        doc::PropertyValue{src("renamed.mp4")});
    PROJECT_ASSET_CHECK(manifest_of(root.set(repointed)).digest != reference);
}

void availability_reports_each_missing_path_against_its_manifest() {
    TemporaryDirectory temporary;
    const fs::path present = temporary.path() / "clip.mp4";
    write(present, "frames");
    const std::string present_source =
        assets::asset_source_from_path(present);
    const std::string absent_source = assets::asset_source_from_path(
        temporary.path() / "gone.glb");

    doc::Root root;
    root = root.set(
        asset_node(10U, assets::schema::media_type, present_source));
    root = root.set(
        placement(20U, scene::schema::media_type, present_source, 10U));
    const auto complete = manifest_of(root);
    const auto ready = app::check_project_assets(complete);
    PROJECT_ASSET_CHECK(ready.ready());
    PROJECT_ASSET_CHECK(ready.missing == 0U);
    PROJECT_ASSET_CHECK(ready.digest == complete.digest);
    PROJECT_ASSET_CHECK(ready.assets.size() == 1U);
    PROJECT_ASSET_CHECK(ready.assets[0U].present);

    root = root.set(placement(21U, scene::schema::model_type, absent_source));
    const auto incomplete = manifest_of(root);
    // The answer is bound to the manifest it answered, so a stale report can
    // never be read as readiness for a newer one.
    PROJECT_ASSET_CHECK(incomplete.digest != complete.digest);
    const auto partial = app::check_project_assets(incomplete);
    PROJECT_ASSET_CHECK(!partial.ready());
    PROJECT_ASSET_CHECK(partial.missing == 1U);
    PROJECT_ASSET_CHECK(partial.digest == incomplete.digest);
    const auto missing = std::find_if(
        partial.assets.begin(), partial.assets.end(),
        [](const app::ProjectAssetPresence& presence) {
            return !presence.present;
        });
    PROJECT_ASSET_CHECK(missing != partial.assets.end());
    PROJECT_ASSET_CHECK(missing->source == absent_source);
}

void fingerprinted_readiness_distinguishes_size_and_revision_mismatches() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "revision.mp4";
    write(source_path, "frames");
    const std::string source = assets::asset_source_from_path(source_path);
    const auto expected = assets::source_fingerprint(source);
    PROJECT_ASSET_CHECK(expected);

    doc::Root root;
    root = root.set(fingerprinted_asset_node(
        10U, assets::schema::media_type, source, expected.value()));
    root = root.set(
        placement(20U, scene::schema::media_type, source, 10U));
    const auto manifest = manifest_of(root);
    PROJECT_ASSET_CHECK(manifest.assets.size() == 1U);
    PROJECT_ASSET_CHECK(
        manifest.assets[0U].expected_fingerprint == expected.value());
    const auto ready = app::check_project_assets(manifest);
    PROJECT_ASSET_CHECK(ready.ready());
    PROJECT_ASSET_CHECK(
        ready.assets[0U].state == app::ProjectAssetPresence::State::Ready);

    const auto original_time = fs::last_write_time(source_path);
    write(source_path, "FRAMES");
    fs::last_write_time(source_path,
                        original_time + std::chrono::seconds{2});
    const auto hash_mismatch = app::check_project_assets(manifest);
    PROJECT_ASSET_CHECK(!hash_mismatch.ready());
    PROJECT_ASSET_CHECK(hash_mismatch.mismatched == 1U);
    PROJECT_ASSET_CHECK(hash_mismatch.missing == 0U);
    PROJECT_ASSET_CHECK(
        hash_mismatch.assets[0U].state ==
        app::ProjectAssetPresence::State::Sha256Mismatch);

    write(source_path, "more frames");
    fs::last_write_time(source_path,
                        original_time + std::chrono::seconds{4});
    const auto size_mismatch = app::check_project_assets(manifest);
    PROJECT_ASSET_CHECK(!size_mismatch.ready());
    PROJECT_ASSET_CHECK(size_mismatch.mismatched == 1U);
    PROJECT_ASSET_CHECK(
        size_mismatch.assets[0U].state ==
        app::ProjectAssetPresence::State::ByteLengthMismatch);
}

void the_routine_scan_never_reads_a_source_and_never_calls_it_stale() {
    TemporaryDirectory temporary;
    const fs::path settled_path = temporary.path() / "settled.mp4";
    const fs::path late_path = temporary.path() / "late.mp4";
    write(settled_path, "frames");
    const std::string settled = assets::asset_source_from_path(settled_path);
    const std::string late = assets::asset_source_from_path(late_path);
    const auto settled_print = assets::content_fingerprint(settled);
    PROJECT_ASSET_CHECK(settled_print);

    doc::Root root;
    root = root.set(fingerprinted_asset_node(
        10U, assets::schema::media_type, settled, settled_print.value()));
    root = root.set(placement(20U, scene::schema::media_type, settled, 10U));
    root = root.set(asset_node(30U, assets::schema::media_type, late));
    root = root.set(placement(40U, scene::schema::media_type, late, 30U));
    const auto manifest = manifest_of(root);
    PROJECT_ASSET_CHECK(manifest.assets.size() == 2U);

    // One present, one absent. Presence is the whole verdict.
    const auto scanned = app::scan_project_assets(manifest);
    PROJECT_ASSET_CHECK(!scanned.ready());
    PROJECT_ASSET_CHECK(scanned.missing == 1U);
    PROJECT_ASSET_CHECK(scanned.mismatched == 0U);

    // Replace the settled source with different bytes. A scan cannot see that
    // and must not pretend to: it reports the file as present, reads nothing,
    // and leaves content to the operator-driven check that does read.
    write(settled_path, "FRAMES");
    const auto rescanned = app::scan_project_assets(manifest);
    const auto settled_at = std::find_if(
        rescanned.assets.begin(), rescanned.assets.end(),
        [&](const app::ProjectAssetPresence& candidate) {
            return candidate.source == settled;
        });
    PROJECT_ASSET_CHECK(settled_at != rescanned.assets.end());
    PROJECT_ASSET_CHECK(
        settled_at->state == app::ProjectAssetPresence::State::Ready);
    PROJECT_ASSET_CHECK(rescanned.mismatched == 0U);
    PROJECT_ASSET_CHECK(!settled_at->observed_fingerprint.has_value());

    // The same manifest, verified, does report it - that is the difference
    // between the two, and the reason the expensive one is asked for.
    PROJECT_ASSET_CHECK(app::check_project_assets(manifest).mismatched == 1U);

    // A file arriving is what a rescan is for, and it costs one stat.
    write(late_path, "late frames");
    const auto arrived = app::scan_project_assets(manifest);
    PROJECT_ASSET_CHECK(arrived.missing == 0U);
    PROJECT_ASSET_CHECK(arrived.ready());

    // And a file leaving is still caught.
    fs::remove(late_path);
    PROJECT_ASSET_CHECK(app::scan_project_assets(manifest).missing == 1U);
}

void manifest_digest_tracks_expected_revision_at_one_path() {
    const auto first_sha = assets::sha256_from_hex(
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad");
    PROJECT_ASSET_CHECK(first_sha.has_value());
    assets::ContentFingerprint first{3U, *first_sha};
    const std::string source = src("revision.mp4");
    doc::Root root;
    root = root.set(fingerprinted_asset_node(
        10U, assets::schema::media_type, source, first));
    root = root.set(
        placement(20U, scene::schema::media_type, source, 10U));
    const core::u64 reference = manifest_of(root).digest;

    assets::ContentFingerprint second = first;
    second.sha256.bytes[0U] ^= 0xffU;
    root = root.set(fingerprinted_asset_node(
        10U, assets::schema::media_type, source, second));
    PROJECT_ASSET_CHECK(manifest_of(root).digest != reference);
}

void a_relative_placement_source_never_enters_the_manifest() {
    doc::Root root;
    // Nothing can promise a relative path means the same file on two machines,
    // so it is dropped rather than sent to a node as a question it cannot
    // answer consistently.
    root = root.set(placement(20U, scene::schema::media_type, "clip.mp4"));
    root = root.set(
        placement(21U, scene::schema::model_type, "../escape.glb"));
    const auto manifest = manifest_of(root);
    PROJECT_ASSET_CHECK(manifest.assets.empty());
    PROJECT_ASSET_CHECK(app::check_project_assets(manifest).ready());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr Case cases[]{
    {"the manifest is what the document actually opens",
     &the_manifest_is_what_the_document_actually_opens},
    {"a placement with no Library record still has to be opened",
     &a_placement_with_no_library_record_still_has_to_be_opened},
    {"source references choose the matching asset role",
     &source_references_choose_the_matching_asset_role},
    {"an asset_id naming the wrong role is counted and repaired",
     &an_asset_id_naming_the_wrong_role_is_counted_and_repaired},
    {"the digest tracks required paths and nothing else",
     &the_digest_tracks_required_paths_and_nothing_else},
    {"availability reports each missing path against its manifest",
     &availability_reports_each_missing_path_against_its_manifest},
    {"the routine scan never reads a source or calls it stale",
     &the_routine_scan_never_reads_a_source_and_never_calls_it_stale},
    {"fingerprinted readiness distinguishes size and revision mismatches",
     &fingerprinted_readiness_distinguishes_size_and_revision_mismatches},
    {"manifest digest tracks expected revision at one path",
     &manifest_digest_tracks_expected_revision_at_one_path},
    {"a relative placement source never enters the manifest",
     &a_relative_placement_source_never_enters_the_manifest},
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
